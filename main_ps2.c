// CTR Native - punto de entrada para PS2.
//
// Equivalente a main.c, pero:
//  - sin SDL,
//  - con los modulos de plataforma de platform/ps2/ en lugar de los de PC
//    (audio, input, plataforma y renderer),
//  - buscando los assets en rutas de PS2 (host:, mass:, cdrom0:).
//
// Se compila con Makefile.ps2. main.c no se toca: el build de PC sigue igual.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>

#define _EnterCriticalSection(x)
#define EnterCriticalSection(x)
#define ExitCriticalSection()

#include "platform/native_assets.h"
#include "platform/native_log.h"
#include "platform/native_memory.h"
#include "platform/native_perf.h"
#include "platform/native_replay_scheduler.h"
#include "platform/native_savestate.h"

#include <platform.h>

#include "game/game_unity.h"

#include "game/zGlobal_RDATA.c"
#include "game/zGlobal_DATA.c"
#include "game/zGlobal_SDATA.c"

#undef RECT

// Modulos de plataforma comunes con PC (C portable, sin SDL).
#include "platform/native_disc_image.c"
#include "platform/native_assets.c"
#include "platform/native_memory.c"
#include "platform/native_checkpoint.c"
#include "platform/native_checkpoint_file.c"
#include "platform/native_cd.c"
#include "platform/native_gpu_links.c"
#include "platform/native_gpu.c"
#include "platform/native_gte_core.c"
#include "platform/native_inline_c.c"
#include "platform/native_libapi.c"
#include "platform/native_libetc.c"
#include "platform/native_libgte.c"
#include "platform/native_libgpu.c"
#include "platform/native_libpad.c"
#include "platform/native_libspu.c"
#include "platform/native_log.c"
#include "platform/native_memcard.c"
#include "platform/native_memcard_adapter.c"
#include "platform/native_perf.c"
#include "platform/native_replay_scheduler.c"
#include "platform/native_savestate.c"
#include "platform/native_state.c"
#include "platform/native_str.c"

// Modulos especificos de PS2 (sustituyen a native_audio.c, native_input.c,
// native_platform.c, native_renderer.c y native_glad.c).
#include "platform/ps2/ps2_audio.c"
#include "platform/ps2/ps2_input.c"
#include "platform/ps2/ps2_platform.c"
#include "platform/ps2/ps2_renderer.c"

#ifndef CTR_NATIVE_VERSION
#define CTR_NATIVE_VERSION "0.0.0-ps2"
#endif

#define PS2_PATH_MAX 256

// Partidas guardadas en la memory card de PS2 (ranura 1).
#define PS2_MEMCARD_ROOT "mc0:/CTRNATIVE"

// Carga los modulos del IOP que hacen falta para la memory card (y mas
// adelante los mandos). Los de rom0: vienen en la BIOS de cualquier PS2.
static void Ps2Main_LoadIopModules(void)
{
	static const char *const modules[] = {"rom0:SIO2MAN", "rom0:MCMAN", "rom0:MCSERV"};

	for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
	{
		const int id = SifLoadModule(modules[i], 0, NULL);
		printf("[CTR PS2] Load %s: %s (%d)\n", modules[i], (id >= 0) ? "ok" : "FAILED", id);
	}
}

// Comprueba si existe <dir>/assets/ctr-u.bin o <dir>/assets/BIGFILE.BIG.
static int Ps2Main_DirHasAssets(const char *dir)
{
	static const char *const required[] = {"assets/ctr-u.bin", "assets/BIGFILE.BIG"};
	char path[PS2_PATH_MAX];
	const size_t dirLen = strlen(dir);

	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
	{
		const char *separator = ((dirLen > 0) && (dir[dirLen - 1] != '/') && (dir[dirLen - 1] != ':')) ? "/" : "";
		snprintf(path, sizeof(path), "%s%s%s", dir, separator, required[i]);

		FILE *file = fopen(path, "rb");
		if (file != NULL)
		{
			fclose(file);
			return 1;
		}
	}

	return 0;
}

