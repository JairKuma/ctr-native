// PS2 port: sustituto minimo de <SDL3/SDL.h>.
//
// Algunos modulos comunes incluyen SDL:
//  - native_gpu.c y native_perf.c: no lo usan en el build de PS2.
//  - native_audio.c: usa SDL solo para la SALIDA de audio (abrir dispositivo,
//    callback que pide muestras, cerrojo). Aqui se implementa ese subconjunto
//    encima de platform/ps2/ps2_audio_out.c (audsrv -> SPU2), de modo que el
//    emulador de la SPU de PS1 y el decodificador XA de native_audio.c se usan
//    sin cambios.
//
// Esta carpeta solo esta en el include path de Makefile.ps2; el build de PC
// sigue usando el SDL real.

#ifndef CTR_PS2_SDL_SHIM_H
#define CTR_PS2_SDL_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define SDL_memcpy memcpy
#define SDL_memset memset

#define SDLCALL

// --- Tipos de audio -------------------------------------------------------

typedef unsigned int SDL_AudioDeviceID;
typedef unsigned int SDL_AudioFormat;
typedef unsigned int SDL_InitFlags;

#define SDL_AUDIO_S16                     0x8010u
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu
#define SDL_INIT_AUDIO                    0x00000010u

#define SDL_HINT_AUDIO_DRIVER             "SDL_AUDIO_DRIVER"
#define SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "SDL_AUDIO_DEVICE_SAMPLE_FRAMES"

typedef struct SDL_AudioSpec
{
	SDL_AudioFormat format;
	int channels;
	int freq;
} SDL_AudioSpec;

typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (*SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

struct SDL_AudioStream
{
	SDL_AudioSpec spec;
	SDL_AudioStreamCallback callback;
	void *userdata;
	int open;
};

// --- Backend de salida (platform/ps2/ps2_audio_out.c) ----------------------

int Ps2AudioOut_Open(int freq, int channels);
void Ps2AudioOut_Close(void);
int Ps2AudioOut_Write(const void *data, int bytes);
int Ps2AudioOut_QueuedBytes(void);

// El unico stream de audio del juego; ps2_audio_out.c lo usa para llamar al
// callback desde el bucle principal (no hay hilos de audio).
static SDL_AudioStream s_ps2SdlAudioStream;

// --- Funciones SDL usadas por native_audio.c ------------------------------

static inline bool SDL_InitSubSystem(SDL_InitFlags flags)
{
	(void)flags;
	return true;
}

static inline void SDL_QuitSubSystem(SDL_InitFlags flags)
{
	(void)flags;
}

static inline const char *SDL_GetError(void)
{
	return "audsrv not available";
}

static inline const char *SDL_GetHint(const char *name)
{
	(void)name;
	return NULL;
}

static inline bool SDL_SetHint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return true;
}

static inline const char *SDL_GetCurrentAudioDriver(void)
{
	return "audsrv";
}

static inline SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata)
{
	(void)devid;

	if ((spec == NULL) || !Ps2AudioOut_Open(spec->freq, spec->channels))
	{
		return NULL;
	}

	s_ps2SdlAudioStream.spec = *spec;
	s_ps2SdlAudioStream.callback = callback;
	s_ps2SdlAudioStream.userdata = userdata;
	s_ps2SdlAudioStream.open = 1;
	return &s_ps2SdlAudioStream;
}

static inline SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream)
{
	return (stream != NULL) ? 1u : 0u;
}

static inline bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sample_frames)
{
	(void)devid;

	if (spec != NULL)
	{
		*spec = s_ps2SdlAudioStream.spec;
	}
	if (sample_frames != NULL)
	{
		*sample_frames = 512;
	}
	return true;
}

static inline bool SDL_GetAudioStreamFormat(SDL_AudioStream *stream, SDL_AudioSpec *src_spec, SDL_AudioSpec *dst_spec)
{
	if (stream == NULL)
	{
		return false;
	}
	if (src_spec != NULL)
	{
		*src_spec = stream->spec;
	}
	if (dst_spec != NULL)
	{
		*dst_spec = stream->spec;
	}
	return true;
}

static inline bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream)
{
	return stream != NULL;
}

static inline void SDL_DestroyAudioStream(SDL_AudioStream *stream)
{
	if ((stream != NULL) && stream->open)
	{
		stream->open = 0;
		stream->callback = NULL;
		Ps2AudioOut_Close();
	}
}

// Un solo hilo: no hace falta cerrojo.
static inline bool SDL_LockAudioStream(SDL_AudioStream *stream)
{
	(void)stream;
	return true;
}

static inline bool SDL_UnlockAudioStream(SDL_AudioStream *stream)
{
	(void)stream;
	return true;
}

static inline bool SDL_ClearAudioStream(SDL_AudioStream *stream)
{
	(void)stream;
	return true;
}

static inline int SDL_GetAudioStreamQueued(SDL_AudioStream *stream)
{
	(void)stream;
	return Ps2AudioOut_QueuedBytes();
}

static inline bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len)
{
	(void)stream;
	return Ps2AudioOut_Write(buf, len) >= 0;
}

#endif
