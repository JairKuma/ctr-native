// PS2 port - capa de plataforma (sustituye a platform/native_platform.c).
//
// Mantiene la misma logica de escena/frame que la version de PC, pero:
//  - sin SDL (no hay ventana, eventos ni teclado),
//  - los VBlanks del juego se sacan de la interrupcion de VBlank real del GS
//    en lugar de un temporizador del sistema operativo.

#include <platform.h>

#include <macros.h>

#include "platform/native_audio.h"
#include "platform/native_gpu.h"
#include "platform/native_input.h"
#include "platform/native_log.h"
#include "platform/native_renderer.h"

#include <kernel.h>

#include <stdio.h>
#include <stdlib.h>

// Definidas en native_libapi.c / native_libetc.c.
void NativeRCnt_EmitVBlank(void);

// La version de PC define estas variables en native_platform.c y otros modulos
// las referencian.
int g_dbg_polygonSelected = 0;

#define PS2_VSYNC_CATCHUP_MAX 8

global_variable int s_platformInitialized = 0;
global_variable int s_platformBeginScene = 0;
global_variable int s_pinnedVramDisplayFrames = 0;
global_variable int s_pinnedVramDisplayCustomRect = 0;
global_variable int s_pinnedVramDisplayX = 0;
global_variable int s_pinnedVramDisplayY = 0;
global_variable int s_pinnedVramDisplayW = 0;
global_variable int s_pinnedVramDisplayH = 0;

// ---------------------------------------------------------------------------
// VBlank
// ---------------------------------------------------------------------------

// Contador incrementado por la interrupcion de VBlank del hardware.
global_variable volatile u32 s_hwVBlankCount = 0;
// Cuantos de esos VBlanks hardware ya se han entregado al juego.
global_variable u32 s_consumedVBlankCount = 0;
// VBlanks que ha visto el juego (lo que devuelve VSync(-1)).
global_variable int s_nativeVBlankCount = 0;
global_variable int s_vblankHandlerId = -1;

internal int Ps2_VBlankHandler(int cause)
{
	(void)cause;
	s_hwVBlankCount++;
	ExitHandler();
	return 0;
}

internal void Ps2_InstallVBlankHandler(void)
{
	if (s_vblankHandlerId >= 0)
	{
		return;
	}

	s_vblankHandlerId = AddIntcHandler(INTC_VBLANK_S, Ps2_VBlankHandler, 0);
	EnableIntc(INTC_VBLANK_S);
	s_consumedVBlankCount = s_hwVBlankCount;
}

internal void Ps2_RemoveVBlankHandler(void)
{
	if (s_vblankHandlerId < 0)
	{
		return;
	}

	DisableIntc(INTC_VBLANK_S);
	RemoveIntcHandler(INTC_VBLANK_S, s_vblankHandlerId);
	s_vblankHandlerId = -1;
}

internal void Ps2_EmitVBlank(void)
{
	NativeRCnt_EmitVBlank();

	if (vsync_callback != NULL)
	{
		vsync_callback();
	}

	NativeAudio_StepVBlank();
	s_nativeVBlankCount++;
}

// Entrega al juego los VBlanks hardware que ya han pasado (frames lentos),
// con un limite para no disparar decenas de callbacks seguidos.
internal int Ps2_CatchUpDueVBlanks(void)
{
	int emitted = 0;
	u32 pending = s_hwVBlankCount - s_consumedVBlankCount;

	if (pending > PS2_VSYNC_CATCHUP_MAX)
	{
		s_consumedVBlankCount = s_hwVBlankCount;
		return 0;
	}

	while (s_consumedVBlankCount != s_hwVBlankCount)
	{
		s_consumedVBlankCount++;
		Ps2_EmitVBlank();
		emitted++;
	}

	return emitted;
}

internal void Ps2_WaitAndEmitVBlank(void)
{
	while (s_hwVBlankCount == s_consumedVBlankCount)
	{
		// Espera activa: el juego original tambien bloquea aqui.
	}

	s_consumedVBlankCount++;
	Ps2_EmitVBlank();
}

int VSync(int mode)
{
	if (mode < 0)
	{
		return s_nativeVBlankCount;
	}

	const int requestedVBlanks = (mode == 0) ? 1 : mode;

	Ps2_CatchUpDueVBlanks();

	for (int i = 0; i < requestedVBlanks; i++)
	{
		Ps2_WaitAndEmitVBlank();
	}

	return s_nativeVBlankCount;
}

