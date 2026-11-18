# Port de CTR Native a PS2 — inventario y plan

Basado en `ctr-native-master` (versión `0.1.0-beta.7.1`).

## 1. Resumen

El port es viable, y la arquitectura de CTR Native ayuda mucho:

- **El juego (`game/`, 943 ficheros) casi no hay que tocarlo.** Es C portable, de punto fijo y sin dependencias de x86.
- **Todo lo que depende del host está en `platform/` (unos 24.700 líneas) y en `main.c`.** El port consiste en escribir un backend PS2 para esa capa.
- **El build exige 32 bits y little-endian, igual que el EE.** Cientos de `CTR_STATIC_ASSERT` verifican el layout retail de las structs. Si algo cambia de tamaño al compilar para MIPS, el compilador avisa.
- **La parte más grande es el renderer.** El actual es OpenGL 3.3 con shaders que emulan la VRAM de PS1. En PS2 hay que reescribirlo contra el GS, pero el GS se diseñó como sucesor de la GPU de PS1 y soporta en hardware casi todo lo que el shader emula.

## 2. Cómo está montado

```
main.c  ──►  Platform_* (platform.h, 16 funciones)
             │
game/ ──►  facade PsyQ (libgpu, libgte, libspu, libcd, libpad, libetc, libapi)
             │          platform/native_lib*.c
             ▼
        native_gpu.c   parsea la OT/primitivas → GrVertex + "splits" (blend, textura)
             ▼
        native_renderer.c   NativeRenderer_* (~35 funciones)  ← OpenGL
```

Las dos interfaces que hay que reimplementar son `NativeRenderer_*` (en `include/platform/native_renderer.h`) y `Platform_*` (en `include/platform.h`). Todo lo que está por encima de ellas se reutiliza tal cual, `native_gpu.c` incluido.

## 3. Inventario por módulo

| Módulo | Líneas | Depende de | En PS2 | Esfuerzo |
|---|---|---|---|---|
| `native_renderer.c` + `native_glad.c` | 2279 + 2058 | OpenGL, GLSL | Reescribir con gsKit/GS (ver §5) | **Alto** |
| `native_platform.c` | 777 | SDL ventana/eventos/timer, GL swap | Init GS, vsync por interrupción, timers del EE | Medio |
| `native_audio.c` | 3517 | SDL_AudioStream (callback en hilo) | Fase 1: mismo mezclador por software → `audsrv`. Fase 2: SPU2 por hardware | Medio → Alto |
| `native_input.c` + `native_libpad.c` | 1067 + 58 | SDL gamepad/teclado | `padman` (DualShock 2, incluye botones analógicos de presión) | Bajo |
| `native_memcard.c` + adapter | 826 + 212 | `fopen` sobre ficheros del host | Fase 1: el mismo código con rutas `mc0:/`. Luego `mcman` con iconos de PS2 | Bajo |
| `native_cd.c` + `native_disc_image.c` | 381 + 577 | `fopen`/`fseek` sobre `ctr-u.bin` | Funciona con newlib en `mass:/` (USB) o `cdrom0:`. Luego lectura raw del disco con `sceCdRead` | Bajo |
| `native_assets.c` | 905 | `SDL_GetBasePath`, stdio | Ruta base fija o `argv[0]` | Bajo |
| `native_str.c` (vídeo STR) | 827 | — | Reutilizable, aunque el MDEC por software consume CPU | Bajo |
| `native_gte_core.c`, `native_libgte.c` | 735 + 986 | C puro | Funciona igual. Optimizar después con VU0 | Bajo (después Medio) |
| `native_gpu.c`, `native_gpu_links.c`, `native_libgpu.c` | ~2500 | C puro | Se reutilizan | Nulo |
| `native_libspu.c`, `native_libapi.c`, `native_libetc.c`, `native_inline_c.c`, `native_memory.c`, `native_state.c`, `native_log.c` | — | C puro / stdio | Se reutilizan | Nulo |
| `native_checkpoint*`, `native_replay_scheduler`, `native_savestate`, `native_perf` | ~5000 | — | **Excluir**: son herramientas internas (`CTR_INTERNAL`). El build PS2 no define `CTR_INTERNAL` | Nulo |

