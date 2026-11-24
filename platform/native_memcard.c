#include <platform/native_memcard.h>

#include <macros.h>

#include "platform/native_path.h"

#include <errno.h>
#if defined(_WIN32)
#include "platform/native_win32.h"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>

#define NATIVE_MEMCARD_MAX_FOUND_FILES 64
#define NATIVE_MEMCARD_MAX_NAME        64
#define NATIVE_MEMCARD_MAX_PATH        512
#define NATIVE_MEMCARD_COPY_BUFFER     8192
#define NATIVE_MEMCARD_DEFAULT_ROOT    "memcards"
#define NATIVE_MEMCARD_SLOT_COUNT      2

struct NativeMemcardFindState
{
	char names[NATIVE_MEMCARD_MAX_FOUND_FILES][NATIVE_MEMCARD_MAX_NAME];
	int count;
	int index;
};

struct NativeMemcardDeviceName
{
	const char *name;
	int slot;
};

global_variable struct NativeMemcardFindState s_memcardFind;
global_variable char s_memcardRoot[NATIVE_MEMCARD_MAX_PATH];
global_variable char s_memcardResolvedPath[NATIVE_MEMCARD_MAX_PATH];

internal struct NativeMemcardDeviceName NativeMemcard_ParseDeviceName(const char *save_name)
{
	struct NativeMemcardDeviceName result = {save_name, 0};

	if (save_name == NULL)
	{
		result.name = "";
		return result;
	}

	if ((save_name[0] == 'b') && (save_name[1] == 'u') && (save_name[2] >= '0') && (save_name[2] <= '9') && (save_name[3] >= '0') && (save_name[3] <= '9') &&
	    (save_name[4] == ':'))
	{
		result.name = save_name + 5;
		result.slot = (save_name[2] == '0') ? save_name[3] - '0' : save_name[2] - '0';
		result.slot %= NATIVE_MEMCARD_SLOT_COUNT;
	}

	return result;
}

internal const char *NativeMemcard_ActiveRoot(void)
{
	if (s_memcardRoot[0] == '\0')
	{
		return NATIVE_MEMCARD_DEFAULT_ROOT;
	}

	return s_memcardRoot;
}

internal int NativeMemcard_CopyString(char *dst, int dst_size, const char *src)
{
	int i;

	if (dst_size <= 0)
	{
		return 0;
	}

	for (i = 0; (i < dst_size - 1) && (src[i] != '\0'); i++)
	{
		dst[i] = src[i];
	}

	dst[i] = '\0';
	return src[i] == '\0';
}

internal int NativeMemcard_IsCtrSaveName(const char *name)
{
	return (strcmp(name, "BASCUS-94426-SLOTS") == 0) || (strncmp(name, "BASCUS-94426G", 13) == 0);
}

internal int NativeMemcard_JoinPath(char *dst, int dst_size, const char *left, const char *right)
{
	return NativePath_Join(dst, (size_t)dst_size, NativeStr8_FromCString(left), NativeStr8_FromCString(right));
}

internal int NativeMemcard_SlotDir(char *dst, int dst_size, int slot)
{
	if (slot < 0)
	{
		slot = 0;
	}

	int written = snprintf(dst, (size_t)dst_size, "slot%d", slot);
	return (written >= 0) && (written < dst_size);
}

internal int NativeMemcard_BuildSlotRoot(char *dst, int dst_size, const char *root, int slot)
{
	char slotDir[16];

	if (!NativeMemcard_SlotDir(slotDir, sizeof(slotDir), slot))
	{
		return 0;
	}

	return NativeMemcard_JoinPath(dst, dst_size, root, slotDir);
}

internal int NativeMemcard_PathExists(const char *path)
{
#if defined(_WIN32)
	DWORD attributes;

	if ((path == NULL) || (path[0] == '\0'))
		return 0;

	attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;

	if ((path == NULL) || (path[0] == '\0'))
	{
		return 0;
	}

	return stat(path, &st) == 0;
#endif
}

internal int NativeMemcard_PathIsDirectory(const char *path)
{
#if defined(_WIN32)
	DWORD attributes;

	if ((path == NULL) || (path[0] == '\0'))
		return 0;

	attributes = GetFileAttributesA(path);
	return (attributes != INVALID_FILE_ATTRIBUTES) && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
	struct stat st;

	if ((path == NULL) || (path[0] == '\0'))
	{
		return 0;
	}
	if (stat(path, &st) != 0)
	{
		return 0;
	}

	return S_ISDIR(st.st_mode);
#endif
}

