// PS2: direct disc access through libcdvd (sceCdRead by sector).
// NOTE: compiled as its own object, NOT part of the unity build: libcdvd.h
// defines PsyQ-compatible macros (CdRead, CdSync, ...) that clash with the
// game's own CD functions.
#include <stdio.h>
#include <string.h>
#include <libcdvd.h>

static int s_ps2CdInitDone = 0;

int Ps2Cd_SearchFile(const char *name, unsigned int *lsnOut, unsigned int *sizeOut)
{
	sceCdlFILE file;

	if (!s_ps2CdInitDone)
	{
		sceCdInit(SCECdINIT);
		s_ps2CdInitDone = 1;
	}
	sceCdDiskReady(0);

	memset(&file, 0, sizeof(file));
	if (sceCdSearchFile(&file, name) != 1)
	{
		return 0;
	}

	*lsnOut = file.lsn;
	*sizeOut = file.size;
	return 1;
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
