// PS2 port - renderer GS (sustituye a platform/native_renderer.c + native_glad.c).
//
// Fase 2 (este fichero):
//  - Copia de la VRAM de PS1 en RAM (LoadImage/StoreImage/MoveImage igual que PC).
//  - Texturas reales con una cache en la memoria del GS:
//      * Paginas de 4 y 8 bits: se copian al GS en formato PSMT4/PSMT8 sin
//        convertir los texeles (el empaquetado de bytes de PS1 coincide con el
//        del GS en una transferencia lineal).
//      * Paletas (CLUT): se convierten a 32 bits (CT32). El alfa de cada color
//        codifica la transparencia de PS1 para la pasada actual (ver "perfiles").
//      * Paginas de 16 bits: se convierten a CT32 (son raras en CTR).
//    Las entradas se invalidan cuando el juego escribe en esa zona de la VRAM.
//  - Transparencias de PS1 con la unidad de blending del GS usando siempre el
//    alfa de la fuente (As) como factor:
//        Cv = ((A - B) * As >> 7) + D
//    Asi un texel con alfa 0 deja el destino intacto (= texel transparente de
//    PS1) sin necesitar el test de alfa, y el resto de modos se consiguen
//    eligiendo el alfa:
//        opaco      (Cs - Cd) * 0x80 + Cd = Cs
//        media      (Cs - Cd) * 0x40 + Cd = (Cs + Cd) / 2
//        suma       (Cs - 0)  * 0x80 + Cd = Cs + Cd
//        resta      (0 - Cs)  * 0x80 + Cd = Cd - Cs
//        cuarto     (Cs - 0)  * 0x20 + Cd = Cd + Cs / 4
//  - Pantallas cargadas directamente en VRAM: se suben como textura CT16 (el
//    formato de color de PS1 coincide con PSMCT16 del GS).
//  - Efectos que leen la propia pantalla (fondo de la pausa, distorsiones): en
//    PS1 el juego texturiza desde la zona de VRAM donde dibujo el fotograma. Aqui
//    se recuerda que zona de VRAM corresponde a cada buffer del GS y, si una
//    textura de 16 bits cae ahi, se usa directamente el framebuffer del GS
//    (que ya es de 16 bits) en vez de la copia en RAM.
//  - Recorte (scissor) y limpieza por zonas, para pantalla partida y menus.
//  - Medidor de rendimiento en la consola cada ~10 segundos.

#include <macros.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>

#include "platform/native_gpu.h"
#include "platform/native_log.h"
#include "platform/native_renderer.h"

#include <dmaKit.h>
#include <gsKit.h>
#include <gsInline.h>
#include <kernel.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define PS2_TEXTURE_ID_WHITE 1u
#define PS2_TEXTURE_ID_VRAM  2u

// Cada cuantos triangulos se vacia la cola de gsKit para no desbordarla.
#define PS2_TRIANGLES_PER_FLUSH 768

// Zona de VRAM que se puede mostrar directamente (PresentVRAMRect).
#define PS2_DISPLAY_TEX_W 512
#define PS2_DISPLAY_TEX_H 512

// Cache de texturas en la memoria del GS.
#define PS2_PAGE_SLOT_BYTES (64 * 1024) // 256x256 PSMT8 (PSMT4 ocupa la mitad)
#define PS2_MAX_PAGE_SLOTS  40
#define PS2_CLUT_SLOT_BYTES 1024 // 256 colores CT32
#define PS2_MAX_CLUT_SLOTS  256
#define PS2_PAGE16_BYTES    (256 * 256 * 4) // pagina de 16 bits convertida a CT32

// Perfiles de alfa: que texeles se dibujan en la pasada actual y con que factor.
enum Ps2AlphaMask
{
	PS2_MASK_ALL = 0,    // todos los texeles no transparentes
	PS2_MASK_NON_STP = 1, // solo texeles sin bit STP (pasada opaca de un semitransparente)
	PS2_MASK_STP = 2,     // solo texeles con bit STP (pasada mezclada)
};

struct Ps2PageSlot
{
	u32 vram;
	u32 lastUse;
	s16 page;
	u8 format; // TexFormat
	u8 valid;
};

struct Ps2ClutSlot
{
	u32 vram;
	u32 lastUse;
	u16 clut;
	u8 format; // TF_4_BIT o TF_8_BIT
	u8 profile;
	u8 valid;
};

struct Ps2Page16Slot
{
	u32 vram;
	s16 page;
	u8 profile;
	u8 valid;
};

// Variables globales que la version de PC define en native_renderer.c /
// native_platform.c y que otros modulos referencian.
int g_cfg_bilinearFiltering = 0;
int g_dbg_texturelessMode = 0;
int g_dbg_wireframeMode = 0;
int g_windowWidth = 640;
int g_windowHeight = 448;

global_variable GSGLOBAL *s_gs = NULL;

// Copia en RAM de la VRAM de PS1 (1024x512, 16 bits por pixel).
global_variable u16 s_vramPixels[VRAM_WIDTH * VRAM_HEIGHT] __attribute__((aligned(128)));

// Buffers de subida al GS (DMA: alineados a 128 bytes).
global_variable u16 s_displayUpload[PS2_DISPLAY_TEX_W * PS2_DISPLAY_TEX_H] __attribute__((aligned(128)));
global_variable u8 s_pageUpload[PS2_PAGE_SLOT_BYTES] __attribute__((aligned(128)));
global_variable u32 s_clutUpload[256] __attribute__((aligned(128)));
global_variable u32 s_page16Upload[256 * 256] __attribute__((aligned(128)));

global_variable GSTEXTURE s_vramDisplayTexture;

global_variable struct Ps2PageSlot s_pageSlots[PS2_MAX_PAGE_SLOTS];
global_variable int s_pageSlotCount = 0;
global_variable struct Ps2ClutSlot s_clutSlots[PS2_MAX_CLUT_SLOTS];
global_variable int s_clutSlotCount = 0;
global_variable struct Ps2Page16Slot s_page16Slot;
global_variable int s_page16Available = 0;
global_variable u32 s_useTick = 1;

// Ultima textura preparada: la mayoria de triangulos seguidos comparten textura.
global_variable int s_lastBindValid = 0;
global_variable s16 s_lastBindPage;
global_variable u16 s_lastBindClut;
global_variable u8 s_lastBindFormat;
global_variable u8 s_lastBindProfile;
global_variable GSTEXTURE s_lastBindTex;
global_variable const void *s_lastBindSurface;

// Zonas de VRAM de PS1 que ahora mismo estan en un buffer del GS.
#define PS2_MAX_FB_SURFACES 2
struct Ps2FbSurface
{
	int valid;
	int x, y, w, h; // rectangulo en VRAM de PS1
	u32 gsVram;     // buffer del GS que lo contiene
	int stp;        // 1 = sus pixeles llevan el bit STP de PS1 (ver s_frameStp)
};
global_variable struct Ps2FbSurface s_fbSurfaces[PS2_MAX_FB_SURFACES];
global_variable int s_fbSurfaceNext = 0;
global_variable const struct Ps2FbSurface *s_boundSurface = NULL;

// Rectangulos de VRAM de PS1 de los fotogramas completos (los dos buffers).
// En pantalla partida el juego pide la copia de la pantalla con el recorte de
// UN jugador (media pantalla), pero la copia del GS es siempre el fotograma
// entero: hay que apuntarla con el rectangulo del fotograma completo.
#define PS2_MAX_FRAME_RECTS 2
global_variable int s_frameRects[PS2_MAX_FRAME_RECTS][4];
global_variable int s_frameRectCount = 0;
global_variable int s_frameRectNext = 0;

internal void Ps2Renderer_RememberFrameRect(int x, int y, int w, int h)
{
	for (int i = 0; i < s_frameRectCount; i++)
	{
		if ((s_frameRects[i][0] == x) && (s_frameRects[i][1] == y) && (s_frameRects[i][2] == w) && (s_frameRects[i][3] == h))
		{
			return;
		}
	}

	s_frameRects[s_frameRectNext][0] = x;
	s_frameRects[s_frameRectNext][1] = y;
	s_frameRects[s_frameRectNext][2] = w;
	s_frameRects[s_frameRectNext][3] = h;
	s_frameRectNext = (s_frameRectNext + 1) % PS2_MAX_FRAME_RECTS;
	if (s_frameRectCount < PS2_MAX_FRAME_RECTS)
	{
		s_frameRectCount++;
	}
}

// Si (x, y, w, h) cae dentro de un fotograma completo conocido, lo cambia por el
// fotograma completo.
internal void Ps2Renderer_ExpandToFrameRect(int *x, int *y, int *w, int *h)
{
	for (int i = 0; i < s_frameRectCount; i++)
	{
		const int fx = s_frameRects[i][0];
		const int fy = s_frameRects[i][1];
		const int fw = s_frameRects[i][2];
		const int fh = s_frameRects[i][3];
		if ((*x >= fx) && (*y >= fy) && ((*x + *w) <= (fx + fw)) && ((*y + *h) <= (fy + fh)))
		{
			*x = fx;
			*y = fy;
			*w = fw;
			*h = fh;
			return;
		}
	}
}

// Copia fija de la pantalla para los efectos que leen lo que se esta dibujando
// (reloj, ondas, presentaciones, demo...). Comparte memoria del GS con la
// textura de PresentVRAMRect (nunca se usan en el mismo fotograma).
global_variable u32 s_fbSnapshotVram = 0;
global_variable u32 s_fbSnapshots = 0; // copias hechas (para el log)

