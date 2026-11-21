// PS2 port - salida de audio por audsrv (SPU2).
//
// native_audio.c (el mismo de PC) emula la SPU de PS1 y decodifica la musica
// XA por software y entrega PCM de 16 bits estereo a 44100 Hz. Aqui se envia
// ese PCM al modulo audsrv del IOP, que lo reproduce por la SPU2.
//
// Como funciona:
//  1. GENERAR: se genera el audio de cada VBlank de HARDWARE que pasa (44100 /
//     59,94 = 735,7 muestras), tambien durante las cargas (desde la espera del
//     lector, ps2_cd.c). Asi el audio y la musica XA avanzan con el reloj real,
//     como en una PS1, aunque el juego vaya lento.
//  2. GUARDAR: lo generado va a un FIFO propio en la EE (s_ps2AudioFifo).
//  3. ENTREGAR: un hilo propio (Ps2AudioOut_FeederThread), despertado por la
//     interrupcion de VBlank 60 veces por segundo, pasa del FIFO a audsrv lo que
//     cabe (audsrv_available). Asi audsrv recibe datos aunque el juego vaya lento
//     (su buffer solo dura ~116 ms) y la EE nunca se bloquea esperando a audsrv.
//
// IMPORTANTE: el buffer de audsrv es pequeno (20480 bytes, ~116 ms) y si se
// vacia, audsrv REPITE su ultimo bloque una y otra vez ("ty ty dog dog"). Si
// esta a punto de vaciarse y no hay audio en el FIFO (carga larga), se le manda
// silencio en vez de dejar que repita.

#include <audsrv.h>
#include <kernel.h>

#include <stdio.h>
#include <string.h>

#include "platform/ps2/ps2_spu2.h"
extern unsigned long long g_ps2ProfSpu2Cycles; // ps2_spu2.c

#define PS2_AUDIO_FRAME_BYTES 4 // 16 bits x 2 canales
// Muestras por VBlank NTSC: 44100 / (60000 / 1001) = 735,735...
#define PS2_AUDIO_RATE_NUM (44100u * 1001u)
#define PS2_AUDIO_RATE_DEN 60000u
// Tamano del buffer de audsrv (ringbuf del IOP). Se ajusta con lo que informe
// audsrv_available() con la cola vacia.
#define PS2_AUDIO_RING_BYTES_DEFAULT 20480
// FIFO propio en la EE: ~740 ms (cabe una rafaga de 24 VBlanks de golpe). Si
// se llena, se descarta lo nuevo.
#define PS2_AUDIO_FIFO_FRAMES 32768u
// Retraso maximo del sonido (FIFO + audsrv, en frames). Si se pasa (porque se
// mando silencio mientras el juego estaba parado), se tira lo mas antiguo del
// FIFO hasta quedar en PS2_AUDIO_TARGET_LATENCY_FRAMES. Sin esto el retraso
// crecia sin parar hasta llenar el FIFO ("dropped" y musica entrecortada).
#define PS2_AUDIO_MAX_LATENCY_FRAMES    (735u * 16u) // ~267 ms
#define PS2_AUDIO_TARGET_LATENCY_FRAMES (735u * 10u) // ~167 ms
// Si en audsrv queda menos que esto y el FIFO esta vacio, se manda silencio.
#define PS2_AUDIO_LOW_BYTES (735 * 2 * PS2_AUDIO_FRAME_BYTES)
#define PS2_AUDIO_SILENCE_FRAMES 735
// Cada cuantos VBlanks de hardware se informa por consola (~5 s).
#define PS2_AUDIO_REPORT_VBLANKS 300

// --- Medidor de CPU -----------------------------------------------------------
// Se cuentan ciclos de la EE (registro Count del COP0, 294,912 MHz) gastados en
// cada cosa. ps2_cd.c (objeto aparte) y ps2_platform.c / ps2_renderer.c suman
// aqui. Cada ~5 s se imprime el reparto en "[CTR PS2] cpu: ...".
#define PS2_EE_CYCLES_PER_VBLANK 4920120ull // 294912000 / 59,94
unsigned long long g_ps2ProfAudioCycles = 0; // generar el sonido (emulacion SPU + XA)
unsigned long long g_ps2ProfCdCycles = 0;    // esperando al lector de disco
unsigned long long g_ps2ProfIdleCycles = 0;  // esperando el VBlank (tiempo libre)
unsigned long long g_ps2ProfGsCycles = 0;    // enviando/esperando al GS al final del frame

static inline u32 Ps2Prof_Now(void)
{
	u32 count;
	__asm__ volatile("mfc0 %0, $9" : "=r"(count));
	return count;
}

