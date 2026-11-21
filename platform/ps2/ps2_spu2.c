// PS2 port - sonido de CTR en el chip SPU2 de la PS2.
//
// La PS1 tiene una SPU con 24 voces que tocan muestras ADPCM. La SPU2 de la
// PS2 tiene DOS cores iguales a esa SPU (mismo formato de muestras, mismos
// registros de volumen, tono, ADSR y eco). Asi que, en vez de emular las 24
// voces por software (lo que mas CPU gastaba), se mandan al core 0 de la SPU2:
//
//   - La RAM de sonido de PS1 (512 KB) se copia a la RAM de la SPU2 a partir
//     de PS2_SPU2_RAM_BASE (las direcciones de PS1 se desplazan esa cantidad).
//   - Cada cambio de una voz (volumen, tono, ADSR, direcciones, key on/off) se
//     apunta en una lista, y una vez por VBlank la lista entera se manda al
//     IOP (ctrspu2.irx) en un solo envio, que la aplica con la libsd.
//   - El eco (reverb) de PS1 es el mismo algoritmo que el de la SPU2.
//
// La musica XA (audio grabado del disco) sigue decodificandose por software y
// sale por audsrv (core 1). La salida del core 0 entra en el core 1 y de ahi
// sale por la tele, mezclada con la XA.
//
// Diferencias con la PS1: la SPU2 funciona a 48000 Hz (la PS1 a 44100), por
// eso el tono se corrige (x 44100/48000). Las envolventes ADSR y el eco van
// ~9% mas rapidos (no se corrige: casi no se nota).
//
// Va en un objeto aparte (como ps2_cd.c) para que las cabeceras de la libsd
// no choquen con las del juego.

#include <kernel.h>
#include <sifrpc.h>
#include <libsd-common.h>

#include <stdio.h>
#include <string.h>

#include "ps2_spu2.h"
#include "../../iop/ctrspu2/ctrspu2_rpc.h"

// Donde va la RAM de sonido de PS1 dentro de la RAM de la SPU2 (2 MB):
//   0x000000-0x004FFF  reservado por la SPU2 (buffers de entrada/salida)
//   0x005000-0x0FFFFF  libre (audsrv solo lo usaria para ADPCM, que no se usa)
//   0x100000-0x17FFFF  RAM de sonido de PS1 (512 KB)       <- aqui
//   0x1C7FC0-0x1DFFFF  zona de trabajo del eco del core 0 (como mucho)
//   0x1E0000-0x1FFFFF  zona de trabajo del eco del core 1 (no se usa)
#define PS2_SPU2_RAM_BASE 0x100000u
#define PS2_SPU2_CORE0_EEA 0x1DFFFFu

#define PS2_SPU2_VOICES 24
#define PS2_SPU2_ALL_VOICES 0xFFFFFFu
#define PS2_SPU2_CORE0 0
#define PS2_SPU2_CORE1 1

// Volumen de la entrada PCM del core 1 (la musica XA que llega por audsrv).
// audsrv lo deja a 0x3FFF (la mitad); 0x7FFF es volumen completo, como en PS1.
#define PS2_SPU2_PCM_VOLUME 0x7FFF

// Sitio en la lista para lo que se anade al mandar (key off/on y volumenes).
#define PS2_SPU2_RESERVED_ENTRIES 8
#define PS2_SPU2_QUEUE_MAX (CTRSPU2_MAX_BATCH - PS2_SPU2_RESERVED_ENTRIES)

int g_ps2Spu2Active = 0;
unsigned long long g_ps2ProfSpu2Cycles = 0; // ciclos de la EE en mandar a la SPU2

static SifRpcClientData_t s_ps2Spu2Client __attribute__((aligned(64)));
static unsigned char s_ps2Spu2Send[CTRSPU2_RPC_BUFFER_BYTES] __attribute__((aligned(64)));
static int s_ps2Spu2Recv[16] __attribute__((aligned(64)));