internal int NativeMemcard_MakeDir(const char *path)
{
	if ((path == NULL) || (path[0] == '\0'))
	{
		return 0;
	}

#if defined(_WIN32)
	if (CreateDirectoryA(path, NULL) != 0)
		return 1;

	return GetLastError() == ERROR_ALREADY_EXISTS;
#else
	if (mkdir(path, 0777) == 0)
	{
		return 1;
	}

#if defined(CTR_PS2)
	// PS2: la memory card no responde EEXIST cuando la carpeta ya existe (da
	// otro codigo). Antes se tomaba como fallo, no se guardaba y el juego decia
	// "MEMORY CARD IS FULL" (la primera vez funcionaba: las carpetas no existian).
	// Si se puede abrir, es que existe.
	{
		DIR *existing = opendir(path);
		if (existing != NULL)
		{
			closedir(existing);
			return 1;
		}
		printf("[CTR PS2] memory card: no se pudo crear la carpeta %s (errno %d)\n", path, errno);
	}
#endif
	return errno == EEXIST;
#endif
}

#if defined(CTR_PS2)
// PS2 (memory card): el navegador de la PS2 solo sabe borrar/copiar carpetas
// sin subcarpetas. Por eso las partidas van directamente en la carpeta raiz,
// con la ranura delante del nombre: "mc0:/CTRNATIVE/0-BASCUS-94426-SLOTS".
// (Antes: mc0:/CTRNATIVE/slot0/BASCUS-94426-SLOTS; al borrar, el navegador
// quitaba el icono, fallaba en la subcarpeta y quedaba "datos danados".)
internal int Ps2Memcard_IsFlatRoot(const char *root)
{
	return (root != NULL) && (strncmp(root, "mc", 2) == 0);
}
#endif

#if defined(CTR_PS2)
// --- Icono de la memory card de PS2 -------------------------------------------
// La PS2 necesita icon.sys (nombre y colores) y un icono 3D en cada carpeta de la
// memory card; sin ellos el navegador la muestra como "datos danados" y no deja
// borrarla ni copiarla. Se crean al guardar si faltan. El icono es propio: una
// bandera de cuadros con borde de color, en una placa de dos caras.
#define PS2_MC_ICON_FILE "ctr.ico"

internal void Ps2Memcard_Put16(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

internal void Ps2Memcard_Put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

internal void Ps2Memcard_PutFloat(u8 *p, float f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	Ps2Memcard_Put32(p, v);
}

// Texto ASCII -> Shift-JIS de ancho completo (lo que muestra el navegador).
internal int Ps2Memcard_ToSjis(u8 *dst, int dstSize, const char *text)
{
	int n = 0;
	for (const char *c = text; (*c != '\0') && (n + 2 <= dstSize); c++)
	{
		u32 code = 0x8140; // espacio
		if ((*c >= '0') && (*c <= '9'))
		{
			code = 0x824f + (u32)(*c - '0');
		}
		else if ((*c >= 'A') && (*c <= 'Z'))
		{
			code = 0x8260 + (u32)(*c - 'A');
		}
		else if ((*c >= 'a') && (*c <= 'z'))
		{
			code = 0x8281 + (u32)(*c - 'a');
		}
		else if (*c == '-')
		{
			code = 0x817c;
		}
		dst[n++] = (u8)(code >> 8);
		dst[n++] = (u8)code;
	}
	return n;
}

internal int Ps2Memcard_WriteIconSys(const char *path)
{
	u8 sys[964];
	memset(sys, 0, sizeof(sys));
	memcpy(&sys[0x000], "PS2D", 4);
	// Titulo en dos lineas: la segunda empieza en el byte indicado en 0x006. El
	// navegador de la PS2 muestra como mucho 16 letras por linea.
	const int line1 = Ps2Memcard_ToSjis(&sys[0x0c0], 68, "Crash Team");
	Ps2Memcard_Put16(&sys[0x006], (u32)line1);
	Ps2Memcard_ToSjis(&sys[0x0c0 + line1], 68 - line1, "Racing PS2");
	Ps2Memcard_Put32(&sys[0x00c], 0x00); // transparencia del fondo
	// Fondo: degradado azul oscuro (arriba) a naranja (abajo).
	const u32 bg[4][3] = {{0x00, 0x10, 0x40}, {0x00, 0x10, 0x40}, {0x60, 0x30, 0x00}, {0x60, 0x30, 0x00}};
	for (int i = 0; i < 4; i++)
	{
		for (int k = 0; k < 3; k++)
		{
			Ps2Memcard_Put32(&sys[0x010 + i * 16 + k * 4], bg[i][k]);
		}
		Ps2Memcard_Put32(&sys[0x010 + i * 16 + 12], 0x00);
	}
	// Luces (direcciones, colores y ambiente), valores habituales.
	const float lightDir[3][4] = {{0.5f, 0.5f, 0.5f, 0.0f}, {0.0f, -0.4f, -0.1f, 0.0f}, {-0.5f, -0.5f, 0.5f, 0.0f}};
	const float lightCol[3][4] = {{0.5f, 0.5f, 0.5f, 0.0f}, {0.3f, 0.3f, 0.3f, 0.0f}, {0.2f, 0.2f, 0.2f, 0.0f}};
	const float ambient[4] = {0.5f, 0.5f, 0.5f, 0.0f};
	for (int i = 0; i < 3; i++)
	{
		for (int k = 0; k < 4; k++)
		{
			Ps2Memcard_PutFloat(&sys[0x050 + i * 16 + k * 4], lightDir[i][k]);
			Ps2Memcard_PutFloat(&sys[0x080 + i * 16 + k * 4], lightCol[i][k]);
		}
	}
	for (int k = 0; k < 4; k++)
	{
		Ps2Memcard_PutFloat(&sys[0x0b0 + k * 4], ambient[k]);
	}
	// El mismo icono para ver, copiar y borrar.
	strcpy((char *)&sys[0x104], PS2_MC_ICON_FILE);
	strcpy((char *)&sys[0x144], PS2_MC_ICON_FILE);
	strcpy((char *)&sys[0x184], PS2_MC_ICON_FILE);

	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		return 0;
	}
	const size_t wrote = fwrite(sys, 1, sizeof(sys), f);
	fclose(f);
	return wrote == sizeof(sys);
}

