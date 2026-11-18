#include "platform/native_disc_image.h"

#include <platform/native_path.h>

#include <limits.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_DISC_IMAGE_PATH_MAX          1024
#define NATIVE_DISC_IMAGE_BIN_PATH          "ctr-u.bin"
#define NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE   2352u
#define NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET 24u
#define NATIVE_DISC_IMAGE_FORM1_DATA_SIZE   2048u
#define NATIVE_DISC_IMAGE_MODE2_USER_OFFSET 16u
#define NATIVE_DISC_IMAGE_MODE2_USER_SIZE   2336u
#define NATIVE_DISC_IMAGE_PVD_LBA           16u
#define NATIVE_DISC_IMAGE_PVD_ROOT_RECORD   156u
#define NATIVE_DISC_IMAGE_DIRECTORY_FLAG    0x02u

// NOTE(aalhendi): This hardcodes only the common NTSC-U raw BIN layout:
// one MODE2/2352 data track at byte zero. The user still supplies all disc
// contents; native only uses this sector contract to read the image.

struct NativeDiscImageDirRecord
{
	u32 lba;
	u32 size;
	u8 flags;
	u8 nameLen;
	const u8 *name;
};

global_variable char s_nativeDiscImagePath[NATIVE_DISC_IMAGE_PATH_MAX];
global_variable FILE *s_nativeDiscImageFile;
global_variable struct NativeDiscImageFile s_nativeDiscImageRoot;
global_variable int s_nativeDiscImageAvailable;

#if defined(CTR_PS2)
// NOTE: PS2 build. PS2 discs (ISO9660) only allow short upper-case names, so a
// disc build stores the image as e.g. cdrom0:\CTR_U.BIN. main_ps2.c sets this
// when it finds the image there; it takes precedence over assets/ctr-u.bin.
const char *g_ps2DiscImagePathOverride = NULL;
#endif

internal int NativeDiscImage_FindHostImagePath(char *dst, size_t dstSize, NativeStr8 assetsDir)
{
#if defined(CTR_PS2)
	if ((g_ps2DiscImagePathOverride != NULL) && (dst != NULL) && (dstSize > 0))
	{
		snprintf(dst, dstSize, "%s", g_ps2DiscImagePathOverride);
		return 1;
	}
#endif
#if defined(_WIN32)
	return NativePath_Join(dst, dstSize, assetsDir, NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH));
#else
	char dirPath[NATIVE_DISC_IMAGE_PATH_MAX];
	struct dirent *entry;
	int found = 0;

	if (!NativePath_NormalizeSlashes(dirPath, sizeof(dirPath), assetsDir))
	{
		return 0;
	}

	DIR *dir = opendir(dirPath);
	if (dir != NULL)
	{
		while ((entry = readdir(dir)) != NULL)
		{
			NativeStr8 entryName = NativeStr8_FromCString(entry->d_name);

			if (!NativeStr8_EqualsIgnoreCaseAscii(entryName, NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH)))
			{
				continue;
			}

			found = NativePath_Join(dst, dstSize, NativeStr8_FromCString(dirPath), entryName);
			break;
		}

		closedir(dir);
	}

	// NOTE: Some file systems can open files but cannot list directories
	// (e.g. PCSX2's host: device). Fall back to the exact file name, like the
	// Windows path above; NativeDiscImage_Init() fails cleanly if fopen() does.
	if (!found)
	{
		found = NativePath_Join(dst, dstSize, NativeStr8_FromCString(dirPath), NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH));
	}

	return found;
#endif
}