global_variable int s_ps2AudioReady = 0; // audsrv inicializado
global_variable int s_ps2AudioOpen = 0;  // formato configurado
global_variable u32 s_ps2AudioBytesSent = 0;
global_variable u32 s_ps2AudioRateAcc = 0;
global_variable int s_ps2AudioRingBytes = PS2_AUDIO_RING_BYTES_DEFAULT;
global_variable u32 s_ps2AudioSilenceFrames = 0;
global_variable u32 s_ps2AudioDroppedFrames = 0;
global_variable int s_ps2AudioInPump = 0;
global_variable u32 s_ps2AudioTrimmedFrames = 0;
// Lo que queda en audsrv (bytes), segun la ultima vez que miro el hilo de entrega.
global_variable volatile u32 s_ps2AudioQueuedEstimate = 0;
// Peticiones de recorte: el hilo principal sube "requested", el de entrega
// recorta y sube "done". Cada contador lo escribe un solo hilo.
global_variable volatile u32 s_ps2AudioTrimRequested = 0;
global_variable volatile u32 s_ps2AudioTrimDone = 0;
global_variable volatile u32 s_ps2AudioTrimFrames = 0;

global_variable s16 s_ps2AudioFifo[PS2_AUDIO_FIFO_FRAMES * 2] __attribute__((aligned(64)));
// FIFO de un productor (hilo principal, escribe) y un consumidor (hilo de
// entrega, lee): cada indice lo modifica un solo hilo.
global_variable volatile u32 s_ps2AudioFifoRead = 0;  // en frames, sin envolver
global_variable volatile u32 s_ps2AudioFifoWrite = 0; // en frames, sin envolver
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

internal void Ps2AudioOut_StartFeeder(void);

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
	Ps2AudioOut_StartFeeder();
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

	u32 write = s_ps2AudioFifoWrite;
	for (u32 i = 0; i < frames; i++)
	{
		if (write - s_ps2AudioFifoRead >= PS2_AUDIO_FIFO_FRAMES)
		{
			// FIFO lleno: se descarta lo nuevo (el indice de lectura es del otro hilo).
			s_ps2AudioDroppedFrames += frames - i;
			break;
		}
		const u32 slot = (write & (PS2_AUDIO_FIFO_FRAMES - 1)) * 2;
		s_ps2AudioFifo[slot + 0] = src[i * 2 + 0];
		s_ps2AudioFifo[slot + 1] = src[i * 2 + 1];
		write++;
	}
	s_ps2AudioFifoWrite = write; // publicar despues de copiar los datos

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
global_variable u32 s_ps2AudioLastServiceVBlank = 0xffffffffu;

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

	// Recorte pedido por el hilo principal: tirar lo mas antiguo del FIFO.
	if (s_ps2AudioTrimRequested != s_ps2AudioTrimDone)
	{
		u32 trim = s_ps2AudioTrimFrames;
		const u32 inFifo = s_ps2AudioFifoWrite - s_ps2AudioFifoRead;
		if (trim > inFifo)
		{
			trim = inFifo;
		}
		s_ps2AudioFifoRead += trim;
		s_ps2AudioTrimmedFrames += trim;
		s_ps2AudioTrimDone = s_ps2AudioTrimRequested;
	}

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
		available -= (int)sizeof(s_ps2AudioSilence);
	}

	const int queuedNow = s_ps2AudioRingBytes - available;
	s_ps2AudioQueuedEstimate = (queuedNow > 0) ? (u32)queuedNow : 0u;

}

// --- Hilo de entrega -------------------------------------------------------
#define PS2_AUDIO_FEEDER_PRIORITY 32 // mas prioritario que el hilo principal
#define PS2_AUDIO_MAIN_PRIORITY   64
global_variable int s_ps2AudioFeedSema = -1;
global_variable int s_ps2AudioFeederThread = -1;
global_variable u8 s_ps2AudioFeederStack[16 * 1024] __attribute__((aligned(16)));
extern void *_gp;

// Desde la interrupcion de VBlank (ps2_platform.c): despierta al hilo.
void Ps2AudioOut_OnVBlankIsr(void)
{
	if (s_ps2AudioFeedSema >= 0)
	{
		iSignalSema(s_ps2AudioFeedSema);
	}
}

internal void Ps2AudioOut_FeederThread(void *arg)
{
	(void)arg;
	for (;;)
	{
		WaitSema(s_ps2AudioFeedSema);
		Ps2AudioOut_Flush();
	}
}

