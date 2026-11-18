// PS2 port: sustituto vacio de <SDL3/SDL.h>.
//
// Algunos modulos comunes (native_gpu.c, native_perf.c) incluyen SDL aunque no
// lo usan en el build de PS2. Esta carpeta solo esta en el include path de
// Makefile.ps2, asi que el build de PC sigue usando el SDL real.

#ifndef CTR_PS2_SDL_SHIM_H
#define CTR_PS2_SDL_SHIM_H

#include <string.h>

#define SDL_memcpy memcpy
#define SDL_memset memset

#endif
