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

#ifdef SDL_VIDEO_DRIVER_XBOX

#include "SDL_main.h"
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_system.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"

#include "SDL_xboxvideo.h"

/* Initialization/Query functions */
static int  XBOX_VideoInit(_THIS);
static void XBOX_VideoQuit(_THIS);

/* Cache the desktop/current mode we register at init so other code
   (renderer, SetDisplayMode, etc.) can reference it if needed. */
SDL_DisplayMode g_XboxDesktopMode;

/* No-op on Xbox. */
static void XBOX_SuspendScreenSaver(_THIS) { (void)_this; }

/* -------------------------------------------------------------------------- */
/*                             Device lifetime                                */
/* -------------------------------------------------------------------------- */

static void
XBOX_DeleteDevice(SDL_VideoDevice* device)
{
    if (!device) return;

    SDL_VideoData* data = (SDL_VideoData*)device->driverdata;
    if (data) {
        SDL_free(data);
        device->driverdata = NULL;
    }
    SDL_free(device);
}

static SDL_VideoDevice*
XBOX_CreateDevice(void)
{
    SDL_VideoDevice* device = (SDL_VideoDevice*)SDL_calloc(1, sizeof(SDL_VideoDevice));
    SDL_VideoData* data = (SDL_VideoData*)SDL_calloc(1, sizeof(SDL_VideoData));
    if (!device || !data) {
        SDL_free(data);
        SDL_free(device);
        SDL_OutOfMemory();
        return NULL;
    }

    device->driverdata = data;

    /* Core video entry points */
    device->VideoInit = XBOX_VideoInit;
    device->VideoQuit = XBOX_VideoQuit;

    /* Display queries */
    device->GetDisplayBounds = NULL;
    device->GetDisplayUsableBounds = NULL;
    device->GetDisplayDPI = NULL;
    device->GetDisplayModes = XBOX_GetDisplayModes;
    device->SetDisplayMode = XBOX_SetDisplayMode;
    device->PumpEvents = XBOX_PumpEvents;
    device->SuspendScreenSaver = XBOX_SuspendScreenSaver;

    /* Window management (only the ones we actually support on Xbox) */
    device->CreateSDLWindow = XBOX_CreateWindow;
    device->CreateSDLWindowFrom = NULL;                 /* not supported on Xbox */
    device->SetWindowTitle = XBOX_SetWindowTitle;
    device->SetWindowIcon = XBOX_SetWindowIcon;
    device->SetWindowPosition = XBOX_SetWindowPosition;
    device->SetWindowSize = XBOX_SetWindowSize;
    device->GetWindowBordersSize = NULL;
    device->SetWindowMinimumSize = NULL;
    device->SetWindowMaximumSize = NULL;
    device->SetWindowOpacity = NULL;
    device->ShowWindow = XBOX_ShowWindow;
    device->HideWindow = XBOX_HideWindow;
    device->RaiseWindow = XBOX_RaiseWindow;
    device->MaximizeWindow = XBOX_MaximizeWindow;
    device->MinimizeWindow = XBOX_MinimizeWindow;
    device->RestoreWindow = XBOX_RestoreWindow;
    device->SetWindowBordered = NULL;
    device->SetWindowResizable = NULL;
    device->SetWindowFullscreen = NULL;                 /* handled by renderer / mode set */
    device->SetWindowGammaRamp = NULL;
    device->GetWindowGammaRamp = NULL;
    device->DestroyWindow = XBOX_DestroyWindow;
    device->GetWindowWMInfo = NULL;

    /* No software window framebuffer path on Xbox */
    device->CreateWindowFramebuffer = NULL;
    device->UpdateWindowFramebuffer = NULL;
    device->DestroyWindowFramebuffer = NULL;

    /* Optional window niceties not used on Xbox */
    device->OnWindowEnter = NULL;
    device->SetWindowHitTest = NULL;
    device->AcceptDragAndDrop = NULL;

    /* No shaped windows on Xbox */
    device->shape_driver.CreateShaper = NULL;
    device->shape_driver.SetWindowShape = NULL;
    device->shape_driver.ResizeWindowShape = NULL;

    /* No OpenGL/Vulkan on Xbox */
    device->GL_LoadLibrary = NULL;
    device->GL_GetProcAddress = NULL;
    device->GL_UnloadLibrary = NULL;
    device->GL_CreateContext = NULL;
    device->GL_MakeCurrent = NULL;
    device->GL_SetSwapInterval = NULL;
    device->GL_GetSwapInterval = NULL;
    device->GL_SwapWindow = NULL;
    device->GL_DeleteContext = NULL;

    device->Vulkan_LoadLibrary = NULL;
    device->Vulkan_UnloadLibrary = NULL;
    device->Vulkan_GetInstanceExtensions = NULL;
    device->Vulkan_CreateSurface = NULL;

    /* Text input / clipboard (not implemented on Xbox) */
    device->StartTextInput = NULL;
    device->StopTextInput = NULL;
    device->SetTextInputRect = NULL;

    device->SetClipboardText = NULL;
    device->GetClipboardText = NULL;
    device->HasClipboardText = NULL;

    /* On-screen keyboard (not currently supported on OG Xbox) */
    device->HasScreenKeyboardSupport = XBOX_HasScreenKeyboardSupport;
    device->ShowScreenKeyboard = XBOX_ShowScreenKeyboard;
    device->HideScreenKeyboard = XBOX_HideScreenKeyboard;
    device->IsScreenKeyboardShown = XBOX_IsScreenKeyboardShown;

    device->free = XBOX_DeleteDevice;
    return device;
}