// Bit STP (bit 15) de lo dibujado. El GS no lo guarda como PS1, asi que se
// apunta por fotograma: CTR, antes de usar la imagen anterior como textura en
// el efecto del reloj/demo, pinta toda la pantalla con "poner bit de mascara"
// (y el propio efecto copia texeles con STP). En PS1 esos texeles se mezclan al
// 50%; sin esto el port los pintaba opacos: la imagen se ampliaba sobre si misma
// en cada fotograma (cuadrados grandes en la demo) y el destello blanco del
// reloj se quedaba fijo (pantalla blanca).
global_variable int s_maskSet = 0;
global_variable int s_frameStp = 0;
global_variable int s_prevFrameStp = 0; // el del fotograma anterior
global_variable u32 s_fbLastSnapX, s_fbLastSnapY, s_fbLastSnapW, s_fbLastSnapH;
global_variable int s_fixAlphaOn = 0;
global_variable u8 s_fixAlphaValue = 0;

// Lectura del framebuffer del GS a RAM (para StoreImage/MoveImage de la pantalla).
global_variable u16 s_readbackPixels[640 * 512] __attribute__((aligned(64)));
global_variable u16 s_readTemp[VRAM_WIDTH * VRAM_HEIGHT] __attribute__((aligned(64)));
global_variable u64 s_readbackPacket[10] __attribute__((aligned(64)));
global_variable u32 s_drawGeneration = 1;
global_variable u32 s_readbackGeneration = 0;
global_variable u32 s_readbackVram = 0xffffffffu;
global_variable int s_readbackOk = 0;

internal const struct Ps2FbSurface *Ps2Renderer_FindSurfaceRect(int x, int y, int w, int h);
internal void Ps2Renderer_ReadRect(u16 *dst, int dstStride, int x, int y, int w, int h);

// Estadisticas para el medidor de rendimiento.
struct Ps2Stats
{
	u32 frames;
	u32 triangles;
	u32 skippedOffscreen;
	u32 pageUploads;
	u32 clutUploads;
	u32 page16Uploads;
	u32 surfaceBinds;
	u32 queueFlushes;
	u32 readbacks;
	u32 untexturedTris;
	u32 texturedTris;
	u32 texFormat[4];   // triangulos por formato: 4, 8, 16 bits, 32 (override)
	u32 bindFail[4];    // 0 formato no soportado, 1 paleta, 2 pagina, 3 pagina 16 bits
	u32 surfacePass2Skips;
	u32 gsClears;
	u32 gsClearsSkipped;
	u32 p16Fallback;   // texturas de 16 bits que NO se encontraron en el GS
	int p16LastX, p16LastY;
	int p16MinU, p16MaxU, p16MinV, p16MaxV; // texeles usados (primer vertice)
	int p16MinX, p16MaxX, p16MinY, p16MaxY; // posicion en pantalla (primer vertice)
	int p16Clut, p16Page;
};

global_variable struct Ps2Stats s_stats;
global_variable clock_t s_statsStart = 0;

global_variable const GrVertex *s_vertices = NULL;
global_variable int s_vertexCount = 0;

global_variable TextureID s_currentTexture = PS2_TEXTURE_ID_WHITE;
global_variable TexFormat s_currentTexFormat = TF_4_BIT;
global_variable BlendMode s_currentBlendMode = BM_NONE;
global_variable int s_semiTransPass = 0;
global_variable u8 s_modeAlpha = 0x80;
global_variable int s_offscreen = 0;
global_variable int s_projectionW = 512;
global_variable int s_projectionH = 240;
global_variable int s_trianglesSinceFlush = 0;

// Recorte actual en pixeles del GS (x0, y0, x1, y1 incluidos) o desactivado.
global_variable int s_scissorOn = 0;
global_variable int s_scissor[4];

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

internal u16 Ps2Renderer_PackRGB24ToPSX15(u8 r, u8 g, u8 b)
{
	return (u16)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

internal void Ps2Renderer_ClampRect(int *x, int *y, int *w, int *h)
{
	if (*x < 0)
	{
		*w += *x;
		*x = 0;
	}
	if (*y < 0)
	{
		*h += *y;
		*y = 0;
	}
	if (*x + *w > VRAM_WIDTH)
	{
		*w = VRAM_WIDTH - *x;
	}
	if (*y + *h > VRAM_HEIGHT)
	{
		*h = VRAM_HEIGHT - *y;
	}
	if (*w < 0)
	{
		*w = 0;
	}
	if (*h < 0)
	{
		*h = 0;
	}
}

internal int Ps2Renderer_RectsOverlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return (ax < bx + bw) && (bx < ax + aw) && (ay < by + bh) && (by < ay + ah);
}

// Escala vertical de PS1 -> GS. CTR dibuja 216 lineas: con 448 cada linea de
// PS1 ocupaba 2,07 del GS (unas filas dobles y otras triples: sombras de letras
// que se movian 1 pixel). Si cabe una escala ENTERA dejando poco borde (216 x 2
// = 432, borde de 8 lineas arriba y abajo) se usa esa, centrada con un borde
// PAR (para no cambiar que lineas van en cada campo del entrelazado).
internal void Ps2Renderer_VMap(int srcH, float *scale, float *offset)
{
	const int screenH = s_gs->Height;
	// DESACTIVADO: en la PS2 real (imagen entrelazada en la tele) las letras y
	// los iconos se veian PEOR con la escala entera. Se vuelve a la escala de
	// antes (448 / 216). Poner a 1 para probar otra vez.
	if (0 && (srcH > 0))
	{
		const int k = screenH / srcH;
		const int border = screenH - k * srcH;
		if ((k >= 1) && (border <= 32))
		{
			*scale = (float)k;
			*offset = (float)((border / 2) & ~1);
			return;
		}
	}
	*scale = (srcH > 0) ? (float)screenH / (float)srcH : 1.0f;
	*offset = 0.0f;
}

internal void Ps2Renderer_FlushQueue(void)
{
	s_stats.queueFlushes++;
	gsKit_queue_exec(s_gs);
	s_trianglesSinceFlush = 0;
}

internal u32 Ps2Renderer_UploadBytes(int width, int height, u32 psm)
{
	const u32 texels = (u32)width * (u32)height;

	switch (psm)
	{
	case GS_PSM_T4:
		return texels / 2;
	case GS_PSM_T8:
		return texels;
	case GS_PSM_CT16:
	case GS_PSM_CT16S:
		return texels * 2;
	default:
		return texels * 4;
	}
}

// Envia datos de la RAM del EE a la memoria del GS de forma inmediata. Antes se
// vacia la cola de dibujo para que los triangulos ya encolados usen los datos
// antiguos, y despues se espera al DMA para poder reutilizar el buffer.
internal void Ps2Renderer_SendToGS(void *data, int width, int height, u32 vram, u32 psm, u32 tbw, u8 clutMode)
{
	s_lastBindValid = 0;
	Ps2Renderer_FlushQueue();
	// Solo hace falta volcar a memoria el trozo de cache de datos que se envia.
	SyncDCache(data, (u8 *)data + Ps2Renderer_UploadBytes(width, height, psm));
	gsKit_texture_send((u32 *)data, width, height, vram, psm, tbw, clutMode);
	dmaKit_wait(DMA_CHANNEL_GIF, 0);
}

internal void Ps2Renderer_ApplyScissor(void)
{
	if (s_scissorOn)
	{
		gsKit_set_scissor(s_gs, GS_SETREG_SCISSOR(s_scissor[0], s_scissor[2], s_scissor[1], s_scissor[3]));
	}
	else
	{
		gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
	}
}

// ---------------------------------------------------------------------------
// Blending
// ---------------------------------------------------------------------------

internal u8 Ps2Renderer_BlendFactor(BlendMode blendMode)
{
	switch (blendMode)
	{
	case BM_AVERAGE:
		return 0x40;
	case BM_ADD_QUATER_SOURCE:
		return 0x20;
	case BM_ADD:
	case BM_SUBTRACT:
	case BM_NONE:
	default:
		return 0x80;
	}
}

// Cv = ((A - B) * C >> 7) + D.  A/B/D: 0 = Cs, 1 = Cd, 2 = 0.  C: 0 = As, 2 = FIX.
internal u64 Ps2Renderer_BlendReg(BlendMode blendMode, int useFix, u8 fix)
{
	const int c = useFix ? 2 : 0;
	switch (blendMode)
	{
	case BM_ADD:
	case BM_ADD_QUATER_SOURCE:
		return GS_SETREG_ALPHA(0, 2, c, 1, fix);
	case BM_SUBTRACT:
		return GS_SETREG_ALPHA(2, 0, c, 1, fix);
	case BM_AVERAGE:
	case BM_NONE:
	default:
		return GS_SETREG_ALPHA(0, 1, c, 1, fix);
	}
}

internal void Ps2Renderer_ApplyBlendMode(BlendMode blendMode)
{
	s_gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(s_gs, Ps2Renderer_BlendReg(blendMode, 0, 0), 0);
	s_modeAlpha = Ps2Renderer_BlendFactor(blendMode);
	s_fixAlphaOn = 0;
}

// Mezcla con un factor fijo (para texturas que son la propia pantalla).
internal void Ps2Renderer_SetFixAlpha(u8 fix)
{
	if (s_fixAlphaOn && (s_fixAlphaValue == fix))
	{
		return;
	}
	s_gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(s_gs, Ps2Renderer_BlendReg(s_currentBlendMode, 1, fix), 0);
	s_fixAlphaOn = 1;
	s_fixAlphaValue = fix;
}

// ---------------------------------------------------------------------------
// Cache de texturas
// ---------------------------------------------------------------------------

