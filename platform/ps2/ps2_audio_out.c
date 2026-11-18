// PS2 port - salida de audio por audsrv (SPU2).
//
// native_audio.c (el mismo de PC) emula la SPU de PS1 y decodifica la musica
// XA por software y entrega PCM de 16 bits estereo a 44100 Hz. Aqui se envia
// ese PCM al modulo audsrv del IOP, que lo reproduce por la SPU2.
//
// Como funciona (no hay hilo de audio):
//  1. GENERAR: en cada VBlank que recibe el juego, Ps2AudioOut_Pump() genera
//     exactamente el audio de ese VBlank (44100 / 59,94 = 735,7 muestras). El
//     audio (y la musica XA) avanza asi al ritmo del juego, que lo necesita:
//     algunas pantallas (p. ej. la bandera de carga) esperan a que avance la XA.
//     Se generan TODOS los VBlanks, aunque el fotograma vaya lento.
//  2. GUARDAR: lo generado va a un FIFO propio en la EE (s_ps2AudioFifo).
//  3. ENTREGAR: del FIFO a audsrv solo lo que cabe (audsrv_available), asi la
//     EE nunca se queda bloqueada esperando a audsrv.
//
// IMPORTANTE: el buffer de audsrv es pequeno (20480 bytes, ~116 ms) y si se
// vacia, audsrv REPITE su ultimo bloque una y otra vez ("ty ty dog dog"). Si
// esta a punto de vaciarse y no hay audio en el FIFO (carga larga), se le manda
// silencio en vez de dejar que repita.

#include <audsrv.h>

#include <stdio.h>
#include <string.h>

#define PS2_AUDIO_FRAME_BYTES 4 // 16 bits x 2 canales
// Muestras por VBlank NTSC: 44100 / (60000 / 1001) = 735,735...
#define PS2_AUDIO_RATE_NUM (44100u * 1001u)
#define PS2_AUDIO_RATE_DEN 60000u
// Tamano del buffer de audsrv (ringbuf del IOP). Se ajusta con lo que informe
// audsrv_available() con la cola vacia.
#define PS2_AUDIO_RING_BYTES_DEFAULT 20480
// FIFO propio en la EE: ~370 ms. Si se llena, se descarta lo mas antiguo.
#define PS2_AUDIO_FIFO_FRAMES 16384u
// Si en audsrv queda menos que esto y el FIFO esta vacio, se manda silencio.
#define PS2_AUDIO_LOW_BYTES (735 * 2 * PS2_AUDIO_FRAME_BYTES)
#define PS2_AUDIO_SILENCE_FRAMES 735
// Cada cuantos VBlanks se informa por consola (~5 s).
#define PS2_AUDIO_REPORT_VBLANKS 300

global_variable int s_ps2AudioReady = 0; // audsrv inicializado
global_variable int s_ps2AudioOpen = 0;  // formato configurado
global_variable u32 s_ps2AudioBytesSent = 0;
global_variable u32 s_ps2AudioRateAcc = 0;
global_variable int s_ps2AudioRingBytes = PS2_AUDIO_RING_BYTES_DEFAULT;
global_variable u32 s_ps2AudioReportCounter = 0;
global_variable u32 s_ps2AudioSilenceFrames = 0;
global_variable u32 s_ps2AudioDroppedFrames = 0;
global_variable int s_ps2AudioInPump = 0;

global_variable s16 s_ps2AudioFifo[PS2_AUDIO_FIFO_FRAMES * 2] __attribute__((aligned(64)));
global_variable u32 s_ps2AudioFifoRead = 0;  // en frames, sin envolver
global_variable u32 s_ps2AudioFifoWrite = 0; // en frames, sin envolver
global_variable s16 s_ps2AudioSilence[PS2_AUDIO_SILENCE_FRAMES * 2] __attribute__((aligned(64)));

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

// Lo llama native_audio.c (via SDL_PutAudioStreamData): guarda en el FIFO.
int Ps2AudioOut_Write(const void *data, int bytes)
{
	if (!s_ps2AudioOpen || (data == NULL) || (bytes <= 0))
	{
		return 0;
	}

	const s16 *src = (const s16 *)data;
	u32 frames = (u32)bytes / PS2_AUDIO_FRAME_BYTES;

	for (u32 i = 0; i < frames; i++)
	{
		if (s_ps2AudioFifoWrite - s_ps2AudioFifoRead >= PS2_AUDIO_FIFO_FRAMES)
		{
			// FIFO lleno: se descarta lo mas antiguo (evita que crezca el retraso).
			s_ps2AudioFifoRead++;
			s_ps2AudioDroppedFrames++;
		}
		const u32 slot = (s_ps2AudioFifoWrite & (PS2_AUDIO_FIFO_FRAMES - 1)) * 2;
		s_ps2AudioFifo[slot + 0] = src[i * 2 + 0];
		s_ps2AudioFifo[slot + 1] = src[i * 2 + 1];
		s_ps2AudioFifoWrite++;
	}

	return bytes;
}

