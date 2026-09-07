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
#define DEBUG_KEYBOARD
#define DEBUG_MOUSE

#include <xtl.h>
#include <xkbd.h>

#include "../../SDL_internal.h"

#if 1//SDL_VIDEO_DRIVER_XBOX

#include "SDL_assert.h"
#include "SDL_xboxvideo.h"

#include "../../events/SDL_mouse_c.h"

extern g_bDevicesInitialized;
static HANDLE g_hMouseDevice[4] = { 0 };
static XINPUT_MOUSE g_MouseInput;
XINPUT_STATE g_MouseStates[4];

VOID Mouse_RefreshDeviceList()
{
	// Get status about gamepad insertions and removals. Note that, in order to
	// not miss devices, we will check for removed device BEFORE checking for
	// insertions

	DWORD i;
	DWORD dwInsertions, dwRemovals;
	XGetDeviceChanges(XDEVICE_TYPE_DEBUG_MOUSE, &dwInsertions, &dwRemovals);

	// Loop through all ports
	for (i = 0; i < XGetPortCount(); i++)
	{
		// Handle removed devices.
		if (dwRemovals & (1 << i))
		{
			XInputClose(g_hMouseDevice[i]);
			g_hMouseDevice[i] = NULL;
		}

		// Handle inserted devices
		if (dwInsertions & (1 << i))
		{
			// Now open the device
			XINPUT_POLLING_PARAMETERS pollValues;
			pollValues.fAutoPoll = TRUE;
			pollValues.fInterruptOut = TRUE;
			pollValues.bInputInterval = 32;
			pollValues.bOutputInterval = 32;
			pollValues.ReservedMBZ1 = 0;
			pollValues.ReservedMBZ2 = 0;

			g_hMouseDevice[i] = XInputOpen(XDEVICE_TYPE_DEBUG_MOUSE, i,
				XDEVICE_NO_SLOT, &pollValues);
		}
	}
}

static Uint32
XBOX_UpdateMouseState()
{
	Uint32 retval = 0;
	int i, j;
	static int prev_buttons;
	static int lastmouseX, lastmouseY;
	static DWORD lastdwPacketNum;
	DWORD dwPacketNum;
	int mouseX, mouseY;
	int buttons, changed;
	int wheel;

	const	static char sdl_mousebtn[] = {
	XINPUT_DEBUG_MOUSE_LEFT_BUTTON,
	XINPUT_DEBUG_MOUSE_MIDDLE_BUTTON,
	XINPUT_DEBUG_MOUSE_RIGHT_BUTTON
	};

	Mouse_RefreshDeviceList();

	SDL_GetMouse()->focus = SDL_GetFocusWindow();

	// Loop through all gamepads
	for (i = 0; i < XGetPortCount(); i++)
	{
		// If we have a valid device, poll it's state and track button changes
		if (g_hMouseDevice[i])
		{
			// Read the input state
			XInputGetState(g_hMouseDevice[i], &g_MouseStates[i]);

			mouseX = mouseY = 0;

			dwPacketNum = g_MouseStates[i].dwPacketNumber;

			// Copy gamepad to local structure
			memcpy(&g_MouseInput, &g_MouseStates[i].DebugMouse, sizeof(XINPUT_MOUSE));

			if ((lastmouseX != g_MouseInput.cMickeysX) ||
				(lastmouseY != g_MouseInput.cMickeysY))
			{
				mouseX = g_MouseInput.cMickeysX;
				mouseY = g_MouseInput.cMickeysY;
			}

			if (mouseX || mouseY)
				SDL_SendMouseMotion(SDL_GetMouse()->focus, 0, 1, mouseX, mouseY);

			buttons = g_MouseInput.bButtons;

			changed = buttons ^ prev_buttons;

			for (i = 0;i < sizeof(sdl_mousebtn);i++)
			{
				if (changed & sdl_mousebtn[i])
					SDL_SendMouseButton(SDL_GetMouse()->focus, 0, (buttons & sdl_mousebtn[i]) ? SDL_PRESSED : SDL_RELEASED, i + 1);
			}

			wheel = g_MouseInput.cWheel;

			if (wheel && dwPacketNum != lastdwPacketNum)
			{
				for (j = 0; j < ((wheel > 0) ? wheel : -wheel); j++) // TODO: mouse wheel stuff
				{
					/*					int button = (wheel > 0)?SDL_BUTTON_WHEELUP:SDL_BUTTON_WHEELDOWN; // TODO: What's the SDL2 equivalent??

										SDL_PrivateMouseButton(SDL_PRESSED, button, 0, 0);
										SDL_PrivateMouseButton(SDL_RELEASED, button, 0, 0);
					*/
				}
			}

			prev_buttons = buttons;
			lastmouseX = g_MouseInput.cMickeysX;
			lastmouseY = g_MouseInput.cMickeysY;
			lastdwPacketNum = dwPacketNum;
		}
	}

	return retval;
}