internal void Ps2AudioOut_StartFeeder(void)
{
	if (s_ps2AudioFeederThread >= 0)
	{
		return;
	}

	ee_sema_t sema;
	memset(&sema, 0, sizeof(sema));
	sema.init_count = 0;
	sema.max_count = 1;
	s_ps2AudioFeedSema = CreateSema(&sema);
	if (s_ps2AudioFeedSema < 0)
	{
		printf("[CTR PS2] audio feeder: CreateSema failed\n");
		return;
	}

	// El hilo de entrega debe poder adelantarse al principal.
	ChangeThreadPriority(GetThreadId(), PS2_AUDIO_MAIN_PRIORITY);

	ee_thread_t thread;
	memset(&thread, 0, sizeof(thread));
	thread.func = (void *)Ps2AudioOut_FeederThread;
	thread.stack = s_ps2AudioFeederStack;
	thread.stack_size = sizeof(s_ps2AudioFeederStack);
	thread.gp_reg = &_gp;
	thread.initial_priority = PS2_AUDIO_FEEDER_PRIORITY;
	s_ps2AudioFeederThread = CreateThread(&thread);
	if (s_ps2AudioFeederThread < 0)
	{
		printf("[CTR PS2] audio feeder: CreateThread failed\n");
		return;
	}
	StartThread(s_ps2AudioFeederThread, NULL);
	printf("[CTR PS2] audio feeder thread started\n");
}

// Audio generado hasta este VBlank de hardware.
global_variable u32 s_ps2AudioGenVBlank = 0;
global_variable int s_ps2AudioGenStarted = 0;
// Colchon inicial: se empieza 4 VBlanks "por detras" (~67 ms en audsrv).
#define PS2_AUDIO_START_LEAD_VBLANKS 4
// Si se va mas atrasado que esto (parada muy larga), se salta lo antiguo.
#define PS2_AUDIO_MAX_BEHIND_VBLANKS 24

// --- Limite de CPU para el sonido ---------------------------------------------
// Emular la SPU de PS1 es caro. Si generar el sonido llega a costar mas que el
// tiempo real (consola real, escenas cargadas), el juego se quedaria generando
// audio sin parar y no avanzaria nunca (parece colgado, p. ej. en la bandera).
// Por eso el sonido tiene un "presupuesto": como mucho este % del tiempo real.
// Si se lo gasta, se salta el audio que falte (se oye un corte) y el juego sigue.
#define PS2_AUDIO_CPU_BUDGET_PERCENT 70u
// Credito maximo acumulado (en VBlanks de CPU): permite rafagas cortas.
#define PS2_AUDIO_CPU_BUDGET_MAX_VBLANKS 8u
global_variable long long s_ps2AudioBudgetCredit = 0; // en ciclos de la EE
global_variable u32 s_ps2AudioBudgetLast = 0;
global_variable int s_ps2AudioBudgetStarted = 0;
global_variable u32 s_ps2AudioBudgetSkippedVBlanks = 0; // para el informe

