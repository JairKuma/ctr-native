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
#include <sbv_patches.h>
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

// Audio: el mismo native_audio.c de PC (emulador de la SPU de PS1 + XA). Su
// salida SDL la cubre platform/ps2/include/SDL3/SDL.h sobre ps2_audio_out.c.
#include "platform/native_audio.c"

// Modulos especificos de PS2 (sustituyen a native_input.c, native_platform.c,
// native_renderer.c y native_glad.c, y dan la salida de audio).
#include "platform/ps2/ps2_audio_out.c"
#include "platform/ps2/ps2_input.c"
#include "platform/ps2/ps2_platform.c"
#include "platform/ps2/ps2_renderer.c"

#ifndef CTR_NATIVE_VERSION
#define CTR_NATIVE_VERSION "0.0.0-ps2"
#endif

#define PS2_PATH_MAX 256

// Partidas guardadas en la memory card de PS2 (ranura 1).
#define PS2_MEMCARD_ROOT "mc0:/CTRNATIVE"

// PRUEBA: 0 = sin memory card (no se cargan MCMAN/MCSERV y no se guarda nada).
// En la PS2 real con OPL + MX4SIO, el MX4SIO va en la ranura de memory card y
// comparte el bus SIO2: si tocar la memory card bloquea ese bus, el lector
// virtual de OPL deja de responder (el juego se quedaba en la bandera, justo
// donde busca las partidas). Poner 1 para volver a guardar en mc0.
#define PS2_USE_MEMCARD 0
// Sin memory card: rutas en rom0: (solo lectura), asi abrir/guardar falla al
// momento sin tocar el bus, igual que si no hubiera partidas.
#define PS2_NO_MEMCARD_ROOT "rom0:/CTRNATIVE"

// --- Colores de diagnostico (consola real, sin texto) ------------------------
// La pantalla muestra el color del ULTIMO paso completado; si se cuelga, el
// fallo esta en el paso siguiente:
//   azul      GS iniciado
//   morado    rom0:XSIO2MAN cargado
//   rosa      rom0:XMCMAN cargado
//   naranja   rom0:XMCSERV cargado
//   amarillo  rom0:XPADMAN cargado
//   celeste   rom0:LIBSD cargado
//   blanco    audsrv.irx cargado
//   gris      audsrv iniciado; buscando CTR_U.BIN
//   verde     CTR_U.BIN encontrado; arrancando el juego
//   rojo      CTR_U.BIN NO encontrado (se queda en rojo)
void Ps2Renderer_DebugScreen(u8 r, u8 g, u8 b);

// audsrv.irx va incrustado en el ELF (Makefile.ps2 lo genera con bin2c).
extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;

// Carga los modulos del IOP: memory card, mandos y audio. Los de rom0: vienen
// en la BIOS de cualquier PS2; audsrv.irx (del PS2SDK) va dentro del ELF.
static void Ps2Main_LoadIopModules(void)
{
	// Se usan las versiones "X" de los modulos (XSIO2MAN, XMCMAN, XMCSERV,
	// XPADMAN): son las que usan los juegos de PS2 y con las que OPL deja
	// convivir al MX4SIO (que va en la ranura de memory card y comparte el bus
	// SIO2). Con las antiguas (SIO2MAN, PADMAN, MCMAN) en la PS2 real el mando
	// salia desconectado y tocar la memory card colgaba el juego en la bandera.
	// El lado EE usa libpadx (Makefile.ps2), la libreria de mandos de XPADMAN.
	// Si la BIOS no tiene la version X (consolas muy antiguas), se prueba la
	// antigua (entonces el mando no funcionara, pero el juego sigue).
	static const char *const modules[] = {"rom0:XSIO2MAN", "rom0:XMCMAN", "rom0:XMCSERV", "rom0:XPADMAN", "rom0:LIBSD"};
	static const char *const fallbacks[] = {"rom0:SIO2MAN", "rom0:MCMAN", "rom0:MCSERV", "rom0:PADMAN", NULL};
	static const u8 colors[][3] = {
	    {0x60, 0x00, 0xa0}, // morado
	    {0xff, 0x60, 0xc0}, // rosa
	    {0xff, 0x80, 0x00}, // naranja
	    {0xff, 0xff, 0x00}, // amarillo
	    {0x40, 0xc0, 0xff}, // celeste
	};

	for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
	{
#if !PS2_USE_MEMCARD
		if ((strcmp(modules[i], "rom0:XMCMAN") == 0) || (strcmp(modules[i], "rom0:XMCSERV") == 0))
		{
			printf("[CTR PS2] Skip %s (memory card desactivada para la prueba)\n", modules[i]);
			Ps2Renderer_DebugScreen(colors[i][0], colors[i][1], colors[i][2]);
			continue;
		}
#endif
		int id = SifLoadModule(modules[i], 0, NULL);
		printf("[CTR PS2] Load %s: %s (%d)\n", modules[i], (id >= 0) ? "ok" : "FAILED", id);
		if ((id < 0) && (fallbacks[i] != NULL))
		{
			id = SifLoadModule(fallbacks[i], 0, NULL);
			printf("[CTR PS2] Load %s (fallback): %s (%d)\n", fallbacks[i], (id >= 0) ? "ok" : "FAILED", id);
		}
		Ps2Renderer_DebugScreen(colors[i][0], colors[i][1], colors[i][2]);
	}

	// Permite cargar modulos desde la RAM del EE en BIOS antiguas.
	sbv_patch_enable_lmb();

	int moduleResult = 0;
	const int audsrvId = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &moduleResult);
	printf("[CTR PS2] Load audsrv.irx: %s (%d, %d)\n", (audsrvId >= 0) ? "ok" : "FAILED", audsrvId, moduleResult);
	Ps2Renderer_DebugScreen(0xff, 0xff, 0xff); // blanco
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

