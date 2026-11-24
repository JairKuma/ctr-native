// PS2 port - sonido por el chip SPU2 de la PS2 (en vez de emularlo).
// Lo implementa platform/ps2/ps2_spu2.c (objeto aparte). native_audio.c llama
// a estas funciones desde las funciones Spu* del juego.
#ifndef PS2_SPU2_H
#define PS2_SPU2_H

// Que ha cambiado en una voz (Ps2Spu2_VoiceUpdate).
#define PS2_SPU2_VOICE_VOLUME   0x01
#define PS2_SPU2_VOICE_PITCH    0x02
#define PS2_SPU2_VOICE_START    0x04
#define PS2_SPU2_VOICE_LOOP     0x08
#define PS2_SPU2_VOICE_ADSR     0x10

// 1 = las voces y el eco suenan en la SPU2 (native_audio.c no los emula).
extern int g_ps2Spu2Active;

// Carga ctrspu2.irx ya hecha (main_ps2.c): conecta y prepara la SPU2.
// Devuelve 1 si va bien; si no, el sonido sigue emulado por software.
int Ps2Spu2_Init(void);

// La RAM de sonido de PS1 (512 KB) ha cambiado en [offset, offset+size).
// "memory" es la copia completa que guarda native_audio.c.
void Ps2Spu2_Upload(const unsigned char *memory, unsigned int memorySize, unsigned int offset, unsigned int size);

void Ps2Spu2_VoiceUpdate(int voice, unsigned int what, int volumeLeft, int volumeRight, unsigned int pitch, unsigned int startAddr, unsigned int loopAddr,
                         unsigned int adsr1, unsigned int adsr2);
void Ps2Spu2_KeyOn(unsigned int voiceBits);
void Ps2Spu2_KeyOff(unsigned int voiceBits);
void Ps2Spu2_SetMasterVolume(int left, int right);
void Ps2Spu2_SetReverbEnabled(int enabled);
void Ps2Spu2_SetReverbMode(int mode, int delay, int feedback);
void Ps2Spu2_SetReverbDepth(int left, int right);
void Ps2Spu2_SetReverbVoices(unsigned int voiceBits);

// Cierra la lista de este VBlank del juego y la deja en la cola de envio.
void Ps2Spu2_Flush(void);

// Desde la interrupcion de VBlank: despierta al hilo de envio.
void Ps2Spu2_OnVBlankIsr(void);

// --- Musica/voces XA en el chip (voces 0 y 1 del core 1) ----------------------
// La XA de PS1 se reordena (sin descomprimir) en bloques ADPCM de la SPU2 y se
// escribe en dos anillos (izquierdo/derecho) de la RAM de la SPU2.
#define PS2_SPU2_XA_RING_BLOCKS 5760u // bloques de 16 bytes (28 muestras) por canal
#define PS2_SPU2_XA_RING_BYTES  (PS2_SPU2_XA_RING_BLOCKS * 16u)
#define PS2_SPU2_XA_BASE_LEFT   0x0A0000u
#define PS2_SPU2_XA_BASE_RIGHT  0x0C0000u

// Sube datos a una direccion de la RAM de la SPU2 (bytes, multiplo de 64).
void Ps2Spu2_UploadRaw(unsigned int spuAddr, const unsigned char *data, unsigned int size);
// Estas mandan al momento (no esperan al VBlank del juego).
void Ps2Spu2_XaStart(unsigned int pitch, int volumeLeft, int volumeRight);
void Ps2Spu2_XaPitch(unsigned int pitch);
void Ps2Spu2_XaVolume(int volumeLeft, int volumeRight);
void Ps2Spu2_XaStop(void);

// Informe para el log (cada ~5 s). totalCycles = ciclos de la EE del periodo.
void Ps2Spu2_Report(unsigned long long totalCycles);

#endif