static sceSdBatch s_ps2Spu2Queue[PS2_SPU2_QUEUE_MAX];
static int s_ps2Spu2QueueCount = 0;
static unsigned int s_ps2Spu2KeyOn = 0;       // key on pendientes
static unsigned int s_ps2Spu2KeyOff = 0;      // key off pendientes
static unsigned int s_ps2Spu2KeyOffLater = 0; // key off para el siguiente envio

static int s_ps2Spu2MasterLeft = 0x3FFF;
static int s_ps2Spu2MasterRight = 0x3FFF;
static int s_ps2Spu2ReverbEnabled = 0;
static int s_ps2Spu2ReverbMode = -1;
static int s_ps2Spu2ReverbDelay = 0;
static int s_ps2Spu2ReverbFeedback = 0;
static int s_ps2Spu2ReverbDepthLeft = 0;
static int s_ps2Spu2ReverbDepthRight = 0;

// Para el informe.
static unsigned int s_ps2Spu2StatFlushes = 0;
static unsigned int s_ps2Spu2StatEntries = 0;
static unsigned int s_ps2Spu2StatUploadBytes = 0;
static unsigned int s_ps2Spu2StatKeyOns = 0;
static unsigned int s_ps2Spu2StatErrors = 0;
static unsigned int s_ps2Spu2TotalUploadKB = 0;

static inline unsigned int Ps2Spu2_Now(void)
{
	unsigned int count;
	__asm__ volatile("mfc0 %0, $9" : "=r"(count));
	return count;
}

static int Ps2Spu2_Call(int command, int sendBytes)
{
	// Tamano multiplo de 16 (la transferencia al IOP va en bloques de 16 bytes).
	sendBytes = (sendBytes + 15) & ~15;
	s_ps2Spu2Recv[0] = -999;
	const int result = SifCallRpc(&s_ps2Spu2Client, command, 0, s_ps2Spu2Send, sendBytes, s_ps2Spu2Recv, 16, NULL, NULL);
	if (result < 0)
	{
		s_ps2Spu2StatErrors++;
		return -1000 + result;
	}
	return s_ps2Spu2Recv[0];
}

static unsigned int Ps2Spu2_PopCount(unsigned int bits)
{
	unsigned int count = 0;
	while (bits != 0)
	{
		bits &= bits - 1;
		count++;
	}
	return count;
}

static void Ps2Spu2_Push(unsigned short func, unsigned short entry, unsigned int value)
{
	if (s_ps2Spu2QueueCount >= PS2_SPU2_QUEUE_MAX)
	{
		Ps2Spu2_Flush(); // lista llena: mandar ya
	}
	sceSdBatch *cmd = &s_ps2Spu2Queue[s_ps2Spu2QueueCount++];
	cmd->func = func;
	cmd->entry = entry;
	cmd->value = value;
}

static unsigned short Ps2Spu2_VoiceEntry(int voice, unsigned short reg)
{
	return (unsigned short)(SD_VOICE(PS2_SPU2_CORE0, voice) | reg);
}

// Volumen fijo de PS1/SPU2: 0..0x3FFF (0x4000 o mas cambiaria de modo/fase).
static unsigned int Ps2Spu2_ClampVolume(int volume)
{
	if (volume < 0)
	{
		return 0;
	}
	if (volume > 0x3FFF)
	{
		return 0x3FFF;
	}
	return (unsigned int)volume;
}

// Tono: en PS1 0x1000 = 44100 Hz; en la SPU2 0x1000 = 48000 Hz.
static unsigned int Ps2Spu2_ConvertPitch(unsigned int pitch)
{
	if (pitch > 0x4000)
	{
		pitch = 0x4000; // la PS1 no pasa de 0x4000
	}
	unsigned int hw = (pitch * 147u + 80u) / 160u; // x 44100 / 48000
	if (hw > 0x3FFF)
	{
		hw = 0x3FFF;
	}
	return hw;
}

static unsigned int Ps2Spu2_ConvertAddr(unsigned int addr)
{
	return PS2_SPU2_RAM_BASE + (addr & 0x7FFFFu);
}

