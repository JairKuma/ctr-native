// PS2: direct disc access through libcdvd (sceCdRead by sector).
// NOTE: compiled as its own object, NOT part of the unity build: libcdvd.h
// defines PsyQ-compatible macros (CdRead, CdSync, ...) that clash with the
// game's own CD functions.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <libcdvd.h>

static int s_ps2CdInitDone = 0;
static unsigned char s_ps2CdDirBuf[2048] __attribute__((aligned(64)));

int Ps2Cd_ReadSectors(unsigned int lsn, unsigned int count, void *buf);

static unsigned int Ps2Cd_LE32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

// Compare an ISO9660 record name ("CTR_U.BIN;1") with the wanted name
// ("CTR_U.BIN"), ignoring case and the ";1" version suffix.
static int Ps2Cd_NameMatches(const unsigned char *recName, int recLen, const char *want)
{
	int wantLen = 0;
	while (want[wantLen] && want[wantLen] != ';')
		wantLen++;
	int n = 0;
	while (n < recLen && recName[n] != ';')
		n++;
	if (n != wantLen)
		return 0;
	for (int i = 0; i < n; i++)
	{
		if (toupper(recName[i]) != toupper((unsigned char)want[i]))
			return 0;
	}
	return 1;
}

// Find a file in the disc's root directory by reading the ISO9660 structures
// ourselves (only needs sceCdRead). name: "\CTR_U.BIN;1" or "CTR_U.BIN".
static int Ps2Cd_SearchRootIso9660(const char *name, unsigned int *lsnOut, unsigned int *sizeOut)
{
	while (*name == '\\' || *name == '/')
		name++;
	if (strchr(name, '\\') || strchr(name, '/'))
	{
		printf("[CTR PS2] ISO9660 search: only root files supported (%s)\n", name);
		return 0;
	}

	// Primary volume descriptor at sector 16.
	if (!Ps2Cd_ReadSectors(16, 1, s_ps2CdDirBuf))
	{
		printf("[CTR PS2] ISO9660 search: cannot read sector 16\n");
		return 0;
	}
	if (s_ps2CdDirBuf[0] != 1 || memcmp(&s_ps2CdDirBuf[1], "CD001", 5) != 0)
	{
		printf("[CTR PS2] ISO9660 search: no primary volume descriptor\n");
		return 0;
	}

	const unsigned char *root = &s_ps2CdDirBuf[156];
	unsigned int dirLsn = Ps2Cd_LE32(&root[2]);
	unsigned int dirSize = Ps2Cd_LE32(&root[10]);
	unsigned int dirSectors = (dirSize + 2047) / 2048;

	for (unsigned int s = 0; s < dirSectors; s++)
	{
		if (!Ps2Cd_ReadSectors(dirLsn + s, 1, s_ps2CdDirBuf))
		{
			printf("[CTR PS2] ISO9660 search: cannot read dir sector %u\n", dirLsn + s);
			return 0;
		}

		unsigned int pos = 0;
		while (pos < 2048)
		{
			unsigned int recLen = s_ps2CdDirBuf[pos];
			if (recLen == 0)
				break; // rest of this sector is padding
			if (pos + recLen > 2048 || recLen < 34)
				break;
			const unsigned char *rec = &s_ps2CdDirBuf[pos];
			int nameLen = rec[32];
			if (!(rec[25] & 2) && Ps2Cd_NameMatches(&rec[33], nameLen, name))
			{
				*lsnOut = Ps2Cd_LE32(&rec[2]);
				*sizeOut = Ps2Cd_LE32(&rec[10]);
				return 1;
			}
			pos += recLen;
		}
	}

	printf("[CTR PS2] ISO9660 search: %s not in root directory\n", name);
	return 0;
}

int Ps2Cd_SearchFile(const char *name, unsigned int *lsnOut, unsigned int *sizeOut)
{
	sceCdlFILE file;

	if (!s_ps2CdInitDone)
	{
		int r = sceCdInit(SCECdINIT);
		printf("[CTR PS2] sceCdInit -> %d, disk type 0x%02x\n", r, sceCdGetDiskType());
		s_ps2CdInitDone = 1;
	}
	sceCdDiskReady(0);

	memset(&file, 0, sizeof(file));
	if (sceCdSearchFile(&file, name) == 1)
	{
		*lsnOut = file.lsn;
		*sizeOut = file.size;
		return 1;
	}
	printf("[CTR PS2] sceCdSearchFile(%s) failed, reading ISO9660 directly\n", name);

	return Ps2Cd_SearchRootIso9660(name, lsnOut, sizeOut);
}

// Reads 'count' 2048-byte sectors starting at 'lsn' into buf (64-byte aligned).
int Ps2Cd_ReadSectors(unsigned int lsn, unsigned int count, void *buf)
{
	sceCdRMode mode;
	mode.trycount = 5;
	mode.spindlctrl = SCECdSpinNom;
	mode.datapattern = SCECdSecS2048;
	mode.pad = 0;

	for (int attempt = 0; attempt < 3; attempt++)
	{
		if (sceCdRead(lsn, count, buf, &mode) != 0)
		{
			sceCdSync(0);
			if (sceCdGetError() == SCECdErNO)
			{
				return 1;
			}
		}
		sceCdSync(0);
	}

	printf("[CTR PS2] sceCdRead failed at lsn %u (err %d)\n", lsn, sceCdGetError());
	return 0;
}