// Genera el audio de los VBlanks de hardware que han pasado y lo entrega.
// El audio avanza con el reloj real, como la SPU y el lector de una PS1, aunque
// el juego este cargando o vaya lento.
internal void Ps2AudioOut_Service(void)
{
	SDL_AudioStream *stream = &s_ps2SdlAudioStream;

	if (!s_ps2AudioOpen || !stream->open || (stream->callback == NULL) || s_ps2AudioInPump)
	{
		return;
	}

	const u32 hw = Ps2_GetHwVBlankCount();
	if (!s_ps2AudioGenStarted)
	{
		s_ps2AudioGenStarted = 1;
		s_ps2AudioGenVBlank = hw - PS2_AUDIO_START_LEAD_VBLANKS;
	}
	if (hw - s_ps2AudioGenVBlank > PS2_AUDIO_MAX_BEHIND_VBLANKS)
	{
		s_ps2AudioGenVBlank = hw - PS2_AUDIO_MAX_BEHIND_VBLANKS;
	}

	// Generar audio puede leer del disco (musica XA), y la lectura del disco
	// llama a Ps2AudioOut_PumpIdle(): no volver a entrar.
	const u32 profStart = Ps2Prof_Now();
	const unsigned long long profCdStart = g_ps2ProfCdCycles;
	s_ps2AudioInPump = 1;

	// XA en el chip SPU2: leer por adelantado y subir los bloques (native_audio.c).
	Ps2XaHw_PumpNoLock();

	// Presupuesto: se gana PS2_AUDIO_CPU_BUDGET_PERCENT % del tiempo pasado.
	{
		const u32 nowBudget = Ps2Prof_Now();
		if (!s_ps2AudioBudgetStarted)
		{
			s_ps2AudioBudgetStarted = 1;
			s_ps2AudioBudgetCredit = (long long)(PS2_EE_CYCLES_PER_VBLANK * PS2_AUDIO_CPU_BUDGET_MAX_VBLANKS);
		}
		else
		{
			const u32 passed = nowBudget - s_ps2AudioBudgetLast;
			s_ps2AudioBudgetCredit += (long long)(((unsigned long long)passed * PS2_AUDIO_CPU_BUDGET_PERCENT) / 100ull);
			const long long creditMax = (long long)(PS2_EE_CYCLES_PER_VBLANK * PS2_AUDIO_CPU_BUDGET_MAX_VBLANKS);
			if (s_ps2AudioBudgetCredit > creditMax)
			{
				s_ps2AudioBudgetCredit = creditMax;
			}
		}
		s_ps2AudioBudgetLast = nowBudget;
	}

	while (s_ps2AudioGenVBlank != hw)
	{
		if (s_ps2AudioBudgetCredit <= 0)
		{
			// Sin presupuesto: se salta el audio que falta hasta "ahora".
			s_ps2AudioBudgetSkippedVBlanks += hw - s_ps2AudioGenVBlank;
			s_ps2AudioGenVBlank = hw;
			break;
		}
		const u32 vblankStart = Ps2Prof_Now();
		const unsigned long long vblankCdStart = g_ps2ProfCdCycles;
		s_ps2AudioGenVBlank++;
		s_ps2AudioRateAcc += PS2_AUDIO_RATE_NUM;
		const int frames = (int)(s_ps2AudioRateAcc / PS2_AUDIO_RATE_DEN);
		s_ps2AudioRateAcc %= PS2_AUDIO_RATE_DEN;
		if (frames > 0)
		{
			const int bytes = frames * PS2_AUDIO_FRAME_BYTES;
			stream->callback(stream->userdata, stream, bytes, bytes);
		}
		// Coste de este VBlank de audio (sin la espera al disco).
		const unsigned long long vblankCost = (unsigned long long)(u32)(Ps2Prof_Now() - vblankStart);
		const unsigned long long vblankCd = g_ps2ProfCdCycles - vblankCdStart;
		s_ps2AudioBudgetCredit -= (long long)((vblankCost > vblankCd) ? (vblankCost - vblankCd) : 0ull);
	}
	s_ps2AudioInPump = 0;
	s_ps2AudioLastServiceVBlank = hw;
	// Tiempo de generar, sin contar lo que se espero al disco (eso va a "cd").
	g_ps2ProfAudioCycles += (unsigned long long)(u32)(Ps2Prof_Now() - profStart) - (g_ps2ProfCdCycles - profCdStart);

	// Aqui el audio generado llega justo hasta "ahora", asi que lo que hay en
	// la cola (FIFO + audsrv) es el retraso del sonido. Si es demasiado, pedir
	// al hilo de entrega que tire lo mas antiguo.
	if (s_ps2AudioTrimRequested == s_ps2AudioTrimDone)
	{
		const u32 pipeline = (s_ps2AudioFifoWrite - s_ps2AudioFifoRead) + (s_ps2AudioQueuedEstimate / PS2_AUDIO_FRAME_BYTES);
		if (pipeline > PS2_AUDIO_MAX_LATENCY_FRAMES)
		{
			s_ps2AudioTrimFrames = pipeline - PS2_AUDIO_TARGET_LATENCY_FRAMES;
			s_ps2AudioTrimRequested = s_ps2AudioTrimDone + 1;
		}
	}
}

// Llamado mientras se espera al lector de disco (ver ps2_cd.c): genera el audio
// pendiente (como mucho una vez por VBlank) para que el FIFO no se vacie.
void Ps2AudioOut_PumpIdle(void)
{
	if (s_ps2AudioInPump || (Ps2_GetHwVBlankCount() == s_ps2AudioLastServiceVBlank))
	{
		return;
	}
	Ps2AudioOut_Service();
}

global_variable u32 s_ps2AudioReportVBlank = 0;
global_variable int s_ps2AudioReportStarted = 0;

internal u32 Ps2Prof_Percent(unsigned long long cycles, unsigned long long total)
{
	if (total == 0)
	{
		return 0;
	}
	unsigned long long pct = (cycles * 100ull + total / 2ull) / total;
	return (pct > 999ull) ? 999u : (u32)pct;
}

