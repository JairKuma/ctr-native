// PS2 port - renderer GS (sustituye a platform/native_renderer.c + native_glad.c).
//
// Fase 1 (este fichero):
//  - Mantiene la copia de la VRAM de PS1 en RAM (LoadImage/StoreImage/MoveImage
//    funcionan igual que en PC).
//  - Dibuja los triangulos que genera native_gpu.c con gsKit, SIN texturas:
//    los poligonos texturizados salen con un color plano segun su tpage.
//    Sirve para comprobar que el juego corre y que la geometria esta bien.
//  - Las pantallas que el juego sube directamente a VRAM (logos, carga, video)
//    se muestran subiendo esa zona de la VRAM como textura de 16 bits. El
//    formato de color de PS1 (R5 G5 B5 + bit STP) coincide con PSMCT16 del GS.
//  - Los modos de transparencia de PS1 se traducen al registro ALPHA del GS.
//
// Fase 2: cache de texturas por tpage (PSMT4/PSMT8 + CLUT) y texturizado real.

#include <macros.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>

#include "platform/native_gpu.h"
#include "platform/native_log.h"
#include "platform/native_renderer.h"

#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>

#include <stdio.h>
#include <string.h>

#define PS2_TEXTURE_ID_WHITE 1u
#define PS2_TEXTURE_ID_VRAM  2u

// Cada cuantos triangulos se vacia la cola de gsKit para no desbordarla.
#define PS2_TRIANGLES_PER_FLUSH 1024

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
// Buffer contiguo para subir una zona de la VRAM al GS.
global_variable u16 s_uploadPixels[VRAM_WIDTH * VRAM_HEIGHT] __attribute__((aligned(128)));
global_variable GSTEXTURE s_vramDisplayTexture;

global_variable const GrVertex *s_vertices = NULL;
global_variable int s_vertexCount = 0;

global_variable TextureID s_currentTexture = PS2_TEXTURE_ID_WHITE;
global_variable BlendMode s_currentBlendMode = BM_NONE;
global_variable int s_offscreen = 0;
global_variable int s_projectionW = 512;
global_variable int s_projectionH = 240;
global_variable int s_trianglesSinceFlush = 0;

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

internal void Ps2Renderer_FlushQueue(void)
{
	gsKit_queue_exec(s_gs);
	s_trianglesSinceFlush = 0;
}

internal void Ps2Renderer_ApplyBlendMode(BlendMode blendMode)
{
	// GS: Cv = ((A - B) * C >> 7) + D
	// A/B/D: 0 = Cs (fuente), 1 = Cd (destino), 2 = 0.  C: 2 = FIX.
	switch (blendMode)
	{
	case BM_AVERAGE: // 0.5*S + 0.5*D
		s_gs->PrimAlphaEnable = GS_SETTING_ON;
		gsKit_set_primalpha(s_gs, GS_SETREG_ALPHA(0, 1, 2, 1, 0x40), 0);
		break;
	case BM_ADD: // S + D
		s_gs->PrimAlphaEnable = GS_SETTING_ON;
		gsKit_set_primalpha(s_gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
		break;
	case BM_SUBTRACT: // D - S
		s_gs->PrimAlphaEnable = GS_SETTING_ON;
		gsKit_set_primalpha(s_gs, GS_SETREG_ALPHA(1, 0, 2, 2, 0x80), 0);
		break;
	case BM_ADD_QUATER_SOURCE: // D + S/4
		s_gs->PrimAlphaEnable = GS_SETTING_ON;
		gsKit_set_primalpha(s_gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x20), 0);
		break;
	case BM_NONE:
	default:
		s_gs->PrimAlphaEnable = GS_SETTING_OFF;
		break;
	}
}

// Color de depuracion para poligonos texturizados mientras no hay texturas.
internal void Ps2Renderer_TexturedDebugColor(const GrVertex *v, u8 *r, u8 *g, u8 *b)
{
	static const u8 palette[8][3] = {
	    {200, 120, 120}, {120, 200, 120}, {120, 120, 200}, {200, 200, 120},
	    {200, 120, 200}, {120, 200, 200}, {220, 160, 100}, {160, 160, 160},
	};
	const u8 *base = palette[((u16)v->page) & 7];

	// En PS1 el color de un poligono texturizado modula la textura (0x80 = x1).
	*r = (u8)(((int)base[0] * v->r) >> 7 > 255 ? 255 : ((int)base[0] * v->r) >> 7);
	*g = (u8)(((int)base[1] * v->g) >> 7 > 255 ? 255 : ((int)base[1] * v->g) >> 7);
	*b = (u8)(((int)base[2] * v->b) >> 7 > 255 ? 255 : ((int)base[2] * v->b) >> 7);
}