internal int Ps2AudioOut_SendToAudsrv(const void *data, int bytes)
{
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

internal u32 Ps2_GetHwVBlankCount(void); // ps2_platform.c
global_variable u32 s_ps2AudioLastFlushVBlank = 0xffffffffu;

// Pasa del FIFO a audsrv todo lo que quepa, sin bloquear. Si audsrv esta a
// punto de vaciarse y no hay nada en el FIFO, le manda silencio.
internal void Ps2AudioOut_Flush(void)
{
	if (!s_ps2AudioOpen)
	{
		return;
	}

	int available = audsrv_available();
	if (available < 0)
	{
		return;
	}
	if (available > s_ps2AudioRingBytes)
	{
		s_ps2AudioRingBytes = available;
	}
	available &= ~(PS2_AUDIO_FRAME_BYTES - 1);

	u32 fifoFrames = s_ps2AudioFifoWrite - s_ps2AudioFifoRead;
	u32 frames = (u32)available / PS2_AUDIO_FRAME_BYTES;
	if (frames > fifoFrames)
	{
		frames = fifoFrames;
	}

	while (frames > 0)
	{
		const u32 start = s_ps2AudioFifoRead & (PS2_AUDIO_FIFO_FRAMES - 1);
		u32 chunk = PS2_AUDIO_FIFO_FRAMES - start; // hasta el final del FIFO
		if (chunk > frames)
		{
			chunk = frames;
		}
		if (Ps2AudioOut_SendToAudsrv(&s_ps2AudioFifo[start * 2], (int)(chunk * PS2_AUDIO_FRAME_BYTES)) <= 0)
		{
			break;
		}
		s_ps2AudioFifoRead += chunk;
		available -= (int)(chunk * PS2_AUDIO_FRAME_BYTES);
		frames -= chunk;
	}

	// Lo que queda en audsrv. Si casi no queda y no hay audio nuevo: silencio.
	const int queued = s_ps2AudioRingBytes - available;
	if ((queued < PS2_AUDIO_LOW_BYTES) && (s_ps2AudioFifoWrite == s_ps2AudioFifoRead) && (available >= (int)sizeof(s_ps2AudioSilence)))
	{
		Ps2AudioOut_SendToAudsrv(s_ps2AudioSilence, (int)sizeof(s_ps2AudioSilence));
		s_ps2AudioSilenceFrames += PS2_AUDIO_SILENCE_FRAMES;
	}

	s_ps2AudioLastFlushVBlank = Ps2_GetHwVBlankCount();
}

// Llamado mientras se espera al lector de disco (ver ps2_cd.c). Solo entrega
// audio ya generado (no genera), y como mucho una vez por VBlank para no saturar
// el IOP con llamadas mientras lee el disco.
void Ps2AudioOut_PumpIdle(void)
{
	if (s_ps2AudioInPump || (Ps2_GetHwVBlankCount() == s_ps2AudioLastFlushVBlank))
	{
		return;
	}
	Ps2AudioOut_Flush();
}

// Llamado en cada VBlank que recibe el juego (ps2_platform.c).
void Ps2AudioOut_Pump(void)
{
	SDL_AudioStream *stream = &s_ps2SdlAudioStream;

	if (!s_ps2AudioOpen || !stream->open || (stream->callback == NULL) || s_ps2AudioInPump)
	{
		return;
	}

	// Generar audio puede leer del disco (musica XA), y la lectura del disco
	// llama a Ps2AudioOut_PumpIdle(): no volver a entrar.
	s_ps2AudioInPump = 1;

	// Audio de este VBlank, arrastrando la parte fraccionaria.
	s_ps2AudioRateAcc += PS2_AUDIO_RATE_NUM;
	const int frames = (int)(s_ps2AudioRateAcc / PS2_AUDIO_RATE_DEN);
	s_ps2AudioRateAcc %= PS2_AUDIO_RATE_DEN;

	if (frames > 0)
	{
		const int bytes = frames * PS2_AUDIO_FRAME_BYTES;
		stream->callback(stream->userdata, stream, bytes, bytes);
	}

	s_ps2AudioInPump = 0;

	Ps2AudioOut_Flush();

	if (++s_ps2AudioReportCounter >= PS2_AUDIO_REPORT_VBLANKS)
	{
		s_ps2AudioReportCounter = 0;
		printf("[CTR PS2] audio: sent %u KB | fifo %u frames | silence %u | dropped %u\n", (unsigned)(s_ps2AudioBytesSent / 1024u),
		       (unsigned)(s_ps2AudioFifoWrite - s_ps2AudioFifoRead), (unsigned)s_ps2AudioSilenceFrames, (unsigned)s_ps2AudioDroppedFrames);
		s_ps2AudioSilenceFrames = 0;
		s_ps2AudioDroppedFrames = 0;
	}
}
