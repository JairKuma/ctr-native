// ctrspu2.irx - modulo del IOP del port de CTR a PS2.
//
// El juego (en la EE) prepara cada VBlank una lista de escrituras de registros
// de la SPU2 (volumenes, tono, ADSR, direcciones, key on/off...) y la manda
// aqui de una vez por SIF RPC. Este modulo la aplica con la libsd de la BIOS
// (rom0:LIBSD). Tambien sube las muestras de sonido a la RAM de la SPU2 y
// configura el eco.
//
// Solo usa funciones basicas de libsd (las que tiene cualquier BIOS).

#include "irx_imports.h"
#include "ctrspu2_rpc.h"

IRX_ID("ctrspu2", 1, 1);

#define CTRSPU2_DMA_CHANNEL 0 // audsrv usa el canal 1 para su PCM
#define CTRSPU2_THREAD_PRIORITY 40 // audsrv (PCM) va a 39: que no le quite tiempo

static SifRpcDataQueue_t s_queue;
static SifRpcServerData_t s_server;
static unsigned char s_rpcBuffer[CTRSPU2_RPC_BUFFER_BYTES] __attribute__((aligned(64)));
static u32 s_batchReturns[CTRSPU2_MAX_BATCH];
static int s_result[4] __attribute__((aligned(16)));

static int CtrSpu2_Upload(const CtrSpu2Header *header, unsigned char *data)
{
	unsigned int size = header->count;
	int tries;

	if ((size == 0) || (size > CTRSPU2_UPLOAD_CHUNK))
	{
		return -1;
	}

	// Si el canal esta ocupado (otra transferencia), esperar un poco.
	for (tries = 0; tries < 200; tries++)
	{
		if (sceSdVoiceTrans(CTRSPU2_DMA_CHANNEL, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, data, (u32 *)header->spuAddr, size) >= 0)
		{
			break;
		}
		DelayThread(100);
	}
	if (tries >= 200)
	{
		return -2;
	}

	// Esperar a que acabe (sin bloquear a los demas hilos del IOP).
	for (tries = 0; tries < 2000; tries++)
	{
		if (sceSdVoiceTransStatus(CTRSPU2_DMA_CHANNEL, 0) != 0)
		{
			return (int)size;
		}
		DelayThread(50);
	}
	return -3;
}

static int CtrSpu2_Effect(const CtrSpu2Effect *effect)
{
	sceSdEffectAttr attr;

	// Apagar el eco mientras se cambia y se limpia su zona de trabajo.
	sceSdSetCoreAttr(0 /* core 0 */ | SD_CORE_EFFECT_ENABLE, 0);

	memset(&attr, 0, sizeof(attr));
	attr.core = 0;
	attr.mode = (effect->mode & 0xff) | SD_EFFECT_MODE_CLEAR;
	attr.depth_L = (short)effect->depthL;
	attr.depth_R = (short)effect->depthR;
	attr.delay = effect->delay;
	attr.feedback = effect->feedback;
	const int result = sceSdSetEffectAttr(0, &attr);

	if (effect->enable && ((effect->mode & 0xff) != SD_EFFECT_MODE_OFF))
	{
		sceSdSetCoreAttr(0 /* core 0 */ | SD_CORE_EFFECT_ENABLE, 1);
	}
	return result;
}

static void *CtrSpu2_RpcHandler(int fno, void *buffer, int length)
{
	const CtrSpu2Header *header = (const CtrSpu2Header *)buffer;
	unsigned char *payload = (unsigned char *)buffer + CTRSPU2_HEADER_BYTES;
	int result = 0;

	(void)length;

	switch (fno)
	{
	case CTRSPU2_CMD_PING:
		result = CTRSPU2_MAGIC;
		break;
	case CTRSPU2_CMD_BATCH:
		if ((header->count > 0) && (header->count <= CTRSPU2_MAX_BATCH))
		{
			result = sceSdProcBatch((const sceSdBatch *)payload, s_batchReturns, header->count);
		}
		break;
	case CTRSPU2_CMD_UPLOAD:
		result = CtrSpu2_Upload(header, payload);
		break;
	case CTRSPU2_CMD_EFFECT:
		result = CtrSpu2_Effect((const CtrSpu2Effect *)buffer);
		break;
	default:
		result = -1;
		break;
	}

	s_result[0] = result;
	return s_result;
}

static void CtrSpu2_RpcThread(void *arg)
{
	(void)arg;

	sceSifInitRpc(0);
	sceSifSetRpcQueue(&s_queue, GetThreadId());
	sceSifRegisterRpc(&s_server, CTRSPU2_RPC_ID, CtrSpu2_RpcHandler, s_rpcBuffer, NULL, NULL, &s_queue);
	sceSifRpcLoop(&s_queue);
}

int _start(int argc, char *argv[])
{
	iop_thread_t thread;
	int threadId;

	(void)argc;
	(void)argv;

	memset(&thread, 0, sizeof(thread));
	thread.attr = TH_C;
	thread.thread = CtrSpu2_RpcThread;
	thread.stacksize = 0x800;
	thread.priority = CTRSPU2_THREAD_PRIORITY;

	threadId = CreateThread(&thread);
	if (threadId <= 0)
	{
		return MODULE_NO_RESIDENT_END;
	}
	StartThread(threadId, NULL);
	return MODULE_RESIDENT_END;
}