internal u32 NativeDiscImage_ReadLE32(const u8 *data)
{
	return ((u32)data[0]) | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

internal int NativeDiscImage_CheckRawSectorHeader(const u8 *sector)
{
	if ((sector[0] != 0x00) || (sector[11] != 0x00) || (sector[15] != 0x02))
	{
		return 0;
	}

	for (u32 i = 1; i < 11; i++)
	{
		if (sector[i] != 0xff)
		{
			return 0;
		}
	}

	return 1;
}

#if defined(CTR_PS2)
// NOTE: PS2 build only. Each fseek/fread on the PS2 (PCSX2 host:, USB, CD) is
// an IOP round trip, and XA preparation / level loads read one sector at a
// time. Read a run of consecutive sectors at once and serve the following
// requests from memory.
#define NATIVE_DISC_IMAGE_CACHE_SECTORS 64u
// Varios huecos de cache (se reemplaza el menos usado). El juego lee de varias
// zonas a la vez: el indice de archivos de la imagen de PS1 (lo consulta en cada
// busqueda), los archivos que carga y la musica XA que suena. Con pocos huecos
// se expulsan entre si y cada lectura es un salto del lector (~125 ms).
// 6 huecos x ~150 KB = ~900 KB de memoria.
#define NATIVE_DISC_IMAGE_CACHE_SLOTS 6
static u8 s_nativeDiscImageCacheSlots[NATIVE_DISC_IMAGE_CACHE_SLOTS][NATIVE_DISC_IMAGE_CACHE_SECTORS * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE] __attribute__((aligned(64)));
static u32 s_nativeDiscImageSlotStart[NATIVE_DISC_IMAGE_CACHE_SLOTS];
static u32 s_nativeDiscImageSlotCount[NATIVE_DISC_IMAGE_CACHE_SLOTS];
static u32 s_nativeDiscImageSlotUse[NATIVE_DISC_IMAGE_CACHE_SLOTS];
static u32 s_nativeDiscImageUseClock = 0;
// Hueco activo (lo usan las rutinas de lectura de abajo).
static u8 *s_nativeDiscImageCache = s_nativeDiscImageCacheSlots[0];
static u32 s_nativeDiscImageCacheStart = 0;
static u32 s_nativeDiscImageCacheCount = 0;
static int s_nativeDiscImageActiveSlot = 0;
// El ultimo hueco es SOLO para la musica XA (native_audio.c pone
// g_ps2DiscXaRead = 1 mientras lee). Asi las cargas del juego no lo expulsan:
// antes, en la intro, cada carga echaba a la XA de la cache y la XA tenia que
// volver a leer lo mismo con un salto del lector (~125 ms) parando el juego.
#define NATIVE_DISC_IMAGE_XA_SLOT (NATIVE_DISC_IMAGE_CACHE_SLOTS - 1)
int g_ps2DiscXaRead = 0;
#define NATIVE_DISC_IMAGE_CACHE_BYTES (NATIVE_DISC_IMAGE_CACHE_SECTORS * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE)

// NOTE: PS2 build. When the image is on the real disc (cdrom0:), read it by
// DVD sector with sceCdRead instead of fopen/fseek/fread: no file layer, no
// unsupported-op ("callnulldev") calls, and it is what OPL emulates best.
// Implemented in platform/ps2/ps2_cd.c (separate object, see note there).
int Ps2Cd_SearchFile(const char *name, unsigned int *lsnOut, unsigned int *sizeOut);
int Ps2Cd_ReadSectors(unsigned int lsn, unsigned int count, void *buf);
#define NATIVE_PS2_CD_SECTOR 2048u
#define NATIVE_PS2_CD_BUF_SECTORS (((NATIVE_DISC_IMAGE_CACHE_SECTORS * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) / NATIVE_PS2_CD_SECTOR) + 2u)
static u8 s_ps2CdBuf[NATIVE_PS2_CD_BUF_SECTORS * NATIVE_PS2_CD_SECTOR] __attribute__((aligned(64)));
static int s_ps2CdDirect = 0;
// 1 mientras se esta leyendo del disco (el audio se genera durante la espera y
// no debe empezar otra lectura a medias, ver native_audio.c).
int g_ps2DiscReadBusy = 0;
static u32 s_ps2CdLsn = 0;
static u32 s_ps2CdSize = 0;

internal int NativeDiscImage_Ps2CdOpen(const char *path)
{
	char name[NATIVE_DISC_IMAGE_PATH_MAX];

	s_ps2CdDirect = 0;
	if ((path == NULL) || (strncmp(path, "cdrom0:", 7) != 0))
	{
		return 0;
	}

	const char *rest = path + 7;
	snprintf(name, sizeof(name), "%s%s%s", (rest[0] == '\\') ? "" : "\\", rest, (strchr(rest, ';') != NULL) ? "" : ";1");
	for (char *c = name; *c; c++)
	{
		if (*c == '/')
		{
			*c = '\\';
		}
	}

	unsigned int lsn = 0, size = 0;
	if (!Ps2Cd_SearchFile(name, &lsn, &size))
	{
		printf("[CTR PS2] sceCdSearchFile failed for %s, using file I/O\n", name);
		return 0;
	}

	s_ps2CdLsn = lsn;
	s_ps2CdSize = size;
	s_ps2CdDirect = 1;
	printf("[CTR PS2] Direct disc reads: %s lsn %u size %u\n", name, (unsigned)s_ps2CdLsn, (unsigned)s_ps2CdSize);
	return 1;
}

// Reads 'size' bytes at byte 'offset' of the image into dst. Returns bytes read.
internal u32 NativeDiscImage_Ps2CdReadBytes(u32 offset, u8 *dst, u32 size)
{
	if (offset >= s_ps2CdSize)
	{
		return 0;
	}
	if (size > s_ps2CdSize - offset)
	{
		size = s_ps2CdSize - offset;
	}

	const u32 firstSector = offset / NATIVE_PS2_CD_SECTOR;
	const u32 skip = offset % NATIVE_PS2_CD_SECTOR;
	u32 count = (skip + size + NATIVE_PS2_CD_SECTOR - 1) / NATIVE_PS2_CD_SECTOR;
	if (count > NATIVE_PS2_CD_BUF_SECTORS)
	{
		count = NATIVE_PS2_CD_BUF_SECTORS;
		size = count * NATIVE_PS2_CD_SECTOR - skip;
	}

	g_ps2DiscReadBusy = 1;
	const int ok = Ps2Cd_ReadSectors(s_ps2CdLsn + firstSector, count, s_ps2CdBuf);
	g_ps2DiscReadBusy = 0;
	if (ok)
	{
		memcpy(dst, s_ps2CdBuf + skip, size);
		return size;
	}

	return 0;
}

internal void NativeDiscImage_InvalidateCache(void)
{
	for (int i = 0; i < NATIVE_DISC_IMAGE_CACHE_SLOTS; i++)
	{
		s_nativeDiscImageSlotStart[i] = 0;
		s_nativeDiscImageSlotCount[i] = 0;
		s_nativeDiscImageSlotUse[i] = 0;
	}
	s_nativeDiscImageCacheStart = 0;
	s_nativeDiscImageCacheCount = 0;
}

// 1 si el sector ya esta en alguna cache (se puede leer sin tocar el lector).
int NativeDiscImage_Ps2IsCached(u32 lba)
{
	if ((s_nativeDiscImageCacheCount != 0) && (lba >= s_nativeDiscImageCacheStart) && (lba < s_nativeDiscImageCacheStart + s_nativeDiscImageCacheCount))
	{
		return 1;
	}
	for (int i = 0; i < NATIVE_DISC_IMAGE_CACHE_SLOTS; i++)
	{
		if ((i != s_nativeDiscImageActiveSlot) && (s_nativeDiscImageSlotCount[i] != 0) && (lba >= s_nativeDiscImageSlotStart[i]) &&
		    (lba < s_nativeDiscImageSlotStart[i] + s_nativeDiscImageSlotCount[i]))
		{
			return 1;
		}
	}
	return 0;
}

// Elige el hueco que contiene 'lba', o si ninguno, el menos usado (vacio).
// Deja s_nativeDiscImageCache/Start/Count apuntando a ese hueco.
internal void NativeDiscImage_SelectCacheSlot(u32 lba)
{
	// Guardar el estado del hueco activo.
	s_nativeDiscImageSlotStart[s_nativeDiscImageActiveSlot] = s_nativeDiscImageCacheStart;
	s_nativeDiscImageSlotCount[s_nativeDiscImageActiveSlot] = s_nativeDiscImageCacheCount;

	int chosen = -1;
	if (g_ps2DiscXaRead)
	{
		// Musica XA: siempre su hueco propio (se rellena si no tiene el sector).
		chosen = NATIVE_DISC_IMAGE_XA_SLOT;
		const u32 xaStart = s_nativeDiscImageSlotStart[chosen];
		const u32 xaCount = s_nativeDiscImageSlotCount[chosen];
		if ((xaCount == 0) || (lba < xaStart) || (lba >= xaStart + xaCount))
		{
			s_nativeDiscImageSlotCount[chosen] = 0;
		}
	}
	else
	{
		// Datos del juego: los demas huecos (el de la XA ni se busca ni se expulsa).
		for (int i = 0; i < NATIVE_DISC_IMAGE_XA_SLOT; i++)
		{
			if ((s_nativeDiscImageSlotCount[i] != 0) && (lba >= s_nativeDiscImageSlotStart[i]) && (lba < s_nativeDiscImageSlotStart[i] + s_nativeDiscImageSlotCount[i]))
			{
				chosen = i;
				break;
			}
		}
		if (chosen < 0)
		{
			chosen = 0;
			for (int i = 1; i < NATIVE_DISC_IMAGE_XA_SLOT; i++)
			{
				if (s_nativeDiscImageSlotUse[i] < s_nativeDiscImageSlotUse[chosen])
				{
					chosen = i;
				}
			}
			s_nativeDiscImageSlotCount[chosen] = 0;
		}
	}

	s_nativeDiscImageSlotUse[chosen] = ++s_nativeDiscImageUseClock;
	s_nativeDiscImageActiveSlot = chosen;
	s_nativeDiscImageCache = s_nativeDiscImageCacheSlots[chosen];
	s_nativeDiscImageCacheStart = s_nativeDiscImageSlotStart[chosen];
	s_nativeDiscImageCacheCount = s_nativeDiscImageSlotCount[chosen];
}

internal int NativeDiscImage_ReadRawSector(u32 lba, u8 *sector)
{
	if ((s_nativeDiscImageFile == NULL) && !s_ps2CdDirect)
	{
		return 0;
	}

	NativeDiscImage_SelectCacheSlot(lba);

	if (s_ps2CdDirect && ((s_nativeDiscImageCacheCount == 0) || (lba < s_nativeDiscImageCacheStart) || (lba >= s_nativeDiscImageCacheStart + s_nativeDiscImageCacheCount)))
	{
		const u64 offset = (u64)lba * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
		const u32 bytesRead = (offset > 0xffffffffull) ? 0 : NativeDiscImage_Ps2CdReadBytes((u32)offset, s_nativeDiscImageCache, NATIVE_DISC_IMAGE_CACHE_BYTES);
		const u32 sectorsRead = bytesRead / NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
		if (sectorsRead == 0)
		{
			NativeDiscImage_InvalidateCache();
			return 0;
		}

		s_nativeDiscImageCacheStart = lba;
		s_nativeDiscImageCacheCount = sectorsRead;
	}

	if ((s_nativeDiscImageCacheCount == 0) || (lba < s_nativeDiscImageCacheStart) || (lba >= s_nativeDiscImageCacheStart + s_nativeDiscImageCacheCount))
	{
		u64 offset = (u64)lba * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
		if ((offset > (u64)LONG_MAX) || (fseek(s_nativeDiscImageFile, (long)offset, SEEK_SET) != 0))
		{
			NativeDiscImage_InvalidateCache();
			return 0;
		}

		const size_t bytesRead = fread(s_nativeDiscImageCache, 1, NATIVE_DISC_IMAGE_CACHE_BYTES, s_nativeDiscImageFile);
		const u32 sectorsRead = (u32)(bytesRead / NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE);
		if (sectorsRead == 0)
		{
			NativeDiscImage_InvalidateCache();
			return 0;
		}

		s_nativeDiscImageCacheStart = lba;
		s_nativeDiscImageCacheCount = sectorsRead;
	}

	memcpy(sector, &s_nativeDiscImageCache[(lba - s_nativeDiscImageCacheStart) * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE], NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE);
	return NativeDiscImage_CheckRawSectorHeader(sector);
}
#else
internal void NativeDiscImage_InvalidateCache(void)
{
}

internal int NativeDiscImage_ReadRawSector(u32 lba, u8 *sector)
{
	if (s_nativeDiscImageFile == NULL)
	{
		return 0;
	}

	u64 offset = (u64)lba * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
	if ((offset > (u64)LONG_MAX) || (fseek(s_nativeDiscImageFile, (long)offset, SEEK_SET) != 0))
	{
		return 0;
	}

	if (fread(sector, 1, NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE, s_nativeDiscImageFile) != NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE)
	{
		return 0;
	}

	return NativeDiscImage_CheckRawSectorHeader(sector);
}

#endif

internal int NativeDiscImage_ReadDataSector(u32 lba, u8 *payload)
{
	u8 sector[NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];

	if (!NativeDiscImage_ReadRawSector(lba, sector))
	{
		return 0;
	}

	memcpy(payload, &sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET], NATIVE_DISC_IMAGE_FORM1_DATA_SIZE);
	return 1;
}

