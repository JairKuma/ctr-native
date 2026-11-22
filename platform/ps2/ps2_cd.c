// PS2: direct disc access through libcdvd (sceCdRead by sector).
// NOTE: compiled as its own object, NOT part of the unity build: libcdvd.h
// defines PsyQ-compatible macros (CdRead, CdSync, ...) that clash with the
// game's own CD functions.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <libcdvd.h>
#include <time.h>

static int s_ps2CdInitDone = 0;

// Vive en ps2_audio_out.c (build unity): rellena el audio mientras el lector
// trabaja, para que una carga larga no deje sin datos a audsrv.
void Ps2AudioOut_PumpIdle(void);
// Medidor de CPU (ps2_audio_out.c): tiempo esperando al lector y tiempo de
// generar sonido (para descontarlo de la espera).
extern unsigned long long g_ps2ProfCdCycles;
extern unsigned long long g_ps2ProfAudioCycles;

static inline unsigned int Ps2Cd_ProfNow(void)
{
	unsigned int count;
	__asm__ volatile("mfc0 %0, $9" : "=r"(count));
	return count;
}
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
// Diagnostico: se registran las primeras lecturas y un resumen periodico.
#define PS2_CD_LOG_FIRST_READS 40
static unsigned int s_ps2CdReadCount = 0;
static unsigned int s_ps2CdStatReads = 0;
static unsigned int s_ps2CdStatSectors = 0;
static double s_ps2CdStatMs = 0.0;
static unsigned int s_ps2CdLastEnd = 0;
static unsigned int s_ps2CdStatSeeks = 0;

static int Ps2Cd_ReadSectorsRaw(unsigned int lsn, unsigned int count, void *buf);

int Ps2Cd_ReadSectors(unsigned int lsn, unsigned int count, void *buf)
{
	const clock_t t0 = clock();
	const int ok = Ps2Cd_ReadSectorsRaw(lsn, count, buf);
	const double ms = (double)(clock() - t0) * 1000.0 / (double)CLOCKS_PER_SEC;

	const int seek = (lsn != s_ps2CdLastEnd);
	s_ps2CdLastEnd = lsn + count;
	s_ps2CdReadCount++;
	s_ps2CdStatReads++;
	s_ps2CdStatSectors += count;
	s_ps2CdStatMs += ms;
	s_ps2CdStatSeeks += seek ? 1u : 0u;

	if (s_ps2CdReadCount <= PS2_CD_LOG_FIRST_READS)
	{
		printf("[CTR PS2] cd read #%u lsn %u count %u %s %.1f ms\n", s_ps2CdReadCount, lsn, count, seek ? "SEEK" : "seq", ms);
	}
	if (s_ps2CdStatReads >= 50)
	{
		printf("[CTR PS2] cd: %u reads (%u seeks), %u KB, %.0f ms total, %.1f ms/read\n", s_ps2CdStatReads, s_ps2CdStatSeeks, s_ps2CdStatSectors * 2u,
		       s_ps2CdStatMs, s_ps2CdStatMs / (double)s_ps2CdStatReads);
		s_ps2CdStatReads = 0;
		s_ps2CdStatSectors = 0;
		s_ps2CdStatMs = 0.0;
		s_ps2CdStatSeeks = 0;
	}
	return ok;
}

static int Ps2Cd_ReadSectorsRawImpl(unsigned int lsn, unsigned int count, void *buf);

static int Ps2Cd_ReadSectorsRaw(unsigned int lsn, unsigned int count, void *buf)
{
	const unsigned int start = Ps2Cd_ProfNow();
	const unsigned long long audioStart = g_ps2ProfAudioCycles;
	const int ok = Ps2Cd_ReadSectorsRawImpl(lsn, count, buf);
	const unsigned long long spent = (unsigned long long)(unsigned int)(Ps2Cd_ProfNow() - start);
	const unsigned long long audio = g_ps2ProfAudioCycles - audioStart;
	g_ps2ProfCdCycles += (spent > audio) ? (spent - audio) : 0ull;
	return ok;
}

