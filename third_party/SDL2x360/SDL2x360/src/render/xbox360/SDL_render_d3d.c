/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

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
#include "SDL_system.h"

#if SDL_VIDEO_RENDER_D3D

#ifdef __XBOX_360__
/* Xenos has no fixed function pipeline, so shaders are required to draw anything.
   SDL's XYZ|DIFFUSE|TEX1 matches POSITION/COLOR0/TEXCOORD0. */
#include "gl_shaders360.h"
static D3DXMATRIX g_x360_proj;

/* D3DXMatrixIdentity is not in the libraries this links against. */
static void x360_identity(D3DXMATRIX *m)
{
    SDL_memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}
#endif

#include "../../core/xbox360/SDL_xbox360.h"

#include "SDL_hints.h"
#include "SDL_loadso.h"
#include "SDL_syswm.h"
#include "../SDL_sysrender.h"
#include "../SDL_d3dmath.h"
#include "../../video/xbox360/SDL_xbox360video.h"

#if SDL_VIDEO_RENDER_D3D
#define D3D_DEBUG_INFO
#include <d3d9.h>
#endif

#include "SDL_shaders_d3d.h"

typedef struct
{
    SDL_Rect viewport;
    SDL_bool viewport_dirty;
    SDL_Texture *texture;
    SDL_BlendMode blend;
    SDL_bool cliprect_enabled;
    SDL_bool cliprect_enabled_dirty;
    SDL_Rect cliprect;
    SDL_bool cliprect_dirty;
    LPDIRECT3DPIXELSHADER9 shader;
} D3D_DrawStateCache;

/* Direct3D renderer implementation */

typedef struct
{
    void *d3dDLL;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device;
    UINT adapter;
    D3DPRESENT_PARAMETERS pparams;
    SDL_bool updateSize;
    SDL_bool beginScene;
    SDL_bool enableSeparateAlphaBlend;
    D3DTEXTUREFILTERTYPE scaleMode[8];
    IDirect3DSurface9 *defaultRenderTarget;
    IDirect3DSurface9 *currentRenderTarget;
    void *d3dxDLL;
#if SDL_HAVE_YUV
    LPDIRECT3DPIXELSHADER9 shaders[NUM_SHADERS];
#endif
    LPDIRECT3DVERTEXBUFFER9 vertexBuffers[8];
    size_t vertexBufferSize[8];
    int currentVertexBuffer;
    SDL_bool reportedVboProblem;
    D3D_DrawStateCache drawstate;
} D3D_RenderData;

typedef struct
{
    SDL_bool dirty;
    int w, h;
    DWORD usage;
    Uint32 format;
    D3DFORMAT d3dfmt;
    IDirect3DTexture9 *texture;
    IDirect3DTexture9 *staging;
} D3D_TextureRep;

typedef struct
{
    D3D_TextureRep texture;
    D3DTEXTUREFILTERTYPE scaleMode;

#if SDL_HAVE_YUV
    /* YV12 texture support */
    SDL_bool yuv;
    D3D_TextureRep utexture;
    D3D_TextureRep vtexture;
    Uint8 *pixels;
    int pitch;
    SDL_Rect locked_rect;
#endif
} D3D_TextureData;

typedef struct
{
    float x, y, z;
    DWORD color;
    float u, v;
} Vertex;

static int D3D_SetError(const char *prefix, HRESULT result)
{
    const char *error;

    switch (result) {
    case D3DERR_WRONGTEXTUREFORMAT:
        error = "WRONGTEXTUREFORMAT";
        break;
    case D3DERR_UNSUPPORTEDCOLOROPERATION:
        error = "UNSUPPORTEDCOLOROPERATION";
        break;
    case D3DERR_UNSUPPORTEDCOLORARG:
        error = "UNSUPPORTEDCOLORARG";
        break;
    case D3DERR_UNSUPPORTEDALPHAOPERATION:
        error = "UNSUPPORTEDALPHAOPERATION";
        break;
    case D3DERR_UNSUPPORTEDALPHAARG:
        error = "UNSUPPORTEDALPHAARG";
        break;
    case D3DERR_TOOMANYOPERATIONS:
        error = "TOOMANYOPERATIONS";
        break;
    case D3DERR_CONFLICTINGTEXTUREFILTER:
        error = "CONFLICTINGTEXTUREFILTER";
        break;
    case D3DERR_UNSUPPORTEDFACTORVALUE:
        error = "UNSUPPORTEDFACTORVALUE";
        break;
    case D3DERR_CONFLICTINGRENDERSTATE:
        error = "CONFLICTINGRENDERSTATE";
        break;
    case D3DERR_UNSUPPORTEDTEXTUREFILTER:
        error = "UNSUPPORTEDTEXTUREFILTER";
        break;
    case D3DERR_DRIVERINTERNALERROR:
        error = "DRIVERINTERNALERROR";
        break;
    case D3DERR_NOTFOUND:
        error = "NOTFOUND";
        break;
    case D3DERR_MOREDATA:
        error = "MOREDATA";
        break;
    case D3DERR_DEVICELOST:
        error = "DEVICELOST";
        break;
    case D3DERR_DEVICENOTRESET:
        error = "DEVICENOTRESET";
        break;
    case D3DERR_NOTAVAILABLE:
        error = "NOTAVAILABLE";
        break;
    case D3DERR_OUTOFVIDEOMEMORY:
        error = "OUTOFVIDEOMEMORY";
        break;
    case D3DERR_INVALIDDEVICE:
        error = "INVALIDDEVICE";
        break;
    case D3DERR_INVALIDCALL:
        error = "INVALIDCALL";
        break;
    default:
        error = "UNKNOWN";
        break;
    }
    return SDL_SetError("%s: %s", prefix, error);
}

static D3DFORMAT PixelFormatToD3DFMT(Uint32 format)
{
    switch (format) {
#ifdef __XBOX_360__
    /* Textures are tiled by default; UpdateTexture writes linear data, so use the
       LIN_ formats. Only for sampled textures -- the back buffer stays tiled. */
    case SDL_PIXELFORMAT_RGB565:
        return D3DFMT_LIN_R5G6B5;
    case SDL_PIXELFORMAT_RGB888:
        return D3DFMT_LIN_X8R8G8B8;
    case SDL_PIXELFORMAT_ARGB8888:
        return D3DFMT_LIN_A8R8G8B8;
#else
    case SDL_PIXELFORMAT_RGB565:
        return D3DFMT_R5G6B5;
    case SDL_PIXELFORMAT_RGB888:
        return D3DFMT_X8R8G8B8;
    case SDL_PIXELFORMAT_ARGB8888:
        return D3DFMT_A8R8G8B8;
#endif
    case SDL_PIXELFORMAT_YV12:
    case SDL_PIXELFORMAT_IYUV:
    case SDL_PIXELFORMAT_NV12:
    case SDL_PIXELFORMAT_NV21:
#ifdef __XBOX_360__
        /* Linear, like every other sampled texture above: a plain L8 here is
           tiled, and uploading linear rows into it is what makes the picture
           come out in shuffled blocks. */
        return D3DFMT_LIN_L8;
#else
        return D3DFMT_L8;
#endif
    default:
        return D3DFMT_UNKNOWN;
    }
}