internal int NativeDiscImage_ParseDirRecord(const u8 *src, size_t available, struct NativeDiscImageDirRecord *record)
{
	if ((src == NULL) || (record == NULL) || (available < 34))
	{
		return 0;
	}

	u8 length = src[0];
	if ((length == 0) || (length > available) || (length < 34))
	{
		return 0;
	}

	record->lba = NativeDiscImage_ReadLE32(&src[2]);
	record->size = NativeDiscImage_ReadLE32(&src[10]);
	record->flags = src[25];
	record->nameLen = src[32];
	record->name = &src[33];

	if ((u32)record->nameLen + 33u > length)
	{
		return 0;
	}

	return 1;
}

internal int NativeDiscImage_NameEquals(const struct NativeDiscImageDirRecord *record, NativeStr8 component)
{
	if ((record == NULL) || (record->name == NULL) || (component.ptr == NULL))
	{
		return 0;
	}

	size_t recordLen = record->nameLen;
	size_t componentLen = component.len;

	while ((recordLen > 0) && (record->name[recordLen - 1u] == ' '))
	{
		recordLen--;
	}

	if ((recordLen > 2) && (record->name[recordLen - 2u] == ';') && (record->name[recordLen - 1u] == '1'))
	{
		recordLen -= 2;
	}

	if ((componentLen > 2) && (component.ptr[componentLen - 2u] == ';') && (component.ptr[componentLen - 1u] == '1'))
	{
		componentLen -= 2;
	}

	if (recordLen != componentLen)
	{
		return 0;
	}

	for (size_t i = 0; i < componentLen; i++)
	{
		if (NativeStr8_ToUpperAscii(record->name[i]) != NativeStr8_ToUpperAscii(component.ptr[i]))
		{
			return 0;
		}
	}

	return 1;
}