internal int Ps2Tex_PageX(s16 page)
{
	return (page & 0xf) * 64;
}

internal int Ps2Tex_PageY(s16 page)
{
	return (page & 0x10) ? 256 : 0;
}

internal int Ps2Tex_PageWidthHalfwords(TexFormat format)
{
	switch (format)
	{
	case TF_4_BIT:
		return 64;
	case TF_8_BIT:
		return 128;
	default:
		return 256;
	}
}

internal int Ps2Tex_ClutX(u16 clut)
{
	return (clut & 0x3f) << 4;
}

internal int Ps2Tex_ClutY(u16 clut)
{
	return (clut >> 6) & 0x1ff;
}

internal u8 Ps2Tex_Profile(enum Ps2AlphaMask mask, u8 factor)
{
	const u8 factorIndex = (factor == 0x40) ? 1 : (factor == 0x20) ? 2 : 0;
	return (u8)((mask << 2) | factorIndex);
}

internal void Ps2Tex_ProfileDecode(u8 profile, enum Ps2AlphaMask *mask, u8 *factor)
{
	static const u8 factors[3] = {0x80, 0x40, 0x20};
	*mask = (enum Ps2AlphaMask)(profile >> 2);
	*factor = factors[(profile & 3) < 3 ? (profile & 3) : 0];
}

// Perfil de alfa para lo que se va a dibujar ahora (segun pasada y blend).
internal u8 Ps2Tex_CurrentProfile(void)
{
	switch (s_semiTransPass)
	{
	case 1:
		return Ps2Tex_Profile(PS2_MASK_NON_STP, 0x80);
	case 2:
		return Ps2Tex_Profile(PS2_MASK_STP, s_modeAlpha);
	default:
		return Ps2Tex_Profile(PS2_MASK_ALL, s_modeAlpha);
	}
}

// Color de 16 bits de PS1 -> RGBA de 32 bits con el alfa del perfil.
internal u32 Ps2Tex_ConvertColor(u16 c, enum Ps2AlphaMask mask, u8 factor)
{
	const u32 r = (u32)(c & 0x1f) << 3;
	const u32 g = (u32)((c >> 5) & 0x1f) << 3;
	const u32 b = (u32)((c >> 10) & 0x1f) << 3;
	const int transparent = (c == 0x0000);
	const int stp = (c & 0x8000) != 0;
	u32 a = 0;

	if (!transparent)
	{
		switch (mask)
		{
		case PS2_MASK_NON_STP:
			a = stp ? 0 : factor;
			break;
		case PS2_MASK_STP:
			a = stp ? factor : 0;
			break;
		case PS2_MASK_ALL:
		default:
			a = factor;
			break;
		}
	}

	return r | (g << 8) | (b << 16) | (a << 24);
}

internal void Ps2Tex_InvalidateRect(int x, int y, int w, int h)
{
	s_lastBindValid = 0;

	// Si el juego escribe desde la CPU encima de una zona que tenia el contenido
	// de un buffer del GS, esa zona ya no corresponde al GS.
	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		struct Ps2FbSurface *surface = &s_fbSurfaces[i];
		if (surface->valid && Ps2Renderer_RectsOverlap(x, y, w, h, surface->x, surface->y, surface->w, surface->h))
		{
			surface->valid = 0;
		}
	}

	for (int i = 0; i < s_pageSlotCount; i++)
	{
		struct Ps2PageSlot *slot = &s_pageSlots[i];
		if (slot->valid &&
		    Ps2Renderer_RectsOverlap(x, y, w, h, Ps2Tex_PageX(slot->page), Ps2Tex_PageY(slot->page), Ps2Tex_PageWidthHalfwords((TexFormat)slot->format), 256))
		{
			slot->valid = 0;
		}
	}

	for (int i = 0; i < s_clutSlotCount; i++)
	{
		struct Ps2ClutSlot *slot = &s_clutSlots[i];
		const int entries = (slot->format == TF_8_BIT) ? 256 : 16;
		if (slot->valid && Ps2Renderer_RectsOverlap(x, y, w, h, Ps2Tex_ClutX(slot->clut), Ps2Tex_ClutY(slot->clut), entries, 1))
		{
			slot->valid = 0;
		}
	}

	if (s_page16Slot.valid &&
	    Ps2Renderer_RectsOverlap(x, y, w, h, Ps2Tex_PageX(s_page16Slot.page), Ps2Tex_PageY(s_page16Slot.page), 256, 256))
	{
		s_page16Slot.valid = 0;
	}
}

// Pagina de 4 u 8 bits en el GS. Devuelve la direccion o 0 si no hay sitio.
internal u32 Ps2Tex_GetPage(s16 page, TexFormat format)
{
	struct Ps2PageSlot *victim = NULL;

	for (int i = 0; i < s_pageSlotCount; i++)
	{
		struct Ps2PageSlot *slot = &s_pageSlots[i];
		if (slot->valid && (slot->page == page) && (slot->format == (u8)format))
		{
			slot->lastUse = s_useTick;
			return slot->vram;
		}

		if ((victim == NULL) || !slot->valid || (victim->valid && (slot->lastUse < victim->lastUse)))
		{
			victim = slot;
		}
	}

	if (victim == NULL)
	{
		return 0;
	}

	// Copia las filas de la pagina a un buffer lineal: 256 texeles por fila,
	// que en VRAM son 64 (4 bits) o 128 (8 bits) halfwords.
	const int pageX = Ps2Tex_PageX(page);
	const int pageY = Ps2Tex_PageY(page);
	const int rowHalfwords = Ps2Tex_PageWidthHalfwords(format);
	const int rowBytes = rowHalfwords * 2;
	int copyHalfwords = rowHalfwords;

	if (pageX + copyHalfwords > VRAM_WIDTH)
	{
		copyHalfwords = VRAM_WIDTH - pageX;
	}

	memset(s_pageUpload, 0, (size_t)rowBytes * 256u);
	for (int row = 0; row < 256; row++)
	{
		memcpy(&s_pageUpload[row * rowBytes], &s_vramPixels[(pageY + row) * VRAM_WIDTH + pageX], (size_t)copyHalfwords * 2u);
	}

	Ps2Renderer_SendToGS(s_pageUpload, 256, 256, victim->vram, (format == TF_8_BIT) ? GS_PSM_T8 : GS_PSM_T4, 4, GS_CLUT_TEXTURE);
	s_stats.pageUploads++;

	victim->page = page;
	victim->format = (u8)format;
	victim->valid = 1;
	victim->lastUse = s_useTick;
	return victim->vram;
}

