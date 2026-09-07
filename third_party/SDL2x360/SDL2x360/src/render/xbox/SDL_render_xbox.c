/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2019 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#include "SDL_render.h"

#if SDL_VIDEO_RENDER_D3D && !SDL_RENDER_DISABLED

#include "../../core/xbox/SDL_xbox.h"

#include "SDL_hints.h"
#include "SDL_syswm.h"
#include "SDL_log.h"
#include "SDL_assert.h"
#include "../SDL_sysrender.h"
#include "../SDL_d3dmath.h"
#include "../../video/xbox/SDL_xboxvideo.h"

#define D3D_DEBUG_INFO

/* -------------------------------------------------------------------------- */
/*                               Xbox-only D3D8                               */
/* -------------------------------------------------------------------------- */

typedef struct
{
    SDL_Rect viewport;
    SDL_bool viewport_dirty;
    SDL_Texture* texture;
    SDL_BlendMode blend;
    SDL_bool cliprect_enabled;
    SDL_bool cliprect_enabled_dirty;
    SDL_Rect cliprect;
    SDL_bool cliprect_dirty;
    SDL_bool is_copy_ex;
    DWORD last_color;
    SDL_bool color_dirty;
} D3D_DrawStateCache;

typedef struct
{
    IDirect3D8* d3d;
    IDirect3DDevice8* device;
    UINT adapter;
    D3DPRESENT_PARAMETERS pparams;
    SDL_bool updateSize;
    SDL_bool beginScene;
    SDL_bool enableSeparateAlphaBlend;
    D3DTEXTUREFILTERTYPE scaleMode[8];
    IDirect3DSurface8* defaultRenderTarget;
    IDirect3DSurface8* currentRenderTarget;
    LPDIRECT3DVERTEXBUFFER8 vertexBuffers[8];
    size_t vertexBufferSize[8];
    int currentVertexBuffer;
    SDL_bool reportedVboProblem;
    D3D_DrawStateCache drawstate;
    SDL_bool backbuffer_cleared;
} D3D_RenderData;

typedef struct
{
    SDL_bool dirty;
    int w, h;
    DWORD usage;
    Uint32 format;
    D3DFORMAT d3dfmt;
    IDirect3DTexture8* texture;
    IDirect3DTexture8* staging;
} D3D_TextureRep;

typedef struct
{
    D3D_TextureRep texture;
    D3DTEXTUREFILTERTYPE scaleMode;

    SDL_bool yuv;
    D3D_TextureRep utexture;
    D3D_TextureRep vtexture;
    Uint8* pixels;
    int pitch;
    SDL_Rect locked_rect;
} D3D_TextureData;

typedef struct
{
    float x, y, z;
    DWORD color;
    float u, v;
} Vertex;

/* ---------------------- Helpers & error handling ---------------------- */

static int D3D_SetError(const char* prefix, HRESULT result)
{
    const char* error;

    switch (result) {
    case D3DERR_WRONGTEXTUREFORMAT:        error = "WRONGTEXTUREFORMAT"; break;
    case D3DERR_UNSUPPORTEDCOLOROPERATION: error = "UNSUPPORTEDCOLOROPERATION"; break;
    case D3DERR_UNSUPPORTEDCOLORARG:       error = "UNSUPPORTEDCOLORARG"; break;
    case D3DERR_UNSUPPORTEDALPHAOPERATION: error = "UNSUPPORTEDALPHAOPERATION"; break;
    case D3DERR_UNSUPPORTEDALPHAARG:       error = "UNSUPPORTEDALPHAARG"; break;
    case D3DERR_TOOMANYOPERATIONS:         error = "TOOMANYOPERATIONS"; break;
    case D3DERR_CONFLICTINGTEXTUREFILTER:  error = "CONFLICTINGTEXTUREFILTER"; break;
    case D3DERR_UNSUPPORTEDTEXTUREFILTER:  error = "UNSUPPORTEDTEXTUREFILTER"; break;
    case D3DERR_CONFLICTINGTEXTUREPALETTE: error = "CONFLICTINGTEXTUREPALETTE"; break;
    case D3DERR_DRIVERINTERNALERROR:       error = "DRIVERINTERNALERROR"; break;
    case D3DERR_NOTFOUND:                  error = "NOTFOUND"; break;
    case D3DERR_MOREDATA:                  error = "MOREDATA"; break;
    case D3DERR_DEVICELOST:                error = "DEVICELOST"; break;
    case D3DERR_DEVICENOTRESET:            error = "DEVICENOTRESET"; break;
    case D3DERR_NOTAVAILABLE:              error = "NOTAVAILABLE"; break;
    case D3DERR_OUTOFVIDEOMEMORY:          error = "OUTOFVIDEOMEMORY"; break;
    case D3DERR_INVALIDDEVICE:             error = "INVALIDDEVICE"; break;
    case D3DERR_INVALIDCALL:               error = "INVALIDCALL"; break;
    default:                               error = "UNKNOWN"; break;
    }
    return SDL_SetError("%s: %s", prefix, error);
}

/* -------------------------------------------------------------------------- */
/*      NEW: Interlace stability (flicker filter + half-line offset)         */
/* -------------------------------------------------------------------------- */
static void ApplyInterlaceStability(IDirect3DDevice8* device, const D3DPRESENT_PARAMETERS* p)
{
    if (!device || !p) return;

    /* For interlaced (480i/576i), enable the flicker filter and align to fields.
       For progressive (480p/720p), ensure both are neutral. */
    if (p->Flags & D3DPRESENTFLAG_INTERLACED) {
        /* Range is 0..7 on Xbox; 5 is a good balance */
        IDirect3DDevice8_SetFlickerFilter(device, 5);
        /* Leave softening off by default (0); set to 1 if you still see shimmer */
        IDirect3DDevice8_SetSoftDisplayFilter(device, 0);
        /* Half-line vertical offset to align with alternating fields */
        IDirect3DDevice8_SetScreenSpaceOffset(device, 0.0f, -0.5f);
        SDL_Log("Xbox D3D: Interlaced mode -> FlickerFilter=5, SoftFilter=0, YOffset=-0.5");
    }
    else {
        /* Progressive: neutral settings */
        IDirect3DDevice8_SetFlickerFilter(device, 0);
        IDirect3DDevice8_SetSoftDisplayFilter(device, 0);
        IDirect3DDevice8_SetScreenSpaceOffset(device, 0.0f, 0.0f);
        SDL_Log("Xbox D3D: Progressive mode -> FlickerFilter=0, SoftFilter=0, YOffset=0");
    }
}

static D3DFORMAT PixelFormatToD3DFMT(Uint32 format)
{
    switch (format) {
    case SDL_PIXELFORMAT_RGB565:   return D3DFMT_LIN_R5G6B5;
    case SDL_PIXELFORMAT_RGB888:   return D3DFMT_LIN_X8R8G8B8;
    case SDL_PIXELFORMAT_ARGB8888: return D3DFMT_LIN_A8R8G8B8;
    case SDL_PIXELFORMAT_YV12:
    case SDL_PIXELFORMAT_IYUV:
    case SDL_PIXELFORMAT_NV12:
    case SDL_PIXELFORMAT_NV21:     return D3DFMT_LIN_L8;
    default:                       return D3DFMT_UNKNOWN;
    }
}

static Uint32 D3DFMTToPixelFormat(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_LIN_R5G6B5:   return SDL_PIXELFORMAT_RGB565;
    case D3DFMT_LIN_X8R8G8B8: return SDL_PIXELFORMAT_RGB888;
    case D3DFMT_LIN_A8R8G8B8: return SDL_PIXELFORMAT_ARGB8888;
    default:                  return SDL_PIXELFORMAT_UNKNOWN;
    }
}

static void D3D_InitRenderState(D3D_RenderData* data)
{
    D3DMATRIX matrix;
    IDirect3DDevice8* device = data->device;

    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);

    IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice8_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice8_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);

    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    SDL_zero(matrix);
    matrix.m[0][0] = 1.0f; matrix.m[1][1] = 1.0f; matrix.m[2][2] = 1.0f; matrix.m[3][3] = 1.0f;
    IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &matrix);
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &matrix);

    SDL_memset(data->scaleMode, 0xFF, sizeof(data->scaleMode));
    data->beginScene = SDL_TRUE;

    /* Apply interlace/progressive stability based on current mode */
    ApplyInterlaceStability(device, &data->pparams);
}


static int D3D_Reset(SDL_Renderer* renderer);

/* -------------------------- Renderer activation -------------------------- */

static int D3D_ActivateRenderer(SDL_Renderer* renderer)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    HRESULT result;

    if (data->updateSize) {
        SDL_Window* window = renderer->window;
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        data->pparams.BackBufferWidth = w;
        data->pparams.BackBufferHeight = h;

        if (D3D_Reset(renderer) < 0) {
            return -1;
        }
        data->updateSize = SDL_FALSE;
    }

    if (data->beginScene) {
        result = IDirect3DDevice8_BeginScene(data->device);
        if (result == D3DERR_DEVICELOST) {
            if (D3D_Reset(renderer) < 0) return -1;
            result = IDirect3DDevice8_BeginScene(data->device);
        }
        if (FAILED(result)) return D3D_SetError("BeginScene()", result);
        data->beginScene = SDL_FALSE;

        /* ALWAYS clear the full backbuffer at frame start. */
        {
            const int backw = renderer->target ? renderer->target->w
                : (int)data->pparams.BackBufferWidth;
            const int backh = renderer->target ? renderer->target->h
                : (int)data->pparams.BackBufferHeight;

            const SDL_bool had_scissor = data->drawstate.cliprect_enabled;
            if (had_scissor) {
                IDirect3DDevice8_SetScissors(data->device, 0, FALSE, NULL);
            }

            const D3DVIEWPORT8 whole = (D3DVIEWPORT8){ 0,0,(DWORD)backw,(DWORD)backh,0.0f,1.0f };
            IDirect3DDevice8_SetViewport(data->device, &whole);
            IDirect3DDevice8_Clear(data->device, 0, NULL, D3DCLEAR_TARGET,
                D3DCOLOR_ARGB(255, 0, 0, 0), 0.0f, 0);

            /* Mark state dirty so later code restores app viewport/scissor. */
            data->drawstate.viewport_dirty = SDL_TRUE;
            if (had_scissor) {
                data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
                data->drawstate.cliprect_dirty = SDL_TRUE;
            }
        }
    }
    return 0;
}


/* ------------------------------ Window event ----------------------------- */

