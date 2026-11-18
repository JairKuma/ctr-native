// PS2 port - salida de audio por audsrv (SPU2).
//
// native_audio.c (el mismo de PC) emula la SPU de PS1 y decodifica la musica
// XA por software y entrega PCM de 16 bits estereo a 44100 Hz. Aqui se envia
// ese PCM al modulo audsrv del IOP, que lo reproduce por la SPU2 (y lo pasa a
// 48000 Hz internamente).
//
// No hay hilo de audio: en cada VBlank el bucle principal llama a
// Ps2AudioOut_Pump(), que pide a native_audio.c (por su callback de SDL)
// exactamente el audio que corresponde a ese VBlank (44100 Hz / 59,94 Hz NTSC).
// Asi el audio (y la musica XA, que avanza al generar muestras) va al ritmo de
// la pantalla sin depender de lo que informe audsrv sobre su cola.

#include <audsrv.h>

#include <stdio.h>
#include <string.h>

#define PS2_AUDIO_FRAME_BYTES 4 // 16 bits x 2 canales
// Muestras por VBlank NTSC: 44100 / (60000 / 1001) = 735,735...
#define PS2_AUDIO_RATE_NUM   (44100u * 1001u)
#define PS2_AUDIO_RATE_DEN   60000u
// Colchon inicial para que la SPU2 no se quede sin datos: ~8 VBlanks (~130 ms).
#define PS2_AUDIO_PRIME_FRAMES (735 * 8)
// Limites de la cola de audsrv para corregir la deriva y los huecos:
// por debajo de LOW se genera un VBlank extra; por encima de HIGH se salta uno.
#define PS2_AUDIO_QUEUE_LOW_BYTES  (735 * 4 * PS2_AUDIO_FRAME_BYTES)
#define PS2_AUDIO_QUEUE_HIGH_BYTES (735 * 14 * PS2_AUDIO_FRAME_BYTES)
// Cada cuantos VBlanks se informa por consola (~5 s).
#define PS2_AUDIO_REPORT_VBLANKS 300

global_variable int s_ps2AudioReady = 0;  // audsrv inicializado
global_variable int s_ps2AudioOpen = 0;   // formato configurado
global_variable u32 s_ps2AudioBytesSent = 0;
global_variable u32 s_ps2AudioRateAcc = 0;
global_variable int s_ps2AudioPrimed = 0;
global_variable u32 s_ps2AudioReportCounter = 0;
global_variable u32 s_ps2AudioUnderruns = 0;

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

	// Muestras de este VBlank, arrastrando la parte fraccionaria.
	s_ps2AudioRateAcc += PS2_AUDIO_RATE_NUM;
	int frames = (int)(s_ps2AudioRateAcc / PS2_AUDIO_RATE_DEN);
	s_ps2AudioRateAcc %= PS2_AUDIO_RATE_DEN;

	if (!s_ps2AudioPrimed)
	{
		s_ps2AudioPrimed = 1;
		frames += PS2_AUDIO_PRIME_FRAMES;
	}
	else
	{
		// Ajuste segun lo que queda en la cola de la SPU2.
		const int queued = Ps2AudioOut_QueuedBytes();
		if (queued < PS2_AUDIO_QUEUE_LOW_BYTES)
		{
			frames += 735;
			s_ps2AudioUnderruns++;
		}
		else if (queued > PS2_AUDIO_QUEUE_HIGH_BYTES)
		{
			frames = 0;
		}
	}

	const int bytes = frames * PS2_AUDIO_FRAME_BYTES;
	if (bytes > 0)
	{
		stream->callback(stream->userdata, stream, bytes, bytes);
	}

	if (++s_ps2AudioReportCounter >= PS2_AUDIO_REPORT_VBLANKS)
	{
		s_ps2AudioReportCounter = 0;
		printf("[CTR PS2] audio: sent %u KB total | audsrv_queued %d | low-queue refills %u\n", (unsigned)(s_ps2AudioBytesSent / 1024u), audsrv_queued(),
		       (unsigned)s_ps2AudioUnderruns);
		s_ps2AudioUnderruns = 0;
	}
}