// Cada ~5 s (segun el reloj real): estado del sonido y reparto de la CPU.
internal void Ps2AudioOut_Report(void)
{
	const u32 hw = Ps2_GetHwVBlankCount();
	if (!s_ps2AudioReportStarted)
	{
		s_ps2AudioReportStarted = 1;
		s_ps2AudioReportVBlank = hw;
		g_ps2ProfAudioCycles = 0;
		g_ps2ProfCdCycles = 0;
		g_ps2ProfIdleCycles = 0;
		g_ps2ProfGsCycles = 0;
		g_ps2ProfSpu2Cycles = 0;
		return;
	}

	const u32 elapsed = hw - s_ps2AudioReportVBlank;
	if (elapsed < PS2_AUDIO_REPORT_VBLANKS)
	{
		return;
	}
	s_ps2AudioReportVBlank = hw;

	printf("[CTR PS2] audio: sent %u KB | fifo %u frames | silence %u | dropped %u | trimmed %u\n", (unsigned)(s_ps2AudioBytesSent / 1024u),
	       (unsigned)(s_ps2AudioFifoWrite - s_ps2AudioFifoRead), (unsigned)s_ps2AudioSilenceFrames, (unsigned)s_ps2AudioDroppedFrames,
	       (unsigned)s_ps2AudioTrimmedFrames);
	s_ps2AudioSilenceFrames = 0;
	s_ps2AudioDroppedFrames = 0;
	s_ps2AudioTrimmedFrames = 0;

	const unsigned long long total = (unsigned long long)elapsed * PS2_EE_CYCLES_PER_VBLANK;

	// Desglose del sonido (native_audio.c) y audio saltado por el limite.
	{
		const unsigned long long mixFrames = (g_ps2ProfMixFrames > 0) ? g_ps2ProfMixFrames : 1ull;
		const unsigned long long voicesX10 = (g_ps2ProfVoiceSamples * 10ull) / mixFrames;
		printf("[CTR PS2] audio cpu: voces %u%% | xa %u%% (+ disco xa %u%%) | eco %u%% | voces sonando %u.%u | saltado %u vblanks\n",
		       Ps2Prof_Percent(g_ps2ProfVoiceCycles, total), Ps2Prof_Percent(g_ps2ProfXaCycles, total), Ps2Prof_Percent(g_ps2ProfXaCdCycles, total),
		       Ps2Prof_Percent(g_ps2ProfReverbCycles, total),
		       (unsigned)(voicesX10 / 10ull), (unsigned)(voicesX10 % 10ull), (unsigned)s_ps2AudioBudgetSkippedVBlanks);
		g_ps2ProfVoiceCycles = 0;
		g_ps2ProfXaCycles = 0;
		g_ps2ProfXaCdCycles = 0;
		g_ps2ProfReverbCycles = 0;
		g_ps2ProfVoiceSamples = 0;
		g_ps2ProfMixFrames = 0;
		s_ps2AudioBudgetSkippedVBlanks = 0;
	}
	const unsigned long long known = g_ps2ProfAudioCycles + g_ps2ProfCdCycles + g_ps2ProfIdleCycles + g_ps2ProfGsCycles + g_ps2ProfSpu2Cycles;
	const unsigned long long rest = (known < total) ? (total - known) : 0ull;
	printf("[CTR PS2] cpu: audio %u%% | spu2 %u%% | cd %u%% | gs %u%% | libre %u%% | juego %u%%\n", Ps2Prof_Percent(g_ps2ProfAudioCycles, total),
	       Ps2Prof_Percent(g_ps2ProfSpu2Cycles, total), Ps2Prof_Percent(g_ps2ProfCdCycles, total), Ps2Prof_Percent(g_ps2ProfGsCycles, total),
	       Ps2Prof_Percent(g_ps2ProfIdleCycles, total), Ps2Prof_Percent(rest, total));
	Ps2Spu2_Report(total); // tambien pone a 0 g_ps2ProfSpu2Cycles
	Ps2Xa_Report();        // native_audio.c
	g_ps2ProfAudioCycles = 0;
	g_ps2ProfCdCycles = 0;
	g_ps2ProfIdleCycles = 0;
	g_ps2ProfGsCycles = 0;
}

// Llamado en cada VBlank que recibe el juego (ps2_platform.c).
void Ps2AudioOut_Pump(void)
{
	if (Ps2_GetHwVBlankCount() == s_ps2AudioLastServiceVBlank)
	{
		return; // ya atendido en este VBlank
	}
	Ps2AudioOut_Service();
	Ps2AudioOut_Report();
}