// Color de la textura (128x128, 16 bits: R 5 | G 5 | B 5 | A 1).
internal u16 Ps2Memcard_Rgb(u32 r, u32 g, u32 b)
{
	return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
}

internal int Ps2Memcard_WriteIcon(const char *path)
{
	// Placa de dos caras (12 vertices), coordenadas en 1/4096.
	enum
	{
		VERTS = 12,
		HEADER = 20,
		VERT_BYTES = 24,
		ANIM = 20,
		FRAME = 16,
		TEX = 128 * 128 * 2
	};
	static u8 ico[HEADER + VERTS * VERT_BYTES + ANIM + FRAME + TEX];
	memset(ico, 0, sizeof(ico));
	Ps2Memcard_Put32(&ico[0], 0x00010000); // version
	Ps2Memcard_Put32(&ico[4], 1);          // formas de animacion
	Ps2Memcard_Put32(&ico[8], 0x07);       // textura sin comprimir
	Ps2Memcard_PutFloat(&ico[12], 1.0f);
	Ps2Memcard_Put32(&ico[16], VERTS);

	// Esquinas de la placa: x de -1,4 a 1,4; y de -1,4 (arriba) a 1,0.
	const s32 x0 = -5734, x1 = 5734, yTop = -5734, yBottom = 4096;
	// Cara delantera (normal hacia -z) y trasera (+z), dos triangulos cada una.
	const s32 quad[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
	u8 *v = &ico[HEADER];
	for (int face = 0; face < 2; face++)
	{
		for (int i = 0; i < 6; i++)
		{
			// La cara trasera recorre los triangulos al reves.
			const int idx = (face == 0) ? i : (5 - i);
			const s32 qx = quad[idx][0];
			const s32 qy = quad[idx][1];
			Ps2Memcard_Put16(&v[0], (u32)(u16)(s16)(qx ? x1 : x0));
			Ps2Memcard_Put16(&v[2], (u32)(u16)(s16)(qy ? yBottom : yTop));
			Ps2Memcard_Put16(&v[4], (u32)(u16)(s16)((face == 0) ? -200 : 200));
			Ps2Memcard_Put16(&v[6], 0);
			Ps2Memcard_Put16(&v[8], 0);
			Ps2Memcard_Put16(&v[10], 0);
			Ps2Memcard_Put16(&v[12], (u32)(u16)(s16)((face == 0) ? -4096 : 4096));
			Ps2Memcard_Put16(&v[14], 0);
			Ps2Memcard_Put16(&v[16], (u32)(qx ? 4096 : 0));
			Ps2Memcard_Put16(&v[18], (u32)(qy ? 4096 : 0));
			v[20] = 0x80;
			v[21] = 0x80;
			v[22] = 0x80;
			v[23] = 0x80;
			v += VERT_BYTES;
		}
	}

	// Animacion: una sola forma, sin movimiento.
	u8 *a = &ico[HEADER + VERTS * VERT_BYTES];
	Ps2Memcard_Put32(&a[0], 0x01);
	Ps2Memcard_Put32(&a[4], 1);
	Ps2Memcard_PutFloat(&a[8], 1.0f);
	Ps2Memcard_Put32(&a[12], 0);
	Ps2Memcard_Put32(&a[16], 1);
	u8 *fr = &a[ANIM];
	Ps2Memcard_Put32(&fr[0], 0); // forma 0
	Ps2Memcard_Put32(&fr[4], 1); // 1 clave (sin datos extra)
	Ps2Memcard_Put32(&fr[8], 1);
	Ps2Memcard_Put32(&fr[12], 1);

	// Textura: bandera de cuadros 8x8 con borde naranja y franja azul.
	u8 *t = &fr[FRAME];
	for (int y = 0; y < 128; y++)
	{
		for (int x = 0; x < 128; x++)
		{
			u16 c;
			if ((x < 8) || (x >= 120) || (y < 8) || (y >= 120))
			{
				c = Ps2Memcard_Rgb(0xff, 0x80, 0x00); // borde
			}
			else if ((y >= 92) && (y < 104))
			{
				c = Ps2Memcard_Rgb(0x20, 0x60, 0xff); // franja
			}
			else
			{
				const int cell = (((x - 8) / 14) + ((y - 8) / 14)) & 1;
				c = cell ? Ps2Memcard_Rgb(0x10, 0x10, 0x10) : Ps2Memcard_Rgb(0xf0, 0xf0, 0xf0);
			}
			Ps2Memcard_Put16(&t[(y * 128 + x) * 2], c);
		}
	}

	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		return 0;
	}
	const size_t wrote = fwrite(ico, 1, sizeof(ico), f);
	fclose(f);
	return wrote == sizeof(ico);
}