internal NativeStr8 NativeDiscImage_NextPathComponent(NativeStr8 *path)
{
	NativeStr8 result = *path;

	while ((result.len != 0) && NativePath_IsSeparator(result.ptr[0]))
	{
		result = NativeStr8_Skip(result, 1);
	}

	for (size_t i = 0; i < result.len; i++)
	{
		if (NativePath_IsSeparator(result.ptr[i]))
		{
			NativeStr8 component = {result.ptr, i};
			*path = NativeStr8_Skip(result, i + 1u);
			return component;
		}
	}

	*path = NativeStr8_Skip(result, result.len);
	return result;
}

internal u32 NativeDiscImage_DataSectorCount(u32 size)
{
	return (size + (NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1u)) / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
}

internal u32 NativeDiscImage_RawSectorCount(u32 size)
{
	if ((size != 0) && ((size % NATIVE_DISC_IMAGE_MODE2_USER_SIZE) == 0))
	{
		return size / NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
	}

	return NativeDiscImage_DataSectorCount(size);
}

internal int NativeDiscImage_ReadDirectoryBytes(const struct NativeDiscImageFile *dir, u8 **dataOut, int *sizeOut)
{
	u8 *data;

	*dataOut = NULL;
	*sizeOut = 0;

	if ((dir == NULL) || (dir->size == 0) || (dir->size > 0x7fffffff))
	{
		return 0;
	}

	u32 sectorCount = NativeDiscImage_DataSectorCount(dir->size);
	if (sectorCount == 0)
	{
		return 0;
	}

	data = (u8 *)malloc((size_t)sectorCount * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE);
	if (data == NULL)
	{
		return 0;
	}

	for (u32 sector = 0; sector < sectorCount; sector++)
	{
		if (!NativeDiscImage_ReadDataSector(dir->lba + sector, &data[sector * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE]))
		{
			free(data);
			return 0;
		}
	}

	*dataOut = data;
	*sizeOut = (int)dir->size;
	return 1;
}