static void D3D_WindowEvent(SDL_Renderer* renderer, const SDL_WindowEvent* event)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    if (event->event == SDL_WINDOWEVENT_SIZE_CHANGED ||
        event->event == SDL_WINDOWEVENT_RESIZED) {
        data->updateSize = SDL_TRUE;
    }
}

/* -------------------------------- Blending ------------------------------- */

static D3DBLEND GetBlendFunc(SDL_BlendFactor factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:                return D3DBLEND_ZERO;
    case SDL_BLENDFACTOR_ONE:                 return D3DBLEND_ONE;
    case SDL_BLENDFACTOR_SRC_COLOR:           return D3DBLEND_SRCCOLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR: return D3DBLEND_INVSRCCOLOR;
    case SDL_BLENDFACTOR_SRC_ALPHA:           return D3DBLEND_SRCALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA: return D3DBLEND_INVSRCALPHA;
    case SDL_BLENDFACTOR_DST_COLOR:           return D3DBLEND_DESTCOLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR: return D3DBLEND_INVDESTCOLOR;
    case SDL_BLENDFACTOR_DST_ALPHA:           return D3DBLEND_DESTALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA: return D3DBLEND_INVDESTALPHA;
    default:                                  return D3DBLEND_ONE;
    }
}

static SDL_bool D3D_SupportsBlendMode(SDL_Renderer* renderer, SDL_BlendMode blendMode)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;

    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    if (colorOperation != SDL_BLENDOPERATION_ADD || alphaOperation != SDL_BLENDOPERATION_ADD) {
        return SDL_FALSE;
    }

#define FACTOR_SUPPORTED(f) \
    ((f) == SDL_BLENDFACTOR_ZERO || (f) == SDL_BLENDFACTOR_ONE || \
     (f) == SDL_BLENDFACTOR_SRC_COLOR || (f) == SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR || \
     (f) == SDL_BLENDFACTOR_SRC_ALPHA || (f) == SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA || \
     (f) == SDL_BLENDFACTOR_DST_COLOR || (f) == SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR || \
     (f) == SDL_BLENDFACTOR_DST_ALPHA || (f) == SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA)

    if (!FACTOR_SUPPORTED(srcColorFactor) ||
        !FACTOR_SUPPORTED(srcAlphaFactor) ||
        !FACTOR_SUPPORTED(dstColorFactor) ||
        !FACTOR_SUPPORTED(dstAlphaFactor)) {
        return SDL_FALSE;
    }

    if ((srcColorFactor != srcAlphaFactor || dstColorFactor != dstAlphaFactor) &&
        !data->enableSeparateAlphaBlend) {
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

/* ------------------------------ Textures --------------------------------- */

static int
D3D_CreateTextureRep(IDirect3DDevice8* device, D3D_TextureRep* texture,
    DWORD usage, Uint32 format, D3DFORMAT d3dfmt, int w, int h)
{
    HRESULT result;

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    texture->dirty = SDL_FALSE;
    texture->w = w;
    texture->h = h;
    texture->usage = usage;
    texture->format = format;
    texture->d3dfmt = d3dfmt;

    result = IDirect3DDevice8_CreateTexture(device, w, h, 1, usage, d3dfmt,
        D3DPOOL_DEFAULT, &texture->texture);
    if (FAILED(result)) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "CreateTexture %dx%d fmt=0x%08x (DEFAULT)", w, h, (unsigned int)d3dfmt);
        msg[sizeof(msg) - 1] = '\0';
        return D3D_SetError(msg, result);
    }
    return 0;
}

static int
D3D_CreateStagingTexture(IDirect3DDevice8* device, D3D_TextureRep* texture)
{
    HRESULT result;
    int w = texture->w;
    int h = texture->h;
    D3DFORMAT fmt = texture->d3dfmt;

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    if (texture->staging) {
        D3DSURFACE_DESC desc;
        IDirect3DTexture8_GetLevelDesc(texture->staging, 0, &desc);
        if ((int)desc.Width != w || (int)desc.Height != h || desc.Format != fmt) {
            IDirect3DTexture8_Release(texture->staging);
            texture->staging = NULL;
        }
    }

    if (texture->staging == NULL) {
        result = IDirect3DDevice8_CreateTexture(device, w, h, 1, 0, fmt, D3DPOOL_SYSTEMMEM, &texture->staging);
        if (FAILED(result)) {
            char msg[128];
            _snprintf(msg, sizeof(msg), "CreateTexture SYSTEMMEM %dx%d fmt=0x%08x", w, h, (unsigned int)fmt);
            msg[sizeof(msg) - 1] = '\0';
            return D3D_SetError(msg, result);
        }
    }

    return 0;
}

static int
D3D_RecreateTextureRep(IDirect3DDevice8* device, D3D_TextureRep* texture)
{
    (void)device;

    if (texture->texture) {
        IDirect3DTexture8_Release(texture->texture);
        texture->texture = NULL;
    }

    if (texture->staging) {
        D3DSURFACE_DESC desc;
        IDirect3DTexture8_GetLevelDesc(texture->staging, 0, &desc);

        if ((int)desc.Width != texture->w || (int)desc.Height != texture->h || desc.Format != texture->d3dfmt) {
            IDirect3DTexture8_Release(texture->staging);
            texture->staging = NULL;
        }
    }

    texture->dirty = SDL_TRUE;
    return 0;
}

static int
D3D_UpdateTextureRep(IDirect3DDevice8* device, D3D_TextureRep* texture,
    int x, int y, int w, int h, const void* pixels, int pitch)
{
    RECT d3drect;
    D3DLOCKED_RECT locked;
    const Uint8* src;
    Uint8* dst;
    int row, need;
    HRESULT result;
    int texw = texture->w;
    int texh = texture->h;
    int bpp = SDL_BYTESPERPIXEL(texture->format);

    if (!pixels || w <= 0 || h <= 0 || bpp <= 0) {
        return 0;
    }

    if (D3D_CreateStagingTexture(device, texture) < 0) {
        return -1;
    }

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > texw) w = texw - x;
    if (y + h > texh) h = texh - y;
    if (w <= 0 || h <= 0) {
        return 0;
    }

    d3drect.left = x;
    d3drect.top = y;
    d3drect.right = x + w;
    d3drect.bottom = y + h;

    result = IDirect3DTexture8_LockRect(texture->staging, 0, &locked, &d3drect, 0);
    if (FAILED(result)) {
        return D3D_SetError("LockRect()", result);
    }

    need = w * bpp;
    if (pitch < need || (int)locked.Pitch < need) {
        IDirect3DTexture8_UnlockRect(texture->staging, 0);
        return SDL_SetError("UpdateTextureRep: insufficient pitch (need %d, src %d, dst %d)", need, pitch, (int)locked.Pitch);
    }

    src = (const Uint8*)pixels;
    dst = (Uint8*)locked.pBits;

    if (pitch == (int)locked.Pitch && need == w * bpp) {
        SDL_memcpy(dst, src, (size_t)need * (size_t)h);
    }
    else {
        for (row = 0; row < h; ++row) {
            SDL_memcpy(dst, src, (size_t)need);
            src += pitch;
            dst += locked.Pitch;
        }
    }

    result = IDirect3DTexture8_UnlockRect(texture->staging, 0);
    if (FAILED(result)) {
        return D3D_SetError("UnlockRect()", result);
    }

    texture->dirty = SDL_TRUE;
    return 0;
}


static void D3D_DestroyTextureRep(D3D_TextureRep* texture)
{
    if (!texture) return;

    if (texture->texture) {
        IDirect3DTexture8_Release(texture->texture);
        texture->texture = NULL;
    }
    if (texture->staging) {
        IDirect3DTexture8_Release(texture->staging);
        texture->staging = NULL;
    }
    texture->dirty = SDL_FALSE;
}

/* --------------------------- SDL texture hooks --------------------------- */

static int D3D_CreateTexture(SDL_Renderer* renderer, SDL_Texture* texture)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* texturedata;
    DWORD usage;
    D3DFORMAT d3dfmt;
    int w = texture->w;
    int h = texture->h;

    texturedata = (D3D_TextureData*)SDL_calloc(1, sizeof(*texturedata));
    if (!texturedata) return SDL_OutOfMemory();

    texturedata->scaleMode = (texture->scaleMode == SDL_ScaleModeNearest) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    texture->driverdata = texturedata;

    d3dfmt = PixelFormatToD3DFMT(texture->format);
    if (d3dfmt == (D3DFORMAT)0 || w <= 0 || h <= 0) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "CreateTexture invalid fmt=0x%08x %dx%d", (unsigned int)texture->format, w, h);
        msg[sizeof(msg) - 1] = '\0';
        SDL_free(texturedata);
        texture->driverdata = NULL;
        return D3D_SetError(msg, D3DERR_INVALIDCALL);
    }

    usage = (texture->access == SDL_TEXTUREACCESS_TARGET) ? D3DUSAGE_RENDERTARGET : 0;

    if (D3D_CreateTextureRep(data->device, &texturedata->texture, usage, texture->format, d3dfmt, w, h) < 0) {
        SDL_free(texturedata);
        texture->driverdata = NULL;
        return -1;
    }

    if (texture->format == SDL_PIXELFORMAT_YV12 || texture->format == SDL_PIXELFORMAT_IYUV) {
        int hw = (w + 1) / 2;
        int hh = (h + 1) / 2;
        texturedata->yuv = SDL_TRUE;

        if (D3D_CreateTextureRep(data->device, &texturedata->utexture, usage, texture->format, d3dfmt, hw, hh) < 0) {
            D3D_DestroyTextureRep(&texturedata->texture);
            SDL_free(texturedata);
            texture->driverdata = NULL;
            return -1;
        }
        if (D3D_CreateTextureRep(data->device, &texturedata->vtexture, usage, texture->format, d3dfmt, hw, hh) < 0) {
            D3D_DestroyTextureRep(&texturedata->utexture);
            D3D_DestroyTextureRep(&texturedata->texture);
            SDL_free(texturedata);
            texture->driverdata = NULL;
            return -1;
        }
    }

    return 0;
}