// Paleta convertida a CT32 con el alfa del perfil. Devuelve la direccion o 0.
internal u32 Ps2Tex_GetClut(u16 clut, TexFormat format, u8 profile)
{
	struct Ps2ClutSlot *victim = NULL;

	for (int i = 0; i < s_clutSlotCount; i++)
	{
		struct Ps2ClutSlot *slot = &s_clutSlots[i];
		if (slot->valid && (slot->clut == clut) && (slot->format == (u8)format) && (slot->profile == profile))
		{
			slot->lastUse = s_useTick;
			return slot->vram;
		}

		if ((victim == NULL) || !slot->valid || (victim->valid && (slot->lastUse < victim->lastUse)))
		{
			victim = slot;
		}
	}

	if (victim == NULL)
	{
		return 0;
	}

	enum Ps2AlphaMask mask;
	u8 factor;
	Ps2Tex_ProfileDecode(profile, &mask, &factor);

	const int clutX = Ps2Tex_ClutX(clut);
	const int clutY = Ps2Tex_ClutY(clut);
	const u16 *src = &s_vramPixels[clutY * VRAM_WIDTH];

	if (format == TF_8_BIT)
	{
		for (int i = 0; i < 256; i++)
		{
			// El GS guarda las paletas de 256 colores (CSM1) con los bloques de 8
			// entradas 8-15 y 16-23 intercambiados dentro de cada grupo de 32.
			const int dst = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
			s_clutUpload[dst] = Ps2Tex_ConvertColor(src[(clutX + i) & (VRAM_WIDTH - 1)], mask, factor);
		}

		Ps2Renderer_SendToGS(s_clutUpload, 16, 16, victim->vram, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
	}
	else
	{
		for (int i = 0; i < 16; i++)
		{
			s_clutUpload[i] = Ps2Tex_ConvertColor(src[(clutX + i) & (VRAM_WIDTH - 1)], mask, factor);
		}

		Ps2Renderer_SendToGS(s_clutUpload, 8, 2, victim->vram, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
	}

	s_stats.clutUploads++;
	victim->clut = clut;
	victim->format = (u8)format;
	victim->profile = profile;
	victim->valid = 1;
	victim->lastUse = s_useTick;
	return victim->vram;
}

// Pagina de 16 bits convertida a CT32 (un unico hueco: en CTR son raras).
internal u32 Ps2Tex_GetPage16(s16 page, u8 profile)
{
	if (!s_page16Available)
	{
		return 0;
	}

	if (s_page16Slot.valid && (s_page16Slot.page == page) && (s_page16Slot.profile == profile))
	{
		return s_page16Slot.vram;
	}

	enum Ps2AlphaMask mask;
	u8 factor;
	Ps2Tex_ProfileDecode(profile, &mask, &factor);

	const int pageX = Ps2Tex_PageX(page);
	const int pageY = Ps2Tex_PageY(page);

	for (int row = 0; row < 256; row++)
	{
		const u16 *src = &s_vramPixels[(pageY + row) * VRAM_WIDTH];
		u32 *dst = &s_page16Upload[row * 256];
		for (int col = 0; col < 256; col++)
		{
			const int x = pageX + col;
			dst[col] = (x < VRAM_WIDTH) ? Ps2Tex_ConvertColor(src[x], mask, factor) : 0;
		}
	}

	Ps2Renderer_SendToGS(s_page16Upload, 256, 256, s_page16Slot.vram, GS_PSM_CT32, 4, GS_CLUT_NONE);
	s_stats.page16Uploads++;

	s_page16Slot.page = page;
	s_page16Slot.profile = profile;
	s_page16Slot.valid = 1;
	return s_page16Slot.vram;
}

internal const struct Ps2FbSurface *Ps2Tex_FindSurface(s16 page)
{
	const int pageX = Ps2Tex_PageX(page);
	const int pageY = Ps2Tex_PageY(page);

	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		const struct Ps2FbSurface *surface = &s_fbSurfaces[i];
		if (surface->valid && Ps2Renderer_RectsOverlap(pageX, pageY, 256, 256, surface->x, surface->y, surface->w, surface->h))
		{
			return surface;
		}
	}

	return NULL;
}

internal int Ps2Tex_BindSlow(const GrVertex *v, GSTEXTURE *tex, u8 profile);

// Prepara la textura de gsKit para un triangulo. Devuelve 0 si no se puede.
internal int Ps2Tex_Bind(const GrVertex *v, GSTEXTURE *tex)
{
	const u8 profile = Ps2Tex_CurrentProfile();

	if (s_lastBindValid && (s_lastBindPage == v->page) && (s_lastBindClut == (u16)v->clut) && (s_lastBindFormat == (u8)s_currentTexFormat) &&
	    (s_lastBindProfile == profile))
	{
		*tex = s_lastBindTex;
		s_boundSurface = (const struct Ps2FbSurface *)s_lastBindSurface;
		return 1;
	}

	if (!Ps2Tex_BindSlow(v, tex, profile))
	{
		return 0;
	}

	s_lastBindPage = v->page;
	s_lastBindClut = (u16)v->clut;
	s_lastBindFormat = (u8)s_currentTexFormat;
	s_lastBindProfile = profile;
	s_lastBindTex = *tex;
	s_lastBindSurface = s_boundSurface;
	s_lastBindValid = 1;
	return 1;
}

internal int Ps2Tex_BindSlow(const GrVertex *v, GSTEXTURE *tex, u8 profile)
{
	s_boundSurface = NULL;
	memset(tex, 0, sizeof(*tex));
	tex->Width = 256;
	tex->Height = 256;
	tex->TBW = 4;
	tex->Filter = g_cfg_bilinearFiltering ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;

	s_useTick++;

	if ((s_currentTexFormat == TF_4_BIT) || (s_currentTexFormat == TF_8_BIT))
	{
		// La paleta primero: si su subida desaloja algo, la pagina se busca
		// despues y queda garantizada para este triangulo.
		const u32 clutVram = Ps2Tex_GetClut((u16)v->clut, s_currentTexFormat, profile);
		const u32 pageVram = Ps2Tex_GetPage(v->page, s_currentTexFormat);

		if ((clutVram == 0) || (pageVram == 0))
		{
			s_stats.bindFail[(clutVram == 0) ? 1 : 2]++;
			return 0;
		}

		tex->PSM = (s_currentTexFormat == TF_8_BIT) ? GS_PSM_T8 : GS_PSM_T4;
		tex->ClutPSM = GS_PSM_CT32;
		tex->Vram = pageVram;
		tex->VramClut = clutVram;
		return 1;
	}

	if (s_currentTexFormat == TF_16_BIT)
	{
		// La textura cae en una zona que el GS ha dibujado: usar su framebuffer.
		const struct Ps2FbSurface *surface = Ps2Tex_FindSurface(v->page);
		if (surface != NULL)
		{
			tex->Width = s_gs->Width;
			tex->Height = s_gs->Height;
			tex->TBW = (u8)(s_gs->Width / 64);
			tex->PSM = s_gs->PSM;
			tex->Vram = surface->gsVram;
			s_boundSurface = surface;
			s_stats.surfaceBinds++;
			return 1;
		}

		if (s_stats.p16Fallback == 0)
		{
			s_stats.p16MinU = s_stats.p16MaxU = v->u;
			s_stats.p16MinV = s_stats.p16MaxV = v->v;
			s_stats.p16MinX = s_stats.p16MaxX = v->x;
			s_stats.p16MinY = s_stats.p16MaxY = v->y;
		}
		if (v->u < s_stats.p16MinU) s_stats.p16MinU = v->u;
		if (v->u > s_stats.p16MaxU) s_stats.p16MaxU = v->u;
		if (v->v < s_stats.p16MinV) s_stats.p16MinV = v->v;
		if (v->v > s_stats.p16MaxV) s_stats.p16MaxV = v->v;
		if (v->x < s_stats.p16MinX) s_stats.p16MinX = v->x;
		if (v->x > s_stats.p16MaxX) s_stats.p16MaxX = v->x;
		if (v->y < s_stats.p16MinY) s_stats.p16MinY = v->y;
		if (v->y > s_stats.p16MaxY) s_stats.p16MaxY = v->y;
		s_stats.p16Clut = (u16)v->clut;
		s_stats.p16Page = (u16)v->page;
		s_stats.p16Fallback++;
		s_stats.p16LastX = Ps2Tex_PageX(v->page);
		s_stats.p16LastY = Ps2Tex_PageY(v->page);
		const u32 pageVram = Ps2Tex_GetPage16(v->page, profile);
		if (pageVram == 0)
		{
			s_stats.bindFail[3]++;
			return 0;
		}

		tex->PSM = GS_PSM_CT32;
		tex->Vram = pageVram;
		return 1;
	}

	// TF_32_BIT_RGBA: texturas "override" nativas, no se usan en el juego retail.
	s_stats.bindFail[0]++;
	return 0;
}

internal void Ps2Tex_AllocatePools(void)
{
	// Paletas: un bloque contiguo dividido en huecos de 1 KB.
	const u32 clutPool = gsKit_vram_alloc(s_gs, PS2_CLUT_SLOT_BYTES * PS2_MAX_CLUT_SLOTS, GSKIT_ALLOC_USERBUFFER);
	if (clutPool != GSKIT_ALLOC_ERROR)
	{
		for (int i = 0; i < PS2_MAX_CLUT_SLOTS; i++)
		{
			memset(&s_clutSlots[i], 0, sizeof(s_clutSlots[i]));
			s_clutSlots[i].vram = clutPool + (u32)(i * PS2_CLUT_SLOT_BYTES);
		}
		s_clutSlotCount = PS2_MAX_CLUT_SLOTS;
	}

	// Un hueco para paginas de 16 bits.
	memset(&s_page16Slot, 0, sizeof(s_page16Slot));
	s_page16Slot.vram = gsKit_vram_alloc(s_gs, PS2_PAGE16_BYTES, GSKIT_ALLOC_USERBUFFER);
	s_page16Available = (s_page16Slot.vram != GSKIT_ALLOC_ERROR);

	// Paginas de 4/8 bits: todos los huecos que quepan en lo que queda.
	s_pageSlotCount = 0;
	while (s_pageSlotCount < PS2_MAX_PAGE_SLOTS)
	{
		const u32 vram = gsKit_vram_alloc(s_gs, PS2_PAGE_SLOT_BYTES, GSKIT_ALLOC_USERBUFFER);
		if (vram == GSKIT_ALLOC_ERROR)
		{
			break;
		}

		memset(&s_pageSlots[s_pageSlotCount], 0, sizeof(s_pageSlots[s_pageSlotCount]));
		s_pageSlots[s_pageSlotCount].vram = vram;
		s_pageSlotCount++;
	}

	printf("[CTR PS2] Texture cache: %d page slots, %d CLUT slots, 16-bit page slot: %s\n", s_pageSlotCount, s_clutSlotCount,
	       s_page16Available ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// Inicializacion
// ---------------------------------------------------------------------------

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen)
{
	(void)windowName;
	(void)width;
	(void)height;
	(void)fullscreen;

	// main_ps2.c inicia el GS al principio (para los colores de diagnostico);
	// Platform_Init lo vuelve a pedir despues.
	if (s_gs != NULL)
	{
		return 1;
	}

	s_gs = gsKit_init_global();
	if (s_gs == NULL)
	{
		return 0;
	}

	// CTR NTSC-U espera 60 Hz. Con una BIOS PAL gsKit elegiria 50 Hz y el juego
	// iria un 17% mas lento, asi que forzamos NTSC 640x448 entrelazado.
	s_gs->Mode = GS_MODE_NTSC;
	s_gs->Interlace = GS_INTERLACED;
	s_gs->Field = GS_FIELD;
	s_gs->Width = 640;
	s_gs->Height = 448;
	// (La escala vertical exacta x2 se hace al dibujar: ver Ps2Renderer_VMap.
	// Cambiar el modo de pantalla a 432 lineas se veia mal en la PS2 real.)

	// Framebuffer de 16 bits: PS1 trabaja con color de 15 bits, asi que no se
	// pierde nada y queda casi el doble de memoria del GS para texturas.
	s_gs->PSM = GS_PSM_CT16S;
	s_gs->PSMZ = GS_PSMZ_16S;
	s_gs->ZBuffering = GS_SETTING_OFF;
	s_gs->DoubleBuffering = GS_SETTING_ON;
	s_gs->PrimAlphaEnable = GS_SETTING_ON;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gsKit_init_screen(s_gs);
	gsKit_mode_switch(s_gs, GS_ONESHOT);
	gsKit_set_clamp(s_gs, GS_CMODE_REPEAT);

	g_windowWidth = s_gs->Width;
	g_windowHeight = s_gs->Height;

	memset(&s_vramDisplayTexture, 0, sizeof(s_vramDisplayTexture));
	s_vramDisplayTexture.PSM = GS_PSM_CT16;
	s_vramDisplayTexture.Filter = GS_FILTER_NEAREST;
	s_vramDisplayTexture.Mem = (u32 *)s_displayUpload;
	{
		// La misma zona sirve para la copia fija de la pantalla (640x448, 16 bits).
		u32 displayBytes = gsKit_texture_size(PS2_DISPLAY_TEX_W, PS2_DISPLAY_TEX_H, GS_PSM_CT16);
		const u32 snapshotBytes = gsKit_texture_size(s_gs->Width, s_gs->Height, GS_PSM_CT16);
		if (snapshotBytes > displayBytes)
		{
			displayBytes = snapshotBytes;
		}
		s_vramDisplayTexture.Vram = gsKit_vram_alloc(s_gs, displayBytes, GSKIT_ALLOC_USERBUFFER);
		s_fbSnapshotVram = s_vramDisplayTexture.Vram;
	}

	if (s_vramDisplayTexture.Vram == GSKIT_ALLOC_ERROR)
	{
		Platform_LogError("[CTR PS2] Not enough GS memory for the VRAM display texture\n");
		return 0;
	}

	Ps2Tex_AllocatePools();
	Ps2Renderer_ApplyBlendMode(BM_NONE);

	gsKit_clear(s_gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	Ps2Renderer_FlushQueue();

	printf("[CTR PS2] GS ready: %dx%d\n", s_gs->Width, s_gs->Height);
	return 1;
}

int NativeRenderer_InitialisePSX(void)
{
	memset(s_vramPixels, 0, sizeof(s_vramPixels));
	return 1;
}

void NativeRenderer_Shutdown(void)
{
	s_gs = NULL;
}

void NativeRenderer_ResetDevice(void)
{
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void NativeRenderer_BeginScene(void)
{
	s_trianglesSinceFlush = 0;
}

void NativeRenderer_EndScene(void)
{
	Ps2Renderer_FlushQueue();
}

void NativeRenderer_EndGpuFrame(void)
{
}

void NativeRenderer_FinishGpuMeasurements(void)
{
}

void NativeRenderer_UpdateSwapIntervalState(int swapInterval)
{
	(void)swapInterval;
}

// gsKit solo trae gsKit_sync_flip(), que ademas espera al VBlank. Aqui el ritmo
// ya lo marca VSync() del juego, asi que solo intercambiamos los buffers: el
// que acabamos de pintar pasa a mostrarse y el otro pasa a ser el de dibujo.
internal void Ps2Renderer_FlipNoWait(void)
{
	if (!s_gs->FirstFrame && (s_gs->DoubleBuffering == GS_SETTING_ON))
	{
		GS_SET_DISPFB2(s_gs->ScreenBuffer[s_gs->ActiveBuffer & 1] / 8192, s_gs->Width / 64, s_gs->PSM, 0, 0);
		s_gs->ActiveBuffer ^= 1;
	}

	gsKit_setactive(s_gs);
}

// Imprime un resumen de rendimiento cada PS2_PERF_REPORT_SECONDS segundos.
#define PS2_PERF_REPORT_SECONDS 10.0
internal void Ps2Renderer_UpdateStats(void)
{
	s_stats.frames++;

	const clock_t now = clock();
	if (s_statsStart == 0)
	{
		s_statsStart = now;
		return;
	}

	const double seconds = (double)(now - s_statsStart) / (double)CLOCKS_PER_SEC;
	if (seconds < PS2_PERF_REPORT_SECONDS)
	{
		return;
	}

	const u32 frames = (s_stats.frames > 0) ? s_stats.frames : 1;
	printf("[CTR PS2] perf: %.1f fps | tris/frame %u | uploads/frame: pages %.1f clut %.1f p16 %.1f | fb-tex %u (copias %u) | offscreen tris %u | flushes/frame %.1f | readbacks %u\n",
	       (double)s_stats.frames / seconds, s_stats.triangles / frames,
	       (double)s_stats.pageUploads / frames, (double)s_stats.clutUploads / frames, (double)s_stats.page16Uploads / frames,
	       s_stats.surfaceBinds, s_fbSnapshots, s_stats.skippedOffscreen, (double)s_stats.queueFlushes / frames, s_stats.readbacks);

	// Diagnostico de los efectos que leen la pantalla.
	printf("[CTR PS2] fb: 16 bits sin GS %u (ultima pag %d,%d) | zonas:", s_stats.p16Fallback, s_stats.p16LastX, s_stats.p16LastY);
	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		const struct Ps2FbSurface *z = &s_fbSurfaces[i];
		if (z->valid)
		{
			printf(" [%d,%d %dx%d %s%s]", z->x, z->y, z->w, z->h, (z->gsVram == s_fbSnapshotVram) ? "copia" : "buffer", z->stp ? " stp" : "");
		}
		else
		{
			printf(" [-]");
		}
	}
	printf(" | ultima copia %u,%u %ux%u | pasadas saltadas %u\n", s_fbLastSnapX, s_fbLastSnapY, s_fbLastSnapW, s_fbLastSnapH, s_stats.surfacePass2Skips);
	if (s_stats.p16Fallback > 0)
	{
		printf("[CTR PS2] 16 bits: page 0x%04x clut 0x%04x | u %d-%d v %d-%d | pantalla x %d-%d y %d-%d\n", s_stats.p16Page, s_stats.p16Clut, s_stats.p16MinU,
		       s_stats.p16MaxU, s_stats.p16MinV, s_stats.p16MaxV, s_stats.p16MinX, s_stats.p16MaxX, s_stats.p16MinY, s_stats.p16MaxY);
	}

	memset(&s_stats, 0, sizeof(s_stats));
	s_fbSnapshots = 0;
	s_statsStart = now;
}

// Pinta toda la pantalla de un color (diagnostico de arranque en consola real,
// donde no hay consola de texto). Se pintan los dos buffers.
void Ps2Renderer_DebugScreen(u8 r, u8 g, u8 b)
{
	if (s_gs == NULL)
	{
		return;
	}

	for (int i = 0; i < 2; i++)
	{
		s_gs->PrimAlphaEnable = GS_SETTING_OFF;
		gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
		gsKit_clear(s_gs, GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00));
		Ps2Renderer_FlushQueue();
		gsKit_finish();
		Ps2Renderer_FlipNoWait();
	}

	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
}

// Con la escala x2 quedan 8 lineas arriba y abajo sin usar: se pintan de negro
// (si no, quedarian restos de un video o de una pantalla de carga).
global_variable int s_presentedVramThisFrame = 0;

internal void Ps2Renderer_ClearBorders(void)
{
	if (s_presentedVramThisFrame)
	{
		return; // imagen de VRAM a pantalla completa (videos, cargas)
	}
	float vs, vo;
	Ps2Renderer_VMap(s_projectionH, &vs, &vo);
	if (vo <= 0.0f)
	{
		return;
	}
	const float bottom = vo + vs * (float)s_projectionH;
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
	gsKit_prim_sprite(s_gs, 0.0f, 0.0f, (float)s_gs->Width, vo, 0, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_prim_sprite(s_gs, 0.0f, bottom, (float)s_gs->Width, (float)s_gs->Height, 0, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	Ps2Renderer_ApplyScissor();
}

// Cuadrito de diagnostico en la esquina (PS2 real, sin log): lo pone el
// vigilante de cargas (ps2_platform.c, donde esta la variable). 0 = no se dibuja.
extern u32 g_ps2DebugCornerColor;
// Fotograma lento en carga (arriba a la derecha), tambien de ps2_platform.c.
extern u32 g_ps2SlowFrameColor;

internal void Ps2Renderer_DrawCornerSquare(float x, float y, u32 c)
{
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
	gsKit_prim_sprite(s_gs, x, y, x + 48.0f, y + 48.0f, 0, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_prim_sprite(s_gs, x + 4.0f, y + 4.0f, x + 44.0f, y + 44.0f, 0, GS_SETREG_RGBAQ((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 0x80, 0x00));
	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	Ps2Renderer_ApplyScissor();
}

internal void Ps2Renderer_DrawDebugCorner(void)
{
	if (g_ps2SlowFrameColor != 0)
	{
		Ps2Renderer_DrawCornerSquare((float)s_gs->Width - 68.0f, 20.0f, g_ps2SlowFrameColor);
	}
	if (g_ps2DebugCornerColor == 0)
	{
		return;
	}
	const u32 c = g_ps2DebugCornerColor;
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
	// Borde negro y el color dentro, para que se vea sobre cualquier fondo.
	gsKit_prim_sprite(s_gs, 20.0f, 20.0f, 68.0f, 68.0f, 0, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_prim_sprite(s_gs, 24.0f, 24.0f, 64.0f, 64.0f, 0, GS_SETREG_RGBAQ((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 0x80, 0x00));
	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	Ps2Renderer_ApplyScissor();
}

void NativeRenderer_SwapWindow(void)
{
	Ps2Renderer_ClearBorders();
	Ps2Renderer_DrawDebugCorner();
	s_presentedVramThisFrame = 0;
	const u32 gsStart = Ps2Prof_Now();
	Ps2Renderer_FlushQueue();
	gsKit_finish();
	g_ps2ProfGsCycles += (u32)(Ps2Prof_Now() - gsStart);
	s_drawGeneration++;
	Ps2Renderer_FlipNoWait();
	// Por si al cambiar de buffer se restablece el recorte.
	Ps2Renderer_ApplyScissor();
	Ps2Renderer_UpdateStats();
}

// ---------------------------------------------------------------------------
// VRAM
// ---------------------------------------------------------------------------

// Apunta que la zona (x, y, w, h) de la VRAM de PS1 esta en el buffer gsVram del GS.
internal void Ps2Renderer_RegisterSurface(int x, int y, int w, int h, u32 gsVram, int stp)
{
	struct Ps2FbSurface *target = NULL;

	// Reutiliza la entrada de la misma zona o la del mismo buffer del GS.
	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		struct Ps2FbSurface *surface = &s_fbSurfaces[i];
		if (surface->valid && ((surface->gsVram == gsVram) || ((surface->x == x) && (surface->y == y))))
		{
			target = surface;
			break;
		}
	}

	if (target == NULL)
	{
		target = &s_fbSurfaces[s_fbSurfaceNext];
		s_fbSurfaceNext = (s_fbSurfaceNext + 1) % PS2_MAX_FB_SURFACES;
	}

	// Una zona de VRAM solo puede estar en un buffer: quita solapes antiguos.
	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		struct Ps2FbSurface *surface = &s_fbSurfaces[i];
		if ((surface != target) && surface->valid && Ps2Renderer_RectsOverlap(x, y, w, h, surface->x, surface->y, surface->w, surface->h))
		{
			surface->valid = 0;
		}
	}

	target->valid = 1;
	target->x = x;
	target->y = y;
	target->w = w;
	target->h = h;
	target->gsVram = gsVram;
	target->stp = stp;
}

// Copia dentro del GS (de memoria del GS a memoria del GS) del buffer en el que
// se dibuja a la zona de la copia fija.
internal void Ps2Renderer_SnapshotDrawBuffer(u32 drawBuffer)
{
	const u32 fbw = (u32)s_gs->Width / 64u;
	u64 *p = (u64 *)gsKit_heap_alloc(s_gs, 5, 80, GIF_AD);
	*p++ = GIF_TAG_AD(5);
	*p++ = GIF_AD;
	*p++ = GS_SETREG_BITBLTBUF(drawBuffer / 256u, fbw, s_gs->PSM, s_fbSnapshotVram / 256u, fbw, s_gs->PSM);
	*p++ = GS_BITBLTBUF;
	*p++ = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
	*p++ = GS_TRXPOS;
	*p++ = GS_SETREG_TRXREG(s_gs->Width, s_gs->Height);
	*p++ = GS_TRXREG;
	*p++ = GS_SETREG_TRXDIR(2); // de memoria del GS a memoria del GS
	*p++ = GS_TRXDIR;
	*p++ = 0;
	*p++ = GS_TEXFLUSH;
	s_fbSnapshots++;
}

// Fin de fotograma (ps2_platform.c): la imagen recien dibujada pasa a mostrarse
// y el siguiente fotograma se dibuja en el otro buffer, asi que se puede leer
// directamente de su buffer (fondo del menu de pausa, etc.).
void Ps2Renderer_StoreDisplayedFrame(int x, int y, int w, int h)
{
	Ps2Renderer_ClampRect(&x, &y, &w, &h);
	if ((s_gs == NULL) || (w <= 0) || (h <= 0))
	{
		return;
	}

	Ps2Renderer_FlushQueue();
	s_lastBindValid = 0;
	Ps2Renderer_RememberFrameRect(x, y, w, h);
	Ps2Renderer_RegisterSurface(x, y, w, h, s_gs->ScreenBuffer[s_gs->ActiveBuffer & 1], s_frameStp);
	s_prevFrameStp = s_frameStp;
	s_frameStp = 0; // el siguiente fotograma empieza sin bit STP
}

void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h)
{
	// Lo llama native_gpu.c justo antes de un grupo de poligonos que usan como
	// textura la imagen que se esta dibujando (reloj, ondas, presentaciones...).
	// En PC se hace una copia fija y los poligonos leen de ella. Antes aqui se
	// leia del MISMO buffer en el que se pintaba: cada trozo leia lo que otro
	// trozo acababa de pintar (cuadrados, imagen deformada, pantalla en blanco).
	// Ahora se copia el buffer dentro del GS y se lee de la copia.
	Ps2Renderer_ClampRect(&x, &y, &w, &h);
	if ((s_gs == NULL) || (w <= 0) || (h <= 0))
	{
		return;
	}

	s_lastBindValid = 0;
	const u32 drawBuffer = s_gs->ScreenBuffer[s_gs->ActiveBuffer & 1];

	// Pantalla partida: el recorte es de un jugador, la copia es la pantalla entera.
	Ps2Renderer_ExpandToFrameRect(&x, &y, &w, &h);

	if ((s_fbSnapshotVram == 0) || (s_fbSnapshotVram == GSKIT_ALLOC_ERROR))
	{
		Ps2Renderer_FlushQueue();
		Ps2Renderer_RegisterSurface(x, y, w, h, drawBuffer, s_frameStp);
		return;
	}

	Ps2Renderer_SnapshotDrawBuffer(drawBuffer);
	Ps2Renderer_FlushQueue();
	// Si el fotograma anterior tenia STP (efecto del reloj/demo en marcha), la
	// copia tambien: en PS1 el efecto lee la imagen anterior, que lo lleva.
	Ps2Renderer_RegisterSurface(x, y, w, h, s_fbSnapshotVram, s_frameStp || s_prevFrameStp);
	s_fbLastSnapX = (u32)x;
	s_fbLastSnapY = (u32)y;
	s_fbLastSnapW = (u32)w;
	s_fbLastSnapH = (u32)h;
}

void NativeRenderer_PresentVRAMRect(int x, int y, int w, int h)
{
	Ps2Renderer_ClampRect(&x, &y, &w, &h);
	if (w > PS2_DISPLAY_TEX_W)
	{
		w = PS2_DISPLAY_TEX_W;
	}
	if (h > PS2_DISPLAY_TEX_H)
	{
		h = PS2_DISPLAY_TEX_H;
	}
	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	for (int row = 0; row < h; row++)
	{
		memcpy(&s_displayUpload[row * w], &s_vramPixels[(y + row) * VRAM_WIDTH + x], (size_t)w * sizeof(u16));
	}

	// Esta subida pisa la copia fija de la pantalla: olvidarla.
	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		if (s_fbSurfaces[i].valid && (s_fbSurfaces[i].gsVram == s_fbSnapshotVram))
		{
			s_fbSurfaces[i].valid = 0;
		}
	}
	s_lastBindValid = 0;

	s_presentedVramThisFrame = 1;
	s_vramDisplayTexture.Width = w;
	s_vramDisplayTexture.Height = h;
	Ps2Renderer_FlushQueue();
	SyncDCache(s_displayUpload, (u8 *)s_displayUpload + (size_t)w * (size_t)h * sizeof(u16));
	gsKit_texture_upload(s_gs, &s_vramDisplayTexture);
	gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);

	// Sin blending: es una copia directa de la imagen.
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_clear(s_gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_prim_sprite_texture(s_gs, &s_vramDisplayTexture,
	                          0.0f, 0.0f, 0.0f, 0.0f,
	                          (float)s_gs->Width, (float)s_gs->Height, (float)w, (float)h,
	                          0, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	Ps2Renderer_ApplyScissor();
	Ps2Renderer_FlushQueue();
}

void NativeRenderer_PresentVRAMDisplay(void)
{
	NativeRenderer_PresentVRAMRect(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
}

void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int readFromFramebuffer)
{
	(void)outputFileName;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)readFromFramebuffer;
}

void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	// Limpia solo la parte del rectangulo que cae en la pantalla mostrada (misma
	// logica que la version de PC), escalada a la resolucion del GS.
	int displayX = activeDispEnv.disp.x;
	int displayY = activeDispEnv.disp.y;
	int displayW = activeDispEnv.disp.w;
	int displayH = activeDispEnv.disp.h;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		displayX = activeDrawEnv.clip.x;
		displayY = activeDrawEnv.clip.y;
		displayW = activeDrawEnv.clip.w;
		displayH = activeDrawEnv.clip.h;
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		return;
	}

	const int overlapX = (x > displayX) ? x : displayX;
	const int overlapY = (y > displayY) ? y : displayY;
	const int overlapRight = ((x + w) < (displayX + displayW)) ? (x + w) : (displayX + displayW);
	const int overlapBottom = ((y + h) < (displayY + displayH)) ? (y + h) : (displayY + displayH);

	if ((overlapRight <= overlapX) || (overlapBottom <= overlapY))
	{
		s_stats.gsClearsSkipped++;
		return;
	}

	s_stats.gsClears++;

	const float scaleX = (float)s_gs->Width / (float)displayW;
	float scaleY, offY;
	Ps2Renderer_VMap(displayH, &scaleY, &offY);

	s_drawGeneration++;
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
	gsKit_prim_sprite(s_gs,
	                  (float)(overlapX - displayX) * scaleX, (float)(overlapY - displayY) * scaleY + offY,
	                  (float)(overlapRight - displayX) * scaleX, (float)(overlapBottom - displayY) * scaleY + offY,
	                  0, GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00));
	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	Ps2Renderer_ApplyScissor();
}

void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	const u16 color = Ps2Renderer_PackRGB24ToPSX15(r, g, b);

	Ps2Renderer_ClampRect(&x, &y, &w, &h);

	for (int row = 0; row < h; row++)
	{
		u16 *dst = &s_vramPixels[(y + row) * VRAM_WIDTH + x];
		for (int col = 0; col < w; col++)
		{
			dst[col] = color;
		}
	}

	Ps2Tex_InvalidateRect(x, y, w, h);
}