// Carpeta del ELF a partir de argv[0] (p. ej. "mass:/CTR/ctr_ps2.elf" -> "mass:/CTR").
static int Ps2Main_DirFromArgv0(const char *argv0, char *dst, size_t dstSize)
{
	if ((argv0 == NULL) || (argv0[0] == '\0') || (strchr(argv0, ':') == NULL))
	{
		return 0;
	}

	snprintf(dst, dstSize, "%s", argv0);

	char *lastSlash = strrchr(dst, '/');
	char *lastBackslash = strrchr(dst, '\\');
	if ((lastBackslash != NULL) && ((lastSlash == NULL) || (lastBackslash > lastSlash)))
	{
		lastSlash = lastBackslash;
	}

	if (lastSlash != NULL)
	{
		*lastSlash = '\0';
		return 1;
	}

	char *colon = strchr(dst, ':');
	colon[1] = '\0';
	return 1;
}

static const char *Ps2Main_FindBaseDir(int argc, char *argv[])
{
	static char argvDir[PS2_PATH_MAX];
	static const char *const fallbacks[] = {
	    "host:",         // PCSX2 con "Host Filesystem" activado / PS2Link
	    "mass:/CTR",     // pendrive USB
	    "mass:",         //
	    "cdrom0:",       // disco grabado
	};

	if ((argc > 0) && Ps2Main_DirFromArgv0(argv[0], argvDir, sizeof(argvDir)))
	{
		printf("[CTR PS2] ELF directory: %s\n", argvDir);
		if (Ps2Main_DirHasAssets(argvDir))
		{
			return argvDir;
		}
	}

	for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++)
	{
		printf("[CTR PS2] Looking for assets in %s\n", fallbacks[i]);
		if (Ps2Main_DirHasAssets(fallbacks[i]))
		{
			return fallbacks[i];
		}
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	SifInitRpc(0);
	Ps2Main_LoadIopModules();

	printf("[CTR PS2] CTR Native %s starting...\n", CTR_NATIVE_VERSION);

	const char *baseDir = Ps2Main_FindBaseDir(argc, argv);
	if (baseDir == NULL)
	{
		printf("[CTR PS2] ERROR: assets/ctr-u.bin not found.\n");
		printf("[CTR PS2] Put the ELF next to an 'assets' folder that contains ctr-u.bin.\n");
		SleepThread();
		return 1;
	}

	printf("[CTR PS2] Base: %s\n", baseDir);

	if (!NativeAssets_Init(baseDir))
	{
		printf("[CTR PS2] ERROR: failed to initialise asset paths.\n");
		SleepThread();
		return 1;
	}

	// Rutas relativas (p. ej. la carpeta "memcards") relativas a la base.
	if (chdir(NativeAssets_GetBaseDir()) != 0)
	{
		printf("[CTR PS2] Warning: chdir(%s) failed\n", NativeAssets_GetBaseDir());
	}

	// En PC las partidas van a la carpeta "memcards" junto al juego. En PS2 van
	// a la memory card: es lo correcto en hardware real y, ademas, el host: de
	// PCSX2 no permite crear ni listar carpetas.
	if (NativeMemcard_SetRoot(PS2_MEMCARD_ROOT) != NATIVE_MEMCARD_OK)
	{
		printf("[CTR PS2] Warning: could not set memcard root %s\n", PS2_MEMCARD_ROOT);
	}
	else
	{
		printf("[CTR PS2] Saves: %s\n", PS2_MEMCARD_ROOT);
	}

	if (!NativeAssets_Validate())
	{
		printf("[CTR PS2] ERROR: asset validation failed.\n");
		SleepThread();
		return 1;
	}

	Platform_Init("Crash Team Racing", 640, 448);
	Platform_InitScratchpad();
	Platform_RepairResidentPointers(0);

	const int result = CTR_Main();

	Platform_Shutdown();
	return result;
}