static int D3D_RecreateTexture(SDL_Renderer* renderer, SDL_Texture* texture)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;

    if (!texturedata) return 0;

    if (D3D_RecreateTextureRep(data->device, &texturedata->texture) < 0) return -1;
    if (D3D_CreateTextureRep(data->device, &texturedata->texture,
        texturedata->texture.usage, texturedata->texture.format,
        texturedata->texture.d3dfmt,
        texturedata->texture.w, texturedata->texture.h) < 0) return -1;
    texturedata->texture.dirty = SDL_TRUE;

    if (texturedata->yuv) {
        if (D3D_RecreateTextureRep(data->device, &texturedata->utexture) < 0) return -1;
        if (D3D_CreateTextureRep(data->device, &texturedata->utexture,
            texturedata->utexture.usage, texturedata->utexture.format,
            texturedata->utexture.d3dfmt,
            texturedata->utexture.w, texturedata->utexture.h) < 0) return -1;
        texturedata->utexture.dirty = SDL_TRUE;

        if (D3D_RecreateTextureRep(data->device, &texturedata->vtexture) < 0) return -1;
        if (D3D_CreateTextureRep(data->device, &texturedata->vtexture,
            texturedata->vtexture.usage, texturedata->vtexture.format,
            texturedata->vtexture.d3dfmt,
            texturedata->vtexture.w, texturedata->vtexture.h) < 0) return -1;
        texturedata->vtexture.dirty = SDL_TRUE;
    }

    return 0;
}

static int
D3D_UpdateTexture(SDL_Renderer* renderer, SDL_Texture* texture,
    const SDL_Rect* rect, const void* pixels, int pitch)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;

    SDL_Rect r;
    int texw, texh;

    if (!texturedata) { SDL_SetError("Texture is not currently available"); return -1; }
    if (!pixels) return 0;

    texw = texture->w;
    texh = texture->h;

    if (rect == NULL) { r.x = 0; r.y = 0; r.w = texw; r.h = texh; }
    else { r = *rect; }

    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > texw) r.w = texw - r.x;
    if (r.y + r.h > texh) r.h = texh - r.y;
    if (r.w <= 0 || r.h <= 0) return 0;

    if (!texturedata->yuv) {
        if (D3D_UpdateTextureRep(data->device, &texturedata->texture, r.x, r.y, r.w, r.h, pixels, pitch) < 0) return -1;
        return 0;
    }

    /* Planar YUV case (I420/YV12-like): Y followed by U and V planes in src */
    {
        const Uint8* base = (const Uint8*)pixels;

        const Uint8* src_after_Y = base + (size_t)r.h * (size_t)pitch;

        const int chroma_w = (r.w + 1) / 2;
        const int chroma_h = (r.h + 1) / 2;
        const int chroma_pitch = (pitch + 1) / 2;

        D3D_TextureRep* first_chroma = (texture->format == SDL_PIXELFORMAT_YV12) ? &texturedata->vtexture : &texturedata->utexture;
        D3D_TextureRep* second_chroma = (texture->format == SDL_PIXELFORMAT_YV12) ? &texturedata->utexture : &texturedata->vtexture;

        if (D3D_UpdateTextureRep(data->device, &texturedata->texture, r.x, r.y, r.w, r.h, base, pitch) < 0) return -1;

        if (chroma_w > 0 && chroma_h > 0) {
            const int cx = r.x / 2, cy = r.y / 2;

            /* U (or V) */
            if (D3D_UpdateTextureRep(data->device, first_chroma, cx, cy, chroma_w, chroma_h, src_after_Y, chroma_pitch) < 0) return -1;

            /* V (or U), lives after the first chroma plane */
            {
                const Uint8* src_second = src_after_Y + (size_t)chroma_h * (size_t)chroma_pitch;
                if (D3D_UpdateTextureRep(data->device, second_chroma, cx, cy, chroma_w, chroma_h, src_second, chroma_pitch) < 0) return -1;
            }
        }
    }

    return 0;
}


static int
D3D_UpdateTextureYUV(SDL_Renderer* renderer, SDL_Texture* texture,
    const SDL_Rect* rect,
    const Uint8* Yplane, int Ypitch,
    const Uint8* Uplane, int Upitch,
    const Uint8* Vplane, int Vpitch)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;

    SDL_Rect r;
    int texw, texh;

    if (!texturedata) { SDL_SetError("Texture is not currently available"); return -1; }
    if (!Yplane || !Uplane || !Vplane) return 0;

    texw = texture->w; texh = texture->h;

    if (rect == NULL) { r.x = 0; r.y = 0; r.w = texw; r.h = texh; }
    else { r = *rect; }

    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > texw) r.w = texw - r.x;
    if (r.y + r.h > texh) r.h = texh - r.y;
    if (r.w <= 0 || r.h <= 0) return 0;

    if (D3D_UpdateTextureRep(data->device, &texturedata->texture, r.x, r.y, r.w, r.h, Yplane, Ypitch) < 0) return -1;

    {
        const int cx = r.x / 2, cy = r.y / 2;
        const int cw = (r.w + 1) / 2, ch = (r.h + 1) / 2;

        if (cw > 0 && ch > 0) {
            if (D3D_UpdateTextureRep(data->device, &texturedata->utexture, cx, cy, cw, ch, Uplane, Upitch) < 0) return -1;
            if (D3D_UpdateTextureRep(data->device, &texturedata->vtexture, cx, cy, cw, ch, Vplane, Vpitch) < 0) return -1;
        }
    }

    return 0;
}

static int
D3D_LockTexture(SDL_Renderer* renderer, SDL_Texture* texture,
    const SDL_Rect* rect, void** pixels, int* pitch)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;
    IDirect3DDevice8* device = data->device;

    SDL_Rect r;
    int texw, texh;

    if (!pixels || !pitch) { SDL_SetError("Invalid output pointers"); return -1; }
    *pixels = NULL; *pitch = 0;

    if (!texturedata) { SDL_SetError("Texture is not currently available"); return -1; }

    texw = texture->w; texh = texture->h;

    if (rect == NULL) { r.x = 0; r.y = 0; r.w = texw; r.h = texh; }
    else { r = *rect; }

    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > texw) r.w = texw - r.x;
    if (r.y + r.h > texh) r.h = texh - r.y;
    if (r.w <= 0 || r.h <= 0) { texturedata->locked_rect = r; return 0; }

    texturedata->locked_rect = r;

    /* Disallow locking planar YUV; callers must use UpdateTextureYUV */
    if (texturedata->yuv) {
        return SDL_SetError("Planar YUV textures must be updated with SDL_UpdateYUVTexture / UpdateTextureYUV");
    }

    {
        RECT d3drect;
        D3DLOCKED_RECT locked;
        HRESULT hr;

        if (D3D_CreateStagingTexture(device, &texturedata->texture) < 0) return -1;

        d3drect.left = r.x; d3drect.top = r.y; d3drect.right = r.x + r.w; d3drect.bottom = r.y + r.h;

        hr = IDirect3DTexture8_LockRect(texturedata->texture.staging, 0, &locked, &d3drect, 0);
        if (FAILED(hr)) return D3D_SetError("LockRect()", hr);

        *pixels = locked.pBits;
        *pitch = (int)locked.Pitch;
        return 0;
    }
}


static void
D3D_UnlockTexture(SDL_Renderer* renderer, SDL_Texture* texture)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;

    if (!texturedata) return;

    {
        SDL_Rect r = texturedata->locked_rect;
        int texw = texture->w, texh = texture->h;

        if (r.w <= 0 || r.h <= 0) return;
        if (r.x < 0) { r.w += r.x; r.x = 0; }
        if (r.y < 0) { r.h += r.y; r.y = 0; }
        if (r.x + r.w > texw) r.w = texw - r.x;
        if (r.y + r.h > texh) r.h = texh - r.y;
        if (r.w <= 0 || r.h <= 0) return;
        texturedata->locked_rect = r;
    }

    if (texturedata->yuv) {
        const SDL_Rect* rect = &texturedata->locked_rect;
        void* pixels = NULL;
        int pitch = 0;

        if (!texturedata->pixels || texturedata->pitch <= 0) return;

        pixels = (void*)((Uint8*)texturedata->pixels +
            (size_t)rect->y * (size_t)texturedata->pitch +
            (size_t)rect->x);
        pitch = texturedata->pitch;

        D3D_UpdateTexture(renderer, texture, rect, pixels, pitch);
        return;
    }
    else {
        if (texturedata->texture.staging) {
            IDirect3DTexture8_UnlockRect(texturedata->texture.staging, 0);
            texturedata->texture.dirty = SDL_TRUE;
            if (data->drawstate.texture == texture) {
                data->drawstate.texture = NULL;
            }
        }
        return;
    }
}

/* ------------------------- GPU upload & binding -------------------------- */

static HRESULT D3D8_UpdateTexture(IDirect3DTexture8* srcTexture, IDirect3DTexture8* dstTexture)
{
    LPDIRECT3DSURFACE8 srcSurface = NULL;
    LPDIRECT3DSURFACE8 dstSurface = NULL;
    IDirect3DDevice8* device = NULL;
    HRESULT hr;

    hr = IDirect3DTexture8_GetSurfaceLevel(srcTexture, 0, &srcSurface);
    if (FAILED(hr)) return hr;

    hr = IDirect3DTexture8_GetSurfaceLevel(dstTexture, 0, &dstSurface);
    if (FAILED(hr)) {
        IDirect3DSurface8_Release(srcSurface);
        return hr;
    }

    hr = IDirect3DBaseTexture8_GetDevice((IDirect3DBaseTexture8*)dstTexture, &device);
    if (FAILED(hr)) {
        IDirect3DSurface8_Release(dstSurface);
        IDirect3DSurface8_Release(srcSurface);
        return hr;
    }

    hr = IDirect3DDevice8_CopyRects(device, srcSurface, NULL, 0, dstSurface, NULL);

    IDirect3DDevice8_Release(device);
    IDirect3DSurface8_Release(dstSurface);
    IDirect3DSurface8_Release(srcSurface);

    return hr;
}

static int UpdateDirtyTexture(IDirect3DDevice8* device, D3D_TextureRep* texture)
{
    HRESULT hr;

    if (!texture || !texture->staging || !texture->dirty) {
        return 0;
    }

    if (!texture->texture) {
        hr = IDirect3DDevice8_CreateTexture(device, texture->w, texture->h, 1,
            texture->usage, texture->d3dfmt,
            D3DPOOL_DEFAULT, &texture->texture);
        if (FAILED(hr)) {
            return D3D_SetError("CreateTexture(D3DPOOL_DEFAULT)", hr);
        }
    }
    else {
        D3DSURFACE_DESC sdesc, ddesc;
        if (SUCCEEDED(IDirect3DTexture8_GetLevelDesc(texture->staging, 0, &sdesc)) &&
            SUCCEEDED(IDirect3DTexture8_GetLevelDesc(texture->texture, 0, &ddesc))) {
            if ((sdesc.Width != ddesc.Width) || (sdesc.Height != ddesc.Height) || (sdesc.Format != ddesc.Format)) {
                IDirect3DTexture8_Release(texture->texture);
                texture->texture = NULL;
                hr = IDirect3DDevice8_CreateTexture(device, texture->w, texture->h, 1,
                    texture->usage, texture->d3dfmt,
                    D3DPOOL_DEFAULT, &texture->texture);
                if (FAILED(hr)) {
                    return D3D_SetError("CreateTexture(D3DPOOL_DEFAULT)", hr);
                }
            }
        }
    }

    hr = D3D8_UpdateTexture(texture->staging, texture->texture);
    if (FAILED(hr)) {
        return D3D_SetError("UpdateTexture()", hr);
    }

    texture->dirty = SDL_FALSE;
    return 0;
}