internal int NativeDiscImage_FindInDirectory(const struct NativeDiscImageFile *dir, NativeStr8 component, struct NativeDiscImageFile *fileOut, u8 *flagsOut)
{
	u8 *data;
	int size;
	int found = 0;

	if (!NativeDiscImage_ReadDirectoryBytes(dir, &data, &size))
	{
		return 0;
	}

	int offset = 0;
	while (offset < size)
	{
		struct NativeDiscImageDirRecord record;
		u8 length = data[offset];

		if (length == 0)
		{
			offset = (offset + (int)NATIVE_DISC_IMAGE_FORM1_DATA_SIZE) & ~((int)NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1);
			continue;
		}

		if ((length < 34) || (offset + length > size))
		{
			break;
		}

		if (NativeDiscImage_ParseDirRecord(&data[offset], (size_t)(size - offset), &record) && NativeDiscImage_NameEquals(&record, component))
		{
			fileOut->lba = record.lba;
			fileOut->size = record.size;
			*flagsOut = record.flags;
			found = 1;
			break;
		}

		offset += length;
	}

	free(data);
	return found;
}

internal int NativeDiscImage_LoadRoot(void)
{
	u8 sector[NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];
	struct NativeDiscImageDirRecord root;

	if (!NativeDiscImage_ReadRawSector(NATIVE_DISC_IMAGE_PVD_LBA, sector))
	{
		return 0;
	}

	if ((memcmp(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + 1], "CD001", 5) != 0) || (sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET] != 1) ||
	    (sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + 6] != 1))
	{
		return 0;
	}

	if (!NativeDiscImage_ParseDirRecord(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + NATIVE_DISC_IMAGE_PVD_ROOT_RECORD],
	                                    NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - NATIVE_DISC_IMAGE_PVD_ROOT_RECORD, &root))
	{
		return 0;
	}

	s_nativeDiscImageRoot.lba = root.lba;
	s_nativeDiscImageRoot.size = root.size;
	return 1;
}