internal u64 Ps2Renderer_VertexColor(const GrVertex *v)
{
	u8 r = v->r;
	u8 g = v->g;
	u8 b = v->b;

	if (s_currentTexture == PS2_TEXTURE_ID_VRAM)
	{
		Ps2Renderer_TexturedDebugColor(v, &r, &g, &b);
	}

	return GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00);
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

	s_gs = gsKit_init_global();
	if (s_gs == NULL)
	{
		return 0;
	}

	s_gs->PSM = GS_PSM_CT24;
	s_gs->PSMZ = GS_PSMZ_16S;
	s_gs->ZBuffering = GS_SETTING_OFF;
	s_gs->DoubleBuffering = GS_SETTING_ON;
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gsKit_init_screen(s_gs);
	gsKit_mode_switch(s_gs, GS_ONESHOT);

	g_windowWidth = s_gs->Width;
	g_windowHeight = s_gs->Height;

	memset(&s_vramDisplayTexture, 0, sizeof(s_vramDisplayTexture));
	s_vramDisplayTexture.PSM = GS_PSM_CT16;
	s_vramDisplayTexture.Filter = GS_FILTER_NEAREST;
	s_vramDisplayTexture.Mem = (u32 *)s_uploadPixels;
	s_vramDisplayTexture.Vram = gsKit_vram_alloc(s_gs, gsKit_texture_size(VRAM_WIDTH, VRAM_HEIGHT, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);

	if (s_vramDisplayTexture.Vram == GSKIT_ALLOC_ERROR)
	{
		Platform_LogError("[CTR PS2] Not enough GS memory for the VRAM display texture\n");
		return 0;
	}

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

void NativeRenderer_SwapWindow(void)
{
	Ps2Renderer_FlushQueue();
	gsKit_finish();
	// Sin esperar al VBlank: el ritmo lo marca VSync() del juego.
	gsKit_flip(s_gs);
}

// ---------------------------------------------------------------------------
// VRAM
// ---------------------------------------------------------------------------

void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h)
{
	// Fase 1: lo que pinta el GS no se copia de vuelta a la VRAM de PS1.
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

void NativeRenderer_PresentVRAMRect(int x, int y, int w, int h)
{
	Ps2Renderer_ClampRect(&x, &y, &w, &h);
	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	for (int row = 0; row < h; row++)
	{
		memcpy(&s_uploadPixels[row * w], &s_vramPixels[(y + row) * VRAM_WIDTH + x], (size_t)w * sizeof(u16));
	}

	s_vramDisplayTexture.Width = w;
	s_vramDisplayTexture.Height = h;
	FlushCache(0);
	gsKit_texture_upload(s_gs, &s_vramDisplayTexture);

	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_clear(s_gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_prim_sprite_texture(s_gs, &s_vramDisplayTexture,
	                          0.0f, 0.0f, 0.0f, 0.0f,
	                          (float)s_gs->Width, (float)s_gs->Height, (float)w, (float)h,
	                          0, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
	Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
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
	// Fase 1: limpia la pantalla entera.
	(void)x;
	(void)y;
	(void)w;
	(void)h;

	const int blendWasOn = (s_gs->PrimAlphaEnable == GS_SETTING_ON);
	s_gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_clear(s_gs, GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00));
	if (blendWasOn)
	{
		Ps2Renderer_ApplyBlendMode(s_currentBlendMode);
	}
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
}

void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY)
{
	int stride = w;

	if (src == NULL)
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
}

void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH)
{
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
	// La VRAM vive en RAM; en la fase 2 aqui se invalidara la cache de texturas.
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
	(void)clipRect;
	(void)displayEnv;
	(void)enable;
}

void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat)
{
	(void)texFormat;
	s_currentTexture = texture;
}

void NativeRenderer_SetOverrideTextureSize(int width, int height)
{
	(void)width;
	(void)height;
}

void NativeRenderer_SetPSXTextureSemiTransPass(int pass)
{
	(void)pass;
}

void NativeRenderer_SetPSXTextureOutputSTP(int enabled)
{
	(void)enabled;
}

void NativeRenderer_SetPSXDrawMaskSet(int maskSet)
{
	(void)maskSet;
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

void NativeRenderer_DrawTriangles(int startVertex, int triangles)
{
	if ((s_gs == NULL) || (s_vertices == NULL) || s_offscreen)
	{
		return;
	}

	const float scaleX = (float)s_gs->Width / (float)s_projectionW;
	const float scaleY = (float)s_gs->Height / (float)s_projectionH;

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

		gsKit_prim_triangle_gouraud_3d(s_gs,
		                               v0->x * scaleX, v0->y * scaleY, 0,
		                               v1->x * scaleX, v1->y * scaleY, 0,
		                               v2->x * scaleX, v2->y * scaleY, 0,
		                               Ps2Renderer_VertexColor(v0),
		                               Ps2Renderer_VertexColor(v1),
		                               Ps2Renderer_VertexColor(v2));

		if (++s_trianglesSinceFlush >= PS2_TRIANGLES_PER_FLUSH)
		{
			Ps2Renderer_FlushQueue();
		}
	}
}

void NativeRenderer_PushDebugLabel(const char *label)
{
	(void)label;
}

void NativeRenderer_PopDebugLabel(void)
{
}
