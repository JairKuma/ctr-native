// PS2 port - salida de audio por audsrv (SPU2).
//
// native_audio.c (el mismo de PC) emula la SPU de PS1 y decodifica la musica
// XA por software y entrega PCM de 16 bits estereo a 44100 Hz. Aqui se envia
// ese PCM al modulo audsrv del IOP, que lo reproduce por la SPU2 (y lo pasa a
// 48000 Hz internamente).
//
// No hay hilo de audio. Modelo "el driver pide, nosotros rellenamos" (como el
// callback de SDL en PC): en cada VBlank, y mientras se espera al lector,
// Ps2AudioOut_Pump() mira cuanto audio queda en la cola de audsrv y genera
// justo lo que falta para llegar a PS2_AUDIO_TARGET_BYTES.
//
// IMPORTANTE: si la cola de audsrv se vacia, audsrv REPITE su ultimo bloque una
// y otra vez (se oye "ty ty dog dog") y ademas informa mal de la cola. Por eso
// la cola se mantiene siempre con un colchon y nunca se deja vaciar.

#include <audsrv.h>

#include <stdio.h>
#include <string.h>

#define PS2_AUDIO_FRAME_BYTES 4 // 16 bits x 2 canales
// Nivel de la cola de audsrv que se mantiene: ~8 VBlanks (~133 ms).
#define PS2_AUDIO_TARGET_FRAMES (735 * 8)
#define PS2_AUDIO_TARGET_BYTES  (PS2_AUDIO_TARGET_FRAMES * PS2_AUDIO_FRAME_BYTES)
// Maximo generado en una llamada (acota el tiempo de CPU de cada relleno).
#define PS2_AUDIO_MAX_FILL_FRAMES (735 * 4)
// Por debajo de esto se cuenta como "cola casi vacia" en el informe.
#define PS2_AUDIO_LOW_BYTES (735 * 2 * PS2_AUDIO_FRAME_BYTES)
// Cada cuantos VBlanks se informa por consola (~5 s).
#define PS2_AUDIO_REPORT_VBLANKS 300

global_variable int s_ps2AudioReady = 0;  // audsrv inicializado
global_variable int s_ps2AudioOpen = 0;   // formato configurado
global_variable u32 s_ps2AudioBytesSent = 0;
global_variable int s_ps2AudioInPump = 0;
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

// Rellena la cola de audsrv hasta PS2_AUDIO_TARGET_BYTES.
internal void Ps2AudioOut_Fill(void)
{
	SDL_AudioStream *stream = &s_ps2SdlAudioStream;

	if (!s_ps2AudioOpen || !stream->open || (stream->callback == NULL) || s_ps2AudioInPump)
	{
		return;
	}

	// Generar audio puede leer del disco (musica XA), y la lectura del disco
	// llama a Ps2AudioOut_PumpIdle(): no volver a entrar.
	s_ps2AudioInPump = 1;

	int queued = Ps2AudioOut_QueuedBytes();
	if (queued < PS2_AUDIO_LOW_BYTES)
	{
		s_ps2AudioUnderruns++;
	}

	int frames = (PS2_AUDIO_TARGET_BYTES - queued) / PS2_AUDIO_FRAME_BYTES;
	if (frames > PS2_AUDIO_MAX_FILL_FRAMES)
	{
		frames = PS2_AUDIO_MAX_FILL_FRAMES;
	}

	// No pedir mas de lo que cabe en el buffer de audsrv (si no, audsrv_play_audio
	// se quedaria esperando).
	const int available = audsrv_available();
	if ((available >= 0) && (frames * PS2_AUDIO_FRAME_BYTES > available))
	{
		frames = available / PS2_AUDIO_FRAME_BYTES;
	}

	if (frames > 0)
	{
		const int bytes = frames * PS2_AUDIO_FRAME_BYTES;
		stream->callback(stream->userdata, stream, bytes, bytes);
	}

	s_ps2AudioInPump = 0;
}

// Llamado mientras se espera al lector de disco (ver ps2_cd.c): mantiene la
// cola llena durante las cargas, que en una PS2 real bloquean un buen rato.
void Ps2AudioOut_PumpIdle(void)
{
	Ps2AudioOut_Fill();
}

// Llamado en cada VBlank desde ps2_platform.c.
void Ps2AudioOut_Pump(void)
{
	Ps2AudioOut_Fill();

	if (++s_ps2AudioReportCounter >= PS2_AUDIO_REPORT_VBLANKS)
	{
		s_ps2AudioReportCounter = 0;
		printf("[CTR PS2] audio: sent %u KB total | audsrv_queued %d | low-queue %u\n", (unsigned)(s_ps2AudioBytesSent / 1024u), audsrv_queued(),
		       (unsigned)s_ps2AudioUnderruns);
		s_ps2AudioUnderruns = 0;
	}
}