// Crea icon.sys y el icono en la carpeta raiz si faltan (solo memory card).
internal void Ps2Memcard_EnsureIcon(const char *root)
{
	static int s_iconChecked = 0;
	if (s_iconChecked || (root == NULL) || (strncmp(root, "mc", 2) != 0))
	{
		return;
	}
	s_iconChecked = 1; // una vez por sesion
	char path[NATIVE_MEMCARD_MAX_PATH];
	snprintf(path, sizeof(path), "%s/icon.sys", root);
	FILE *f = fopen(path, "rb");
	if (f != NULL)
	{
		// Ya esta. Si es el de la primera version (titulo de 17 letras, que el
		// navegador cortaba), se vuelve a escribir.
		u8 head[8];
		const size_t got = fread(head, 1, sizeof(head), f);
		fclose(f);
		if ((got == sizeof(head)) && (head[6] != 34))
		{
			return;
		}
	}
	const int sysOk = Ps2Memcard_WriteIconSys(path);
	snprintf(path, sizeof(path), "%s/%s", root, PS2_MC_ICON_FILE);
	const int icoOk = Ps2Memcard_WriteIcon(path);
	printf("[CTR PS2] memory card: icono %s (icon.sys %s, %s %s)\n", (sysOk && icoOk) ? "creado" : "NO creado", sysOk ? "ok" : "fallo", PS2_MC_ICON_FILE,
	       icoOk ? "ok" : "fallo");
}
#endif

internal int NativeMemcard_EnsureSlotRoot(const char *root, int slot)
{
	char slotRoot[NATIVE_MEMCARD_MAX_PATH];

	if (!NativeMemcard_MakeDir(root))
	{
		return 0;
	}
#if defined(CTR_PS2)
	Ps2Memcard_EnsureIcon(root);
	if (Ps2Memcard_IsFlatRoot(root))
	{
		(void)slot;
		(void)slotRoot;
		return 1; // formato plano: sin subcarpetas
	}
#endif
	if (!NativeMemcard_BuildSlotRoot(slotRoot, sizeof(slotRoot), root, slot))
	{
		return 0;
	}

	return NativeMemcard_MakeDir(slotRoot);
}

internal int NativeMemcard_BuildPathFromDeviceName(char *dst, int dst_size, const char *save_name, int ensureSlotRoot)
{
	struct NativeMemcardDeviceName device = NativeMemcard_ParseDeviceName(save_name);
	const char *root = NativeMemcard_ActiveRoot();
	char slotRoot[NATIVE_MEMCARD_MAX_PATH];

#if defined(CTR_PS2)
	if (Ps2Memcard_IsFlatRoot(root))
	{
		char flatName[NATIVE_MEMCARD_MAX_NAME + 4];
		if (ensureSlotRoot != 0)
		{
			if (!NativeMemcard_MakeDir(root))
			{
				return 0;
			}
			Ps2Memcard_EnsureIcon(root);
		}
		snprintf(flatName, sizeof(flatName), "%d-%s", device.slot, device.name);
		return NativeMemcard_JoinPath(dst, dst_size, root, flatName);
	}
#endif

	if (ensureSlotRoot != 0)
	{
		if (!NativeMemcard_EnsureSlotRoot(root, device.slot))
		{
			return 0;
		}
	}
	if (!NativeMemcard_BuildSlotRoot(slotRoot, sizeof(slotRoot), root, device.slot))
	{
		return 0;
	}

	return NativeMemcard_JoinPath(dst, dst_size, slotRoot, device.name);
}

internal const char *NativeMemcard_PathFromDeviceName(const char *save_name, int ensureSlotRoot)
{
	if (!NativeMemcard_BuildPathFromDeviceName(s_memcardResolvedPath, sizeof(s_memcardResolvedPath), save_name, ensureSlotRoot))
	{
		s_memcardResolvedPath[0] = '\0';
	}
	return s_memcardResolvedPath;
}

internal enum NativeMemcardResult NativeMemcard_CopyFile(const char *src_path, const char *dst_path)
{
	unsigned char buffer[NATIVE_MEMCARD_COPY_BUFFER];
	size_t read_bytes;
	enum NativeMemcardResult result = NATIVE_MEMCARD_OK;

