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

internal u32 Ps2_GetHwVBlankCount(void)
{
	return s_hwVBlankCount;
}
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
	// Rellena la cola de audsrv hasta su nivel objetivo (solo genera lo que
	// falta, asi que varios VBlanks seguidos no generan audio de mas).
	Ps2AudioOut_Pump();
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

void Platform_EndFrame(void)
{
	Platform_EndScene();
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