// --- Lectura en segundo plano (para la musica XA) ------------------------------
// La XA se lee mientras el juego sigue: se pide la lectura y se recoge cuando
// acaba (antes el juego se quedaba parado esperando al lector, y con el MX4SIO
// en la consola se notaba como lag cada vez que sonaba una XA).
// Estados: 0 libre, 1 leyendo, 2 terminada bien, 3 terminada con error.
static volatile int s_ps2CdAsyncState = 0;
static sceCdRMode s_ps2CdAsyncMode;
static unsigned int s_ps2CdAsyncReads = 0;
static unsigned int s_ps2CdAsyncWaits = 0;

static void Ps2Cd_AsyncCheck(void)
{
	if ((s_ps2CdAsyncState == 1) && (sceCdSync(1) == 0))
	{
		s_ps2CdAsyncState = (sceCdGetError() == SCECdErNO) ? 2 : 3;
	}
}

// Pide leer 'count' sectores de 2048 bytes. 1 = pedida, 0 = no se pudo.
int Ps2Cd_AsyncStart(unsigned int lsn, unsigned int count, void *buf)
{
	if (s_ps2CdAsyncState != 0)
	{
		return 0;
	}
	s_ps2CdAsyncMode.trycount = 5;
	s_ps2CdAsyncMode.spindlctrl = SCECdSpinNom;
	s_ps2CdAsyncMode.datapattern = SCECdSecS2048;
	s_ps2CdAsyncMode.pad = 0;
	if (sceCdRead(lsn, count, buf, &s_ps2CdAsyncMode) == 0)
	{
		return 0; // el lector esta ocupado
	}
	s_ps2CdAsyncState = 1;
	s_ps2CdAsyncReads++;
	return 1;
}

// 0 libre, 1 leyendo, 2 lista, 3 error (no cambia el estado salvo al terminar).
int Ps2Cd_AsyncPoll(void)
{
	Ps2Cd_AsyncCheck();
	return s_ps2CdAsyncState;
}

// Ya se han recogido los datos: el lector queda libre.
void Ps2Cd_AsyncConsume(void)
{
	if (s_ps2CdAsyncState >= 2)
	{
		s_ps2CdAsyncState = 0;
	}
}

// Esperar a que acabe la lectura en segundo plano (generando audio mientras).
void Ps2Cd_AsyncWait(void)
{
	if (s_ps2CdAsyncState != 1)
	{
		return;
	}
	s_ps2CdAsyncWaits++;
	const unsigned int start = Ps2Cd_ProfNow();
	const unsigned long long audioStart = g_ps2ProfAudioCycles;
	while (s_ps2CdAsyncState == 1)
	{
		Ps2Cd_AsyncCheck();
		if (s_ps2CdAsyncState == 1)
		{
			Ps2AudioOut_PumpIdle();
		}
	}
	const unsigned long long spent = (unsigned long long)(unsigned int)(Ps2Cd_ProfNow() - start);
	const unsigned long long audio = g_ps2ProfAudioCycles - audioStart;
	g_ps2ProfCdCycles += (spent > audio) ? (spent - audio) : 0ull;
}

void Ps2Cd_AsyncStats(unsigned int *reads, unsigned int *waits)
{
	*reads = s_ps2CdAsyncReads;
	*waits = s_ps2CdAsyncWaits;
	s_ps2CdAsyncReads = 0;
	s_ps2CdAsyncWaits = 0;
}

static int Ps2Cd_ReadSectorsRawImpl(unsigned int lsn, unsigned int count, void *buf)
{
	sceCdRMode mode;

	// Si hay una lectura de XA en marcha, el lector no admite otra: esperar a
	// que acabe (sus datos se recogen despues).
	Ps2Cd_AsyncWait();
	mode.trycount = 5;
	mode.spindlctrl = SCECdSpinNom;
	mode.datapattern = SCECdSecS2048;
	mode.pad = 0;

	for (int attempt = 0; attempt < 3; attempt++)
	{
		if (sceCdRead(lsn, count, buf, &mode) != 0)
		{
			// Espera no bloqueante: mientras lee, seguimos alimentando el audio.
			while (sceCdSync(1) != 0)
			{
				Ps2AudioOut_PumpIdle();
			}
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