	FILE *src = fopen(src_path, "rb");
	if (src == NULL)
	{
		return NATIVE_MEMCARD_NOT_FOUND;
	}

	FILE *dst = fopen(dst_path, "wb");
	if (dst == NULL)
	{
		fclose(src);
		return NATIVE_MEMCARD_OPEN_FAILED;
	}

	while ((read_bytes = fread(buffer, 1, sizeof(buffer), src)) != 0)
	{
		if (fwrite(buffer, 1, read_bytes, dst) != read_bytes)
		{
			result = NATIVE_MEMCARD_IO_ERROR;
			break;
		}
	}

	if (ferror(src) != 0)
	{
		result = NATIVE_MEMCARD_IO_ERROR;
	}

	fclose(dst);
	fclose(src);
	return result;
}

internal enum NativeMemcardResult NativeMemcard_CopyOneSave(const char *src_root, const char *dst_root, const char *name, int overwrite)
{
	char src_path[NATIVE_MEMCARD_MAX_PATH];
	char dst_path[NATIVE_MEMCARD_MAX_PATH];

	if (!NativeMemcard_JoinPath(src_path, sizeof(src_path), src_root, name))
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}
	if (!NativeMemcard_JoinPath(dst_path, sizeof(dst_path), dst_root, name))
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}
	if (NativeMemcard_PathIsDirectory(src_path))
	{
		return NATIVE_MEMCARD_OK;
	}
	if ((overwrite == 0) && NativeMemcard_PathExists(dst_path))
	{
		return NATIVE_MEMCARD_OK;
	}

	return NativeMemcard_CopyFile(src_path, dst_path);
}

internal enum NativeMemcardResult NativeMemcard_CopyCtrSavesFromDir(const char *src_root, const char *dst_root, int overwrite)
{
	enum NativeMemcardResult result;

	if (!NativeMemcard_PathExists(src_root))
	{
		return NATIVE_MEMCARD_OK;
	}
	if (!NativeMemcard_MakeDir(dst_root))
	{
		return NATIVE_MEMCARD_OPEN_FAILED;
	}

#if defined(_WIN32)
	{
		char search_path[NATIVE_MEMCARD_MAX_PATH];
		WIN32_FIND_DATAA find_data;
		HANDLE find_handle;

		if (!NativeMemcard_JoinPath(search_path, sizeof(search_path), src_root, "BASCUS-94426*"))
			return NATIVE_MEMCARD_IO_ERROR;

		find_handle = FindFirstFileA(search_path, &find_data);
		if (find_handle == INVALID_HANDLE_VALUE)
			return NATIVE_MEMCARD_OK;

		do
		{
			if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				continue;
			if (!NativeMemcard_IsCtrSaveName(find_data.cFileName))
				continue;

			result = NativeMemcard_CopyOneSave(src_root, dst_root, find_data.cFileName, overwrite);
			if (result != NATIVE_MEMCARD_OK)
			{
				FindClose(find_handle);
				return result;
			}
		} while (FindNextFileA(find_handle, &find_data) != 0);

		FindClose(find_handle);
	}
#else
	{
		DIR *dir_handle = opendir(src_root);
		struct dirent *entry;

		if (dir_handle == NULL)
		{
			return NATIVE_MEMCARD_OK;
		}

		while ((entry = readdir(dir_handle)) != NULL)
		{
			if (!NativeMemcard_IsCtrSaveName(entry->d_name))
			{
				continue;
			}

			result = NativeMemcard_CopyOneSave(src_root, dst_root, entry->d_name, overwrite);
			if (result != NATIVE_MEMCARD_OK)
			{
				closedir(dir_handle);
				return result;
			}
		}

		closedir(dir_handle);
	}
#endif

	return NATIVE_MEMCARD_OK;
}

#if !defined(_WIN32)
internal int NativeMemcard_MatchesPattern(const char *name, const char *pattern)
{
	while (*pattern != '\0')
	{
		if (*pattern == '*')
		{
			pattern++;
			if (*pattern == '\0')
			{
				return 1;
			}

			while (*name != '\0')
			{
				if (NativeMemcard_MatchesPattern(name, pattern) != 0)
				{
					return 1;
				}
				name++;
			}

			return 0;
		}

		if ((*pattern != '?') && (*pattern != *name))
		{
			return 0;
		}

		if (*name == '\0')
		{
			return 0;
		}

		name++;
		pattern++;
	}

	return *name == '\0';
}
#endif

internal void NativeMemcard_SplitPattern(const char *path, char *dir, int dir_size, char *pattern, int pattern_size)
{
	const char *slash = strrchr(path, '/');
#if defined(_WIN32)
	const char *backslash = strrchr(path, '\\');
	if ((slash == NULL) || ((backslash != NULL) && (backslash > slash)))
		slash = backslash;
#endif

	if (slash == NULL)
	{
		NativeMemcard_CopyString(dir, dir_size, ".");
		NativeMemcard_CopyString(pattern, pattern_size, path);
		return;
	}

	{
		int dir_len = (int)(slash - path);

		if (dir_len <= 0)
		{
			NativeMemcard_CopyString(dir, dir_size, ".");
		}
		else
		{
			if (dir_len >= dir_size)
			{
				dir_len = dir_size - 1;
			}

			memcpy(dir, path, dir_len);
			dir[dir_len] = '\0';
		}

		NativeMemcard_CopyString(pattern, pattern_size, slash + 1);
	}
}