int Ps2Spu2_Init(void)
{
	int i;

	memset(&s_ps2Spu2Client, 0, sizeof(s_ps2Spu2Client));
	for (i = 0; i < 100000; i++)
	{
		if (SifBindRpc(&s_ps2Spu2Client, CTRSPU2_RPC_ID, 0) < 0)
		{
			printf("[CTR PS2] spu2: SifBindRpc failed\n");
			return 0;
		}
		if (s_ps2Spu2Client.server != NULL)
		{
			break;
		}
		nopdelay();
	}
	if (s_ps2Spu2Client.server == NULL)
	{
		printf("[CTR PS2] spu2: ctrspu2.irx no responde\n");
		return 0;
	}

	memset(s_ps2Spu2Send, 0, CTRSPU2_HEADER_BYTES);
	const int ping = Ps2Spu2_Call(CTRSPU2_CMD_PING, CTRSPU2_HEADER_BYTES);
	if (ping != CTRSPU2_MAGIC)
	{
		printf("[CTR PS2] spu2: ping incorrecto (%d)\n", ping);
		return 0;
	}

	// Estado inicial del core 0: todo callado, sin eco, voces a la salida.
	s_ps2Spu2QueueCount = 0;
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_KOFF, PS2_SPU2_ALL_VOICES);
	Ps2Spu2_Push(SD_BATCH_SETCORE, PS2_SPU2_CORE0 | SD_CORE_EFFECT_ENABLE, 0);
	Ps2Spu2_Push(SD_BATCH_SETPARAM, PS2_SPU2_CORE0 | SD_PARAM_EVOLL, 0);
	Ps2Spu2_Push(SD_BATCH_SETPARAM, PS2_SPU2_CORE0 | SD_PARAM_EVOLR, 0);
	Ps2Spu2_Push(SD_BATCH_SETADDR, PS2_SPU2_CORE0 | SD_ADDR_EEA, PS2_SPU2_CORE0_EEA);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_PMON, 0);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_NON, 0);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_VMIXL, PS2_SPU2_ALL_VOICES);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_VMIXR, PS2_SPU2_ALL_VOICES);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_VMIXEL, 0);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_VMIXER, 0);
	for (i = 0; i < PS2_SPU2_VOICES; i++)
	{
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(i, SD_VPARAM_VOLL), 0);
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(i, SD_VPARAM_VOLR), 0);
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(i, SD_VPARAM_PITCH), 0);
		Ps2Spu2_Push(SD_BATCH_SETADDR, Ps2Spu2_VoiceEntry(i, SD_VADDR_SSA), PS2_SPU2_RAM_BASE);
	}
	// La salida del core 0 entra en el core 1 por su "entrada externa".
	Ps2Spu2_Push(SD_BATCH_SETPARAM, PS2_SPU2_CORE1 | SD_PARAM_AVOLL, 0x7FFF);
	Ps2Spu2_Push(SD_BATCH_SETPARAM, PS2_SPU2_CORE1 | SD_PARAM_AVOLR, 0x7FFF);

	g_ps2Spu2Active = 1; // a partir de aqui Flush manda de verdad
	Ps2Spu2_Flush();
	if (s_ps2Spu2StatErrors != 0)
	{
		g_ps2Spu2Active = 0;
		printf("[CTR PS2] spu2: error al preparar la SPU2: se usa el sonido por software\n");
		return 0;
	}

	printf("[CTR PS2] spu2: voces y eco en el chip SPU2 (RAM de PS1 en 0x%06x)\n", PS2_SPU2_RAM_BASE);
	return 1;
}