void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY)
{
	int stride = w;

	if ((src == NULL) && (w > 0) && (h > 0) && (w <= VRAM_WIDTH) && (h <= VRAM_HEIGHT) && (Ps2Renderer_FindSurfaceRect(x, y, w, h) != NULL))
	{
		// MoveImage desde una zona dibujada por el GS: leerla primero del GS.
		Ps2Renderer_ReadRect(s_readTemp, w, x, y, w, h);
		src = s_readTemp;
		x = 0;
		y = 0;
	}
	else if (src == NULL)
	{
		src = s_vramPixels;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;
	u16 *dst = s_vramPixels + dstX + dstY * VRAM_WIDTH;

	for (int row = 0; row < h; row++)
	{
		if ((dstY + row) >= VRAM_HEIGHT)
		{
			break;
		}

		int copyW = w;
		if (dstX + copyW > VRAM_WIDTH)
		{
			copyW = VRAM_WIDTH - dstX;
		}

		if (copyW > 0)
		{
			memmove(dst, src, (size_t)copyW * sizeof(u16));
		}

		dst += VRAM_WIDTH;
		src += stride;
	}

	Ps2Tex_InvalidateRect(dstX, dstY, w, h);
}

// --- Lectura GS -> RAM -------------------------------------------------------
// Secuencia estandar del hardware: se pide al GS una transferencia
// local -> host (TRXDIR = 1) por el canal GIF y los datos se recogen por la
// FIFO de VIF1 con el DMA 1 en direccion hacia memoria.

#define PS2_D1_CHCR   ((volatile u32 *)0x10009000)
#define PS2_D1_MADR   ((volatile u32 *)0x10009010)
#define PS2_D1_QWC    ((volatile u32 *)0x10009020)
#define PS2_D2_CHCR   ((volatile u32 *)0x1000A000)
#define PS2_D2_MADR   ((volatile u32 *)0x1000A010)
#define PS2_D2_QWC    ((volatile u32 *)0x1000A020)
#define PS2_VIF1_STAT ((volatile u32 *)0x10003C00)
#define PS2_GS_BUSDIR ((volatile u64 *)0x12001040)

#define PS2_DMA_STR          (1u << 8)
#define PS2_VIF1_STAT_FDR    (1u << 23)
#define PS2_READBACK_TIMEOUT 20000000

internal int Ps2Renderer_WaitDma(volatile u32 *chcr)
{
	for (int i = 0; i < PS2_READBACK_TIMEOUT; i++)
	{
		if ((*chcr & PS2_DMA_STR) == 0)
		{
			return 1;
		}
	}

	return 0;
}

internal u32 Ps2Renderer_PhysAddr(const void *ptr)
{
	return (u32)(uintptr_t)ptr & 0x0fffffffu;
}

// Lee el buffer del GS que empieza en gsVram (Width x Height, PSM del framebuffer).
internal int Ps2Renderer_ReadbackGS(u32 gsVram)
{
	const u32 width = (u32)s_gs->Width;
	const u32 height = (u32)s_gs->Height;
	const u32 bytes = width * height * 2u;
	const u32 qwc = bytes / 16u;

	if ((s_readbackVram == gsVram) && (s_readbackGeneration == s_drawGeneration))
	{
		return s_readbackOk;
	}

	// Que el GS termine todo lo pendiente antes de leer.
	Ps2Renderer_FlushQueue();
	gsKit_finish();
	dmaKit_wait(DMA_CHANNEL_GIF, 0);

	// GIFtag: 4 registros en modo A+D, fin de paquete.
	s_readbackPacket[0] = 4ull | (1ull << 15) | (1ull << 60);
	s_readbackPacket[1] = 0x0eull;
	// BITBLTBUF: origen = buffer del GS.
	s_readbackPacket[2] = (u64)(gsVram / 256u) | ((u64)(width / 64u) << 16) | ((u64)s_gs->PSM << 24);
	s_readbackPacket[3] = 0x50;
	// TRXPOS: desde (0,0).
	s_readbackPacket[4] = 0;
	s_readbackPacket[5] = 0x51;
	// TRXREG: tamano.
	s_readbackPacket[6] = (u64)width | ((u64)height << 32);
	s_readbackPacket[7] = 0x52;
	// TRXDIR: local -> host.
	s_readbackPacket[8] = 1;
	s_readbackPacket[9] = 0x53;

	SyncDCache(s_readbackPacket, (u8 *)s_readbackPacket + sizeof(s_readbackPacket));

	int ok = Ps2Renderer_WaitDma(PS2_D2_CHCR);
	if (ok)
	{
		*PS2_D2_MADR = Ps2Renderer_PhysAddr(s_readbackPacket);
		*PS2_D2_QWC = sizeof(s_readbackPacket) / 16u;
		*PS2_D2_CHCR = PS2_DMA_STR | 1u; // DIR = desde memoria
		ok = Ps2Renderer_WaitDma(PS2_D2_CHCR);
	}

	if (ok)
	{
		InvalidDCache(s_readbackPixels, (u8 *)s_readbackPixels + bytes);

		*PS2_VIF1_STAT = PS2_VIF1_STAT_FDR;
		*PS2_GS_BUSDIR = 1;

		*PS2_D1_MADR = Ps2Renderer_PhysAddr(s_readbackPixels);
		*PS2_D1_QWC = qwc;
		*PS2_D1_CHCR = PS2_DMA_STR; // DIR = hacia memoria
		ok = Ps2Renderer_WaitDma(PS2_D1_CHCR);

		*PS2_VIF1_STAT = 0;
		*PS2_GS_BUSDIR = 0;

		InvalidDCache(s_readbackPixels, (u8 *)s_readbackPixels + bytes);
	}

	if (!ok)
	{
		printf("[CTR PS2] GS readback timed out\n");
	}

	s_stats.readbacks++;
	s_readbackVram = gsVram;
	s_readbackGeneration = s_drawGeneration;
	s_readbackOk = ok;
	return ok;
}

internal const struct Ps2FbSurface *Ps2Renderer_FindSurfaceRect(int x, int y, int w, int h)
{
	for (int i = 0; i < PS2_MAX_FB_SURFACES; i++)
	{
		const struct Ps2FbSurface *surface = &s_fbSurfaces[i];
		if (surface->valid && Ps2Renderer_RectsOverlap(x, y, w, h, surface->x, surface->y, surface->w, surface->h))
		{
			return surface;
		}
	}

	return NULL;
}

// Copia un rectangulo de "VRAM de PS1" a dst. Lo que caiga en una zona que el GS
// ha dibujado se lee del GS (escalado a la resolucion de PS1); el resto, de la
// copia en RAM.
internal void Ps2Renderer_ReadRect(u16 *dst, int dstStride, int x, int y, int w, int h)
{
	const struct Ps2FbSurface *surface = Ps2Renderer_FindSurfaceRect(x, y, w, h);
	const int haveGs = (surface != NULL) && Ps2Renderer_ReadbackGS(surface->gsVram);

	for (int row = 0; row < h; row++)
	{
		const int vy = y + row;
		u16 *out = &dst[row * dstStride];

		for (int col = 0; col < w; col++)
		{
			const int vx = x + col;

			if (haveGs && (vx >= surface->x) && (vx < surface->x + surface->w) && (vy >= surface->y) && (vy < surface->y + surface->h))
			{
				const int gx = ((vx - surface->x) * s_gs->Width) / surface->w;
				float vs, vo;
				Ps2Renderer_VMap(surface->h, &vs, &vo);
				int gy = (int)vo + (int)((float)(vy - surface->y) * vs);
				if (gy >= s_gs->Height)
				{
					gy = s_gs->Height - 1;
				}
				// Lo dibujado en PS1 no lleva bit STP.
				out[col] = (u16)(s_readbackPixels[gy * s_gs->Width + gx] & 0x7fff);
			}
			else if ((vx >= 0) && (vx < VRAM_WIDTH) && (vy >= 0) && (vy < VRAM_HEIGHT))
			{
				out[col] = s_vramPixels[vy * VRAM_WIDTH + vx];
			}
			else
			{
				out[col] = 0;
			}
		}
	}
}

void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH)
{
	if (Ps2Renderer_FindSurfaceRect(x, y, dstW, dstH) != NULL)
	{
		Ps2Renderer_ReadRect(dst, dstW, x, y, dstW, dstH);
		return;
	}

	const u16 *src = s_vramPixels + x + VRAM_WIDTH * y;

	for (int row = 0; row < dstH; row++)
	{
		memcpy(dst, src, (size_t)dstW * sizeof(u16));
		dst += dstW;
		src += VRAM_WIDTH;
	}
}