VideoBootStrap XBOX_bootstrap = {
    "Xbox",
    "SDL Xbox video driver",
    XBOX_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

/* -------------------------------------------------------------------------- */
/*                               Video init/quit                              */
/* -------------------------------------------------------------------------- */

int
XBOX_VideoInit(_THIS)
{
    SDL_VideoDisplay display;
    SDL_DisplayMode  current_mode;
    SDL_Window* pWindow = NULL;

    SDL_zero(current_mode);
    SDL_zero(display);

    /* Input devices */
    XBOX_InitKeyboard(_this);
    XBOX_InitMouse(_this);

    /* If a window already exists, prefer its size. */
    pWindow = SDL_GetFocusWindow();
    if (pWindow) {
        current_mode.w = pWindow->w;
        current_mode.h = pWindow->h;
        current_mode.refresh_rate = 60;
    }
    else {
        /* Pick best available mode based on Xbox dashboard/video flags. */
        DWORD vflags = XGetVideoFlags();
        const int is_pal = (XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I);

        if (vflags & XC_VIDEO_FLAGS_HDTV_1080i) {
            current_mode.w = 1920; current_mode.h = 1080; current_mode.refresh_rate = 60;
        }
        else if (vflags & XC_VIDEO_FLAGS_HDTV_720p) {
            current_mode.w = 1280; current_mode.h = 720;  current_mode.refresh_rate = 60;
        }
        else if (vflags & XC_VIDEO_FLAGS_HDTV_480p) {
            current_mode.w = 720;  current_mode.h = 480;  current_mode.refresh_rate = 60;
        }
        else if (is_pal) {
            current_mode.w = 720;  current_mode.h = 576;  current_mode.refresh_rate =
                (vflags & XC_VIDEO_FLAGS_PAL_60Hz) ? 60 : 50;
        }
        else {
            current_mode.w = 640;  current_mode.h = 480;  current_mode.refresh_rate = 60;
        }
    }

    /* 32 bpp default; keep consistent with renderer (ARGB8888) */
    current_mode.format = SDL_PIXELFORMAT_ARGB8888;
    current_mode.driverdata = NULL;

    /* Single Xbox display */
    display.desktop_mode = current_mode;
    display.current_mode = current_mode;
    display.driverdata = NULL;

    SDL_AddVideoDisplay(&display, SDL_FALSE);
    g_XboxDesktopMode = current_mode;

    SDL_Log("Xbox desktop: %dx%d@%d (%s)",
        current_mode.w, current_mode.h, current_mode.refresh_rate,
        (current_mode.h >= 1080) ? "1080i" :
        (current_mode.h >= 720) ? "720p" :
        (current_mode.h == 576) ? "576i" : "480");

    return 0;
}

void
XBOX_VideoQuit(_THIS)
{
    XBOX_QuitKeyboard(_this);
    XBOX_QuitMouse(_this);
}

/* -------------------------------------------------------------------------- */
/*                       Xbox D3D8 interface acquisition                      */
/* -------------------------------------------------------------------------- */

SDL_bool
D3D_LoadDLL(IDirect3D8** pDirect3D8Interface)
{
    if (!pDirect3D8Interface) {
        return SDL_FALSE;
    }
    /* On OG Xbox, Direct3D8 is provided by the XDK. No DLL to load. */
    *pDirect3D8Interface = Direct3DCreate8(D3D_SDK_VERSION);
    return (*pDirect3D8Interface != NULL) ? SDL_TRUE : SDL_FALSE;
}

/* -------------------------------------------------------------------------- */
/*                 DXGI helper (not available on OG Xbox)                     */
/* -------------------------------------------------------------------------- */

SDL_bool
SDL_DXGIGetOutputInfo(int displayIndex, int* adapterIndex, int* outputIndex)
{
    (void)displayIndex;
    if (!adapterIndex) { SDL_InvalidParamError("adapterIndex"); return SDL_FALSE; }
    if (!outputIndex) { SDL_InvalidParamError("outputIndex");  return SDL_FALSE; }
    *adapterIndex = -1;
    *outputIndex = -1;
    SDL_SetError("DXGI is not available on this platform");
    return SDL_FALSE;
}

/* -------------------------------------------------------------------------- */
/*                  On-screen keyboard (not implemented)                      */
/* -------------------------------------------------------------------------- */

SDL_bool XBOX_HasScreenKeyboardSupport(_THIS) { (void)_this; return SDL_FALSE; }
void     XBOX_ShowScreenKeyboard(_THIS, SDL_Window* window) { (void)_this; (void)window; }
void     XBOX_HideScreenKeyboard(_THIS, SDL_Window* window) { (void)_this; (void)window; }
SDL_bool XBOX_IsScreenKeyboardShown(_THIS, SDL_Window* window) { (void)_this; (void)window; return SDL_FALSE; }

#endif /* SDL_VIDEO_DRIVER_XBOX */

/* vim: set ts=4 sw=4 expandtab: */
