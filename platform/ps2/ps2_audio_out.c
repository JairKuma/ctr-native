// PS2 port - salida de audio por audsrv (SPU2).
//
// native_audio.c (el mismo de PC) emula la SPU de PS1 y decodifica la musica
// XA por software y entrega PCM de 16 bits estereo a 44100 Hz. Aqui se envia
// ese PCM al modulo audsrv del IOP, que lo reproduce por la SPU2 (y lo pasa a
// 48000 Hz internamente).
//
// No hay hilo de audio: en cada VBlank el bucle principal llama a
// Ps2AudioOut_Pump(), que pide a native_audio.c (por su callback de SDL) las
// muestras que falten para mantener unos pocos fotogramas en cola.

#include <audsrv.h>

#include <stdio.h>
#include <string.h>

// Cuanto audio se intenta mantener en cola: ~3 VBlanks (~50 ms).
#define PS2_AUDIO_TARGET_FRAMES ((44100 / 60) * 3)
#define PS2_AUDIO_FRAME_BYTES   4 // 16 bits x 2 canales

global_variable int s_ps2AudioReady = 0;  // audsrv inicializado
global_variable int s_ps2AudioOpen = 0;   // formato configurado
global_variable u32 s_ps2AudioBytesSent = 0;

int Ps2AudioOut_Init(void)
{
	if (s_ps2AudioReady)
	{
		return 1;
	}

	const int result = audsrv_init();
	if (result != 0)
	{
		printf("[CTR PS2] audsrv_init failed (%d)\n", result);
		return 0;
	}

	s_ps2AudioReady = 1;
	printf("[CTR PS2] audsrv ready\n");
	return 1;
}

int Ps2AudioOut_Open(int freq, int channels)
{
	struct audsrv_fmt_t format;

	if (!s_ps2AudioReady)
	{
		return 0;
	}

	memset(&format, 0, sizeof(format));
	format.bits = 16;
	format.freq = freq;
	format.channels = channels;

	const int result = audsrv_set_format(&format);
	if (result < 0)
	{
		printf("[CTR PS2] audsrv_set_format(%d Hz, %d ch) failed (%d)\n", freq, channels, result);
		return 0;
	}

	audsrv_set_volume(MAX_VOLUME);
	s_ps2AudioOpen = 1;
	printf("[CTR PS2] Audio output: %d Hz, %d ch\n", freq, channels);
	return 1;
}

void Ps2AudioOut_Close(void)
{
	if (s_ps2AudioOpen)
	{
		audsrv_stop_audio();
		s_ps2AudioOpen = 0;
	}
}

int Ps2AudioOut_Write(const void *data, int bytes)
{
	if (!s_ps2AudioOpen || (data == NULL) || (bytes <= 0))
	{
		return 0;
	}

	const int sent = audsrv_play_audio((const char *)data, bytes);
	if (sent > 0)
	{
		s_ps2AudioBytesSent += (u32)sent;
	}
	return sent;
}

int Ps2AudioOut_QueuedBytes(void)
{
	if (!s_ps2AudioOpen)
	{
		return 0;
	}

	const int queued = audsrv_queued();
	return (queued > 0) ? queued : 0;
}

// Llamado en cada VBlank desde ps2_platform.c.
void Ps2AudioOut_Pump(void)
{
	SDL_AudioStream *stream = &s_ps2SdlAudioStream;

	if (!s_ps2AudioOpen || !stream->open || (stream->callback == NULL))
	{
		return;
	}

	const int targetBytes = PS2_AUDIO_TARGET_FRAMES * PS2_AUDIO_FRAME_BYTES;
	const int queuedBytes = Ps2AudioOut_QueuedBytes();

	if (queuedBytes >= targetBytes)
	{
		return;
	}

	int needed = targetBytes - queuedBytes;
	needed -= needed % PS2_AUDIO_FRAME_BYTES;
	if (needed > 0)
	{
		stream->callback(stream->userdata, stream, needed, needed);
	}
}