void NativeRenderer_UpdateVRAM(void)
{
	// La VRAM vive en RAM; la cache de texturas ya se invalida al escribir.
}

int NativeRenderer_GetVRAMStateSize(void)
{
	return (int)sizeof(s_vramPixels);
}

int NativeRenderer_CaptureVRAMState(void *dst, int dstSize)
{
	if ((dst == NULL) || (dstSize < (int)sizeof(s_vramPixels)))
	{
		return 0;
	}

	memcpy(dst, s_vramPixels, sizeof(s_vramPixels));
	return 1;
}

int NativeRenderer_RestoreVRAMState(const void *src, int srcSize)
{
	if ((src == NULL) || (srcSize < (int)sizeof(s_vramPixels)))
	{
		return 0;
	}

	memcpy(s_vramPixels, src, sizeof(s_vramPixels));
	Ps2Tex_InvalidateRect(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
	return 1;
}

TextureID NativeRenderer_GetVRAMTexture(void)
{
	return PS2_TEXTURE_ID_VRAM;
}

TextureID NativeRenderer_GetWhiteTexture(void)
{
	return PS2_TEXTURE_ID_WHITE;
}

// ---------------------------------------------------------------------------
// Estado de dibujo
// ---------------------------------------------------------------------------

void NativeRenderer_SetBlendMode(BlendMode blendMode)
{
	if (blendMode == s_currentBlendMode)
	{
		return;
	}

	s_currentBlendMode = blendMode;
	Ps2Renderer_ApplyBlendMode(blendMode);
}

void NativeRenderer_SetStencilMode(int drawPrim)
{
	(void)drawPrim;
}

void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, int enable)
{
	(void)offscreenRect;
	s_offscreen = enable;
}