int Platform_GetVBlankCount(void)
{
	return s_nativeVBlankCount;
}

void Platform_WaitUntilVBlank(int targetVBlank)
{
	if (targetVBlank - s_nativeVBlankCount <= 0)
	{
		return;
	}

	Ps2_CatchUpDueVBlanks();

	while (s_nativeVBlankCount < targetVBlank)
	{
		Ps2_WaitAndEmitVBlank();
	}
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------

void Platform_Init(const char *title, int width, int height)
{
	Platform_LogInit(title);
	Platform_Log("[CTR PS2] Initialising platform\n");

	s_platformInitialized = 1;

	if (!NativeRenderer_InitialiseRender((char *)title, width, height, 1))
	{
		Platform_LogError("[CTR PS2] Failed to initialise GS\n");
		Platform_Shutdown();
		return;
	}

	if (!NativeRenderer_InitialisePSX())
	{
		Platform_LogError("[CTR PS2] Failed to initialise PSX renderer state\n");
		Platform_Shutdown();
		return;
	}

	Ps2_InstallVBlankHandler();
	Platform_InputInit();
}

void Platform_Shutdown(void)
{
	if (s_platformInitialized == 0)
	{
		return;
	}

	s_platformInitialized = 0;
	Ps2_RemoveVBlankHandler();
	Platform_InputShutdown();
	NativeAudio_Shutdown();
	NativeRenderer_Shutdown();
	Platform_LogShutdown();
}

// ---------------------------------------------------------------------------
// Escena y frame (misma logica que native_platform.c)
// ---------------------------------------------------------------------------

void Platform_BeginFrame(void)
{
}

int Platform_BeginScene(void)
{
	if (s_platformBeginScene)
	{
		return 0;
	}

	NativeRenderer_BeginScene();

	if (activeDrawEnv.isbg)
	{
		const RECT16 clipenv = activeDrawEnv.clip;
		NativeRenderer_Clear(clipenv.x, clipenv.y, clipenv.w, clipenv.h, activeDrawEnv.r0, activeDrawEnv.g0, activeDrawEnv.b0);
	}

	s_platformBeginScene = 1;
	Platform_LogFlush();
	return 1;
}

void Platform_EndScene(void)
{
	if (!s_platformBeginScene)
	{
		return;
	}

	s_platformBeginScene = 0;

	NativeRenderer_EndScene();

	if (s_pinnedVramDisplayFrames > 0)
	{
		if (s_pinnedVramDisplayCustomRect)
		{
			NativeRenderer_PresentVRAMRect(s_pinnedVramDisplayX, s_pinnedVramDisplayY, s_pinnedVramDisplayW, s_pinnedVramDisplayH);
		}
		else
		{
			NativeRenderer_PresentVRAMDisplay();
		}

		NativeRenderer_EndGpuFrame();
		NativeRenderer_SwapWindow();
		s_pinnedVramDisplayFrames--;
		if (s_pinnedVramDisplayFrames <= 0)
		{
			s_pinnedVramDisplayCustomRect = 0;
		}
		return;
	}

	NativeRenderer_StoreFrameBuffer(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
	NativeRenderer_EndGpuFrame();
	NativeRenderer_SwapWindow();
}

// ---------------------------------------------------------------------------
// Diagnostico de visibilidad (temporal): cada ~60 frames imprime que decide el
// juego sobre que partes del nivel se ven. Sirve para comparar la intro de la
// carrera (donde solo sale el cielo) con la carrera normal.
// ---------------------------------------------------------------------------

global_variable int s_visDiagFrame = 0;

internal int Ps2Diag_CountBits(const int *words, int bitCount)
{
	int total = 0;

	if (words == NULL)
	{
		return -1;
	}

	for (int i = 0; i < (bitCount + 31) / 32; i++)
	{
		u32 w = (u32)words[i];
		while (w != 0)
		{
			w &= w - 1;
			total++;
		}
	}

	return total;
}

internal void Ps2Diag_PrintVisibility(void)
{
	struct GameTracker *gGT = sdata->gGT;

	if (gGT == NULL)
	{
		return;
	}

	struct CameraDC *cam = &gGT->cameraDC[0];
	struct PushBuffer *pb = &gGT->pushBuffer[0];
	struct Driver *driver = gGT->drivers[0];
	struct mesh_info *mesh = (gGT->level1 != NULL) ? gGT->level1->ptr_mesh_info : NULL;
	int leafBits = -1;
	int leafTotal = 0;
	int *leafSrc = NULL;

	if ((gGT->visMem1 != NULL) && (mesh != NULL))
	{
		leafTotal = mesh->numBspNodes;
		leafBits = Ps2Diag_CountBits(gGT->visMem1->visLeafList[0], leafTotal);
		leafSrc = gGT->visMem1->visLeafSrc[0];
	}

	const int spawnCount = ((gGT->level1 != NULL) && (gGT->level1->ptrSpawnType1 != NULL)) ? gGT->level1->ptrSpawnType1->count : -1;

	printf("[CTR PS2] vis: f %d level %d mode1 %08x render %08x | cam mode %d hit %d quad %p camLeafSrc %p | drv under %p | visLeafSrc %p visible leaves %d/%d drawn %d | "
	       "pos %d,%d,%d dist %d | trans frame %d/%d blend %d flags %08x st1 %d\n",
	       gGT->frameTimer_MainFrame_ResetDB, gGT->levelID, (unsigned)gGT->gameMode1, (unsigned)gGT->renderFlags, cam->cameraMode, (int)cam->quadBlockSearchHit,
	       (void *)cam->ptrQuadBlock, (void *)cam->visLeafSrc, (driver != NULL) ? (void *)driver->underDriver : NULL, (void *)leafSrc, leafBits, leafTotal,
	       gGT->bspLeafsDrawn, pb->pos.x, pb->pos.y, pb->pos.z, pb->distanceToScreen_PREV, cam->transitionFrame, cam->transitionFrameCount, cam->transitionBlend,
	       (unsigned)cam->flags, spawnCount);
}

// Prueba directa (una vez por carrera) de la camara de la intro: datos del
// recorrido, un RotTrans con una matriz conocida y el resultado de
// CAM_StartLine_FlyIn para varios fotogramas.
global_variable int s_flyInProbeDone = 0;

internal void Ps2Diag_FlyInProbe(struct GameTracker *gGT)
{
	struct Level *lev = gGT->level1;

	if ((lev == NULL) || (lev->ptrSpawnType1 == NULL))
	{
		printf("[CTR PS2] flyin: no level / spawn data\n");
		return;
	}

	struct SpawnType1 *st1 = lev->ptrSpawnType1;
	if (st1->count < 4)
	{
		printf("[CTR PS2] flyin: st1 count %d (no fly-in)\n", st1->count);
		return;
	}

	void **pointers = ST1_GETPOINTERS(st1);
	u8 *cameraPath = (u8 *)pointers[ST1_CAMERA_PATH];
	const s16 *pathStart = (const s16 *)cameraPath;
	const s16 *pathEnd = (const s16 *)(cameraPath + 0x354);

	printf("[CTR PS2] flyin: st1 %p count %d cameraPath %p\n", (void *)st1, st1->count, (void *)cameraPath);
	printf("[CTR PS2] flyin: start[0] %d,%d,%d start[1] %d,%d,%d start[70] %d,%d,%d\n", pathStart[0], pathStart[1], pathStart[2], pathStart[3], pathStart[4],
	       pathStart[5], pathStart[210], pathStart[211], pathStart[212]);
	printf("[CTR PS2] flyin: end[0] %d,%d,%d end[1] %d,%d,%d end[70] %d,%d,%d\n", pathEnd[0], pathEnd[1], pathEnd[2], pathEnd[3], pathEnd[4], pathEnd[5],
	       pathEnd[210], pathEnd[211], pathEnd[212]);
	printf("[CTR PS2] flyin: spawn0 pos %d,%d,%d rot %d,%d,%d | spawn1 %d,%d,%d | spawn2 %d,%d,%d\n", lev->DriverSpawn[0].pos.x, lev->DriverSpawn[0].pos.y,
	       lev->DriverSpawn[0].pos.z, lev->DriverSpawn[0].rot.x, lev->DriverSpawn[0].rot.y, lev->DriverSpawn[0].rot.z, lev->DriverSpawn[1].pos.x,
	       lev->DriverSpawn[1].pos.y, lev->DriverSpawn[1].pos.z, lev->DriverSpawn[2].pos.x, lev->DriverSpawn[2].pos.y, lev->DriverSpawn[2].pos.z);

	// RotTrans con matriz identidad y traslacion (100,200,300): debe dar 110,220,330.
	{
		MATRIX m;
		SVECTOR v;
		VECTOR out;
		s32 flag = 0;

		memset(&m, 0, sizeof(m));
		m.m[0][0] = 4096;
		m.m[1][1] = 4096;
		m.m[2][2] = 4096;
		m.t[0] = 100;
		m.t[1] = 200;
		m.t[2] = 300;
		memset(&v, 0, sizeof(v));
		v.vx = 10;
		v.vy = 20;
		v.vz = 30;
		memset(&out, 0, sizeof(out));

		SetRotMatrix(&m);
		SetTransMatrix(&m);
		RotTrans(&v, &out, &flag);
		printf("[CTR PS2] flyin: RotTrans test -> %d,%d,%d (expected 110,220,330) flag %08x\n", (int)out.vx, (int)out.vy, (int)out.vz, (unsigned)flag);
	}

	// La funcion real con varios fotogramas.
	{
		static const s32 frames[] = {0, 50, 100, 150};
		struct FlyInData flyInData;

		flyInData.ptrEnd = cameraPath + 0x354;
		flyInData.ptrStart = cameraPath;
		flyInData.frameCount1 = 0x96;
		flyInData.frameCount2 = 0x8e;

		for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++)
		{
			SVec3 pos;
			SVec3 rot;

			memset(&pos, 0, sizeof(pos));
			memset(&rot, 0, sizeof(rot));
			CAM_StartLine_FlyIn(&flyInData, 0x96, frames[i], &pos, &rot);
			printf("[CTR PS2] flyin: frame %d -> pos %d,%d,%d rot %d,%d,%d\n", (int)frames[i], pos.x, pos.y, pos.z, rot.x, rot.y, rot.z);
		}
	}
}

void Platform_EndFrame(void)
{
	Platform_EndScene();

	if ((sdata->gGT != NULL) && ((sdata->gGT->gameMode1 & START_OF_RACE) != 0))
	{
		if (!s_flyInProbeDone)
		{
			s_flyInProbeDone = 1;
			Ps2Diag_FlyInProbe(sdata->gGT);
		}
	}
	else
	{
		s_flyInProbeDone = 0;
	}

	// Durante la intro de la carrera (START_OF_RACE) se muestrea mas a menudo.
	const int startOfRace = (sdata->gGT != NULL) && ((sdata->gGT->gameMode1 & START_OF_RACE) != 0);
	if (++s_visDiagFrame >= (startOfRace ? 10 : 60))
	{
		s_visDiagFrame = 0;
		Ps2Diag_PrintVisibility();
	}
}

void Platform_PresentVRAMDisplay(void)
{
	Platform_PinVRAMDisplayFrames(1);
	Platform_BeginScene();
	Platform_EndFrame();
}

void Platform_PinVRAMDisplayFrames(int frameCount)
{
	if (frameCount > s_pinnedVramDisplayFrames)
	{
		s_pinnedVramDisplayFrames = frameCount;
		s_pinnedVramDisplayCustomRect = 0;
	}
}

void Platform_PinVRAMDisplayRect(int x, int y, int w, int h, int frameCount)
{
	if ((frameCount <= 0) || (w <= 0) || (h <= 0))
	{
		return;
	}

	s_pinnedVramDisplayX = x;
	s_pinnedVramDisplayY = y;
	s_pinnedVramDisplayW = w;
	s_pinnedVramDisplayH = h;
	s_pinnedVramDisplayFrames = frameCount;
	s_pinnedVramDisplayCustomRect = 1;
}

// ---------------------------------------------------------------------------
// Entrada
// ---------------------------------------------------------------------------

void Platform_PollHostEvents(void)
{
	// En PS2 no hay eventos de ventana.
}

int Platform_PollInput(void)
{
	Platform_InputUpdate();
	return 1;
}

int NikoGetEnterKey(void)
{
	// En PC lee la tecla Enter del teclado. En PS2 no hay teclado.
	return 0;
}
