// PS2 port - capa de plataforma (sustituye a platform/native_platform.c).
//
// Mantiene la misma logica de escena/frame que la version de PC, pero:
//  - sin SDL (no hay ventana, eventos ni teclado),
//  - los VBlanks del juego se sacan de la interrupcion de VBlank real del GS
//    en lugar de un temporizador del sistema operativo.

#include <platform.h>

#include <macros.h>

#include "platform/native_audio.h"
#include "platform/ps2/ps2_spu2.h"
#include "platform/native_gpu.h"
#include "platform/native_input.h"
#include "platform/native_log.h"
#include "platform/native_renderer.h"

#include <kernel.h>

void Ps2Renderer_StoreDisplayedFrame(int x, int y, int w, int h); // ps2_renderer.c

#include <stdio.h>
#include <stdlib.h>

// Definidas en native_libapi.c / native_libetc.c.
void NativeRCnt_EmitVBlank(void);

// La version de PC define estas variables en native_platform.c y otros modulos
// las referencian.
int g_dbg_polygonSelected = 0;

// Si un fotograma tarda mucho (carga), se entregan al juego todos los VBlanks
// pasados (hasta 1 s) para que su reloj no se pare, como en la PS1.
#define PS2_VSYNC_CATCHUP_MAX 60

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
	// Despierta al hilo que entrega el audio a audsrv (ps2_audio_out.c).
	Ps2AudioOut_OnVBlankIsr();
	// Y al que manda las listas de cambios al chip SPU2 (ps2_spu2.c).
	Ps2Spu2_OnVBlankIsr();
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
	// Lo que el juego ha cambiado en las voces este VBlank (key on/off,
	// volumenes, tono...) se manda de una vez al chip SPU2 (ps2_spu2.c).
	Ps2Spu2_Flush();
	// Audio: avanza con el reloj real (VBlanks de hardware), no con los del
	// juego; si no hay nada pendiente no hace nada.
	Ps2AudioOut_Pump();
	s_nativeVBlankCount++;
}

// Entrega al juego los VBlanks hardware que ya han pasado (frames lentos),
// con un limite para no disparar decenas de callbacks seguidos.
internal int Ps2_CatchUpDueVBlanks(void)
{
	int emitted = 0;

	// Foto del contador: los VBlanks que lleguen mientras se entregan estos se
	// entregan en la siguiente llamada (si no, el bucle podria no acabar nunca).
	const u32 target = s_hwVBlankCount;
	u32 pending = target - s_consumedVBlankCount;

	if (pending > PS2_VSYNC_CATCHUP_MAX)
	{
		// Solo se descarta lo que pase de 1 s.
		s_consumedVBlankCount = target - PS2_VSYNC_CATCHUP_MAX;
	}

	while (s_consumedVBlankCount != target)
	{
		s_consumedVBlankCount++;
		Ps2_EmitVBlank();
		emitted++;
	}

	return emitted;
}

void NativeCD_Ps2Pump(void); // platform/native_cd.c

internal void Ps2_WaitAndEmitVBlank(void)
{
	// Carga del disco en segundo plano: recoger el bloque y pedir el siguiente.
	NativeCD_Ps2Pump();
	if (s_hwVBlankCount == s_consumedVBlankCount)
	{
		const u32 idleStart = Ps2Prof_Now();
		while (s_hwVBlankCount == s_consumedVBlankCount)
		{
			// Espera activa: el juego original tambien bloquea aqui. Se
			// aprovecha para avanzar la carga del disco (si no hay, no hace nada).
			NativeCD_Ps2Pump();
		}
		g_ps2ProfIdleCycles += (u32)(Ps2Prof_Now() - idleStart);
	}

	s_consumedVBlankCount++;
	Ps2_EmitVBlank();
}

// Medidor de las cargas: durante el LOADING apunta los fotogramas lentos
// (4 VBlanks o mas) y en que se fue el tiempo. Diagnostico (primeros 80).
extern unsigned long long g_ps2ProfJobCycles;         // native_cd.c
extern unsigned long long g_ps2ProfJobCallbackCycles; // native_cd.c

internal u32 Ps2_ProfDeltaMs(unsigned long long now, unsigned long long *last)
{
	// Los contadores del medidor se ponen a 0 cada ~5 s (ps2_audio_out.c).
	const unsigned long long delta = (now >= *last) ? (now - *last) : now;
	*last = now;
	return (u32)(delta / 294912ull);
}

internal void Ps2_ReportSlowLoadFrame(void)
{
	static u32 lastHw = 0;
	static u32 reported = 0;
	static unsigned long long lastCd = 0;
	static unsigned long long lastGs = 0;
	static unsigned long long lastAudio = 0;
	static unsigned long long lastJob = 0;
	static unsigned long long lastCb = 0;
	const u32 hw = s_hwVBlankCount;
	const u32 frame = hw - lastHw;
	const u32 cdMs = Ps2_ProfDeltaMs(g_ps2ProfCdCycles, &lastCd);
	const u32 gsMs = Ps2_ProfDeltaMs(g_ps2ProfGsCycles, &lastGs);
	const u32 audioMs = Ps2_ProfDeltaMs(g_ps2ProfAudioCycles, &lastAudio);
	const u32 jobMs = Ps2_ProfDeltaMs(g_ps2ProfJobCycles, &lastJob);
	const u32 cbMs = Ps2_ProfDeltaMs(g_ps2ProfJobCallbackCycles, &lastCb);
	if ((lastHw != 0) && (frame >= 4) && (reported < 80) && (sdata->Loading.stage != LOAD_IDLE))
	{
		reported++;
		const u32 frameMs = frame * 1000u / 60u;
		const u32 known = cdMs + gsMs + audioMs + jobMs + cbMs;
		printf("[CTR PS2] fotograma lento en carga: %u ms | etapa %d | esperando lector %u | gs %u | audio %u | copia carga %u | callbacks %u | resto (juego) %u\n",
		       (unsigned)frameMs, (int)sdata->Loading.stage, (unsigned)cdMs, (unsigned)gsMs, (unsigned)audioMs, (unsigned)jobMs, (unsigned)cbMs,
		       (unsigned)((frameMs > known) ? (frameMs - known) : 0u));
	}
	lastHw = hw;

	// Vigilante de cargas: si el LOADING no cambia de etapa en 10 s, escribir
	// en que esta esperando (cada 10 s mientras siga igual). Diagnostico.
	{
		static int lastStage = -1;
		static u32 stageSinceHw = 0;
		static u32 lastReportHw = 0;
		const int stage = sdata->Loading.stage;
		if (stage != lastStage)
		{
			lastStage = stage;
			stageSinceHw = hw;
		}
		else if ((stage != LOAD_IDLE) && ((hw - stageSinceHw) >= 600u) && ((hw - lastReportHw) >= 600u))
		{
			lastReportHw = hw;
			printf("[CTR PS2] vigilante carga: etapa %d desde hace %u s | cola %d | cola lista %d | reintentar %d | XA estado %d | VLC %d | estado juego %d\n",
			       stage, (unsigned)((hw - stageSinceHw) / 60u), (int)sdata->queueLength, (int)sdata->queueReady, (int)sdata->queueRetry, (int)sdata->XA_State,
			       (int)sdata->bool_IsLoaded_VlcTable, (int)sdata->mainGameState);
			NativeDiscImage_Ps2JobDebugPrint();
		}
	}
}

int VSync(int mode)
{
	if (mode < 0)
	{
		return s_nativeVBlankCount;
	}

	Ps2_ReportSlowLoadFrame();

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

	Ps2Renderer_StoreDisplayedFrame(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
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