## 4. Riesgos que he revisado

| Riesgo | Resultado |
|---|---|
| Uso de `double` (en el EE va por software, lentísimo) | **Sin problema.** Hay unas 10 apariciones en todo el código, casi todas en logs o declaraciones. El juego es todo punto fijo |
| SSE / intrínsecos x86 | Solo el flag `-msse` / `/arch:SSE2` en CMake. No hay intrínsecos en el código |
| Asm MIPS de PS1 (`mtc2` a la GTE, secciones de PROC/Particle/DrawConfetti) | Está dentro de `#ifndef CTR_NATIVE`. **Importante:** en el R5900, COP2 es la VU0 y no la GTE, así que esas rutas no deben compilarse nunca. Hoy no se compilan |
| `#ifdef` por arquitectura (`__mips`, `__i386`) | No hay. Solo `_WIN32` y `__linux__` en la capa de plataforma |
| Alineación de tipos de 64 bits en structs (i386 los alinea a 4 bytes, MIPS a 8) | Riesgo bajo. Solo hay 17 usos de `s64`/`u64` en `game/`, casi todos variables locales, y los static asserts lo detectarían |
| Accesos desalineados (x86 los tolera, MIPS lanza excepción) | **Pendiente de comprobar con el compilador.** El código del juego viene de MIPS, así que debería estar bien. El riesgo está en el código nativo nuevo. Los lectores LE de audio leen byte a byte, lo cual es correcto |
| Hilos | Solo el callback de audio de SDL. En PS2 se sustituye por un hilo del EE con semáforos |
| Memoria (32 MB de RAM) | Hay margen (ver §6) |

## 5. Renderer GS: cómo se traduce

| Lo que hace el shader GL | En el GS |
|---|---|
| VRAM de PS1 de 1024×512×16 bits como textura | Copia en la memoria local del GS (1 MB de los 4 MB). Se mantiene la sombra en CPU que ya existe (`s_vram.cpuPixels`) |
| Texturas de 4 y 8 bits con CLUT, buscada en el shader | Formatos nativos PSMT4 y PSMT8 con CLUT PSMCT16. Los índices se cachean por tpage (convirtiéndolos una vez al subir) y la CLUT se cambia por draw vía `TEX0.CBP`. Para invalidar se usa el dirty tracking que ya existe (`gpuNewerTiles`) |
| Blend `BM_AVERAGE` | `ALPHA`: (Cs − Cd)·FIX64 + Cd |
| Blend `BM_ADD` | (Cs − 0)·FIX128 + Cd |
| Blend `BM_SUBTRACT` | (Cd − Cs)·FIX128 + 0 |
| Blend `BM_ADD_QUATER_SOURCE` | (Cs − 0)·FIX32 + Cd |
| Bit STP / semitransparencia por texel / negro transparente | `TEXA` (TA0/TA1, AEM=1) y test de alpha |
| Bit de máscara (`psxDrawMaskSet`) | `FBA` y test de destino (`DATE`) |
| Dithering | `DTHE` y `DIMX` |
| Feedback del framebuffer (`NativeGpu_PrepareFramebufferFeedback`) | El GS puede leer como textura directamente del framebuffer en su memoria |
| Mejoras de resolución | Renderizar a 640×448 escalando las coordenadas (el GS usa 12.4, con subpíxel), con filtrado bilineal opcional |

`NativeRenderer_UpdateVertexBuffer` y `NativeRenderer_DrawTriangles` pasan a ser la construcción de paquetes GIF (PRIM TRIANGLE con RGBAQ/UV/XYZ2), enviados por DMA path 3. Más adelante, path 1 con VU1.

## 6. Memoria estimada (EE, 32 MB)

| Concepto | Tamaño |
|---|---|
| Ejecutable (código + datos del juego y de la plataforma) | ~3–5 MB (a medir con `size` una vez compile) |
| `s_mempackMemory` (heap del juego, con el tamaño retail) | 2 MB |
| Sombra de VRAM en CPU | 1 MB |
| RAM de SPU emulada | 512 KB |
| Pista XA preparada (`NativeAudio_PrepareXAStream` copia a RAM todos los sectores de un canal) | Unos MB por pista. Conviene pasar a streaming |
| gsKit, buffers DMA, pila, newlib | ~2–4 MB |