void NativeRenderer_SetProjection(const RECT16 *drawRect, const DISPENV *displayEnv, int offscreen)
{
	if (offscreen)
	{
		s_projectionW = (drawRect->w > 0) ? drawRect->w : 1;
		s_projectionH = (drawRect->h > 0) ? drawRect->h : 1;
		return;
	}

	s_projectionW = (displayEnv->disp.w > 0) ? displayEnv->disp.w : 1;
	s_projectionH = (displayEnv->disp.h > 0) ? displayEnv->disp.h : 1;
}

void NativeRenderer_SetupClipMode(const RECT16 *clipRect, const DISPENV *displayEnv, int enable)
{
	int on = 0;
	int box[4] = {0, 0, 0, 0};

	if ((s_gs != NULL) && enable && (displayEnv->disp.w > 0) && (displayEnv->disp.h > 0))
	{
		if ((clipRect->w <= 0) || (clipRect->h <= 0))
		{
			// Area vacia: no se dibuja nada. Un recorte de 1 pixel fuera de pantalla.
			on = 1;
			box[0] = box[2] = s_gs->Width + 1;
			box[1] = box[3] = s_gs->Height + 1;
		}
		else
		{
			const int relX = clipRect->x - displayEnv->disp.x;
			const int relY = clipRect->y - displayEnv->disp.y;

			float vs, vo;
			Ps2Renderer_VMap(displayEnv->disp.h, &vs, &vo);
			// Solo hace falta recortar si el area no cubre toda la pantalla (o si
			// hay borde arriba/abajo por la escala x2: no pintar en el borde).
			if ((relX > 0) || (relY > 0) || (clipRect->w < displayEnv->disp.w) || (clipRect->h < displayEnv->disp.h) || (vo > 0.0f))
			{
				const float scaleX = (float)s_gs->Width / (float)displayEnv->disp.w;
				float scaleY, offY;
				Ps2Renderer_VMap(displayEnv->disp.h, &scaleY, &offY);

				on = 1;
				box[0] = (int)((float)relX * scaleX);
				box[1] = (int)((float)relY * scaleY + offY);
				box[2] = (int)((float)(relX + clipRect->w) * scaleX) - 1;
				box[3] = (int)((float)(relY + clipRect->h) * scaleY + offY) - 1;

				if (box[0] < 0)
				{
					box[0] = 0;
				}
				if (box[1] < 0)
				{
					box[1] = 0;
				}
			}
		}
	}

	if ((on == s_scissorOn) && (!on || (memcmp(box, s_scissor, sizeof(box)) == 0)))
	{
		return;
	}

	s_scissorOn = on;
	memcpy(s_scissor, box, sizeof(box));
	if (s_gs != NULL)
	{
		Ps2Renderer_ApplyScissor();
	}
}