static Uint32 D3DFMTToPixelFormat(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_R5G6B5:
        return SDL_PIXELFORMAT_RGB565;
    case D3DFMT_X8R8G8B8:
        return SDL_PIXELFORMAT_RGB888;
    case D3DFMT_A8R8G8B8:
        return SDL_PIXELFORMAT_ARGB8888;
    default:
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

static void D3D_InitRenderState(D3D_RenderData *data)
{
    D3DMATRIX matrix;

    IDirect3DDevice9 *device = data->device;
    IDirect3DDevice9_SetPixelShader(device, NULL);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetTexture(device, 1, NULL);
    IDirect3DDevice9_SetTexture(device, 2, NULL);
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    IDirect3DDevice9_SetVertexShader(device, NULL);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
/*    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE); */

    /* Enable color modulation by diffuse color */
/*
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP,
                                          D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1,
                                          D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG2,
                                          D3DTA_DIFFUSE);
*/
    /* Enable alpha modulation by diffuse alpha */
/*
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP,
                                          D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1,
                                          D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAARG2,
                                          D3DTA_DIFFUSE);
										  */
    /* Enable separate alpha blend function, if possible */
    if (data->enableSeparateAlphaBlend) {
        IDirect3DDevice9_SetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    }

    /* Disable second texture stage, since we're done */
/*    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP,
                                          D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_ALPHAOP,
                                          D3DTOP_DISABLE);
*/
    /* Set an identity world and view matrix */
    SDL_zero(matrix);
    matrix.m[0][0] = 1.0f;
    matrix.m[1][1] = 1.0f;
    matrix.m[2][2] = 1.0f;
    matrix.m[3][3] = 1.0f;
/*    IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &matrix);
    IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &matrix);*/

    /* Reset our current scale mode */
    SDL_memset(data->scaleMode, 0xFF, sizeof(data->scaleMode));

    /* Start the render with beginScene */
    data->beginScene = SDL_TRUE;
}

static int D3D_Reset(SDL_Renderer *renderer);

static int D3D_ActivateRenderer(SDL_Renderer *renderer)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    HRESULT result;

    if (data->updateSize) {
        SDL_Window *window = renderer->window;
        int w, h;
        Uint32 window_flags = SDL_GetWindowFlags(window);

        SDL_GetWindowSizeInPixels(window, &w, &h);
        data->pparams.BackBufferWidth = w;
        data->pparams.BackBufferHeight = h;
        if (window_flags & SDL_WINDOW_FULLSCREEN && (window_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != SDL_WINDOW_FULLSCREEN_DESKTOP) {
            SDL_DisplayMode fullscreen_mode;
            SDL_GetWindowDisplayMode(window, &fullscreen_mode);
            data->pparams.Windowed = FALSE;
            data->pparams.BackBufferFormat = PixelFormatToD3DFMT(fullscreen_mode.format);
            data->pparams.FullScreen_RefreshRateInHz = fullscreen_mode.refresh_rate;
        } else {
            data->pparams.Windowed = TRUE;
            data->pparams.BackBufferFormat = D3DFMT_UNKNOWN;
            data->pparams.FullScreen_RefreshRateInHz = 0;
        }
        if (D3D_Reset(renderer) < 0) {
            return -1;
        }

        data->updateSize = SDL_FALSE;
    }
    if (data->beginScene) {
        result = IDirect3DDevice9_BeginScene(data->device);
        if (result == D3DERR_DEVICELOST) {
            if (D3D_Reset(renderer) < 0) {
                return -1;
            }
            result = IDirect3DDevice9_BeginScene(data->device);
        }
        if (FAILED(result)) {
            return D3D_SetError("BeginScene()", result);
        }
        data->beginScene = SDL_FALSE;
    }
    return 0;
}

static void D3D_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;

    if (event->event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        data->updateSize = SDL_TRUE;
    }
}

static int D3D_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    SDL_GetWindowSizeInPixels(renderer->window, w, h);
    return 0;
}

static D3DBLEND GetBlendFunc(SDL_BlendFactor factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:
        return D3DBLEND_ZERO;
    case SDL_BLENDFACTOR_ONE:
        return D3DBLEND_ONE;
    case SDL_BLENDFACTOR_SRC_COLOR:
        return D3DBLEND_SRCCOLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        return D3DBLEND_INVSRCCOLOR;
    case SDL_BLENDFACTOR_SRC_ALPHA:
        return D3DBLEND_SRCALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        return D3DBLEND_INVSRCALPHA;
    case SDL_BLENDFACTOR_DST_COLOR:
        return D3DBLEND_DESTCOLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        return D3DBLEND_INVDESTCOLOR;
    case SDL_BLENDFACTOR_DST_ALPHA:
        return D3DBLEND_DESTALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        return D3DBLEND_INVDESTALPHA;
    default:
        break;
    }
    return (D3DBLEND)0;
}

static D3DBLENDOP GetBlendEquation(SDL_BlendOperation operation)
{
    switch (operation) {
    case SDL_BLENDOPERATION_ADD:
        return D3DBLENDOP_ADD;
    case SDL_BLENDOPERATION_SUBTRACT:
        return D3DBLENDOP_SUBTRACT;
    case SDL_BLENDOPERATION_REV_SUBTRACT:
        return D3DBLENDOP_REVSUBTRACT;
    case SDL_BLENDOPERATION_MINIMUM:
        return D3DBLENDOP_MIN;
    case SDL_BLENDOPERATION_MAXIMUM:
        return D3DBLENDOP_MAX;
    default:
        break;
    }
    return (D3DBLENDOP)0;
}

static SDL_bool D3D_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    if (!GetBlendFunc(srcColorFactor) || !GetBlendFunc(srcAlphaFactor) ||
        !GetBlendEquation(colorOperation) ||
        !GetBlendFunc(dstColorFactor) || !GetBlendFunc(dstAlphaFactor) ||
        !GetBlendEquation(alphaOperation)) {
        return SDL_FALSE;
    }

    if (!data->enableSeparateAlphaBlend) {
        if ((srcColorFactor != srcAlphaFactor) || (dstColorFactor != dstAlphaFactor) || (colorOperation != alphaOperation)) {
            return SDL_FALSE;
        }
    }
    return SDL_TRUE;
}

static int D3D_CreateTextureRep(IDirect3DDevice9 *device, D3D_TextureRep *texture, DWORD usage, Uint32 format, D3DFORMAT d3dfmt, int w, int h)
{
    HRESULT result;

    texture->dirty = SDL_FALSE;
    texture->w = w;
    texture->h = h;
    texture->usage = usage;
    texture->format = format;
    texture->d3dfmt = d3dfmt;

    result = IDirect3DDevice9_CreateTexture(device, w, h, 1, usage,
                                            PixelFormatToD3DFMT(format),
                                            D3DPOOL_DEFAULT, &texture->texture, NULL);
    if (FAILED(result)) {
        return D3D_SetError("CreateTexture(D3DPOOL_DEFAULT)", result);
    }
    return 0;
}

static int D3D_CreateStagingTexture(IDirect3DDevice9 *device, D3D_TextureRep *texture)
{
    HRESULT result;

    if (!texture->staging) {
#ifdef __XBOX_360__
        /* Unified memory and no UpdateTexture on this console, so a separate
           SYSTEMMEM staging surface can never reach the GPU. Alias staging to the
           real texture so a lock writes into the surface that gets sampled. */
        if (!texture->texture) {
            return SDL_SetError("staging requested before the texture exists");
        }
        IDirect3DTexture9_AddRef(texture->texture);   /* staging holds its own reference */
        texture->staging = texture->texture;
        return 0;
#else
        result = IDirect3DDevice9_CreateTexture(device, texture->w, texture->h, 1, 0,
                                                texture->d3dfmt, D3DPOOL_SYSTEMMEM, &texture->staging, NULL);
        if (FAILED(result)) {
            return D3D_SetError("CreateTexture(D3DPOOL_SYSTEMMEM)", result);
        }
#endif
    }
    return 0;
}

static int D3D_RecreateTextureRep(IDirect3DDevice9 *device, D3D_TextureRep *texture)
{
    if (texture->texture) {
        IDirect3DTexture9_Release(texture->texture);
        texture->texture = NULL;
    }
/*
    if (texture->staging) {
        IDirect3DTexture9_AddDirtyRect(texture->staging, NULL);
        texture->dirty = SDL_TRUE;
    }
*/
    return 0;
}

static int D3D_UpdateTextureRep(IDirect3DDevice9 *device, D3D_TextureRep *texture, int x, int y, int w, int h, const void *pixels, int pitch)
{
    RECT d3drect;
    D3DLOCKED_RECT locked;
    const Uint8 *src;
    Uint8 *dst;
    int row, length;
    HRESULT result;

    if (D3D_CreateStagingTexture(device, texture) < 0) {
        return -1;
    }

    d3drect.left = x;
    d3drect.right = (LONG)x + w;
    d3drect.top = y;
    d3drect.bottom = (LONG)y + h;

    result = IDirect3DTexture9_LockRect(texture->staging, 0, &locked, &d3drect, 0);
    if (FAILED(result)) {
        return D3D_SetError("LockRect()", result);
    }

    src = (const Uint8 *)pixels;
    dst = (Uint8 *)locked.pBits;
    length = w * SDL_BYTESPERPIXEL(texture->format);
    if (length == pitch && length == locked.Pitch) {
        SDL_memcpy(dst, src, (size_t)length * h);
    } else {
        if (length > pitch) {
            length = pitch;
        }
        if (length > locked.Pitch) {
            length = locked.Pitch;
        }
        for (row = 0; row < h; ++row) {
            SDL_memcpy(dst, src, length);
            src += pitch;
            dst += locked.Pitch;
        }
    }
    result = IDirect3DTexture9_UnlockRect(texture->staging, 0);
    if (FAILED(result)) {
        return D3D_SetError("UnlockRect()", result);
    }
    texture->dirty = SDL_TRUE;

    return 0;
}