internal int NativeMemcard_CompareNames(const char *left, const char *right)
{
	return strcmp(left, right) > 0;
}

internal void NativeMemcard_AddFoundName(const char *name)
{
	int i;

	if (s_memcardFind.count >= NATIVE_MEMCARD_MAX_FOUND_FILES)
	{
		return;
	}

	int insert = s_memcardFind.count;
	for (i = 0; i < s_memcardFind.count; i++)
	{
		if (NativeMemcard_CompareNames(s_memcardFind.names[i], name) != 0)
		{
			insert = i;
			break;
		}
	}

	for (i = s_memcardFind.count; i > insert; i--)
	{
		memcpy(s_memcardFind.names[i], s_memcardFind.names[i - 1], sizeof(s_memcardFind.names[i]));
	}

	NativeMemcard_CopyString(s_memcardFind.names[insert], sizeof(s_memcardFind.names[insert]), name);
	s_memcardFind.count++;
}

internal void NativeMemcard_BuildFindList(const char *pattern)
{
	const char *path = NativeMemcard_PathFromDeviceName(pattern, 0);
	char dir[NATIVE_MEMCARD_MAX_PATH];
	char file_pattern[NATIVE_MEMCARD_MAX_NAME];

	memset(&s_memcardFind, 0, sizeof(s_memcardFind));
	NativeMemcard_SplitPattern(path, dir, sizeof(dir), file_pattern, sizeof(file_pattern));

#if defined(_WIN32)
	{
		char search_path[NATIVE_MEMCARD_MAX_PATH];
		WIN32_FIND_DATAA find_data;
		HANDLE find_handle;

		if (strcmp(dir, ".") == 0)
			NativeMemcard_CopyString(search_path, sizeof(search_path), file_pattern);
		else if (!NativeMemcard_JoinPath(search_path, sizeof(search_path), dir, file_pattern))
			return;

		find_handle = FindFirstFileA(search_path, &find_data);
		if (find_handle == INVALID_HANDLE_VALUE)
			return;

		do
		{
			if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				NativeMemcard_AddFoundName(find_data.cFileName);
		} while (FindNextFileA(find_handle, &find_data) != 0);

		FindClose(find_handle);
	}
#else
	{
		DIR *dir_handle = opendir(dir);
		struct dirent *entry;

		if (dir_handle == NULL)
		{
			return;
		}

		while ((entry = readdir(dir_handle)) != NULL)
		{
			if (NativeMemcard_MatchesPattern(entry->d_name, file_pattern) != 0)
			{
#if defined(CTR_PS2)
				// Formato plano: el juego no ve el "N-" de la ranura.
				if (Ps2Memcard_IsFlatRoot(NativeMemcard_ActiveRoot()) && (entry->d_name[0] >= '0') && (entry->d_name[0] <= '9') && (entry->d_name[1] == '-'))
				{
					NativeMemcard_AddFoundName(entry->d_name + 2);
					continue;
				}
#endif
				NativeMemcard_AddFoundName(entry->d_name);
			}
		}

		closedir(dir_handle);
	}
#endif
}

enum NativeMemcardResult NativeMemcard_SetRoot(const char *root_path)
{
	if ((root_path == NULL) || (root_path[0] == '\0') || (strcmp(root_path, ".") == 0))
	{
		NativeMemcard_ClearRoot();
		return NATIVE_MEMCARD_OK;
	}

	if (!NativePath_NormalizeSlashes(s_memcardRoot, sizeof(s_memcardRoot), NativeStr8_FromCString(root_path)))
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}

	memset(&s_memcardFind, 0, sizeof(s_memcardFind));