int NativeDiscImage_Init(const char *assetsDir)
{
	char path[NATIVE_DISC_IMAGE_PATH_MAX];

	s_nativeDiscImageAvailable = 0;
	s_nativeDiscImagePath[0] = '\0';

	if (s_nativeDiscImageFile != NULL)
	{
		fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
	}

	if ((assetsDir == NULL) || !NativeDiscImage_FindHostImagePath(path, sizeof(path), NativeStr8_FromCString(assetsDir)))
	{
		return 0;
	}

	NativeDiscImage_InvalidateCache();
#if defined(CTR_PS2)
	if (NativeDiscImage_Ps2CdOpen(path))
	{
		if (!NativeDiscImage_LoadRoot())
		{
			s_ps2CdDirect = 0;
			NativeDiscImage_InvalidateCache();
		}
		else
		{
			goto image_open;
		}
	}
#endif
	s_nativeDiscImageFile = fopen(path, "rb");
	if (s_nativeDiscImageFile == NULL)
	{
		return 0;
	}

	if (!NativeDiscImage_LoadRoot())
	{
		fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
		return 0;
	}
#if defined(CTR_PS2)
image_open:
#endif

	if (!NativePath_NormalizeSlashes(s_nativeDiscImagePath, sizeof(s_nativeDiscImagePath), NativeStr8_FromCString(path)))
	{
		if (s_nativeDiscImageFile != NULL)
			fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
		return 0;
	}

	s_nativeDiscImageAvailable = 1;
	return 1;
}

