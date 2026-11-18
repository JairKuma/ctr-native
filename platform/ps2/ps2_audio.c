// PS2 port - audio backend (fase 1: silencio).
//
// Implementa la misma API publica que platform/native_audio.c, pero sin sonido.
// El objetivo de la fase 1 es arrancar y pintar. En la fase 3 se sustituye por
// el mezclador real (salida por audsrv) y despues por la SPU2 por hardware.

#include <macros.h>
#include <psx/libspu.h>

#include "platform/native_audio.h"

#include <string.h>

#define PS2_AUDIO_SPU_MEMSIZE (512 * 1024)

global_variable u32 s_ps2SpuTransferAddr;

s32 NativeAudio_SpuInit(void)
{
	s_ps2SpuTransferAddr = 0;
	return 1;
}

u32 NativeAudio_SpuSetTransferStartAddr(u32 addr)
{
	if (addr > PS2_AUDIO_SPU_MEMSIZE)
	{
		return 0;
	}

	s_ps2SpuTransferAddr = addr;
	return (addr < 0x1010) ? 0 : 1;
}

u32 NativeAudio_SpuWrite(const u8 *addr, u32 size)
{
	if ((addr == NULL) || (size == 0) || (size > (u32)(PS2_AUDIO_SPU_MEMSIZE - s_ps2SpuTransferAddr)))
	{
		return 0;
	}

	return size;
}

void NativeAudio_SpuSetVoiceAttr(SpuVoiceAttr *psxAttrib)
{
	(void)psxAttrib;
}

void NativeAudio_SpuSetKey(s32 on_off, u32 voice_bit)
{
	(void)on_off;
	(void)voice_bit;
}

s32 NativeAudio_SpuSetReverb(s32 on_off)
{
	(void)on_off;
	return 0;
}

s32 NativeAudio_SpuSetReverbModeParam(SpuReverbAttr *attr)
{
	(void)attr;
	return 0;
}

void NativeAudio_SpuSetReverbModeDepth(s16 left, s16 right)
{
	(void)left;
	(void)right;
}

u32 NativeAudio_SpuSetReverbVoice(s32 on_off, u32 voice_bit)
{
	(void)on_off;
	(void)voice_bit;
	return 0;
}

void NativeAudio_SpuSetCommonMasterVolume(s16 left, s16 right)
{
	(void)left;
	(void)right;
}

void NativeAudio_SpuSetCommonCDMix(s32 enabled)
{
	(void)enabled;
}

void NativeAudio_SpuSetCommonCDVolume(s16 left, s16 right)
{
	(void)left;
	(void)right;
}

void NativeAudio_SpuSetCommonCDReverb(s32 enabled)
{
	(void)enabled;
}

// Devolver 0 hace que CDSYS trate la musica XA como "no disponible" y siga.
int NativeAudio_PlayXATrack(int categoryID, int xaID, int volumeLeft, int volumeRight)
{
	(void)categoryID;
	(void)xaID;
	(void)volumeLeft;
	(void)volumeRight;
	return 0;
}

int NativeAudio_PlayXAFile(const char *relativePath, int channelFilter, int volumeLeft, int volumeRight)
{
	(void)relativePath;
	(void)channelFilter;
	(void)volumeLeft;
	(void)volumeRight;
	return 0;
}

int NativeAudio_GetXATrackLength(int categoryID, int xaID)
{
	(void)categoryID;
	(void)xaID;
	return 0;
}

int NativeAudio_IsXAPlaying(void)
{
	return 0;
}

int NativeAudio_GetXACurrOffset(void)
{
	return 0;
}

int NativeAudio_GetXAMaxSample(void)
{
	return 0;
}

int NativeAudio_GetXAMaxSampleAtOffset(int xaCurrOffset)
{
	(void)xaCurrOffset;
	return 0;
}

void NativeAudio_SetXAVolume(int volumeLeft, int volumeRight)
{
	(void)volumeLeft;
	(void)volumeRight;
}

void NativeAudio_StopXA(void)
{
}

void NativeAudio_StepVBlank(void)
{
}

int NativeAudio_RenderFrames(s16 *out, int frameCount)
{
	if ((out != NULL) && (frameCount > 0))
	{
		memset(out, 0, (size_t)frameCount * 2u * sizeof(s16));
	}
	return frameCount;
}

void NativeAudio_SetDeterministicRenderMode(int enabled)
{
	(void)enabled;
}

int NativeAudio_IsDeterministicRenderMode(void)
{
	return 0;
}

int NativeAudio_QueueRenderedFrames(const s16 *frames, int frameCount)
{
	(void)frames;
	return frameCount;
}

void NativeAudio_ClearOutputQueue(void)
{
}

void NativeAudio_Shutdown(void)
{
}

int NativeAudio_GetStateSize(void)
{
	return 0;
}

int NativeAudio_CaptureState(void *dst, int dstSize)
{
	(void)dst;
	(void)dstSize;
	return 0;
}

int NativeAudio_RestoreState(const void *src, int srcSize)
{
	(void)src;
	(void)srcSize;
	return 0;
}