static Uint32
XBOX_GetGlobalMouseState(int* x, int* y)
{
	XBOX_UpdateMouseState();
	*x = g_MouseInput.cMickeysX;
	*y = g_MouseInput.cMickeysY;

	return 0; //TODO
}

static SDL_Cursor*
XBOX_CreateDefaultCursor()
{
	// TODO
	return NULL;
}

static SDL_Cursor*
XBOX_CreateCursor(SDL_Surface* surface, int hot_x, int hot_y)
{
	// TODO
	return NULL;
}

static SDL_Cursor*
XBOX_CreateSystemCursor(SDL_SystemCursor id)
{
	// TODO
	return NULL;
}

static void
XBOX_FreeCursor(SDL_Cursor* cursor)
{
	// TODO
}

static int
XBOX_ShowCursor(SDL_Cursor* cursor)
{
	// TODO
	return 0;
}

static void
XBOX_WarpMouse(SDL_Window* window, int x, int y)
{
	// TODO
}

static int
XBOX_WarpMouseGlobal(int x, int y)
{
	// TODO
	return 0;
}

static int
XBOX_SetRelativeMouseMode(SDL_bool enabled)
{
	// TODO
	return 1;
}

static int
XBOX_CaptureMouse(SDL_Window* window)
{
	// TODO
	return 0;
}

void
XBOX_QuitMouse(_THIS)
{
}

void
XBOX_InitMouse(_THIS)
{
	DWORD dwDeviceMask;
	DWORD i;
	SDL_Mouse* pMouse = NULL;

	if (!g_bDevicesInitialized)
		XInitDevices(0, NULL);

	dwDeviceMask = XGetDevices(XDEVICE_TYPE_DEBUG_MOUSE);

	// Open the devices
	for (i = 0; i < XGetPortCount(); i++)
	{
		if (dwDeviceMask & (1 << i))
		{
			// Now open the device
			XINPUT_POLLING_PARAMETERS pollValues;
			pollValues.fAutoPoll = TRUE;
			pollValues.fInterruptOut = TRUE;
			pollValues.bInputInterval = 32;
			pollValues.bOutputInterval = 32;
			pollValues.ReservedMBZ1 = 0;
			pollValues.ReservedMBZ2 = 0;

			g_hMouseDevice[i] = XInputOpen(XDEVICE_TYPE_DEBUG_MOUSE, i,
				XDEVICE_NO_SLOT, &pollValues);
		}
	}

	pMouse = SDL_GetMouse();

	if (pMouse)
	{
#if 1 // TODO: Where are these ment to be set?
		pMouse->relative_mode = SDL_TRUE;
		pMouse->relative_speed_scale = pMouse->scale_accum_x = 1.0f;
		pMouse->relative_speed_scale = pMouse->scale_accum_y = 1.0f;
		pMouse->normal_speed_scale = pMouse->scale_accum_x = 1.0f;
		pMouse->normal_speed_scale = pMouse->scale_accum_y = 1.0f;
#endif
		pMouse->CreateCursor = XBOX_CreateCursor;
		pMouse->CreateSystemCursor = XBOX_CreateSystemCursor;
		pMouse->ShowCursor = XBOX_ShowCursor;
		pMouse->FreeCursor = XBOX_FreeCursor;
		pMouse->WarpMouse = XBOX_WarpMouse;
		pMouse->WarpMouseGlobal = XBOX_WarpMouseGlobal;
		pMouse->SetRelativeMouseMode = XBOX_SetRelativeMouseMode;
		pMouse->CaptureMouse = XBOX_CaptureMouse;
		pMouse->GetGlobalMouseState = XBOX_GetGlobalMouseState;
		//pMouse->UpdateMouseState = XBOX_UpdateMouseState;
	}

	SDL_SetDefaultCursor(XBOX_CreateDefaultCursor());
}

#endif /* SDL_VIDEO_DRIVER_XBOX */

/* vi: set ts=4 sw=4 expandtab: */