void Ps2Spu2_Flush(void)
{
	if (!g_ps2Spu2Active)
	{
		return;
	}

	const unsigned int t0 = Ps2Spu2_Now();
	CtrSpu2Header *header = (CtrSpu2Header *)s_ps2Spu2Send;
	sceSdBatch *out = (sceSdBatch *)(s_ps2Spu2Send + CTRSPU2_HEADER_BYTES);
	int n = 0;

	const unsigned int keyOff = s_ps2Spu2KeyOff;
	const unsigned int keyOn = s_ps2Spu2KeyOn;

	// 1. Key off (antes que todo, asi hay un pequeno margen hasta los key on).
	if (keyOff != 0)
	{
		out[n].func = SD_BATCH_SETSWITCH;
		out[n].entry = PS2_SPU2_CORE0 | SD_SWITCH_KOFF;
		out[n].value = keyOff;
		n++;
	}
	// 2. Cambios de las voces, en el mismo orden en que los hizo el juego.
	memcpy(&out[n], s_ps2Spu2Queue, (size_t)s_ps2Spu2QueueCount * sizeof(sceSdBatch));
	n += s_ps2Spu2QueueCount;
	// 3. Volumenes generales. Se repiten en cada envio porque audsrv los cambia
	//    al empezar a sonar (pone el core 0 a 0 y su entrada PCM a la mitad).
	out[n].func = SD_BATCH_SETPARAM;
	out[n].entry = PS2_SPU2_CORE0 | SD_PARAM_MVOLL;
	out[n].value = Ps2Spu2_ClampVolume(s_ps2Spu2MasterLeft);
	n++;
	out[n].func = SD_BATCH_SETPARAM;
	out[n].entry = PS2_SPU2_CORE0 | SD_PARAM_MVOLR;
	out[n].value = Ps2Spu2_ClampVolume(s_ps2Spu2MasterRight);
	n++;
	out[n].func = SD_BATCH_SETPARAM;
	out[n].entry = PS2_SPU2_CORE1 | SD_PARAM_BVOLL;
	out[n].value = PS2_SPU2_PCM_VOLUME;
	n++;
	out[n].func = SD_BATCH_SETPARAM;
	out[n].entry = PS2_SPU2_CORE1 | SD_PARAM_BVOLR;
	out[n].value = PS2_SPU2_PCM_VOLUME;
	n++;
	// 4. Key on (al final: la direccion, el tono y la ADSR ya estan puestos).
	if (keyOn != 0)
	{
		out[n].func = SD_BATCH_SETSWITCH;
		out[n].entry = PS2_SPU2_CORE0 | SD_SWITCH_KON;
		out[n].value = keyOn;
		n++;
	}

	memset(header, 0, sizeof(*header));
	header->count = (unsigned int)n;
	Ps2Spu2_Call(CTRSPU2_CMD_BATCH, CTRSPU2_HEADER_BYTES + n * (int)sizeof(sceSdBatch));

	s_ps2Spu2StatFlushes++;
	s_ps2Spu2StatEntries += (unsigned int)n;
	s_ps2Spu2StatKeyOns += Ps2Spu2_PopCount(keyOn);
	s_ps2Spu2QueueCount = 0;
	s_ps2Spu2KeyOn = 0;
	s_ps2Spu2KeyOff = s_ps2Spu2KeyOffLater;
	s_ps2Spu2KeyOffLater = 0;

	g_ps2ProfSpu2Cycles += (unsigned int)(Ps2Spu2_Now() - t0);
}

void Ps2Spu2_Upload(const unsigned char *memory, unsigned int memorySize, unsigned int offset, unsigned int size)
{
	if (!g_ps2Spu2Active || (memory == NULL) || (size == 0) || (offset >= memorySize))
	{
		return;
	}

	// Lo pendiente va antes (el orden importa).
	Ps2Spu2_Flush();

	const unsigned int t0 = Ps2Spu2_Now();

	// La SPU2 copia en bloques de 64 bytes: se amplia la zona a multiplos de 64
	// y se manda desde la copia completa, asi nunca se pisa nada con basura.
	unsigned int start = offset & ~63u;
	unsigned int end = offset + size;
	if (end > memorySize)
	{
		end = memorySize;
	}
	end = (end + 63u) & ~63u;
	if (end > memorySize)
	{
		end = memorySize & ~63u;
	}

	CtrSpu2Header *header = (CtrSpu2Header *)s_ps2Spu2Send;
	unsigned int pos = start;
	while (pos < end)
	{
		unsigned int chunk = end - pos;
		if (chunk > CTRSPU2_UPLOAD_CHUNK)
		{
			chunk = CTRSPU2_UPLOAD_CHUNK;
		}
		memset(header, 0, sizeof(*header));
		header->count = chunk;
		header->spuAddr = PS2_SPU2_RAM_BASE + pos;
		memcpy(s_ps2Spu2Send + CTRSPU2_HEADER_BYTES, memory + pos, chunk);
		const int result = Ps2Spu2_Call(CTRSPU2_CMD_UPLOAD, CTRSPU2_HEADER_BYTES + (int)chunk);
		if (result != (int)chunk)
		{
			s_ps2Spu2StatErrors++;
			printf("[CTR PS2] spu2: fallo al subir 0x%x bytes a 0x%x (%d)\n", chunk, header->spuAddr, result);
		}
		s_ps2Spu2StatUploadBytes += chunk;
		pos += chunk;
	}

	g_ps2ProfSpu2Cycles += (unsigned int)(Ps2Spu2_Now() - t0);
}