static int BindTextureRep(IDirect3DDevice8* device, D3D_TextureRep* texture, DWORD sampler)
{
    HRESULT hr;

    if (!device || !texture) return D3D_SetError("BindTextureRep(): invalid args", D3DERR_INVALIDCALL);

    if (UpdateDirtyTexture(device, texture) < 0) return -1;

    if (!texture->texture) return D3D_SetError("BindTextureRep(): no GPU texture", D3DERR_INVALIDCALL);

    hr = IDirect3DDevice8_SetTexture(device, sampler, (IDirect3DBaseTexture8*)texture->texture);
    if (FAILED(hr)) return D3D_SetError("SetTexture()", hr);

    return 0;
}

static void UpdateTextureScaleMode(D3D_RenderData* data, D3D_TextureData* texturedata, unsigned index)
{
    if (!data || !texturedata) return;

    if (texturedata->scaleMode != data->scaleMode[index]) {
        IDirect3DDevice8_SetTextureStageState(data->device, index, D3DTSS_MINFILTER, texturedata->scaleMode);
        IDirect3DDevice8_SetTextureStageState(data->device, index, D3DTSS_MAGFILTER, texturedata->scaleMode);
        data->scaleMode[index] = texturedata->scaleMode;
    }

    IDirect3DDevice8_SetTextureStageState(data->device, index, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice8_SetTextureStageState(data->device, index, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(data->device, index, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
}

static int SetupTextureState(D3D_RenderData* data, SDL_Texture* texture)
{
    D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;

    if (!texturedata) { SDL_SetError("Texture is not currently available"); return -1; }

    UpdateTextureScaleMode(data, texturedata, 0);
    if (BindTextureRep(data->device, &texturedata->texture, 0) < 0) return -1;

    if (texturedata->yuv) {
        UpdateTextureScaleMode(data, texturedata, 1);
        UpdateTextureScaleMode(data, texturedata, 2);

        if (BindTextureRep(data->device, &texturedata->utexture, 1) < 0) return -1;
        if (BindTextureRep(data->device, &texturedata->vtexture, 2) < 0) return -1;

        IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

        IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    else {
        IDirect3DDevice8_SetTexture(data->device, 1, NULL);
        IDirect3DDevice8_SetTexture(data->device, 2, NULL);
        IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    return 0;
}

/* -------------------------- State management ----------------------------- */

static int SetDrawState(D3D_RenderData* data, const SDL_RenderCommand* cmd)
{
    const SDL_bool was_copy_ex = data->drawstate.is_copy_ex;
    const SDL_bool is_copy_ex = (cmd->command == SDL_RENDERCMD_COPY_EX);
    SDL_Texture* texture = cmd->data.draw.texture;
    const SDL_BlendMode blend = cmd->data.draw.blend;

    if (texture != data->drawstate.texture) {
        D3D_TextureData* oldtex = data->drawstate.texture ? (D3D_TextureData*)data->drawstate.texture->driverdata : NULL;
        D3D_TextureData* newtex = texture ? (D3D_TextureData*)texture->driverdata : NULL;

        if (texture == NULL) {
            IDirect3DDevice8_SetTexture(data->device, 0, NULL);
        }
        if ((!newtex || !newtex->yuv) && (oldtex && oldtex->yuv)) {
            IDirect3DDevice8_SetTexture(data->device, 1, NULL);
            IDirect3DDevice8_SetTexture(data->device, 2, NULL);
            IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

        if (texture) {
            if (SetupTextureState(data, texture) < 0) return -1;
        }

        data->drawstate.texture = texture;
    }
    else if (texture) {
        D3D_TextureData* texdata = (D3D_TextureData*)texture->driverdata;
        UpdateDirtyTexture(data->device, &texdata->texture);
        if (texdata->yuv) {
            UpdateDirtyTexture(data->device, &texdata->utexture);
            UpdateDirtyTexture(data->device, &texdata->vtexture);
        }
    }

    if (blend != data->drawstate.blend) {
        if (blend == SDL_BLENDMODE_NONE) {
            IDirect3DDevice8_SetRenderState(data->device, D3DRS_ALPHABLENDENABLE, FALSE);
        }
        else {
            IDirect3DDevice8_SetRenderState(data->device, D3DRS_ALPHABLENDENABLE, TRUE);
            IDirect3DDevice8_SetRenderState(data->device, D3DRS_SRCBLEND,
                GetBlendFunc(SDL_GetBlendModeSrcColorFactor(blend)));
            IDirect3DDevice8_SetRenderState(data->device, D3DRS_DESTBLEND,
                GetBlendFunc(SDL_GetBlendModeDstColorFactor(blend)));
        }
        data->drawstate.blend = blend;
    }

    if (is_copy_ex != was_copy_ex) {
        if (!is_copy_ex) {
            const Float4X4 m = MatrixIdentity();
            IDirect3DDevice8_SetTransform(data->device, D3DTS_VIEW, (const D3DMATRIX*)&m);
        }
        data->drawstate.is_copy_ex = is_copy_ex;
    }

    if (data->drawstate.viewport_dirty) {
        const SDL_Rect* vp = &data->drawstate.viewport;
        const D3DVIEWPORT8 d3dvp = (D3DVIEWPORT8){ (DWORD)vp->x, (DWORD)vp->y, (DWORD)vp->w, (DWORD)vp->h, 0.0f, 1.0f };
        IDirect3DDevice8_SetViewport(data->device, &d3dvp);

        if (vp->w && vp->h) {
            D3DMATRIX proj; SDL_zero(proj);
            proj.m[0][0] = 2.0f / (float)vp->w;
            proj.m[1][1] = -2.0f / (float)vp->h;
            proj.m[2][2] = 1.0f;
            proj.m[3][0] = -1.0f;
            proj.m[3][1] = 1.0f;
            proj.m[3][3] = 1.0f;
            IDirect3DDevice8_SetTransform(data->device, D3DTS_PROJECTION, &proj);
        }
        data->drawstate.viewport_dirty = SDL_FALSE;
    }

    if (data->drawstate.cliprect_enabled_dirty) {
        data->drawstate.cliprect_enabled_dirty = SDL_FALSE;
    }

    if (data->drawstate.cliprect_dirty) {
        const SDL_Rect* vp = &data->drawstate.viewport;
        const SDL_Rect* rect = &data->drawstate.cliprect;
        const D3DRECT d3drect = { vp->x + rect->x, vp->y + rect->y,
                                  vp->x + rect->x + rect->w, vp->y + rect->y + rect->h };
        const BOOL enable = (data->drawstate.cliprect_enabled && rect->w > 0 && rect->h > 0) ? TRUE : FALSE;
        IDirect3DDevice8_SetScissors(data->device, enable ? 1 : 0, FALSE, enable ? &d3drect : NULL);
        data->drawstate.cliprect_dirty = SDL_FALSE;
    }

    return 0;
}


/* -------------------------- Render command queue ------------------------- */

static int D3D_QueueSetViewport(SDL_Renderer* renderer, SDL_RenderCommand* cmd) { (void)renderer; (void)cmd; return 0; }
static int D3D_QueueSetDrawColor(SDL_Renderer* r, SDL_RenderCommand* c) { (void)r; (void)c; return 0; }

static int
D3D_QueueDrawPoints(SDL_Renderer* renderer, SDL_RenderCommand* cmd, const SDL_FPoint* points, int count)
{
    const DWORD color = D3DCOLOR_ARGB(cmd->data.draw.a, cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b);
    const size_t vertslen = (size_t)count * sizeof(Vertex);
    Vertex* verts;

    if (count <= 0) { cmd->data.draw.count = 0; return 0; }

    verts = (Vertex*)SDL_AllocateRenderVertices(renderer, vertslen, 0, &cmd->data.draw.first);
    if (!verts) return -1;

    cmd->data.draw.count = count;

    for (int i = 0; i < count; i++, verts++, points++) {
        verts->x = points->x - 0.5f;
        verts->y = points->y - 0.5f;
        verts->z = 0.0f;
        verts->color = color;
        verts->u = 0.0f; verts->v = 0.0f;
    }

    return 0;
}


static int D3D_QueueDrawLines(SDL_Renderer* r, SDL_RenderCommand* c,
    const SDL_FPoint* p, int n) {
    return D3D_QueueDrawPoints(r, c, p, n);
}



static int
D3D_QueueFillRects(SDL_Renderer* renderer, SDL_RenderCommand* cmd, const SDL_FRect* rects, int count)
{
    const DWORD color = D3DCOLOR_ARGB(cmd->data.draw.a, cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b);
    if (count <= 0) { cmd->data.draw.count = 0; return 0; }

    const size_t vertslen = (size_t)count * sizeof(Vertex) * 4;
    Vertex* verts = (Vertex*)SDL_AllocateRenderVertices(renderer, vertslen, 0, &cmd->data.draw.first);
    if (!verts) return -1;

    cmd->data.draw.count = count;

    for (int i = 0; i < count; i++) {
        const SDL_FRect* rect = &rects[i];
        const float minx = rect->x - 0.5f;
        const float miny = rect->y - 0.5f;
        const float maxx = rect->x + rect->w - 0.5f;
        const float maxy = rect->y + rect->h - 0.5f;

        verts->x = minx; verts->y = miny; verts->z = 0.0f; verts->color = color; verts->u = 0.0f; verts->v = 0.0f; verts++;
        verts->x = maxx; verts->y = miny; verts->z = 0.0f; verts->color = color; verts->u = 0.0f; verts->v = 0.0f; verts++;
        verts->x = maxx; verts->y = maxy; verts->z = 0.0f; verts->color = color; verts->u = 0.0f; verts->v = 0.0f; verts++;
        verts->x = minx; verts->y = maxy; verts->z = 0.0f; verts->color = color; verts->u = 0.0f; verts->v = 0.0f; verts++;
    }

    return 0;
}

static int
D3D_QueueCopy(SDL_Renderer* renderer, SDL_RenderCommand* cmd, SDL_Texture* texture,
    const SDL_Rect* srcrect, const SDL_FRect* dstrect)
{
    const DWORD color = D3DCOLOR_ARGB(cmd->data.draw.a, cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b);

    if (!texture || !dstrect || dstrect->w <= 0.0f || dstrect->h <= 0.0f) { cmd->data.draw.count = 0; return 0; }

    float minx, miny, maxx, maxy;
    float minu, maxu, minv, maxv;

    const size_t vertslen = sizeof(Vertex) * 4;
    Vertex* verts = (Vertex*)SDL_AllocateRenderVertices(renderer, vertslen, 0, &cmd->data.draw.first);
    if (!verts) return -1;

    cmd->data.draw.count = 1;

    minx = dstrect->x - 0.5f;
    miny = dstrect->y - 0.5f;
    maxx = dstrect->x + dstrect->w - 0.5f;
    maxy = dstrect->y + dstrect->h - 0.5f;

    if (srcrect) {
        if (srcrect->w <= 0 || srcrect->h <= 0) { cmd->data.draw.count = 0; return 0; }
        minu = (float)srcrect->x + 0.5f;
        maxu = (float)(srcrect->x + srcrect->w) - 0.5f;
        minv = (float)srcrect->y + 0.5f;
        maxv = (float)(srcrect->y + srcrect->h) - 0.5f;
        if (maxu < minu) maxu = minu;
        if (maxv < minv) maxv = minv;
    }
    else {
        minu = 0.5f; minv = 0.5f;
        maxu = (float)texture->w - 0.5f;
        maxv = (float)texture->h - 0.5f;
    }

    verts->x = minx; verts->y = miny; verts->z = 0.0f; verts->color = color; verts->u = minu; verts->v = minv; verts++;
    verts->x = maxx; verts->y = miny; verts->z = 0.0f; verts->color = color; verts->u = maxu; verts->v = minv; verts++;
    verts->x = maxx; verts->y = maxy; verts->z = 0.0f; verts->color = color; verts->u = maxu; verts->v = maxv; verts++;
    verts->x = minx; verts->y = maxy; verts->z = 0.0f; verts->color = color; verts->u = minu; verts->v = maxv; verts++;

    return 0;
}

static int
D3D_QueueCopyEx(SDL_Renderer* renderer, SDL_RenderCommand* cmd, SDL_Texture* texture,
    const SDL_Rect* srcquad, const SDL_FRect* dstrect,
    const double angle, const SDL_FPoint* center, const SDL_RendererFlip flip,
    float scale_x, float scale_y)
{
    const DWORD color = D3DCOLOR_ARGB(cmd->data.draw.a, cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b);
    float minx, miny, maxx, maxy;
    float minu, maxu, minv, maxv;
    SDL_FPoint ctr = { 0.0f, 0.0f };

    if (!texture || !srcquad || !dstrect || dstrect->w <= 0.0f || dstrect->h <= 0.0f) {
        cmd->data.draw.count = 0;
        return 0;
    }
    if (center) ctr = *center;

    const size_t vertslen = sizeof(Vertex) * 5;
    Vertex* verts = (Vertex*)SDL_AllocateRenderVertices(renderer, vertslen, 0, &cmd->data.draw.first);
    if (!verts) return -1;
    cmd->data.draw.count = 1;

    const float w = dstrect->w * (scale_x != 0.0f ? scale_x : 1.0f);
    const float h = dstrect->h * (scale_y != 0.0f ? scale_y : 1.0f);

    minx = -ctr.x; maxx = w - ctr.x;
    miny = -ctr.y; maxy = h - ctr.y;

    minu = (float)srcquad->x + 0.5f;
    maxu = (float)(srcquad->x + srcquad->w) - 0.5f;
    minv = (float)srcquad->y + 0.5f;
    maxv = (float)(srcquad->y + srcquad->h) - 0.5f;

    if (flip & SDL_FLIP_HORIZONTAL) { float t = minu; minu = maxu; maxu = t; }
    if (flip & SDL_FLIP_VERTICAL) { float t = minv; minv = maxv; maxv = t; }

    verts->x = minx; verts->y = miny; verts->z = 0.0f; verts->color = color; verts->u = minu; verts->v = minv; verts++;
    verts->x = maxx; verts->y = miny; verts->z = 0.0f; verts->color = color; verts->u = maxu; verts->v = minv; verts++;
    verts->x = maxx; verts->y = maxy; verts->z = 0.0f; verts->color = color; verts->u = maxu; verts->v = maxv; verts++;
    verts->x = minx; verts->y = maxy; verts->z = 0.0f; verts->color = color; verts->u = minu; verts->v = maxv; verts++;

    /* pack translation + rotation (radians) in the sentinel vertex like before */
    verts->x = dstrect->x + ctr.x - 0.5f;
    verts->y = dstrect->y + ctr.y - 0.5f;
    verts->z = (float)(angle * (3.14159265358979323846 / 180.0)); /* avoid M_PI dependency */
    verts->color = 0; verts->u = 0.0f; verts->v = 0.0f;

    return 0;
}


static int
D3D_RunCommandQueue(SDL_Renderer* renderer, SDL_RenderCommand* cmd, void* vertices, size_t vertsize)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    const int vboidx = data->currentVertexBuffer;
    LPDIRECT3DVERTEXBUFFER8 vbo = NULL;
    const SDL_bool istarget = (renderer->target != NULL);
    size_t i;

    if (D3D_ActivateRenderer(renderer) < 0) return -1;

    vbo = data->vertexBuffers[vboidx];
    if (!vbo || (data->vertexBufferSize[vboidx] < vertsize)) {
        const DWORD usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
        const DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
        if (vbo) { IDirect3DVertexBuffer8_Release(vbo); vbo = NULL; }
        if (vertsize > 0 && SUCCEEDED(IDirect3DDevice8_CreateVertexBuffer(data->device, (UINT)vertsize, usage, fvf, D3DPOOL_DEFAULT, &vbo))) {
            data->vertexBuffers[vboidx] = vbo;
            data->vertexBufferSize[vboidx] = vertsize;
        }
        else {
            data->vertexBuffers[vboidx] = NULL;
            data->vertexBufferSize[vboidx] = 0;
        }
    }

    if (vbo) {
        void* ptr = NULL;
        const DWORD lockFlags =
#ifdef D3DLOCK_DISCARD
            D3DLOCK_DISCARD
#else
            0
#endif
            ;
        if (SUCCEEDED(IDirect3DVertexBuffer8_Lock(vbo, 0, (UINT)vertsize, (BYTE**)&ptr, lockFlags))) {
            SDL_memcpy(ptr, vertices, vertsize);
            if (FAILED(IDirect3DVertexBuffer8_Unlock(vbo))) vbo = NULL;
        }
        else {
            vbo = NULL;
        }
    }

    if (vbo) {
        data->currentVertexBuffer++;
        if (data->currentVertexBuffer >= SDL_arraysize(data->vertexBuffers)) data->currentVertexBuffer = 0;
    }
    else if (!data->reportedVboProblem) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL failed to get a vertex buffer for this Direct3D 8 rendering batch!");
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Dropping back to a slower method.");
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "This might be a brief hiccup, but if performance is bad, this is probably why.");
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "This error will not be logged again for this renderer.");
        data->reportedVboProblem = SDL_TRUE;
    }

    IDirect3DDevice8_SetVertexShader(data->device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    IDirect3DDevice8_SetStreamSource(data->device, 0, vbo, sizeof(Vertex));

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETDRAWCOLOR:
            break;

        case SDL_RENDERCMD_SETVIEWPORT: {
            SDL_Rect* viewport = &data->drawstate.viewport;
            if (SDL_memcmp(viewport, &cmd->data.viewport.rect, sizeof(SDL_Rect)) != 0) {
                SDL_memcpy(viewport, &cmd->data.viewport.rect, sizeof(SDL_Rect));
                data->drawstate.viewport_dirty = SDL_TRUE;
                data->drawstate.cliprect_dirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT: {
            const SDL_Rect* rect = &cmd->data.cliprect.rect;
            if (data->drawstate.cliprect_enabled != cmd->data.cliprect.enabled) {
                data->drawstate.cliprect_enabled = cmd->data.cliprect.enabled;
                data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
            }
            if (SDL_memcmp(&data->drawstate.cliprect, rect, sizeof(SDL_Rect)) != 0) {
                SDL_memcpy(&data->drawstate.cliprect, rect, sizeof(SDL_Rect));
                data->drawstate.cliprect_dirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_CLEAR: {
            const DWORD color = D3DCOLOR_ARGB(cmd->data.color.a, cmd->data.color.r, cmd->data.color.g, cmd->data.color.b);

            const SDL_Rect* viewport = &data->drawstate.viewport;
            const int backw = istarget ? renderer->target->w : (int)data->pparams.BackBufferWidth;
            const int backh = istarget ? renderer->target->h : (int)data->pparams.BackBufferHeight;

            const SDL_bool viewport_equal =
                ((viewport->x == 0) && (viewport->y == 0) &&
                    (viewport->w == backw) && (viewport->h == backh)) ? SDL_TRUE : SDL_FALSE;

            const SDL_bool had_scissor = data->drawstate.cliprect_enabled;
            if (had_scissor) IDirect3DDevice8_SetScissors(data->device, 0, FALSE, NULL);

            if (!data->drawstate.viewport_dirty && viewport_equal) {
                IDirect3DDevice8_Clear(data->device, 0, NULL, D3DCLEAR_TARGET, color, 0.0f, 0);
            }
            else {
                const D3DVIEWPORT8 wholeviewport = (D3DVIEWPORT8){ 0, 0, (DWORD)backw, (DWORD)backh, 0.0f, 1.0f };
                IDirect3DDevice8_SetViewport(data->device, &wholeviewport);
                data->drawstate.viewport_dirty = SDL_TRUE;
                IDirect3DDevice8_Clear(data->device, 0, NULL, D3DCLEAR_TARGET, color, 0.0f, 0);
            }

            if (had_scissor) {
                data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
                data->drawstate.cliprect_dirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS: {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            SetDrawState(data, cmd);
            if (vbo) {
                IDirect3DDevice8_DrawPrimitive(data->device, D3DPT_POINTLIST, (UINT)(first / sizeof(Vertex)), (UINT)count);
            }
            else {
                const Vertex* verts = (const Vertex*)(((const Uint8*)vertices) + first);
                IDirect3DDevice8_DrawPrimitiveUP(data->device, D3DPT_POINTLIST, (UINT)count, verts, sizeof(Vertex));
            }
            break;
        }

        case SDL_RENDERCMD_DRAW_LINES: {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const Vertex* verts = (const Vertex*)(((const Uint8*)vertices) + first);
            const SDL_bool close_endpoint = ((count == 2) || (verts[0].x != verts[count - 1].x) || (verts[0].y != verts[count - 1].y));
            SetDrawState(data, cmd);
            if (vbo) {
                IDirect3DDevice8_DrawPrimitive(data->device, D3DPT_LINESTRIP, (UINT)(first / sizeof(Vertex)), (UINT)(count - 1));
                if (close_endpoint) {
                    IDirect3DDevice8_DrawPrimitive(data->device, D3DPT_POINTLIST, (UINT)((first / sizeof(Vertex)) + (count - 1)), 1);
                }
            }
            else {
                IDirect3DDevice8_DrawPrimitiveUP(data->device, D3DPT_LINESTRIP, (UINT)(count - 1), verts, sizeof(Vertex));
                if (close_endpoint) {
                    IDirect3DDevice8_DrawPrimitiveUP(data->device, D3DPT_POINTLIST, 1, &verts[count - 1], sizeof(Vertex));
                }
            }
            break;
        }

        case SDL_RENDERCMD_FILL_RECTS: {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            SetDrawState(data, cmd);
            if (vbo) {
                size_t offset = 0;
                for (i = 0; i < count; ++i, offset += 4) {
                    IDirect3DDevice8_DrawPrimitive(data->device, D3DPT_TRIANGLEFAN, (UINT)((first / sizeof(Vertex)) + offset), 2);
                }
            }
            else {
                const Vertex* verts = (const Vertex*)(((const Uint8*)vertices) + first);
                for (i = 0; i < count; ++i, verts += 4) {
                    IDirect3DDevice8_DrawPrimitiveUP(data->device, D3DPT_TRIANGLEFAN, 2, verts, sizeof(Vertex));
                }
            }
            break;
        }

        case SDL_RENDERCMD_COPY: {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            SetDrawState(data, cmd);
            if (vbo) {
                size_t offset = 0;
                for (i = 0; i < count; ++i, offset += 4) {
                    IDirect3DDevice8_DrawPrimitive(data->device, D3DPT_TRIANGLEFAN, (UINT)((first / sizeof(Vertex)) + offset), 2);
                }
            }
            else {
                const Vertex* verts = (const Vertex*)(((const Uint8*)vertices) + first);
                for (i = 0; i < count; ++i, verts += 4) {
                    IDirect3DDevice8_DrawPrimitiveUP(data->device, D3DPT_TRIANGLEFAN, 2, verts, sizeof(Vertex));
                }
            }
            break;
        }

        case SDL_RENDERCMD_COPY_EX: {
            const size_t first = cmd->data.draw.first;
            const Vertex* verts = (const Vertex*)(((const Uint8*)vertices) + first);
            const Vertex* transvert = verts + 4;
            const float translatex = transvert->x;
            const float translatey = transvert->y;
            const float rotation = transvert->z;
            const Float4X4 d3dmatrix = MatrixMultiply(MatrixRotationZ(rotation), MatrixTranslation(translatex, translatey, 0.0f));
            SetDrawState(data, cmd);
            IDirect3DDevice8_SetTransform(data->device, D3DTS_VIEW, (const D3DMATRIX*)&d3dmatrix);
            if (vbo) {
                IDirect3DDevice8_DrawPrimitive(data->device, D3DPT_TRIANGLEFAN, (UINT)(first / sizeof(Vertex)), 2);
            }
            else {
                IDirect3DDevice8_DrawPrimitiveUP(data->device, D3DPT_TRIANGLEFAN, 2, verts, sizeof(Vertex));
            }
            break;
        }

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }

    return 0;
}

/* ----------------------------- ReadPixels -------------------------------- */

static int
D3D_RenderReadPixels(SDL_Renderer* renderer, const SDL_Rect* rect,
    Uint32 format, void* pixels, int pitch)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    IDirect3DSurface8* srcRT = NULL;
    IDirect3DSurface8* sysmemSurf = NULL;
    D3DSURFACE_DESC desc;
    RECT d3drect;
    D3DLOCKED_RECT locked;
    HRESULT hr;

    if (!renderer || !rect || !pixels) return SDL_SetError("D3D_RenderReadPixels: invalid args");

    srcRT = data->currentRenderTarget ? data->currentRenderTarget : data->defaultRenderTarget;
    if (!srcRT) return SDL_SetError("D3D_RenderReadPixels: no render target");

    IDirect3DSurface8_AddRef(srcRT);

    hr = IDirect3DSurface8_GetDesc(srcRT, &desc);
    if (FAILED(hr)) { IDirect3DSurface8_Release(srcRT); return D3D_SetError("GetDesc()", hr); }

    hr = IDirect3DDevice8_CreateImageSurface(data->device, desc.Width, desc.Height, desc.Format, &sysmemSurf);
    if (FAILED(hr)) { IDirect3DSurface8_Release(srcRT); return D3D_SetError("CreateImageSurface()", hr); }

    hr = IDirect3DDevice8_CopyRects(data->device, srcRT, NULL, 0, sysmemSurf, NULL);
    IDirect3DSurface8_Release(srcRT);
    if (FAILED(hr)) { IDirect3DSurface8_Release(sysmemSurf); return D3D_SetError("CopyRects()", hr); }

    d3drect.left = rect->x;
    d3drect.top = rect->y;
    d3drect.right = rect->x + rect->w;
    d3drect.bottom = rect->y + rect->h;

    hr = IDirect3DSurface8_LockRect(sysmemSurf, &locked, &d3drect, D3DLOCK_READONLY);
    if (FAILED(hr)) { IDirect3DSurface8_Release(sysmemSurf); return D3D_SetError("LockRect()", hr); }

    SDL_ConvertPixels(rect->w, rect->h,
        D3DFMTToPixelFormat(desc.Format), locked.pBits, locked.Pitch,
        format, pixels, pitch);

    IDirect3DSurface8_UnlockRect(sysmemSurf);
    IDirect3DSurface8_Release(sysmemSurf);
    return 0;
}

/* ------------------------------- Present --------------------------------- */

static int D3D_SetRenderTargetInternal(SDL_Renderer* renderer, SDL_Texture* texture)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    IDirect3DDevice8* device = data->device;
    HRESULT hr;

    if (data->currentRenderTarget != NULL) {
        IDirect3DSurface8_Release(data->currentRenderTarget);
        data->currentRenderTarget = NULL;
    }

    if (texture == NULL) {
        hr = IDirect3DDevice8_SetRenderTarget(device, data->defaultRenderTarget, NULL);
        if (FAILED(hr)) {
            return D3D_SetError("SetRenderTarget(default)", hr);
        }
        return 0;
    }

    {
        D3D_TextureData* texturedata = (D3D_TextureData*)texture->driverdata;
        D3D_TextureRep* texturerep;

        if (!texturedata) {
            SDL_SetError("Texture is not currently available");
            return -1;
        }

        texturerep = &texturedata->texture;
        if (texturerep->dirty && texturerep->staging) {
            if (!texturerep->texture) {
                hr = IDirect3DDevice8_CreateTexture(device, texturerep->w, texturerep->h, 1,
                    texturerep->usage, texturerep->d3dfmt,
                    D3DPOOL_DEFAULT, &texturerep->texture);
                if (FAILED(hr)) {
                    return D3D_SetError("CreateTexture(D3DPOOL_DEFAULT)", hr);
                }
            }
            hr = D3D8_UpdateTexture(texturerep->staging, texturerep->texture);
            if (FAILED(hr)) {
                return D3D_SetError("UpdateTexture()", hr);
            }
            texturerep->dirty = SDL_FALSE;
        }

        hr = IDirect3DTexture8_GetSurfaceLevel(texturedata->texture.texture, 0, &data->currentRenderTarget);
        if (FAILED(hr)) {
            return D3D_SetError("GetSurfaceLevel()", hr);
        }

        hr = IDirect3DDevice8_SetRenderTarget(device, data->currentRenderTarget, NULL);
        if (FAILED(hr)) {
            IDirect3DSurface8_Release(data->currentRenderTarget);
            data->currentRenderTarget = NULL;
            return D3D_SetError("SetRenderTarget(texture)", hr);
        }
    }

    return 0;
}


static int D3D_SetRenderTarget(SDL_Renderer* renderer, SDL_Texture* texture)
{
    if (D3D_ActivateRenderer(renderer) < 0) return -1;
    return D3D_SetRenderTargetInternal(renderer, texture);
}

static int D3D_RenderPresent(SDL_Renderer* renderer)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    HRESULT hr;

    if (!data->beginScene) {
        hr = IDirect3DDevice8_EndScene(data->device);
        if (FAILED(hr)) {
            D3D_SetError("EndScene()", hr);
            data->beginScene = SDL_TRUE;
            return -1;
        }
        data->beginScene = SDL_TRUE;
    }

    if (renderer->target != NULL) return 0;

    hr = IDirect3DDevice8_Present(data->device, NULL, NULL, NULL, NULL);
    if (FAILED(hr)) { D3D_SetError("Present()", hr); return -1; }

    data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
    data->drawstate.cliprect_dirty = SDL_TRUE;

    return 0;
}

/* ---------------------------- Destruction -------------------------------- */

static void D3D_DestroyTexture(SDL_Renderer* renderer, SDL_Texture* texture)
{
    D3D_RenderData* renderdata = (D3D_RenderData*)renderer->driverdata;
    D3D_TextureData* data = (D3D_TextureData*)texture->driverdata;

    if (!texture) return;

    if (renderer->target == texture) {
        D3D_SetRenderTarget(renderer, NULL);
        renderer->target = NULL;
    }

    if (renderdata) {
        if (renderdata->drawstate.texture == texture) {
            IDirect3DDevice8_SetTexture(renderdata->device, 0, NULL);
            renderdata->drawstate.texture = NULL;
        }
        if (data && data->yuv) {
            IDirect3DDevice8_SetTexture(renderdata->device, 1, NULL);
            IDirect3DDevice8_SetTexture(renderdata->device, 2, NULL);
            IDirect3DDevice8_SetTextureStageState(renderdata->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(renderdata->device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(renderdata->device, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(renderdata->device, 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    }

    if (!data) { texture->driverdata = NULL; return; }

    D3D_DestroyTextureRep(&data->texture);
    D3D_DestroyTextureRep(&data->utexture);
    D3D_DestroyTextureRep(&data->vtexture);
    SDL_free(data->pixels);
    SDL_free(data);
    texture->driverdata = NULL;
}

static void D3D_DestroyRenderer(SDL_Renderer* renderer)
{
    if (!renderer) return;

    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;

    if (data) {
        int i;

        if (!data->beginScene && data->device) {
            IDirect3DDevice8_EndScene(data->device);
            data->beginScene = SDL_TRUE;
        }

        if (data->device) {
            IDirect3DDevice8_SetTexture(data->device, 0, NULL);
            IDirect3DDevice8_SetTexture(data->device, 1, NULL);
            IDirect3DDevice8_SetTexture(data->device, 2, NULL);
            IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(data->device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            IDirect3DDevice8_SetTextureStageState(data->device, 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

        if (data->currentRenderTarget) { IDirect3DSurface8_Release(data->currentRenderTarget); data->currentRenderTarget = NULL; }
        if (data->defaultRenderTarget) { IDirect3DSurface8_Release(data->defaultRenderTarget); data->defaultRenderTarget = NULL; }

        for (i = 0; i < (int)SDL_arraysize(data->vertexBuffers); ++i) {
            if (data->vertexBuffers[i]) {
                IDirect3DVertexBuffer8_Release(data->vertexBuffers[i]);
                data->vertexBuffers[i] = NULL;
            }
            data->vertexBufferSize[i] = 0;
        }
        data->currentVertexBuffer = 0;

        if (data->device) { IDirect3DDevice8_Release(data->device); data->device = NULL; }
        if (data->d3d) { IDirect3D8_Release(data->d3d);         data->d3d = NULL; }

        SDL_free(data);
        renderer->driverdata = NULL;
    }

    SDL_free(renderer);
}

/* -------------------------------- Reset ---------------------------------- */

static int D3D_Reset(SDL_Renderer* renderer)
{
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;
    const Float4X4 viewIdent = MatrixIdentity();
    HRESULT result;
    SDL_Texture* texture;
    int i;

    if (data->currentRenderTarget) { IDirect3DSurface8_Release(data->currentRenderTarget); data->currentRenderTarget = NULL; }
    if (data->defaultRenderTarget) { IDirect3DSurface8_Release(data->defaultRenderTarget); data->defaultRenderTarget = NULL; }

    for (texture = renderer->textures; texture; texture = texture->next) {
        if (texture->access == SDL_TEXTUREACCESS_TARGET) {
            D3D_DestroyTexture(renderer, texture);
        }
        else {
            D3D_RecreateTexture(renderer, texture);
        }
    }

    for (i = 0; i < (int)SDL_arraysize(data->vertexBuffers); ++i) {
        if (data->vertexBuffers[i]) { IDirect3DVertexBuffer8_Release(data->vertexBuffers[i]); data->vertexBuffers[i] = NULL; }
        data->vertexBufferSize[i] = 0;
    }
    data->currentVertexBuffer = 0;
    data->reportedVboProblem = SDL_FALSE;

    result = IDirect3DDevice8_Reset(data->device, &data->pparams);
    if (FAILED(result)) {
        if (result == D3DERR_DEVICELOST) return 0;
        return D3D_SetError("Reset()", result);
    }

    /* NEW: After reset, re-apply interlace/progressive stability */
    ApplyInterlaceStability(data->device, &data->pparams);

    for (texture = renderer->textures; texture; texture = texture->next) {
        if (texture->access == SDL_TEXTUREACCESS_TARGET) {
            if (D3D_CreateTexture(renderer, texture) < 0) { /* keep going */ }
        }
    }

    result = IDirect3DDevice8_GetRenderTarget(data->device, &data->defaultRenderTarget);
    if (FAILED(result)) return D3D_SetError("GetRenderTarget()", result);

    D3D_InitRenderState(data);
    if (D3D_SetRenderTargetInternal(renderer, renderer->target) < 0) {
        D3D_SetRenderTargetInternal(renderer, NULL);
    }

    if (renderer->target == NULL) {
        data->drawstate.viewport.x = 0;
        data->drawstate.viewport.y = 0;
        data->drawstate.viewport.w = (int)data->pparams.BackBufferWidth;
        data->drawstate.viewport.h = (int)data->pparams.BackBufferHeight;

        {
            const D3DVIEWPORT8 vp = (D3DVIEWPORT8){ 0,0,(DWORD)data->pparams.BackBufferWidth,(DWORD)data->pparams.BackBufferHeight,0.0f,1.0f };
            IDirect3DDevice8_SetViewport(data->device, &vp);
        }

        IDirect3DDevice8_SetScissors(data->device, 0, FALSE, NULL);

        if (SUCCEEDED(IDirect3DDevice8_BeginScene(data->device))) {
            IDirect3DDevice8_Clear(data->device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 255, 0, 255), 1.0f, 0);
            IDirect3DDevice8_EndScene(data->device);
            IDirect3DDevice8_Present(data->device, NULL, NULL, NULL, NULL);
        }
        data->beginScene = SDL_TRUE;
    }

    data->drawstate.viewport_dirty = SDL_TRUE;
    data->drawstate.cliprect_dirty = SDL_TRUE;
    data->drawstate.cliprect_enabled = SDL_FALSE;
    data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
    data->drawstate.texture = NULL;
    data->drawstate.blend = SDL_BLENDMODE_INVALID;
    data->drawstate.is_copy_ex = SDL_FALSE;

    IDirect3DDevice8_SetTransform(data->device, D3DTS_VIEW, (const D3DMATRIX*)&viewIdent);

    {
        SDL_Event event;
        event.type = SDL_RENDER_TARGETS_RESET;
        SDL_PushEvent(&event);
    }

    return 0;
}

/* --------------------------- Xbox video mode ------------------------------ */

static void FinalizeXboxMode(D3DPRESENT_PARAMETERS* p)
{
    DWORD vf = XGetVideoFlags();
    const BOOL ws = (vf & XC_VIDEO_FLAGS_WIDESCREEN) != 0;
    const BOOL pal60 = (vf & XC_VIDEO_FLAGS_PAL_60Hz) != 0;
    const BOOL is_pal = (XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I);

    /* HDTV capability bits reported by the dashboard.
       IMPORTANT: On PAL-region consoles these do NOT enable 480p/720p/1080i
       unless the EEPROM is set to NTSC. So gate them on !is_pal. */
    const BOOL can480p = ((vf & XC_VIDEO_FLAGS_HDTV_480p) != 0) && !is_pal;
    const BOOL can720p = ((vf & XC_VIDEO_FLAGS_HDTV_720p) != 0) && !is_pal;
    const BOOL can1080i = ((vf & XC_VIDEO_FLAGS_HDTV_1080i) != 0) && !is_pal;

    p->Flags = 0;

    /* Decide flags and refresh by exact backbuffer size. We do not change
       BackBufferWidth/Height here; the choice of size should have happened
       earlier (CreateRenderer). */
    if (p->BackBufferWidth == 1280 && p->BackBufferHeight == 720) {
        /* 720p is only valid if the console is NTSC-region with 720p enabled. */
        if (can720p) {
            p->Flags |= D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
            p->FullScreen_RefreshRateInHz = 60;
        }
        else {
            /* Size was forced earlier; make it as safe as possible here. */
            SDL_Log("WARN: 720p requested but not permitted on this console/region; forcing interlaced 60 Hz.");
            p->Flags |= D3DPRESENTFLAG_INTERLACED | D3DPRESENTFLAG_WIDESCREEN;
            p->FullScreen_RefreshRateInHz = 60;
        }
    }
    else if (p->BackBufferWidth == 1920 && p->BackBufferHeight == 1080) {
        /* 1080 is always interlaced on Xbox; only permitted for NTSC-region with 1080i enabled. */
        p->Flags |= D3DPRESENTFLAG_INTERLACED | D3DPRESENTFLAG_WIDESCREEN;
        p->FullScreen_RefreshRateInHz = 60;
        if (!can1080i) {
            SDL_Log("WARN: 1080i requested but not permitted on this console/region.");
        }
    }
    else if (p->BackBufferWidth == 720 && p->BackBufferHeight == 576) {
        /* PAL 50 Hz (576i). Widescreen flag allowed here. */
        p->Flags |= D3DPRESENTFLAG_INTERLACED;
        if (ws) p->Flags |= D3DPRESENTFLAG_WIDESCREEN;
        p->FullScreen_RefreshRateInHz = 50;
    }
    else if ((p->BackBufferWidth == 640 && p->BackBufferHeight == 480) ||
        (p->BackBufferWidth == 720 && p->BackBufferHeight == 480)) {
        /* 480-line modes. On PAL consoles: never progressive. If PAL60 is off,
           480-line modes generally shouldn't be selected (576i should have been),
           but if they are, keep them interlaced. */
        if (can480p) {
            p->Flags |= D3DPRESENTFLAG_PROGRESSIVE;
        }
        else {
            p->Flags |= D3DPRESENTFLAG_INTERLACED;
        }
        if (ws && p->BackBufferWidth == 720) p->Flags |= D3DPRESENTFLAG_WIDESCREEN;
        /* 480 modes are 60 Hz; PAL60 allows 60 Hz on PAL consoles, otherwise we
           still set 60 here (renderer chooses 576i@50 earlier when PAL60 is off). */
        p->FullScreen_RefreshRateInHz = 60;
        if (is_pal && !can480p) {
            SDL_Log("Xbox D3D: 480-line mode selected on PAL console -> progressive disabled (interlaced only).");
            if (!pal60) {
                SDL_Log("WARN: PAL60 disabled; 480-line @60 may be invalid. Prefer 720x576i@50.");
            }
        }
    }
    else {
        /* Unknown size: safest default is 60 Hz interlaced, no widescreen flag. */
        p->Flags |= D3DPRESENTFLAG_INTERLACED;
        p->FullScreen_RefreshRateInHz = 60;
    }

    p->BackBufferFormat = D3DFMT_LIN_X8R8G8B8;
    p->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    /* Safety checks / coercions */
    if ((p->BackBufferHeight == 1080 || p->BackBufferHeight == 576) &&
        (p->Flags & D3DPRESENTFLAG_PROGRESSIVE)) {
        SDL_Log("WARN: illegal progressive for %u-line mode; forcing interlaced\n", (unsigned)p->BackBufferHeight);
        p->Flags &= ~D3DPRESENTFLAG_PROGRESSIVE;
        p->Flags |= D3DPRESENTFLAG_INTERLACED;
    }
    if (p->BackBufferHeight == 576 && p->FullScreen_RefreshRateInHz != 50) {
        SDL_Log("WARN: 576 must be 50 Hz; overriding\n");
        p->FullScreen_RefreshRateInHz = 50;
    }
    if ((p->BackBufferWidth == 640) && (p->Flags & D3DPRESENTFLAG_WIDESCREEN)) {
        SDL_Log("WARN: 640-wide cannot be widescreen; clearing WS flag\n");
        p->Flags &= ~D3DPRESENTFLAG_WIDESCREEN;
    }
    if ((p->Flags & D3DPRESENTFLAG_PROGRESSIVE) &&
        !(p->BackBufferHeight == 480 || p->BackBufferHeight == 720)) {
        SDL_Log("WARN: progressive set on non-480/720; forcing interlaced\n");
        p->Flags &= ~D3DPRESENTFLAG_PROGRESSIVE;
        p->Flags |= D3DPRESENTFLAG_INTERLACED;
    }

    SDL_Log("Xbox final display mode: %ux%u flags=0x%08x @ %u Hz  (WS=%d 480p=%d 720p=%d 1080i=%d PAL=%d)\n",
        (unsigned)p->BackBufferWidth, (unsigned)p->BackBufferHeight,
        (unsigned)p->Flags, (unsigned)p->FullScreen_RefreshRateInHz,
        (int)ws, (int)can480p, (int)can720p, (int)can1080i, (int)is_pal);
}


/* ------------------------------ Create ----------------------------------- */

int D3D_CreateRenderer(SDL_Renderer* renderer, SDL_Window* window, Uint32 flags)
{
    D3D_RenderData* data;
    HRESULT result;
    D3DPRESENT_PARAMETERS pparams;
    D3DCAPS8 caps;
    DWORD device_flags;
    DWORD vidflags;
    int i;

    data = (D3D_RenderData*)SDL_calloc(1, sizeof(*data));
    if (!data) { SDL_free(renderer); SDL_OutOfMemory(); return -1; }

    if (!D3D_LoadDLL(/*&data->d3dDLL,*/ &data->d3d)) {
        SDL_free(renderer); SDL_free(data);
        SDL_SetError("Unable to create Direct3D interface\n");
        return -1;
    }

    renderer->always_batch = SDL_TRUE;

    renderer->WindowEvent = D3D_WindowEvent;
    renderer->SupportsBlendMode = D3D_SupportsBlendMode;
    renderer->CreateTexture = D3D_CreateTexture;
    renderer->UpdateTexture = D3D_UpdateTexture;
    renderer->UpdateTextureYUV = D3D_UpdateTextureYUV;
    renderer->LockTexture = D3D_LockTexture;
    renderer->UnlockTexture = D3D_UnlockTexture;
    renderer->SetRenderTarget = D3D_SetRenderTarget;
    renderer->QueueSetViewport = D3D_QueueSetViewport;
    renderer->QueueSetDrawColor = D3D_QueueSetDrawColor;
    renderer->QueueDrawPoints = D3D_QueueDrawPoints;
    renderer->QueueDrawLines = D3D_QueueDrawLines;
    renderer->QueueFillRects = D3D_QueueFillRects;
    renderer->QueueCopy = D3D_QueueCopy;
    renderer->QueueCopyEx = D3D_QueueCopyEx;
    renderer->RunCommandQueue = D3D_RunCommandQueue;
    renderer->RenderReadPixels = D3D_RenderReadPixels;
    renderer->RenderPresent = D3D_RenderPresent;
    renderer->DestroyTexture = D3D_DestroyTexture;
    renderer->DestroyRenderer = D3D_DestroyRenderer;

    renderer->info = D3D_RenderDriver.info;
    renderer->info.flags = (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);

    renderer->driverdata = data;
    renderer->window = window;

    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);

    SDL_zero(pparams);
    pparams.BackBufferWidth = (UINT)w;
    pparams.BackBufferHeight = (UINT)h;
    pparams.BackBufferCount = 1;
    pparams.SwapEffect = D3DSWAPEFFECT_DISCARD;

    device_flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;

    pparams.EnableAutoDepthStencil = TRUE;
    pparams.AutoDepthStencilFormat = D3DFMT_D16;
    pparams.hDeviceWindow = NULL;
    pparams.Windowed = FALSE;
    pparams.BackBufferFormat = D3DFMT_LIN_X8R8G8B8;
    pparams.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pparams.MultiSampleType = D3DMULTISAMPLE_NONE;

    {
        vidflags = XGetVideoFlags();
        const SDL_bool is_pal = (XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I);
        const SDL_bool pal60 = (vidflags & XC_VIDEO_FLAGS_PAL_60Hz) ? SDL_TRUE : SDL_FALSE;
        const SDL_bool allow1080i = (vidflags & XC_VIDEO_FLAGS_HDTV_1080i) ? SDL_TRUE : SDL_FALSE;
        const SDL_bool allow720p = (vidflags & XC_VIDEO_FLAGS_HDTV_720p) ? SDL_TRUE : SDL_FALSE;
        const SDL_bool allow480p = (vidflags & XC_VIDEO_FLAGS_HDTV_480p) ? SDL_TRUE : SDL_FALSE;

        int reqw = 0, reqh = 0;
        SDL_GetWindowSize(window, &reqw, &reqh);

        SDL_bool matched = SDL_FALSE;

        if (reqw == 1280 && reqh == 720 && allow720p) {
            pparams.BackBufferWidth = 1280; pparams.BackBufferHeight = 720; matched = SDL_TRUE;
        }
        else if (reqw == 1920 && reqh == 1080 && allow1080i) {
            pparams.BackBufferWidth = 1920; pparams.BackBufferHeight = 1080; matched = SDL_TRUE;
        }
        else if (reqw == 720 && reqh == 480 && allow480p) {
            pparams.BackBufferWidth = 720;  pparams.BackBufferHeight = 480; matched = SDL_TRUE;
        }
        else if (reqw == 640 && reqh == 480) {
            if (allow480p) {
                pparams.BackBufferWidth = 720; pparams.BackBufferHeight = 480;
                SDL_Log("Xbox D3D: requested 640x480; 480p enabled -> using 720x480p");
            }
            else if (is_pal && !pal60) {
                pparams.BackBufferWidth = 720; pparams.BackBufferHeight = 576;
                SDL_Log("Xbox D3D: requested 640x480; PAL50 only -> using 720x576i@50");
            }
            else {
                pparams.BackBufferWidth = 640; pparams.BackBufferHeight = 480;
                SDL_Log("Xbox D3D: requested 640x480; 480p disabled -> using 640x480i");
            }
            matched = SDL_TRUE;
        }

        if (!matched) {
            UINT fw = pparams.BackBufferWidth, fh = pparams.BackBufferHeight;
            if (allow720p) { fw = 1280; fh = 720; }
            else if (allow1080i) { fw = 1920; fh = 1080; }
            else if (allow480p) { fw = 720;  fh = 480; }
            else if (is_pal && !pal60) { fw = 720;  fh = 576; }
            else { fw = 640;  fh = 480; }
            SDL_Log("Xbox D3D: requested %dx%d not permitted; falling back to %ux%u",
                reqw, reqh, (unsigned)fw, (unsigned)fh);
            pparams.BackBufferWidth = fw; pparams.BackBufferHeight = fh;
        }
    }

    FinalizeXboxMode(&pparams);

    result = IDirect3D8_CreateDevice(data->d3d, 0, D3DDEVTYPE_HAL, NULL, device_flags, &pparams, &data->device);
    if (FAILED(result)) {
        D3D_DestroyRenderer(renderer);
        D3D_SetError("CreateDevice()", result);
        return -1;
    }

    /* NEW: Immediately apply interlace/progressive stability settings */
    data->pparams = pparams;
    ApplyInterlaceStability(data->device, &data->pparams);

    SDL_SetWindowSize(window, (int)pparams.BackBufferWidth, (int)pparams.BackBufferHeight);

    if (pparams.FullScreen_PresentationInterval == D3DPRESENT_INTERVAL_ONE) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }

    /* Keep pparams stored for later (Reset, etc.) */
    data->pparams = pparams;

    IDirect3DDevice8_GetDeviceCaps(data->device, &caps);
    renderer->info.max_texture_width = caps.MaxTextureWidth;
    renderer->info.max_texture_height = caps.MaxTextureHeight;

    for (i = 0; i < (int)SDL_arraysize(data->vertexBuffers); ++i) {
        data->vertexBuffers[i] = NULL;
        data->vertexBufferSize[i] = 0;
    }
    data->currentVertexBuffer = 0;
    data->reportedVboProblem = SDL_FALSE;

    IDirect3DDevice8_GetRenderTarget(data->device, &data->defaultRenderTarget);
    data->currentRenderTarget = NULL;

    D3D_InitRenderState(data);

    data->drawstate.viewport.x = 0;
    data->drawstate.viewport.y = 0;
    data->drawstate.viewport.w = (int)data->pparams.BackBufferWidth;
    data->drawstate.viewport.h = (int)data->pparams.BackBufferHeight;

    {
        const D3DVIEWPORT8 vp = (D3DVIEWPORT8){ 0,0,(DWORD)data->pparams.BackBufferWidth,(DWORD)data->pparams.BackBufferHeight,0.0f,1.0f };
        IDirect3DDevice8_SetViewport(data->device, &vp);
    }

    IDirect3DDevice8_SetScissors(data->device, 0, FALSE, NULL);

    if (SUCCEEDED(IDirect3DDevice8_BeginScene(data->device))) {
        IDirect3DDevice8_Clear(data->device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        IDirect3DDevice8_EndScene(data->device);
        IDirect3DDevice8_Present(data->device, NULL, NULL, NULL, NULL);
    }
    data->beginScene = SDL_TRUE;

    data->drawstate.viewport_dirty = SDL_TRUE;
    data->drawstate.cliprect_dirty = SDL_TRUE;
    data->drawstate.cliprect_enabled = SDL_FALSE;
    data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
    data->drawstate.texture = NULL;
    data->drawstate.blend = SDL_BLENDMODE_INVALID;
    data->drawstate.is_copy_ex = SDL_FALSE;

    return 0;
}

/* ------------------------------ Driver info ------------------------------ */

SDL_RenderDriver D3D_RenderDriver = {
    D3D_CreateRenderer,
    {
        "direct3d",
        (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE),
        1,
        {SDL_PIXELFORMAT_ARGB8888},
        0,
        0
    }
};
#endif /* SDL_VIDEO_RENDER_D3D && !SDL_RENDER_DISABLED */

/* Always present for the Dynamic API. */
IDirect3DDevice8* SDL_RenderGetD3D8Device(SDL_Renderer* renderer)
{
    IDirect3DDevice8* device = NULL;

#if SDL_VIDEO_RENDER_D3D && !SDL_RENDER_DISABLED
    D3D_RenderData* data = (D3D_RenderData*)renderer->driverdata;

    if (renderer->DestroyRenderer != D3D_DestroyRenderer) {
        SDL_SetError("Renderer is not a D3D renderer");
        return NULL;
    }

    device = data->device;
    if (device) { IDirect3DDevice8_AddRef(device); }
#endif

    return device;
}

/* vi: set ts=4 sw=4 expandtab: */