static void D3D_DestroyTextureRep(D3D_TextureRep *texture)
{
    if (texture->texture) {
        IDirect3DTexture9_Release(texture->texture);
        texture->texture = NULL;
    }
    if (texture->staging) {
        IDirect3DTexture9_Release(texture->staging);
        texture->staging = NULL;
    }
}

static int D3D_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata;
    DWORD usage;

    texturedata = (D3D_TextureData *)SDL_calloc(1, sizeof(*texturedata));
    if (!texturedata) {
        return SDL_OutOfMemory();
    }
    texturedata->scaleMode = (texture->scaleMode == SDL_ScaleModeNearest) ? D3DTEXF_POINT : D3DTEXF_LINEAR;

    texture->driverdata = texturedata;

    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        usage = D3DUSAGE_RENDERTARGET;
    } else {
        usage = 0;
    }

    if (D3D_CreateTextureRep(data->device, &texturedata->texture, usage, texture->format, PixelFormatToD3DFMT(texture->format), texture->w, texture->h) < 0) {
        return -1;
    }
#if SDL_HAVE_YUV
    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        texturedata->yuv = SDL_TRUE;

        if (D3D_CreateTextureRep(data->device, &texturedata->utexture, usage, texture->format, PixelFormatToD3DFMT(texture->format), (texture->w + 1) / 2, (texture->h + 1) / 2) < 0) {
            return -1;
        }

        if (D3D_CreateTextureRep(data->device, &texturedata->vtexture, usage, texture->format, PixelFormatToD3DFMT(texture->format), (texture->w + 1) / 2, (texture->h + 1) / 2) < 0) {
            return -1;
        }
    }
#endif
    return 0;
}

static int D3D_RecreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;

    if (!texturedata) {
        return 0;
    }

    if (D3D_RecreateTextureRep(data->device, &texturedata->texture) < 0) {
        return -1;
    }
#if SDL_HAVE_YUV
    if (texturedata->yuv) {
        if (D3D_RecreateTextureRep(data->device, &texturedata->utexture) < 0) {
            return -1;
        }

        if (D3D_RecreateTextureRep(data->device, &texturedata->vtexture) < 0) {
            return -1;
        }
    }
#endif
    return 0;
}

static int D3D_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                             const SDL_Rect *rect, const void *pixels, int pitch)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;

    if (!texturedata) {
        return SDL_SetError("Texture is not currently available");
    }

    if (D3D_UpdateTextureRep(data->device, &texturedata->texture, rect->x, rect->y, rect->w, rect->h, pixels, pitch) < 0) {
        return -1;
    }
#if SDL_HAVE_YUV
    if (texturedata->yuv) {
        /* Skip to the correct offset into the next texture */
        pixels = (const void *)((const Uint8 *)pixels + rect->h * pitch);

        if (D3D_UpdateTextureRep(data->device, texture->format == SDL_PIXELFORMAT_YV12 ? &texturedata->vtexture : &texturedata->utexture, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, pixels, (pitch + 1) / 2) < 0) {
            return -1;
        }

        /* Skip to the correct offset into the next texture */
        pixels = (const void *)((const Uint8 *)pixels + ((rect->h + 1) / 2) * ((pitch + 1) / 2));
        if (D3D_UpdateTextureRep(data->device, texture->format == SDL_PIXELFORMAT_YV12 ? &texturedata->utexture : &texturedata->vtexture, rect->x / 2, (rect->y + 1) / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, pixels, (pitch + 1) / 2) < 0) {
            return -1;
        }
    }
#endif
    return 0;
}

#if SDL_HAVE_YUV
static int D3D_UpdateTextureYUV(SDL_Renderer *renderer, SDL_Texture *texture,
                                const SDL_Rect *rect,
                                const Uint8 *Yplane, int Ypitch,
                                const Uint8 *Uplane, int Upitch,
                                const Uint8 *Vplane, int Vpitch)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;

    if (!texturedata) {
        return SDL_SetError("Texture is not currently available");
    }

    if (D3D_UpdateTextureRep(data->device, &texturedata->texture, rect->x, rect->y, rect->w, rect->h, Yplane, Ypitch) < 0) {
        return -1;
    }
    if (D3D_UpdateTextureRep(data->device, &texturedata->utexture, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, Uplane, Upitch) < 0) {
        return -1;
    }
    if (D3D_UpdateTextureRep(data->device, &texturedata->vtexture, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, Vplane, Vpitch) < 0) {
        return -1;
    }
    return 0;
}
#endif

static int D3D_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                           const SDL_Rect *rect, void **pixels, int *pitch)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;
    IDirect3DDevice9 *device = data->device;

    if (!texturedata) {
        return SDL_SetError("Texture is not currently available");
    }
#if SDL_HAVE_YUV
    texturedata->locked_rect = *rect;

    if (texturedata->yuv) {
        /* It's more efficient to upload directly... */
        if (!texturedata->pixels) {
            texturedata->pitch = texture->w;
            texturedata->pixels = (Uint8 *)SDL_malloc((texture->h * texturedata->pitch * 3) / 2);
            if (!texturedata->pixels) {
                return SDL_OutOfMemory();
            }
        }
        *pixels =
            (void *)(texturedata->pixels + rect->y * texturedata->pitch +
                     rect->x * SDL_BYTESPERPIXEL(texture->format));
        *pitch = texturedata->pitch;
    } else
#endif
    {
        RECT d3drect;
        D3DLOCKED_RECT locked;
        HRESULT result;

        if (D3D_CreateStagingTexture(device, &texturedata->texture) < 0) {
            return -1;
        }

        d3drect.left = rect->x;
        d3drect.right = (LONG)rect->x + rect->w;
        d3drect.top = rect->y;
        d3drect.bottom = (LONG)rect->y + rect->h;

        result = IDirect3DTexture9_LockRect(texturedata->texture.staging, 0, &locked, &d3drect, 0);
        if (FAILED(result)) {
            return D3D_SetError("LockRect()", result);
        }
        *pixels = locked.pBits;
        *pitch = locked.Pitch;
    }
    return 0;
}

static void D3D_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;

    if (!texturedata) {
        return;
    }
#if SDL_HAVE_YUV
    if (texturedata->yuv) {
        const SDL_Rect *rect = &texturedata->locked_rect;
        void *pixels =
            (void *)(texturedata->pixels + rect->y * texturedata->pitch +
                     rect->x * SDL_BYTESPERPIXEL(texture->format));
        D3D_UpdateTexture(renderer, texture, rect, pixels, texturedata->pitch);
    } else
#endif
    {
        IDirect3DTexture9_UnlockRect(texturedata->texture.staging, 0);
        texturedata->texture.dirty = SDL_TRUE;
        if (data->drawstate.texture == texture) {
            data->drawstate.texture = NULL;
            data->drawstate.shader = NULL;
            IDirect3DDevice9_SetPixelShader(data->device, NULL);
            IDirect3DDevice9_SetTexture(data->device, 0, NULL);
        }
    }
}

static void D3D_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;

    if (!texturedata) {
        return;
    }

    texturedata->scaleMode = (scaleMode == SDL_ScaleModeNearest) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
}