Queda margen de sobra, incluso para ampliar el mempack para personajes y pistas extra (objetivo de la fase 3).

## 7. Plan por fases

**Fase 0 — Toolchain**
- Instalar PS2SDK, gsKit y ps2-packer. Lo más fácil es la imagen Docker `ps2dev/ps2dev`.
- Crear `build-ps2.sh` con un Makefile al estilo ps2sdk, o un toolchain file de CMake. Usar `-DCTR_NATIVE -DCTR_PS2`, sin `CTR_INTERNAL` y sin `-msse`.

**Fase 1 — Que compile y arranque (objetivo: primer frame)**
1. Separar SDL de `main.c` y de `native_platform.c` detrás de `#if defined(CTR_PS2)`, o crear `platform/ps2/` con sus propios `native_platform_ps2.c`, `native_renderer_ps2.c`, `native_input_ps2.c` y `native_audio_ps2.c`.
2. Hacer stubs de todo `NativeRenderer_*`, del audio y del input para que enlace.
3. `Platform_Init`: inicializar el GS en modo NTSC 640×448 entrelazado.
4. `VSync`/`Platform_WaitUntilVBlank`: usar el handler de interrupción de vsync del GS.
5. Leer `ctr-u.bin` desde `mass:/` (USB) con el código actual de `native_disc_image.c`.
6. Validar con PCSX2 primero, y luego en hardware real con PS2Link.

**Fase 2 — Renderer GS (§5), input con padman y memcard con `mc0:`**

**Fase 3 — Audio:** primero el mezclador de software sacado por `audsrv`. Después pasarlo a la SPU2 real, que tiene casi el mismo formato ADPCM, ADSR y reverb que la SPU de PS1. Eso libera mucha CPU del EE.

**Fase 4 — Rendimiento:** medir, llevar las funciones GTE más usadas a VU0 (RTPT, NCLIP, MVMVA) y estudiar 60 fps.

**Fase 5 — Contenido:** IA de los bots, personajes y pistas extra. Esto se puede desarrollar en paralelo en el build de PC, porque todo vive en `game/`.

## 8. Cómo compilar y probar (fase 1)

**Ficheros del port** (el build de PC no se toca):

| Fichero | Qué hace |
|---|---|
| `main_ps2.c` | Punto de entrada de PS2. Incluye el juego y los módulos comunes, cambia los de SDL por los de `platform/ps2/` y busca `assets/ctr-u.bin` en `host:`, `mass:/CTR`, `mass:` o `cdrom0:` |
| `platform/ps2/ps2_platform.c` | `Platform_*` y `VSync`, con los VBlanks sacados de la interrupción real del GS |
| `platform/ps2/ps2_renderer.c` | `NativeRenderer_*` con gsKit. Geometría sin texturas, pantallas cargadas en VRAM y blend modes de PS1 |
| `platform/ps2/ps2_input.c` | Stub: mando 1 conectado sin botones pulsados |
| `platform/ps2/ps2_audio.c` | Stub: sin sonido |
| `platform/ps2/include/SDL3/SDL.h` | Shim vacío para los módulos comunes que incluyen SDL sin usarlo |
| `Makefile.ps2` | Build con PS2SDK y gsKit |
| `.github/workflows/ps2.yml` | Compila en GitHub Actions en cada push |

**Compilar:** cada push a GitHub lanza el workflow *PS2 build*. El resultado (`ctr_ps2.elf` y `build-log.txt`) se descarga en Actions → la ejecución → Artifacts.

**Probar en PCSX2:**
1. Crea una carpeta con `ctr_ps2.elf` y, a su lado, `assets/ctr-u.bin`.
2. En PCSX2, activa *Host Filesystem*. En la 1.6.0 está en el menú *System* (*Enable host filesystem*).
3. Arranca con *System → Run ELF…* y elige `ctr_ps2.elf`.
4. Los mensajes `[CTR PS2]` salen en la consola o el log de PCSX2 (activa la consola del EE/IOP si no se ve).

**Qué se espera ver en la fase 1:** las pantallas que el juego carga directamente en VRAM, y después la geometría de menús y pistas en colores planos (sin texturas) y sin sonido.