// Disco de PS2 (ISO cargada por OPL, o disco grabado): los nombres del disco
// son cortos y en mayusculas, asi que la imagen se llama CTR_U.BIN.
static int Ps2Main_FindDiscImage(void)
{
	static const char *const candidates[] = {
	    "cdrom0:\\CTR_U.BIN",
	    "cdrom0:\\ASSETS\\CTR_U.BIN",
	};

	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
	{
		FILE *file = fopen(candidates[i], "rb");
		if (file != NULL)
		{
			fclose(file);
			g_ps2DiscImagePathOverride = candidates[i];
			printf("[CTR PS2] Disc image: %s\n", candidates[i]);
			return 1;
		}
	}

	return 0;
}

static const char *Ps2Main_FindBaseDir(int argc, char *argv[])
{
	static char argvDir[PS2_PATH_MAX];
	static const char *const fallbacks[] = {
	    "host:",         // PCSX2 con "Host Filesystem" activado / PS2Link
	    "mass:/CTR",     // pendrive USB
	    "mass:",         //
	};

	if ((argc > 0) && Ps2Main_DirFromArgv0(argv[0], argvDir, sizeof(argvDir)))
	{
		printf("[CTR PS2] ELF directory: %s\n", argvDir);
		if (Ps2Main_DirHasAssets(argvDir))
		{
			return argvDir;
		}
	}

	printf("[CTR PS2] Looking for CTR_U.BIN on the disc (cdrom0:)\n");
	if (Ps2Main_FindDiscImage())
	{
		// Todo sale de CTR_U.BIN. La base solo sirve para buscar assets
		// extraidos sueltos (mods), que en un disco nunca estan; cada busqueda
		// fallida en el disco es lenta (busqueda en el lector). rom0: responde
		// al instante que no existen. Ademas activamos el modo "solo disco":
		// no se busca ningun fichero suelto, todo va directo a CTR_U.BIN.
		g_ps2AssetsDiscOnly = 1;
		printf("[CTR PS2] Disc-only mode: loose asset probes disabled\n");
		return "rom0:";
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

	// El GS se inicia lo primero para poder mostrar colores de diagnostico.
	NativeRenderer_InitialiseRender((char *)"Crash Team Racing", 640, 448, 1);
	Ps2Renderer_DebugScreen(0x00, 0x00, 0xc0); // azul

	Ps2Main_LoadIopModules();
	Ps2AudioOut_Init();
	Ps2Renderer_DebugScreen(0x80, 0x80, 0x80); // gris

	printf("[CTR PS2] CTR Native %s starting...\n", CTR_NATIVE_VERSION);

	const char *baseDir = Ps2Main_FindBaseDir(argc, argv);
	if (baseDir == NULL)
	{
		Ps2Renderer_DebugScreen(0xc0, 0x00, 0x00); // rojo
		printf("[CTR PS2] ERROR: assets/ctr-u.bin (or CTR_U.BIN on the disc) not found.\n");
		printf("[CTR PS2] Put the ELF next to an 'assets' folder that contains ctr-u.bin,\n");
		printf("[CTR PS2] or build a disc/ISO with CTR_U.BIN in its root.\n");
		SleepThread();
		return 1;
	}

	printf("[CTR PS2] Base: %s\n", baseDir);
	Ps2Renderer_DebugScreen(0x00, 0xc0, 0x00); // verde

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
#if PS2_USE_MEMCARD
	const char *memcardRoot = PS2_MEMCARD_ROOT;
#else
	const char *memcardRoot = PS2_NO_MEMCARD_ROOT;
#endif
	if (NativeMemcard_SetRoot(memcardRoot) != NATIVE_MEMCARD_OK)
	{
		printf("[CTR PS2] Warning: could not set memcard root %s\n", memcardRoot);
	}
	else
	{
		printf("[CTR PS2] Saves: %s%s\n", memcardRoot, PS2_USE_MEMCARD ? "" : " (memory card desactivada: no se guarda)");
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