void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat)
{
	s_currentTexture = texture;
	s_currentTexFormat = texFormat;
}

void NativeRenderer_SetOverrideTextureSize(int width, int height)
{
	(void)width;
	(void)height;
}

void NativeRenderer_SetPSXTextureSemiTransPass(int pass)
{
	s_semiTransPass = pass;
}

void NativeRenderer_SetPSXTextureOutputSTP(int enabled)
{
	(void)enabled;
}

void NativeRenderer_SetPSXDrawMaskSet(int maskSet)
{
	s_maskSet = maskSet;
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------

void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int count)
{
	// native_gpu.c mantiene el buffer vivo mientras dibuja todos los "splits".
	s_vertices = vertices;
	s_vertexCount = count;
}

internal u64 Ps2Renderer_VertexColor(const GrVertex *v, u8 alpha)
{
	return GS_SETREG_RGBAQ(v->r, v->g, v->b, alpha, 0x00);
}

void NativeRenderer_DrawTriangles(int startVertex, int triangles)
{
	if ((s_gs == NULL) || (s_vertices == NULL))
	{
		return;
	}

	s_drawGeneration++;

	if (s_offscreen)
	{
		// Dibujo fuera de pantalla (a otra zona de VRAM): pendiente.
		s_stats.skippedOffscreen += (u32)triangles;
		return;
	}

	const float scaleX = (float)s_gs->Width / (float)s_projectionW;
	float scaleY, offY;
	Ps2Renderer_VMap(s_projectionH, &scaleY, &offY);
	const int textured = (s_currentTexture == PS2_TEXTURE_ID_VRAM);
	GSTEXTURE tex;

	if (s_maskSet && (triangles > 0))
	{
		s_frameStp = 1; // se dibuja con "poner bit de mascara": pixeles con STP
	}

	for (int t = 0; t < triangles; t++)
	{
		const int index = startVertex + (t * 3);
		if (index + 2 >= s_vertexCount)
		{
			break;
		}

		const GrVertex *v0 = &s_vertices[index + 0];
		const GrVertex *v1 = &s_vertices[index + 1];
		const GrVertex *v2 = &s_vertices[index + 2];

		if (textured)
		{
			// El alfa del vertice (0x80 = x1) modula el de la textura, que ya
			// lleva el factor del modo de mezcla.
			s_stats.texFormat[((unsigned)s_currentTexFormat) & 3]++;

			if (!Ps2Tex_Bind(v0, &tex))
			{
				continue;
			}

			s_stats.texturedTris++;

			float uv[3][2] = {
			    {(float)v0->u, (float)v0->v},
			    {(float)v1->u, (float)v1->v},
			    {(float)v2->u, (float)v2->v},
			};

			if (s_boundSurface != NULL)
			{
				// Texeles = pixeles de la pantalla: todos con STP o todos sin el.
				// Pasada 1 (texeles sin STP, opacos) y pasada 2 (con STP, mezclados).
				const int stp = s_boundSurface->stp;
				if (((s_semiTransPass == 2) && !stp) || ((s_semiTransPass == 1) && stp))
				{
					s_stats.surfacePass2Skips++;
					continue;
				}
				Ps2Renderer_SetFixAlpha((s_semiTransPass == 1) ? 0x80 : s_modeAlpha);
				if (stp)
				{
					s_frameStp = 1; // PS1 copia el STP del texel al pixel
				}

				// Coordenadas de VRAM de PS1 -> pixeles del framebuffer del GS.
				const float surfScaleX = (float)s_gs->Width / (float)s_boundSurface->w;
				float surfScaleY, surfOffY;
				Ps2Renderer_VMap(s_boundSurface->h, &surfScaleY, &surfOffY);
				const float baseX = (float)(Ps2Tex_PageX(v0->page) - s_boundSurface->x);
				const float baseY = (float)(Ps2Tex_PageY(v0->page) - s_boundSurface->y);

				for (int k = 0; k < 3; k++)
				{
					uv[k][0] = (baseX + uv[k][0]) * surfScaleX;
					uv[k][1] = (baseY + uv[k][1]) * surfScaleY + surfOffY;
				}
			}

			else if (s_fixAlphaOn)
			{
				Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
			}

			gsKit_prim_triangle_goraud_texture_3d(s_gs, &tex,
			                                      v0->x * scaleX, v0->y * scaleY + offY, 0, uv[0][0], uv[0][1],
			                                      v1->x * scaleX, v1->y * scaleY + offY, 0, uv[1][0], uv[1][1],
			                                      v2->x * scaleX, v2->y * scaleY + offY, 0, uv[2][0], uv[2][1],
			                                      Ps2Renderer_VertexColor(v0, 0x80),
			                                      Ps2Renderer_VertexColor(v1, 0x80),
			                                      Ps2Renderer_VertexColor(v2, 0x80));
		}
		else
		{
			if (s_fixAlphaOn)
			{
				Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
			}
			s_stats.untexturedTris++;
			// Sin textura, el alfa del vertice es directamente el factor de mezcla.
			gsKit_prim_triangle_gouraud_3d(s_gs,
			                               v0->x * scaleX, v0->y * scaleY + offY, 0,
			                               v1->x * scaleX, v1->y * scaleY + offY, 0,
			                               v2->x * scaleX, v2->y * scaleY + offY, 0,
			                               Ps2Renderer_VertexColor(v0, s_modeAlpha),
			                               Ps2Renderer_VertexColor(v1, s_modeAlpha),
			                               Ps2Renderer_VertexColor(v2, s_modeAlpha));
		}

		s_stats.triangles++;

		if (++s_trianglesSinceFlush >= PS2_TRIANGLES_PER_FLUSH)
		{
			Ps2Renderer_FlushQueue();
		}
	}

	if (s_fixAlphaOn)
	{
		Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	}
}

void NativeRenderer_PushDebugLabel(const char *label)
{
	(void)label;
}

void NativeRenderer_PopDebugLabel(void)
{
}