void Ps2Spu2_VoiceUpdate(int voice, unsigned int what, int volumeLeft, int volumeRight, unsigned int pitch, unsigned int startAddr, unsigned int loopAddr,
                         unsigned int adsr1, unsigned int adsr2)
{
	if (!g_ps2Spu2Active || (voice < 0) || (voice >= PS2_SPU2_VOICES))
	{
		return;
	}

	if (what & PS2_SPU2_VOICE_VOLUME)
	{
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(voice, SD_VPARAM_VOLL), Ps2Spu2_ClampVolume(volumeLeft));
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(voice, SD_VPARAM_VOLR), Ps2Spu2_ClampVolume(volumeRight));
	}
	if (what & PS2_SPU2_VOICE_PITCH)
	{
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(voice, SD_VPARAM_PITCH), Ps2Spu2_ConvertPitch(pitch));
	}
	if (what & PS2_SPU2_VOICE_START)
	{
		Ps2Spu2_Push(SD_BATCH_SETADDR, Ps2Spu2_VoiceEntry(voice, SD_VADDR_SSA), Ps2Spu2_ConvertAddr(startAddr));
	}
	if (what & PS2_SPU2_VOICE_LOOP)
	{
		Ps2Spu2_Push(SD_BATCH_SETADDR, Ps2Spu2_VoiceEntry(voice, SD_VADDR_LSAX), Ps2Spu2_ConvertAddr(loopAddr));
	}
	if (what & PS2_SPU2_VOICE_ADSR)
	{
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(voice, SD_VPARAM_ADSR1), adsr1 & 0xFFFFu);
		Ps2Spu2_Push(SD_BATCH_SETPARAM, Ps2Spu2_VoiceEntry(voice, SD_VPARAM_ADSR2), adsr2 & 0xFFFFu);
	}
}

void Ps2Spu2_KeyOn(unsigned int voiceBits)
{
	voiceBits &= PS2_SPU2_ALL_VOICES;
	s_ps2Spu2KeyOn |= voiceBits;
	// Un key on despues de un key off en el mismo VBlank: vale solo el key on.
	s_ps2Spu2KeyOff &= ~voiceBits;
	s_ps2Spu2KeyOffLater &= ~voiceBits;
}

void Ps2Spu2_KeyOff(unsigned int voiceBits)
{
	voiceBits &= PS2_SPU2_ALL_VOICES;
	// Si la voz acaba de recibir key on en este VBlank, el key off se manda en
	// el siguiente envio (si van juntos, la SPU2 puede perder uno de los dos).
	s_ps2Spu2KeyOffLater |= voiceBits & s_ps2Spu2KeyOn;
	s_ps2Spu2KeyOff |= voiceBits & ~s_ps2Spu2KeyOn;
}

void Ps2Spu2_SetMasterVolume(int left, int right)
{
	s_ps2Spu2MasterLeft = left;
	s_ps2Spu2MasterRight = right;
}

void Ps2Spu2_SetReverbEnabled(int enabled)
{
	enabled = enabled ? 1 : 0;
	s_ps2Spu2ReverbEnabled = enabled;
	if (!g_ps2Spu2Active)
	{
		return;
	}
	// Solo se enciende si ya hay un modo de eco preparado (zona limpia).
	const int on = enabled && (s_ps2Spu2ReverbMode > 0);
	Ps2Spu2_Push(SD_BATCH_SETCORE, PS2_SPU2_CORE0 | SD_CORE_EFFECT_ENABLE, (unsigned int)on);
}