static int D3D_SetRenderTargetInternal(SDL_Renderer *renderer, SDL_Texture *texture)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *texturedata;
    D3D_TextureRep *texturerep;
    HRESULT result;
    IDirect3DDevice9 *device = data->device;

    /* Release the previous render target if it wasn't the default one */
    if (data->currentRenderTarget) {
        IDirect3DSurface9_Release(data->currentRenderTarget);
        data->currentRenderTarget = NULL;
    }

    if (!texture) {
        IDirect3DDevice9_SetRenderTarget(data->device, 0, data->defaultRenderTarget);
        return 0;
    }

    texturedata = (D3D_TextureData *)texture->driverdata;
    if (!texturedata) {
        return SDL_SetError("Texture is not currently available");
    }

    /* Make sure the render target is updated if it was locked and written to */
    texturerep = &texturedata->texture;
    if (texturerep->dirty && texturerep->staging) {
        if (!texturerep->texture) {
            result = IDirect3DDevice9_CreateTexture(device, texturerep->w, texturerep->h, 1, texturerep->usage,
                                                    PixelFormatToD3DFMT(texturerep->format), D3DPOOL_DEFAULT, &texturerep->texture, NULL);
            if (FAILED(result)) {
                return D3D_SetError("CreateTexture(D3DPOOL_DEFAULT)", result);
            }
        }
/*
        result = IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)texturerep->staging, (IDirect3DBaseTexture9 *)texturerep->texture);
        if (FAILED(result)) {
            return D3D_SetError("UpdateTexture()", result);
        }
*/
        texturerep->dirty = SDL_FALSE;
    }

    result = IDirect3DTexture9_GetSurfaceLevel(texturedata->texture.texture, 0, &data->currentRenderTarget);
    if (FAILED(result)) {
        return D3D_SetError("GetSurfaceLevel()", result);
    }
    result = IDirect3DDevice9_SetRenderTarget(data->device, 0, data->currentRenderTarget);
    if (FAILED(result)) {
        return D3D_SetError("SetRenderTarget()", result);
    }

    return 0;
}

static int D3D_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    if (D3D_ActivateRenderer(renderer) < 0) {
        return -1;
    }

    return D3D_SetRenderTargetInternal(renderer, texture);
}

static int D3D_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    return 0; /* nothing to do in this backend. */
}

static int D3D_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    const DWORD color = D3DCOLOR_ARGB(cmd->data.draw.a, cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b);
    const size_t vertslen = count * sizeof(Vertex);
    Vertex *verts = (Vertex *)SDL_AllocateRenderVertices(renderer, vertslen, 0, &cmd->data.draw.first);
    int i;

    if (!verts) {
        return -1;
    }

    SDL_memset(verts, '\0', vertslen);
    cmd->data.draw.count = count;

    for (i = 0; i < count; i++, verts++, points++) {
        verts->x = points->x;
        verts->y = points->y;
        verts->color = color;
    }

    return 0;
}

static int D3D_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                             const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
                             int num_vertices, const void *indices, int num_indices, int size_indices,
                             float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;
    Vertex *verts = (Vertex *)SDL_AllocateRenderVertices(renderer, count * sizeof(Vertex), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    for (i = 0; i < count; i++) {
        int j;
        float *xy_;
        SDL_Color col_;
        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        } else {
            j = i;
        }

        xy_ = (float *)((char *)xy + j * xy_stride);
        col_ = *(SDL_Color *)((char *)color + j * color_stride);

        verts->x = xy_[0] * scale_x - 0.5f;
        verts->y = xy_[1] * scale_y - 0.5f;
        verts->z = 0.0f;
        verts->color = D3DCOLOR_ARGB(col_.a, col_.r, col_.g, col_.b);

        if (texture) {
            float *uv_ = (float *)((char *)uv + j * uv_stride);
            verts->u = uv_[0];
            verts->v = uv_[1];
        } else {
            verts->u = 0.0f;
            verts->v = 0.0f;
        }

        verts += 1;
    }
    return 0;
}

static int UpdateDirtyTexture(IDirect3DDevice9 *device, D3D_TextureRep *texture)
{
    if (texture->dirty && texture->staging) {
        HRESULT result;
        if (!texture->texture) {
            result = IDirect3DDevice9_CreateTexture(device, texture->w, texture->h, 1, texture->usage,
                                                    PixelFormatToD3DFMT(texture->format), D3DPOOL_DEFAULT, &texture->texture, NULL);
            if (FAILED(result)) {
                return D3D_SetError("CreateTexture(D3DPOOL_DEFAULT)", result);
            }
        }
/*
		result = IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9 *)texture->staging, (IDirect3DBaseTexture9 *)texture->texture);
        if (FAILED(result)) {
            return D3D_SetError("UpdateTexture()", result);
        }
*/
        texture->dirty = SDL_FALSE;
    }
    return 0;
}

static int BindTextureRep(IDirect3DDevice9 *device, D3D_TextureRep *texture, DWORD sampler)
{
    HRESULT result;
    UpdateDirtyTexture(device, texture);
    result = IDirect3DDevice9_SetTexture(device, sampler, (IDirect3DBaseTexture9 *)texture->texture);
    if (FAILED(result)) {
        return D3D_SetError("SetTexture()", result);
    }

#ifdef __XBOX_360__
    /* And set the fetch constant directly, because SetTexture does not arrive
     * intact under this toolchain.
     *
     * D3DDevice_SetTexture_Inline works out which fetch constants a binding
     * dirties and passes that to the library as a UINT64:
     *
     *     UINT64 pendingMask3 = D3DTAG_MASKENCODE(...);
     *     D3DDevice_SetTexture(pDevice, Sampler, pTexture, pendingMask3);
     *
     * OXDK360's llvm/README.md lists 64-bit integer arguments as a known gap
     * in the Xenon ABI patch: clang splits a long long into two 32-bit halves
     * across two argument slots, where the XDK's own compiler passes one
     * 64-bit register. So the mask the library receives is not the mask that
     * was computed, the fetch constant is never marked dirty, and the GPU goes
     * on sampling whatever it had -- which is why every texture on screen was
     * the same one however correctly each was created, uploaded and bound.
     *
     * SetTextureFetchConstant takes a UINT and a pointer and nothing else, so
     * it crosses the boundary intact. SetTexture above is left in place to
     * keep D3D's own bookkeeping straight; this is what actually reaches the
     * hardware.
     *
     * The same gap applies to SetStreamSource, SetVertexShaderConstantFN and
     * SetPixelShaderConstantFN, which take the same kind of mask. Fixing it
     * properly belongs in OXDK360, not here. */
    IDirect3DDevice9_SetTextureFetchConstant(
        device, GPU_CONVERT_D3D_TO_HARDWARE_TEXTUREFETCHCONSTANT(sampler),
        (IDirect3DBaseTexture9 *)texture->texture);
#endif
    return 0;
}