#if defined(CTR_PS2)
	// Si la carpeta ya existe: pasar las partidas del formato viejo (subcarpetas
	// slot0/slot1) al plano, quitar esas subcarpetas y poner el icono. Asi deja
	// de salir como "datos danados" y se puede borrar desde el navegador.
	if (Ps2Memcard_IsFlatRoot(s_memcardRoot))
	{
		DIR *existing = opendir(s_memcardRoot);
		if (existing != NULL)
		{
			closedir(existing);
			for (int slot = 0; slot < NATIVE_MEMCARD_SLOT_COUNT; slot++)
			{
				char oldDir[NATIVE_MEMCARD_MAX_PATH];
				char names[16][NATIVE_MEMCARD_MAX_NAME];
				int count = 0;
				if (!NativeMemcard_BuildSlotRoot(oldDir, sizeof(oldDir), s_memcardRoot, slot))
				{
					continue;
				}
				DIR *old = opendir(oldDir);
				if (old == NULL)
				{
					continue;
				}
				struct dirent *entry;
				while (((entry = readdir(old)) != NULL) && (count < 16))
				{
					if ((strcmp(entry->d_name, ".") != 0) && (strcmp(entry->d_name, "..") != 0))
					{
						NativeMemcard_CopyString(names[count], sizeof(names[count]), entry->d_name);
						count++;
					}
				}
				closedir(old);
				int moved = 0;
				for (int i = 0; i < count; i++)
				{
					char src[NATIVE_MEMCARD_MAX_PATH];
					char dst[NATIVE_MEMCARD_MAX_PATH];
					snprintf(src, sizeof(src), "%s/%s", oldDir, names[i]);
					snprintf(dst, sizeof(dst), "%s/%d-%s", s_memcardRoot, slot, names[i]);
					if (NativeMemcard_CopyFile(src, dst) == NATIVE_MEMCARD_OK)
					{
						remove(src);
						moved++;
					}
					else
					{
						printf("[CTR PS2] memory card: no se pudo mover %s\n", src);
					}
				}
				const int removed = (moved == count) && (rmdir(oldDir) == 0);
				printf("[CTR PS2] memory card: formato viejo %s -> %d partidas movidas, carpeta %s\n", oldDir, moved, removed ? "quitada" : "NO quitada");
			}
			Ps2Memcard_EnsureIcon(s_memcardRoot);
		}
	}
#endif
	return NATIVE_MEMCARD_OK;
}

void NativeMemcard_ClearRoot(void)
{
	s_memcardRoot[0] = '\0';
	s_memcardResolvedPath[0] = '\0';
	memset(&s_memcardFind, 0, sizeof(s_memcardFind));
}

enum NativeMemcardResult NativeMemcard_RemoveRoot(const char *root_path)
{
	if ((root_path == NULL) || (root_path[0] == '\0') || (strcmp(root_path, ".") == 0))
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}
	if (strcmp(root_path, NATIVE_MEMCARD_DEFAULT_ROOT) == 0)
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}

	if (!NativeMemcard_PathExists(root_path))
	{
		return NATIVE_MEMCARD_OK;
	}

#if defined(_WIN32)
	{
		char search_path[NATIVE_MEMCARD_MAX_PATH];
		WIN32_FIND_DATAA find_data;
		HANDLE find_handle;

		if (!NativeMemcard_JoinPath(search_path, sizeof(search_path), root_path, "*"))
			return NATIVE_MEMCARD_IO_ERROR;

		find_handle = FindFirstFileA(search_path, &find_data);
		if (find_handle != INVALID_HANDLE_VALUE)
		{
			do
			{
				char file_path[NATIVE_MEMCARD_MAX_PATH];

				if ((strcmp(find_data.cFileName, ".") == 0) || (strcmp(find_data.cFileName, "..") == 0))
					continue;
				if (!NativeMemcard_JoinPath(file_path, sizeof(file_path), root_path, find_data.cFileName))
				{
					FindClose(find_handle);
					return NATIVE_MEMCARD_IO_ERROR;
				}
				if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					if (NativeMemcard_RemoveRoot(file_path) != NATIVE_MEMCARD_OK)
					{
						FindClose(find_handle);
						return NATIVE_MEMCARD_IO_ERROR;
					}
				}
				else if (DeleteFileA(file_path) == 0)
				{
					FindClose(find_handle);
					return NATIVE_MEMCARD_IO_ERROR;
				}
			} while (FindNextFileA(find_handle, &find_data) != 0);

			FindClose(find_handle);
		}

		if (RemoveDirectoryA(root_path) == 0)
			return NATIVE_MEMCARD_IO_ERROR;
	}
#else
	{
		DIR *dir_handle = opendir(root_path);
		struct dirent *entry;

		if (dir_handle != NULL)
		{
			while ((entry = readdir(dir_handle)) != NULL)
			{
				char file_path[NATIVE_MEMCARD_MAX_PATH];

				if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0))
				{
					continue;
				}
				if (!NativeMemcard_JoinPath(file_path, sizeof(file_path), root_path, entry->d_name))
				{
					closedir(dir_handle);
					return NATIVE_MEMCARD_IO_ERROR;
				}
				if (NativeMemcard_PathIsDirectory(file_path))
				{
					if (NativeMemcard_RemoveRoot(file_path) != NATIVE_MEMCARD_OK)
					{
						closedir(dir_handle);
						return NATIVE_MEMCARD_IO_ERROR;
					}
				}
				else if (unlink(file_path) != 0)
				{
					closedir(dir_handle);
					return NATIVE_MEMCARD_IO_ERROR;
				}
			}

			closedir(dir_handle);
		}

		if (rmdir(root_path) != 0)
		{
			return NATIVE_MEMCARD_IO_ERROR;
		}
	}
#endif

	return NATIVE_MEMCARD_OK;
}