void Ps2Spu2_SetReverbMode(int mode, int delay, int feedback)
{
	mode &= 0xff;
	if ((mode < 0) || (mode > 9))
	{
		mode = 0;
	}
	if (!g_ps2Spu2Active)
	{
		return;
	}
	if ((mode == s_ps2Spu2ReverbMode) && (delay == s_ps2Spu2ReverbDelay) && (feedback == s_ps2Spu2ReverbFeedback))
	{
		return; // igual que ya esta: no volver a limpiar la zona del eco
	}

	Ps2Spu2_Flush();
	const unsigned int t0 = Ps2Spu2_Now();

	s_ps2Spu2ReverbMode = mode;
	s_ps2Spu2ReverbDelay = delay;
	s_ps2Spu2ReverbFeedback = feedback;

	CtrSpu2Effect *effect = (CtrSpu2Effect *)s_ps2Spu2Send;
	memset(effect, 0, sizeof(*effect));
	effect->mode = mode;
	effect->depthL = s_ps2Spu2ReverbDepthLeft;
	effect->depthR = s_ps2Spu2ReverbDepthRight;
	effect->delay = delay;
	effect->feedback = feedback;
	effect->enable = s_ps2Spu2ReverbEnabled;
	const int result = Ps2Spu2_Call(CTRSPU2_CMD_EFFECT, (int)sizeof(*effect));
	printf("[CTR PS2] spu2: eco modo %d (delay %d, feedback %d) -> %d\n", mode, delay, feedback, result);

	g_ps2ProfSpu2Cycles += (unsigned int)(Ps2Spu2_Now() - t0);
}

void Ps2Spu2_SetReverbDepth(int left, int right)
{
	s_ps2Spu2ReverbDepthLeft = left;
	s_ps2Spu2ReverbDepthRight = right;
	if (!g_ps2Spu2Active)
	{
		return;
	}
	Ps2Spu2_Push(SD_BATCH_SETPARAM, PS2_SPU2_CORE0 | SD_PARAM_EVOLL, (unsigned int)left & 0xFFFFu);
	Ps2Spu2_Push(SD_BATCH_SETPARAM, PS2_SPU2_CORE0 | SD_PARAM_EVOLR, (unsigned int)right & 0xFFFFu);
}

void Ps2Spu2_SetReverbVoices(unsigned int voiceBits)
{
	if (!g_ps2Spu2Active)
	{
		return;
	}
	voiceBits &= PS2_SPU2_ALL_VOICES;
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_VMIXEL, voiceBits);
	Ps2Spu2_Push(SD_BATCH_SETSWITCH, PS2_SPU2_CORE0 | SD_SWITCH_VMIXER, voiceBits);
}

void Ps2Spu2_Report(unsigned long long totalCycles)
{
	if (!g_ps2Spu2Active)
	{
		printf("[CTR PS2] spu2: desactivado (voces emuladas por software)\n");
		return;
	}

	unsigned int pct = 0;
	if (totalCycles > 0)
	{
		pct = (unsigned int)((g_ps2ProfSpu2Cycles * 100ull + totalCycles / 2ull) / totalCycles);
	}
	s_ps2Spu2TotalUploadKB += s_ps2Spu2StatUploadBytes / 1024u;
	printf("[CTR PS2] spu2: envios %u | cambios %u | notas %u | subido %u KB (total %u KB) | eco modo %d %s | errores %u | cpu %u%%\n",
	       s_ps2Spu2StatFlushes, s_ps2Spu2StatEntries, s_ps2Spu2StatKeyOns, s_ps2Spu2StatUploadBytes / 1024u, s_ps2Spu2TotalUploadKB,
	       s_ps2Spu2ReverbMode, s_ps2Spu2ReverbEnabled ? "on" : "off", s_ps2Spu2StatErrors, pct);
	s_ps2Spu2StatFlushes = 0;
	s_ps2Spu2StatEntries = 0;
	s_ps2Spu2StatUploadBytes = 0;
	s_ps2Spu2StatKeyOns = 0;
	s_ps2Spu2StatErrors = 0;
	g_ps2ProfSpu2Cycles = 0;
}