static void UpdateTextureScaleMode(D3D_RenderData *data, D3D_TextureData *texturedata, unsigned index)
{
    if (texturedata->scaleMode != data->scaleMode[index]) {
        IDirect3DDevice9_SetSamplerState(data->device, index, D3DSAMP_MINFILTER,
                                         texturedata->scaleMode);
        IDirect3DDevice9_SetSamplerState(data->device, index, D3DSAMP_MAGFILTER,
                                         texturedata->scaleMode);
        IDirect3DDevice9_SetSamplerState(data->device, index, D3DSAMP_ADDRESSU,
                                         D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(data->device, index, D3DSAMP_ADDRESSV,
                                         D3DTADDRESS_CLAMP);
        data->scaleMode[index] = texturedata->scaleMode;
    }
}

static int SetupTextureState(D3D_RenderData *data, SDL_Texture *texture, LPDIRECT3DPIXELSHADER9 *shader)
{
    D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;

    SDL_assert(*shader == NULL);

    if (!texturedata) {
        return SDL_SetError("Texture is not currently available");
    }

    UpdateTextureScaleMode(data, texturedata, 0);

    if (BindTextureRep(data->device, &texturedata->texture, 0) < 0) {
        return -1;
    }
#if SDL_HAVE_YUV
    if (texturedata->yuv) {
        switch (SDL_GetYUVConversionModeForResolution(texture->w, texture->h)) {
        case SDL_YUV_CONVERSION_JPEG:
            *shader = data->shaders[SHADER_YUV_JPEG];
            break;
        case SDL_YUV_CONVERSION_BT601:
            *shader = data->shaders[SHADER_YUV_BT601];
            break;
        case SDL_YUV_CONVERSION_BT709:
            *shader = data->shaders[SHADER_YUV_BT709];
            break;
        default:
            return SDL_SetError("Unsupported YUV conversion mode");
        }

        UpdateTextureScaleMode(data, texturedata, 1);
        UpdateTextureScaleMode(data, texturedata, 2);

        if (BindTextureRep(data->device, &texturedata->utexture, 1) < 0) {
            return -1;
        }
        if (BindTextureRep(data->device, &texturedata->vtexture, 2) < 0) {
            return -1;
        }
    }
#endif
    return 0;
}

static int SetDrawState(D3D_RenderData *data, const SDL_RenderCommand *cmd)
{
    SDL_Texture *texture = cmd->data.draw.texture;
    const SDL_BlendMode blend = cmd->data.draw.blend;

    if (texture != data->drawstate.texture) {
#if SDL_HAVE_YUV
        D3D_TextureData *oldtexturedata = data->drawstate.texture ? (D3D_TextureData *)data->drawstate.texture->driverdata : NULL;
        D3D_TextureData *newtexturedata = texture ? (D3D_TextureData *)texture->driverdata : NULL;
#endif
        LPDIRECT3DPIXELSHADER9 shader = NULL;

        /* disable any enabled textures we aren't going to use, let SetupTextureState() do the rest. */
        if (!texture) {
            IDirect3DDevice9_SetTexture(data->device, 0, NULL);
        }
#if SDL_HAVE_YUV
        if ((!newtexturedata || !newtexturedata->yuv) && (oldtexturedata && oldtexturedata->yuv)) {
            IDirect3DDevice9_SetTexture(data->device, 1, NULL);
            IDirect3DDevice9_SetTexture(data->device, 2, NULL);
        }
#endif
        if (texture && SetupTextureState(data, texture, &shader) < 0) {
            return -1;
        }

        if (shader != data->drawstate.shader) {
            const HRESULT result = IDirect3DDevice9_SetPixelShader(data->device, shader);
            if (FAILED(result)) {
                return D3D_SetError("IDirect3DDevice9_SetPixelShader()", result);
            }
            data->drawstate.shader = shader;
        }

        data->drawstate.texture = texture;
    } else if (texture) {
        D3D_TextureData *texturedata = (D3D_TextureData *)texture->driverdata;
        UpdateDirtyTexture(data->device, &texturedata->texture);
#if SDL_HAVE_YUV
        if (texturedata->yuv) {
            UpdateDirtyTexture(data->device, &texturedata->utexture);
            UpdateDirtyTexture(data->device, &texturedata->vtexture);
        }
#endif
    }

    if (blend != data->drawstate.blend) {
        if (blend == SDL_BLENDMODE_NONE) {
            IDirect3DDevice9_SetRenderState(data->device, D3DRS_ALPHABLENDENABLE, FALSE);
        } else {
            IDirect3DDevice9_SetRenderState(data->device, D3DRS_ALPHABLENDENABLE, TRUE);
            IDirect3DDevice9_SetRenderState(data->device, D3DRS_SRCBLEND,
                                            GetBlendFunc(SDL_GetBlendModeSrcColorFactor(blend)));
            IDirect3DDevice9_SetRenderState(data->device, D3DRS_DESTBLEND,
                                            GetBlendFunc(SDL_GetBlendModeDstColorFactor(blend)));
            IDirect3DDevice9_SetRenderState(data->device, D3DRS_BLENDOP,
                                            GetBlendEquation(SDL_GetBlendModeColorOperation(blend)));
            if (data->enableSeparateAlphaBlend) {
                IDirect3DDevice9_SetRenderState(data->device, D3DRS_SRCBLENDALPHA,
                                                GetBlendFunc(SDL_GetBlendModeSrcAlphaFactor(blend)));
                IDirect3DDevice9_SetRenderState(data->device, D3DRS_DESTBLENDALPHA,
                                                GetBlendFunc(SDL_GetBlendModeDstAlphaFactor(blend)));
                IDirect3DDevice9_SetRenderState(data->device, D3DRS_BLENDOPALPHA,
                                                GetBlendEquation(SDL_GetBlendModeAlphaOperation(blend)));
            }
        }

        data->drawstate.blend = blend;
    }

    if (data->drawstate.viewport_dirty) {
        const SDL_Rect *viewport = &data->drawstate.viewport;
        D3DVIEWPORT9 d3dviewport;
        d3dviewport.X = viewport->x;
        d3dviewport.Y = viewport->y;
        d3dviewport.Width = viewport->w;
        d3dviewport.Height = viewport->h;
        d3dviewport.MinZ = 0.0f;
        d3dviewport.MaxZ = 1.0f;
        IDirect3DDevice9_SetViewport(data->device, &d3dviewport);

#ifdef __XBOX_360__
        /* SetTransform does not exist here, so the ortho projection goes to the
           vertex shader as constants instead. */
        if (viewport->w && viewport->h) {
            x360_identity(&g_x360_proj);
            g_x360_proj.m[0][0] =  2.0f / viewport->w;
            g_x360_proj.m[1][1] = -2.0f / viewport->h;
            g_x360_proj.m[3][0] = -1.0f;
            g_x360_proj.m[3][1] =  1.0f;
        }
#endif

        /* Set an orthographic projection matrix */
/*
		if (viewport->w && viewport->h) {
            D3DMATRIX d3dmatrix;
            SDL_zero(d3dmatrix);
            d3dmatrix.m[0][0] = 2.0f / viewport->w;
            d3dmatrix.m[1][1] = -2.0f / viewport->h;
            d3dmatrix.m[2][2] = 1.0f;
            d3dmatrix.m[3][0] = -1.0f;
            d3dmatrix.m[3][1] = 1.0f;
            d3dmatrix.m[3][3] = 1.0f;
            IDirect3DDevice9_SetTransform(data->device, D3DTS_PROJECTION, &d3dmatrix);
        }
*/
        data->drawstate.viewport_dirty = SDL_FALSE;
    }

    if (data->drawstate.cliprect_enabled_dirty) {
        IDirect3DDevice9_SetRenderState(data->device, D3DRS_SCISSORTESTENABLE, data->drawstate.cliprect_enabled ? TRUE : FALSE);
        data->drawstate.cliprect_enabled_dirty = SDL_FALSE;
    }

    if (data->drawstate.cliprect_dirty) {
        const SDL_Rect *viewport = &data->drawstate.viewport;
        const SDL_Rect *rect = &data->drawstate.cliprect;
        RECT d3drect;
        d3drect.left = (LONG)viewport->x + rect->x;
        d3drect.top = (LONG)viewport->y + rect->y;
        d3drect.right = (LONG)viewport->x + rect->x + rect->w;
        d3drect.bottom = (LONG)viewport->y + rect->y + rect->h;
        IDirect3DDevice9_SetScissorRect(data->device, &d3drect);
        data->drawstate.cliprect_dirty = SDL_FALSE;
    }


#ifdef __XBOX_360__
    /* Bind vertex declaration, pixel shader and projection -- the fixed function
       calls SDL uses for this do nothing on Xenos. */
    {
        GLShaders_Select(texture ? GLPS_MODULATE : GLPS_COLOR_ONLY);
        GLShaders_Apply(data->device,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                        &g_x360_proj);

        /* GLShaders_Apply ends with a SetPixelShader of its own, which undoes
           the YUV shader chosen above -- so a YUV texture drew through
           GLPS_MODULATE, which samples the luma plane alone and modulates it
           by the vertex colour. That is a correct greyscale picture, which is
           why it looks like a working renderer rather than a broken one.
           Rebind ours last. The vertex shader and its constants are still the
           ones just set, which is what we want; only the pixel stage differs. */
        if (data->drawstate.shader) {
            IDirect3DDevice9_SetPixelShader(data->device, data->drawstate.shader);
        }
    }
#endif

    return 0;
}

static int D3D_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    const int vboidx = data->currentVertexBuffer;
    IDirect3DVertexBuffer9 *vbo = NULL;
    const SDL_bool istarget = renderer->target != NULL;

#ifdef __XBOX_360__
    /* Draw immediate on this console: no vertex buffer.
     *
     * Every renderer on this machine that draws many textures in a frame does
     * it this way and none of them uses a vertex buffer -- FakeGLX360 for
     * Quake, XBMC-360's GUI textures, and XBMC-360's font cache, all
     * SetTexture followed by DrawPrimitiveUP. The SDL2x360 titles that do work
     * (Chocolate Doom, Doom Retro) draw a single framebuffer texture, so the
     * batched vertex-buffer path has never had to interleave two textures.
     *
     * The buffer it replaces is filled by a plain memcpy into a locked
     * D3DPOOL_DEFAULT buffer with no DISCARD and no fence, and then cycles
     * through several buffers "so D3D has some time with the data" -- a
     * heuristic, not synchronisation. DrawPrimitiveUP copies the vertices into
     * the command buffer, which is neither racy nor stale. */
    const SDL_bool useVertexBuffer = SDL_FALSE;
#else
    const SDL_bool useVertexBuffer = SDL_TRUE;
#endif

    if (D3D_ActivateRenderer(renderer) < 0) {
        return -1;
    }

    if (useVertexBuffer && vertsize > 0) {
        /* upload the new VBO data for this set of commands. */
        vbo = data->vertexBuffers[vboidx];
        if (data->vertexBufferSize[vboidx] < vertsize) {
            const DWORD usage = D3DUSAGE_WRITEONLY;
            const DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
            if (vbo) {
                IDirect3DVertexBuffer9_Release(vbo);
            }

            if (FAILED(IDirect3DDevice9_CreateVertexBuffer(data->device, (UINT)vertsize, usage, fvf, D3DPOOL_DEFAULT, &vbo, NULL))) {
                vbo = NULL;
            }
            data->vertexBuffers[vboidx] = vbo;
            data->vertexBufferSize[vboidx] = vbo ? vertsize : 0;
        }
        /* This was behind #ifdef WIP, so the vertex buffer was never filled and the
           GPU drew whatever was already in that memory. */
        if (vbo) {
            void *ptr;
            if (FAILED(IDirect3DVertexBuffer9_Lock(vbo, 0, (UINT)vertsize, &ptr, 0 /* no D3DLOCK_DISCARD on this console */))) {
                vbo = NULL; /* oh well, we'll do immediate mode drawing.  :(  */
            } else {
                SDL_memcpy(ptr, vertices, vertsize);
                if (FAILED(IDirect3DVertexBuffer9_Unlock(vbo))) {
                    vbo = NULL; /* oh well, we'll do immediate mode drawing.  :(  */
                }
            }
        }
        /* cycle through a few VBOs so D3D has some time with the data before we replace it. */
        if (vbo) {
            data->currentVertexBuffer++;
            if (data->currentVertexBuffer >= SDL_arraysize(data->vertexBuffers)) {
                data->currentVertexBuffer = 0;
            }
        } else if (!data->reportedVboProblem) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL failed to get a vertex buffer for this Direct3D 9 rendering batch!");
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Dropping back to a slower method.");
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "This might be a brief hiccup, but if performance is bad, this is probably why.");
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "This error will not be logged again for this renderer.");
            data->reportedVboProblem = SDL_TRUE;
        }
    }

    IDirect3DDevice9_SetStreamSource(data->device, 0, vbo, 0, sizeof(Vertex));

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETDRAWCOLOR:
        {
            /* currently this is sent with each vertex, but if we move to
               shaders, we can put this in a uniform here and reduce vertex
               buffer bandwidth */
            break;
        }

        case SDL_RENDERCMD_SETVIEWPORT:
        {
            SDL_Rect *viewport = &data->drawstate.viewport;
            if (SDL_memcmp(viewport, &cmd->data.viewport.rect, sizeof(cmd->data.viewport.rect)) != 0) {
                SDL_copyp(viewport, &cmd->data.viewport.rect);
                data->drawstate.viewport_dirty = SDL_TRUE;
                data->drawstate.cliprect_dirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT:
        {
            const SDL_Rect *rect = &cmd->data.cliprect.rect;
            if (data->drawstate.cliprect_enabled != cmd->data.cliprect.enabled) {
                data->drawstate.cliprect_enabled = cmd->data.cliprect.enabled;
                data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
            }

            if (SDL_memcmp(&data->drawstate.cliprect, rect, sizeof(*rect)) != 0) {
                SDL_copyp(&data->drawstate.cliprect, rect);
                data->drawstate.cliprect_dirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_CLEAR:
        {
            const DWORD color = D3DCOLOR_ARGB(cmd->data.color.a, cmd->data.color.r, cmd->data.color.g, cmd->data.color.b);
            const SDL_Rect *viewport = &data->drawstate.viewport;
            const int backw = istarget ? renderer->target->w : data->pparams.BackBufferWidth;
            const int backh = istarget ? renderer->target->h : data->pparams.BackBufferHeight;
            const SDL_bool viewport_equal = ((viewport->x == 0) && (viewport->y == 0) && (viewport->w == backw) && (viewport->h == backh)) ? SDL_TRUE : SDL_FALSE;

            if (data->drawstate.cliprect_enabled || data->drawstate.cliprect_enabled_dirty) {
                IDirect3DDevice9_SetRenderState(data->device, D3DRS_SCISSORTESTENABLE, FALSE);
                data->drawstate.cliprect_enabled_dirty = data->drawstate.cliprect_enabled;
            }

            /* Don't reset the viewport if we don't have to! */
            if (!data->drawstate.viewport_dirty && viewport_equal) {
                IDirect3DDevice9_Clear(data->device, 0, NULL, D3DCLEAR_TARGET, color, 0.0f, 0);
            } else {
                /* Clear is defined to clear the entire render target */
                D3DVIEWPORT9 wholeviewport = { 0, 0, 0, 0, 0.0f, 1.0f };
                wholeviewport.Width = backw;
                wholeviewport.Height = backh;
                IDirect3DDevice9_SetViewport(data->device, &wholeviewport);
                data->drawstate.viewport_dirty = SDL_TRUE; /* we still need to (re)set orthographic projection, so always mark it dirty. */
                IDirect3DDevice9_Clear(data->device, 0, NULL, D3DCLEAR_TARGET, color, 0.0f, 0);
            }

            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
        {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            SetDrawState(data, cmd);
            if (vbo) {
                IDirect3DDevice9_DrawPrimitive(data->device, D3DPT_POINTLIST, (UINT)(first / sizeof(Vertex)), (UINT)count);
            } else {
                const Vertex *verts = (Vertex *)(((Uint8 *)vertices) + first);
                IDirect3DDevice9_DrawPrimitiveUP(data->device, D3DPT_POINTLIST, (UINT)count, verts, sizeof(Vertex));
            }
            break;
        }

        case SDL_RENDERCMD_DRAW_LINES:
        {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const Vertex *verts = (Vertex *)(((Uint8 *)vertices) + first);

            /* DirectX 9 has the same line rasterization semantics as GDI,
               so we need to close the endpoint of the line with a second draw call.
               NOLINTNEXTLINE(clang-analyzer-core.NullDereference): FIXME: Can verts truly not be NULL ? */
            const SDL_bool close_endpoint = ((count == 2) || (verts[0].x != verts[count - 1].x) || (verts[0].y != verts[count - 1].y));

            SetDrawState(data, cmd);

            if (vbo) {
                IDirect3DDevice9_DrawPrimitive(data->device, D3DPT_LINESTRIP, (UINT)(first / sizeof(Vertex)), (UINT)(count - 1));
                if (close_endpoint) {
                    IDirect3DDevice9_DrawPrimitive(data->device, D3DPT_POINTLIST, (UINT)((first / sizeof(Vertex)) + (count - 1)), 1);
                }
            } else {
                IDirect3DDevice9_DrawPrimitiveUP(data->device, D3DPT_LINESTRIP, (UINT)(count - 1), verts, sizeof(Vertex));
                if (close_endpoint) {
                    IDirect3DDevice9_DrawPrimitiveUP(data->device, D3DPT_POINTLIST, 1, &verts[count - 1], sizeof(Vertex));
                }
            }
            break;
        }

        case SDL_RENDERCMD_FILL_RECTS: /* unused */
            break;

        case SDL_RENDERCMD_COPY: /* unused */
            break;

        case SDL_RENDERCMD_COPY_EX: /* unused */
            break;

        case SDL_RENDERCMD_GEOMETRY:
        {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            SetDrawState(data, cmd);
            if (vbo) {
                IDirect3DDevice9_DrawPrimitive(data->device, D3DPT_TRIANGLELIST, (UINT)(first / sizeof(Vertex)), (UINT)count / 3);
            } else {
                const Vertex *verts = (Vertex *)(((Uint8 *)vertices) + first);
                IDirect3DDevice9_DrawPrimitiveUP(data->device, D3DPT_TRIANGLELIST, (UINT)count / 3, verts, sizeof(Vertex));
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

static int D3D_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect,
                                Uint32 format, void *pixels, int pitch)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    D3DSURFACE_DESC desc;
    LPDIRECT3DSURFACE9 backBuffer;
    LPDIRECT3DSURFACE9 surface;
    RECT d3drect;
    D3DLOCKED_RECT locked;
    HRESULT result;
    int status;

    if (data->currentRenderTarget) {
        backBuffer = data->currentRenderTarget;
    } else {
        backBuffer = data->defaultRenderTarget;
    }

    result = IDirect3DSurface9_GetDesc(backBuffer, &desc);
    if (FAILED(result)) {
        return D3D_SetError("GetDesc()", result);
    }
/*
    result = IDirect3DDevice9_CreateOffscreenPlainSurface(data->device, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &surface, NULL);
    if (FAILED(result)) {
        return D3D_SetError("CreateOffscreenPlainSurface()", result);
    }

    result = IDirect3DDevice9_GetRenderTargetData(data->device, backBuffer, surface);
    if (FAILED(result)) {
        IDirect3DSurface9_Release(surface);
        return D3D_SetError("GetRenderTargetData()", result);
    }
*/
    d3drect.left = rect->x;
    d3drect.right = (LONG)rect->x + rect->w;
    d3drect.top = rect->y;
    d3drect.bottom = (LONG)rect->y + rect->h;

    result = IDirect3DSurface9_LockRect(surface, &locked, &d3drect, D3DLOCK_READONLY);
    if (FAILED(result)) {
        IDirect3DSurface9_Release(surface);
        return D3D_SetError("LockRect()", result);
    }

    status = SDL_ConvertPixels(rect->w, rect->h,
                               D3DFMTToPixelFormat(desc.Format), locked.pBits, locked.Pitch,
                               format, pixels, pitch);

    IDirect3DSurface9_UnlockRect(surface);

    IDirect3DSurface9_Release(surface);

    return status;
}

static int D3D_RenderPresent(SDL_Renderer *renderer)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    HRESULT result;

    if (!data->beginScene) {
        IDirect3DDevice9_EndScene(data->device);
        data->beginScene = SDL_TRUE;
    }

    /* result was read here before being assigned (upstream fills it with
       TestCooperativeLevel first), so a healthy device could be reset. The
       device is never lost on this console, so drop the check. */
    result = IDirect3DDevice9_Present(data->device, NULL, NULL, NULL, NULL);
    if (FAILED(result)) {
        return D3D_SetError("Present()", result);
    }
    return 0;
}

static void D3D_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    D3D_RenderData *renderdata = (D3D_RenderData *)renderer->driverdata;
    D3D_TextureData *data = (D3D_TextureData *)texture->driverdata;

    if (renderdata->drawstate.texture == texture) {
        renderdata->drawstate.texture = NULL;
        renderdata->drawstate.shader = NULL;
        IDirect3DDevice9_SetPixelShader(renderdata->device, NULL);
        IDirect3DDevice9_SetTexture(renderdata->device, 0, NULL);
#if SDL_HAVE_YUV
        if (data->yuv) {
            IDirect3DDevice9_SetTexture(renderdata->device, 1, NULL);
            IDirect3DDevice9_SetTexture(renderdata->device, 2, NULL);
        }
#endif
    }

    if (!data) {
        return;
    }

    D3D_DestroyTextureRep(&data->texture);
#if SDL_HAVE_YUV
    D3D_DestroyTextureRep(&data->utexture);
    D3D_DestroyTextureRep(&data->vtexture);
    SDL_free(data->pixels);
#endif
    SDL_free(data);
    texture->driverdata = NULL;
}

static void D3D_DestroyRenderer(SDL_Renderer *renderer)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;

    if (data) {
        int i;

        /* Release the render target */
        if (data->defaultRenderTarget) {
            IDirect3DSurface9_Release(data->defaultRenderTarget);
            data->defaultRenderTarget = NULL;
        }
        if (data->currentRenderTarget) {
            IDirect3DSurface9_Release(data->currentRenderTarget);
            data->currentRenderTarget = NULL;
        }
#if SDL_HAVE_YUV
        for (i = 0; i < SDL_arraysize(data->shaders); ++i) {
            if (data->shaders[i]) {
                IDirect3DPixelShader9_Release(data->shaders[i]);
                data->shaders[i] = NULL;
            }
        }
#endif
        /* Release all vertex buffers */
        for (i = 0; i < SDL_arraysize(data->vertexBuffers); ++i) {
            if (data->vertexBuffers[i]) {
                IDirect3DVertexBuffer9_Release(data->vertexBuffers[i]);
            }
            data->vertexBuffers[i] = NULL;
        }
        if (data->device) {
            IDirect3DDevice9_Release(data->device);
            data->device = NULL;
        }
        if (data->d3d) {
            IDirect3D9_Release(data->d3d);
            SDL_UnloadObject(data->d3dDLL);
        }
        SDL_free(data);
    }
}

static int D3D_Reset(SDL_Renderer *renderer)
{
    D3D_RenderData *data = (D3D_RenderData *)renderer->driverdata;
    const Float4X4 d3dmatrix = MatrixIdentity();
    HRESULT result;
    SDL_Texture *texture;
    int i;

    /* Cancel any scene that we've started */
    if (!data->beginScene) {
        IDirect3DDevice9_EndScene(data->device);
        data->beginScene = SDL_TRUE;
    }

    /* Release the default render target before reset */
    if (data->defaultRenderTarget) {
        IDirect3DSurface9_Release(data->defaultRenderTarget);
        data->defaultRenderTarget = NULL;
    }
    if (data->currentRenderTarget) {
        IDirect3DSurface9_Release(data->currentRenderTarget);
        data->currentRenderTarget = NULL;
    }

    /* Release application render targets */
    for (texture = renderer->textures; texture; texture = texture->next) {
        if (texture->access == SDL_TEXTUREACCESS_TARGET) {
            D3D_DestroyTexture(renderer, texture);
        } else {
            D3D_RecreateTexture(renderer, texture);
        }
    }

    /* Release all vertex buffers */
    for (i = 0; i < SDL_arraysize(data->vertexBuffers); ++i) {
        if (data->vertexBuffers[i]) {
            IDirect3DVertexBuffer9_Release(data->vertexBuffers[i]);
        }
        data->vertexBuffers[i] = NULL;
        data->vertexBufferSize[i] = 0;
    }

    result = IDirect3DDevice9_Reset(data->device, &data->pparams);
    if (FAILED(result)) {
        if (result == D3DERR_DEVICELOST) {
            /* Don't worry about it, we'll reset later... */
            return 0;
        } else {
            return D3D_SetError("Reset()", result);
        }
    }

    /* Allocate application render targets */
    for (texture = renderer->textures; texture; texture = texture->next) {
        if (texture->access == SDL_TEXTUREACCESS_TARGET) {
            D3D_CreateTexture(renderer, texture);
        }
    }

    IDirect3DDevice9_GetRenderTarget(data->device, 0, &data->defaultRenderTarget);
    D3D_InitRenderState(data);
    D3D_SetRenderTargetInternal(renderer, renderer->target);
    data->drawstate.viewport_dirty = SDL_TRUE;
    data->drawstate.cliprect_dirty = SDL_TRUE;
    data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
    data->drawstate.texture = NULL;
    data->drawstate.shader = NULL;
    data->drawstate.blend = SDL_BLENDMODE_INVALID;
/*    IDirect3DDevice9_SetTransform(data->device, D3DTS_VIEW, (D3DMATRIX *)&d3dmatrix); */

    /* Let the application know that render targets were reset */
    {
        SDL_Event event;
        event.type = SDL_RENDER_TARGETS_RESET;
        SDL_PushEvent(&event);
    }

    return 0;
}

static int D3D_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    D3D_RenderData *data = renderer->driverdata;
    if (vsync) {
        data->pparams.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    } else {
        data->pparams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        renderer->info.flags &= ~SDL_RENDERER_PRESENTVSYNC;
    }
    if (D3D_Reset(renderer) < 0) {
        /* D3D_Reset will call SDL_SetError() */
        return -1;
    }
    return 0;
}

int D3D_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, Uint32 flags)
{
    D3D_RenderData *data;
    SDL_SysWMinfo windowinfo;
    HRESULT result;
    D3DPRESENT_PARAMETERS pparams;
    D3DCAPS9 caps;
    DWORD device_flags;
    Uint32 window_flags;
    int w, h;
    SDL_DisplayMode fullscreen_mode;
    int displayIndex;

    data = (D3D_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }

    if (!D3D_LoadDLL(&data->d3d)) {
        SDL_free(data);
        return SDL_SetError("Unable to create Direct3D interface");
    }

    renderer->WindowEvent = D3D_WindowEvent;
    renderer->GetOutputSize = D3D_GetOutputSize;
    renderer->SupportsBlendMode = D3D_SupportsBlendMode;
    renderer->CreateTexture = D3D_CreateTexture;
    renderer->UpdateTexture = D3D_UpdateTexture;
#if SDL_HAVE_YUV
    renderer->UpdateTextureYUV = D3D_UpdateTextureYUV;
#endif
    renderer->LockTexture = D3D_LockTexture;
    renderer->UnlockTexture = D3D_UnlockTexture;
    renderer->SetTextureScaleMode = D3D_SetTextureScaleMode;
    renderer->SetRenderTarget = D3D_SetRenderTarget;
    renderer->QueueSetViewport = D3D_QueueSetViewport;
    renderer->QueueSetDrawColor = D3D_QueueSetViewport; /* SetViewport and SetDrawColor are (currently) no-ops. */
    renderer->QueueDrawPoints = D3D_QueueDrawPoints;
    renderer->QueueDrawLines = D3D_QueueDrawPoints; /* lines and points queue vertices the same way. */
    renderer->QueueGeometry = D3D_QueueGeometry;
    renderer->RunCommandQueue = D3D_RunCommandQueue;
    renderer->RenderReadPixels = D3D_RenderReadPixels;
    renderer->RenderPresent = D3D_RenderPresent;
    renderer->DestroyTexture = D3D_DestroyTexture;
    renderer->DestroyRenderer = D3D_DestroyRenderer;
    renderer->SetVSync = D3D_SetVSync;
    renderer->info = D3D_RenderDriver.info;
    renderer->info.flags = (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    renderer->driverdata = data;

    SDL_VERSION(&windowinfo.version);
    if (!SDL_GetWindowWMInfo(window, &windowinfo) ||
        windowinfo.subsystem != SDL_SYSWM_WINDOWS) {
        SDL_free(data);
        return SDL_SetError("Couldn't get window handle");
    }

    window_flags = SDL_GetWindowFlags(window);
    SDL_GetWindowSizeInPixels(window, &w, &h);
    SDL_GetWindowDisplayMode(window, &fullscreen_mode);

    SDL_zero(pparams);
    /* pparams.hDeviceWindow = windowinfo.info.win.window; */
    pparams.BackBufferWidth = w;
    pparams.BackBufferHeight = h;
    pparams.BackBufferCount = 1;
    pparams.SwapEffect = D3DSWAPEFFECT_DISCARD;

    if (window_flags & SDL_WINDOW_FULLSCREEN && (window_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != SDL_WINDOW_FULLSCREEN_DESKTOP) {
        pparams.Windowed = FALSE;
        pparams.BackBufferFormat = PixelFormatToD3DFMT(fullscreen_mode.format);
        pparams.FullScreen_RefreshRateInHz = fullscreen_mode.refresh_rate;
    } else {
        pparams.Windowed = TRUE;
        pparams.BackBufferFormat = D3DFMT_UNKNOWN;
        pparams.FullScreen_RefreshRateInHz = 0;
    }
    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        pparams.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    } else {
        pparams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    /* Get the adapter for the display that the window is on */
    displayIndex = SDL_GetWindowDisplayIndex(window);

#ifdef __XBOX_360__
    /* No windowed mode here, and D3DFMT_UNKNOWN is not accepted for a back
       buffer. These are what a title is set to. */
    pparams.Windowed                  = FALSE;
    /* Back buffer must match SDL's window size, or the viewport exceeds the surface. */
    pparams.BackBufferWidth           = (w > 0) ? w : 1280;
    pparams.BackBufferHeight          = (h > 0) ? h : 720;
    pparams.BackBufferFormat          = D3DFMT_X8R8G8B8;
    pparams.FullScreen_RefreshRateInHz = 0;
    pparams.EnableAutoDepthStencil    = TRUE;
    pparams.AutoDepthStencilFormat    = D3DFMT_D24S8;

    /* GetDeviceCaps before device creation is unsupported here; this GPU is always
       hardware vertex processing. */
    device_flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
#else
    IDirect3D9_GetDeviceCaps(data->d3d, data->adapter, D3DDEVTYPE_HAL, &caps);

    device_flags = D3DCREATE_FPU_PRESERVE;
    if (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) {
        device_flags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
    }
#endif

    result = IDirect3D9_CreateDevice(data->d3d, data->adapter,
                                     D3DDEVTYPE_HAL,
                                     pparams.hDeviceWindow,
                                     device_flags,
                                     &pparams, &data->device);
#ifdef __XBOX_360__
    GLShaders_Init(data->device);
    x360_identity(&g_x360_proj);
#endif

    /* Keep the parameters the device was created with. The clear and viewport
       paths read data->pparams for the back buffer size; left zeroed, everything
       was clipped to a 0x0 viewport while every call still returned success. */
    data->pparams = pparams;
    if (FAILED(result)) {
        D3D_DestroyRenderer(renderer);
        return D3D_SetError("CreateDevice()", result);
    }
  
#ifdef __XBOX_360__
    /* One known GPU, and the caps query is not supported the same way here, so
       use its known limits. */
    SDL_zero(caps);
    renderer->info.max_texture_width  = 4096;
    renderer->info.max_texture_height = 4096;
    data->enableSeparateAlphaBlend = SDL_TRUE;
#else
    IDirect3DDevice9_GetDeviceCaps(data->device, &caps);
    renderer->info.max_texture_width = caps.MaxTextureWidth;
    renderer->info.max_texture_height = caps.MaxTextureHeight;

    if (caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) {
        data->enableSeparateAlphaBlend = SDL_TRUE;
    }
#endif

    /* Store the default render target */
    IDirect3DDevice9_GetRenderTarget(data->device, 0, &data->defaultRenderTarget);
    data->currentRenderTarget = NULL;

    /* Set up parameters for rendering */
    D3D_InitRenderState(data);
#if SDL_HAVE_YUV
    /* MaxSimultaneousTextures describes the fixed-function multitexture
       pipeline, which Xenos does not have -- it reports 0, and gating on it
       skips the YUV shaders entirely, so every frame silently goes back to
       being converted on the CPU. This GPU has 16 texture fetch constants and
       nothing but shaders; whether the three compile is the only real test. */
#ifdef __XBOX_360__
    if (SDL_TRUE) {
#else
    if (caps.MaxSimultaneousTextures >= 3) {
#endif
        int i;
        for (i = 0; i < SDL_arraysize(data->shaders); ++i) {
            result = D3D9_CreatePixelShader(data->device, (D3D9_Shader)i, &data->shaders[i]);
            if (FAILED(result)) {
                D3D_SetError("CreatePixelShader()", result);
#ifdef __XBOX_360__
                {
                    char line[128];
                    SDL_snprintf(line, sizeof(line),
                                 "[d3d] YUV shader %d failed, hr 0x%08x\r\n",
                                 i, (unsigned)result);
                    OutputDebugStringA(line);
                }
#endif
            }
        }
        if (data->shaders[SHADER_YUV_JPEG] && data->shaders[SHADER_YUV_BT601] && data->shaders[SHADER_YUV_BT709]) {
            renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_YV12;
            renderer->info.texture_formats[renderer->info.num_texture_formats++] = SDL_PIXELFORMAT_IYUV;
        }
    }
#endif
    data->drawstate.viewport_dirty = SDL_TRUE;
    data->drawstate.cliprect_dirty = SDL_TRUE;
    data->drawstate.cliprect_enabled_dirty = SDL_TRUE;
    data->drawstate.blend = SDL_BLENDMODE_INVALID;

    return 0;
}

SDL_RenderDriver D3D_RenderDriver = {
    D3D_CreateRenderer,
    { "direct3d",
      (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE),
      1,
      { SDL_PIXELFORMAT_ARGB8888 },
      0,
      0 }
};
#endif /* SDL_VIDEO_RENDER_D3D */

/* vi: set ts=4 sw=4 expandtab: */