enum NativeMemcardResult NativeMemcard_CloneRoot(const char *src_root, const char *dst_root)
{
	if ((dst_root == NULL) || (dst_root[0] == '\0') || (strcmp(dst_root, ".") == 0))
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}

	if ((src_root == NULL) || (src_root[0] == '\0'))
	{
		src_root = NATIVE_MEMCARD_DEFAULT_ROOT;
	}
	if (strcmp(src_root, dst_root) == 0)
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}

	enum NativeMemcardResult result = NativeMemcard_RemoveRoot(dst_root);
	if (result != NATIVE_MEMCARD_OK)
	{
		return result;
	}

	if (!NativeMemcard_MakeDir(dst_root))
	{
		return NATIVE_MEMCARD_OPEN_FAILED;
	}

	if (!NativeMemcard_PathExists(src_root))
	{
		return NATIVE_MEMCARD_OK;
	}

	for (int slot = 0; slot < NATIVE_MEMCARD_SLOT_COUNT; slot++)
	{
		char srcSlotRoot[NATIVE_MEMCARD_MAX_PATH];
		char dstSlotRoot[NATIVE_MEMCARD_MAX_PATH];

		if (!NativeMemcard_BuildSlotRoot(srcSlotRoot, sizeof(srcSlotRoot), src_root, slot))
		{
			return NATIVE_MEMCARD_IO_ERROR;
		}
		if (!NativeMemcard_BuildSlotRoot(dstSlotRoot, sizeof(dstSlotRoot), dst_root, slot))
		{
			return NATIVE_MEMCARD_IO_ERROR;
		}
		if (!NativeMemcard_PathExists(srcSlotRoot))
		{
			continue;
		}

		result = NativeMemcard_CopyCtrSavesFromDir(srcSlotRoot, dstSlotRoot, 1);
		if (result != NATIVE_MEMCARD_OK)
		{
			return result;
		}
	}

	return NATIVE_MEMCARD_OK;
}

enum NativeMemcardResult NativeMemcard_CloneCurrentRoot(const char *dst_root)
{
	return NativeMemcard_CloneRoot(NativeMemcard_ActiveRoot(), dst_root);
}

int NativeMemcard_FindFirstFile(const char *pattern, char *dst_name, int dst_size)
{
	NativeMemcard_BuildFindList(pattern);
	s_memcardFind.index = 0;
	return NativeMemcard_FindNextFile(dst_name, dst_size);
}

int NativeMemcard_FindNextFile(char *dst_name, int dst_size)
{
	if (s_memcardFind.index >= s_memcardFind.count)
	{
		return 0;
	}

	NativeMemcard_CopyString(dst_name, dst_size, s_memcardFind.names[s_memcardFind.index]);
	s_memcardFind.index++;
	return 1;
}

int NativeMemcard_FileExists(const char *save_name)
{
	const char *path = NativeMemcard_PathFromDeviceName(save_name, 0);
	FILE *file = fopen(path, "rb");

	if (file == NULL)
	{
		return 0;
	}

	fclose(file);
	return 1;
}

enum NativeMemcardResult NativeMemcard_RemoveFile(const char *save_name)
{
	const char *path = NativeMemcard_PathFromDeviceName(save_name, 0);

	return remove(path) == 0 ? NATIVE_MEMCARD_OK : NATIVE_MEMCARD_NOT_FOUND;
}

enum NativeMemcardResult NativeMemcard_ReadSaveData(const char *save_name, unsigned char *dst, int byte_count, int data_offset)
{
	const char *path = NativeMemcard_PathFromDeviceName(save_name, 0);

	FILE *file = fopen(path, "rb");
	if (file == NULL)
	{
		return NATIVE_MEMCARD_NOT_FOUND;
	}

	if (fseek(file, data_offset, SEEK_SET) != 0)
	{
		fclose(file);
		return NATIVE_MEMCARD_IO_ERROR;
	}

	size_t read_bytes = fread(dst, 1, byte_count, file);
	fclose(file);

	return read_bytes == (size_t)byte_count ? NATIVE_MEMCARD_OK : NATIVE_MEMCARD_IO_ERROR;
}

enum NativeMemcardResult NativeMemcard_WriteSaveData(const char *save_name, const void *icon, int icon_byte_count, const unsigned char *src, int byte_count)
{
	const char *path = NativeMemcard_PathFromDeviceName(save_name, 1);

	FILE *file = fopen(path, "wb");
	if (file == NULL)
	{
		return NATIVE_MEMCARD_OPEN_FAILED;
	}

	size_t wrote_icon = fwrite(icon, 1, icon_byte_count, file);
	size_t wrote_data = fwrite(src, 1, byte_count, file);
	fclose(file);

	if ((wrote_icon != (size_t)icon_byte_count) || (wrote_data != (size_t)byte_count))
	{
		return NATIVE_MEMCARD_IO_ERROR;
	}

	return NATIVE_MEMCARD_OK;
}