int NativeDiscImage_FindFile(const char *path, struct NativeDiscImageFile *fileOut)
{
	NativeStr8 remaining = NativePath_SkipLeadingSeparators(NativeStr8_FromCString(path));
	struct NativeDiscImageFile current = s_nativeDiscImageRoot;
	u8 flags = NATIVE_DISC_IMAGE_DIRECTORY_FLAG;

	if (!s_nativeDiscImageAvailable || (path == NULL) || (fileOut == NULL))
	{
		return 0;
	}

	while (remaining.len != 0)
	{
		NativeStr8 component = NativeDiscImage_NextPathComponent(&remaining);

		if (component.len == 0)
		{
			continue;
		}

		if ((flags & NATIVE_DISC_IMAGE_DIRECTORY_FLAG) == 0)
		{
			return 0;
		}

		if (!NativeDiscImage_FindInDirectory(&current, component, &current, &flags))
		{
			return 0;
		}
	}

	if ((flags & NATIVE_DISC_IMAGE_DIRECTORY_FLAG) != 0)
	{
		return 0;
	}

	*fileOut = current;
	return 1;
}

internal int NativeDiscImage_ReadDataBytes(const struct NativeDiscImageFile *file, u32 offset, void *dst, size_t size)
{
	u8 sector[NATIVE_DISC_IMAGE_FORM1_DATA_SIZE];
	u8 *out = (u8 *)dst;

	if ((file == NULL) || (dst == NULL) || ((u64)offset + size > file->size))
	{
		return 0;
	}

	while (size != 0)
	{
		u32 sectorIndex = offset / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u32 sectorOffset = offset % NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		size_t copySize = NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - sectorOffset;

		if (copySize > size)
		{
			copySize = size;
		}

		if (!NativeDiscImage_ReadDataSector(file->lba + sectorIndex, sector))
		{
			return 0;
		}

		memcpy(out, &sector[sectorOffset], copySize);
		out += copySize;
		offset += (u32)copySize;
		size -= copySize;
	}

	return 1;
}

int NativeDiscImage_ReadDataSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst)
{
	u8 *out = (u8 *)dst;

	if ((file == NULL) || (dst == NULL))
	{
		return 0;
	}

	u32 fileSectorCount = NativeDiscImage_DataSectorCount(file->size);
	if ((sector > fileSectorCount) || (sectorCount > fileSectorCount - sector))
	{
		return 0;
	}

	for (u32 i = 0; i < sectorCount; i++)
	{
		if (!NativeDiscImage_ReadDataSector(file->lba + sector + i, &out[i * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE]))
		{
			return 0;
		}
	}

	return 1;
}

int NativeDiscImage_ReadRawSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst)
{
	u8 raw[NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];
	u8 *out = (u8 *)dst;

	if ((file == NULL) || (dst == NULL))
	{
		return 0;
	}

	u32 fileSectorCount = NativeDiscImage_RawSectorCount(file->size);
	if ((sector > fileSectorCount) || (sectorCount > fileSectorCount - sector))
	{
		return 0;
	}

	for (u32 i = 0; i < sectorCount; i++)
	{
		if (!NativeDiscImage_ReadRawSector(file->lba + sector + i, raw))
		{
			return 0;
		}

		memcpy(&out[i * NATIVE_DISC_IMAGE_MODE2_USER_SIZE], &raw[NATIVE_DISC_IMAGE_MODE2_USER_OFFSET], NATIVE_DISC_IMAGE_MODE2_USER_SIZE);
	}

	return 1;
}

int NativeDiscImage_ReadFileBytes(const char *path, int rawSectors, u8 **dataOut, int *sizeOut)
{
	struct NativeDiscImageFile file;
	u32 sectorCount;
	u32 size;

	*dataOut = NULL;
	*sizeOut = 0;

	if (!NativeDiscImage_FindFile(path, &file))
	{
		return 0;
	}

	if (rawSectors)
	{
		sectorCount = NativeDiscImage_RawSectorCount(file.size);
		size = sectorCount * NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
	}
	else
	{
		sectorCount = NativeDiscImage_DataSectorCount(file.size);
		size = file.size;
	}

	if ((sectorCount == 0) || (size > 0x7fffffffu))
	{
		return 0;
	}

	u8 *data = (u8 *)malloc((size_t)size);
	if (data == NULL)
	{
		return 0;
	}

	if (rawSectors)
	{
		if (!NativeDiscImage_ReadRawSectors(&file, 0, sectorCount, data))
		{
			free(data);
			return 0;
		}
	}
	else if (!NativeDiscImage_ReadDataBytes(&file, 0, data, size))
	{
		free(data);
		return 0;
	}

	*dataOut = data;
	*sizeOut = (int)size;
	return 1;
}
