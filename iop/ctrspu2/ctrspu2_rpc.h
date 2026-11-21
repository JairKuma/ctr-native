// Protocolo entre el juego (EE) y ctrspu2.irx (IOP). Lo incluyen los dos lados.
#ifndef CTRSPU2_RPC_H
#define CTRSPU2_RPC_H

#define CTRSPU2_RPC_ID 0x0C7A5B02

#define CTRSPU2_CMD_PING   0 // responde CTRSPU2_MAGIC
#define CTRSPU2_CMD_BATCH  1 // lista de escrituras de registros (sceSdProcBatch)
#define CTRSPU2_CMD_UPLOAD 2 // copia datos a la RAM de la SPU2 (sceSdVoiceTrans)
#define CTRSPU2_CMD_EFFECT 3 // cambia el eco/reverb del core 0 (sceSdSetEffectAttr)

#define CTRSPU2_MAGIC 0x53505532 // "SPU2"

// Escrituras por envio (cada una ocupa 8 bytes).
#define CTRSPU2_MAX_BATCH 1024
// Bytes por subida a la RAM de la SPU2 (multiplo de 64).
#define CTRSPU2_UPLOAD_CHUNK 16384
// Cabecera delante de los datos (BATCH: n.o de escrituras; UPLOAD: direccion y tamano).
#define CTRSPU2_HEADER_BYTES 64
// Buffer de recepcion del IOP.
#define CTRSPU2_RPC_BUFFER_BYTES (CTRSPU2_HEADER_BYTES + CTRSPU2_UPLOAD_CHUNK)

typedef struct
{
	unsigned int count; // BATCH: escrituras; UPLOAD: bytes
	unsigned int spuAddr; // UPLOAD: direccion en bytes de la RAM de la SPU2
	unsigned int pad[14];
} CtrSpu2Header;

typedef struct
{
	int mode; // SD_EFFECT_MODE_* (0 = sin eco)
	int depthL;
	int depthR;
	int delay;
	int feedback;
	int enable; // 1 = eco activado
	int pad[10];
} CtrSpu2Effect;

#endif
