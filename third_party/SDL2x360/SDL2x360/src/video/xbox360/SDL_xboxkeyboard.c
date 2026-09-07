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

#if 1//SDL_VIDEO_DRIVER_XBOX

#include "SDL_xboxkeyboard.h"
#include "SDL_keyboard.h"
#include "SDL_events.h"
#include "..\..\events\SDL_keyboard_c.h"

#define DEBUG_KEYBOARD

#include <xtl.h>
#include <xkbd.h>

// Missing from Xbox keyboard header
#define VK_bracketleft  0xdb
#define VK_bracketright 0xdd
#define VK_comma	    0xbc
#define VK_period       0xbe
#define VK_slash        0xbf
#define VK_semicolon    0xba
#define VK_grave        0xc0
#define VK_minus        0xbd
#define VK_equal        0xbb
#define VK_quote        0xde
#define VK_backslash	0xdc

BOOL g_bDevicesInitialized = FALSE;
static SDL_Keycode xbox_keymap[256];
static HANDLE g_hKeyboardDevice[4] = { 0 };
static XINPUT_DEBUG_KEYSTROKE g_keyboardStroke;

CHAR XBInput_GetKeyboardInput()
{
	DWORD i;
	DWORD dwInsertions, dwRemovals;
	XGetDeviceChanges(XDEVICE_TYPE_DEBUG_KEYBOARD, &dwInsertions, &dwRemovals);

	// Get status about gamepad insertions and removals. Note that, in order to
	// not miss devices, we will check for removed device BEFORE checking for
	// insertions

	// Loop through all ports
	for (i = 0; i < XGetPortCount(); i++)
	{
		// Handle removed devices.
		if (dwRemovals & (1 << i))
		{
			XInputClose(g_hKeyboardDevice[i]);
			g_hKeyboardDevice[i] = NULL;
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

			g_hKeyboardDevice[i] = XInputOpen(XDEVICE_TYPE_DEBUG_KEYBOARD, i,
				XDEVICE_NO_SLOT, &pollValues);
		}

		// If we have a valid device, poll it's state and track button changes
		if (g_hKeyboardDevice[i])
		{
			if (ERROR_SUCCESS == XInputDebugGetKeystroke(&g_keyboardStroke))
				return g_keyboardStroke.Ascii;
		}
	}

	return '\0';
}

void XBOX_UpdateKeyboard(void)
{
	XBInput_GetKeyboardInput();

	if (g_keyboardStroke.VirtualKey == 0)
		return;

	if (g_keyboardStroke.Flags & XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP) {
		SDL_SendKeyboardKey(SDL_RELEASED, xbox_keymap[g_keyboardStroke.VirtualKey]);
	}
	else {
		SDL_SendKeyboardKey(SDL_PRESSED, xbox_keymap[g_keyboardStroke.VirtualKey]);
	}
}

void XBOX_QuitKeyboard(_THIS)
{
}

void XBOX_InitKeyboard(_THIS)
{
	DWORD i;
	DWORD dwDeviceMask;

	XINPUT_DEBUG_KEYQUEUE_PARAMETERS keyboardSettings;

	/* Map the DIK scancodes to SDL keysyms */
	for (i = 0; i < SDL_TABLESIZE(xbox_keymap); ++i)
		xbox_keymap[i] = 0;

	/* Defined DIK_* constants */
	xbox_keymap[VK_ESCAPE] = SDL_GetScancodeFromKey(SDLK_ESCAPE);
	xbox_keymap['1'] = SDL_GetScancodeFromKey(SDLK_1);
	xbox_keymap['2'] = SDL_GetScancodeFromKey(SDLK_2);
	xbox_keymap['3'] = SDL_GetScancodeFromKey(SDLK_3);
	xbox_keymap['4'] = SDL_GetScancodeFromKey(SDLK_4);
	xbox_keymap['5'] = SDL_GetScancodeFromKey(SDLK_5);
	xbox_keymap['6'] = SDL_GetScancodeFromKey(SDLK_6);
	xbox_keymap['7'] = SDL_GetScancodeFromKey(SDLK_7);
	xbox_keymap['8'] = SDL_GetScancodeFromKey(SDLK_8);
	xbox_keymap['9'] = SDL_GetScancodeFromKey(SDLK_9);
	xbox_keymap['0'] = SDL_GetScancodeFromKey(SDLK_0);
	xbox_keymap[VK_SUBTRACT] = SDL_GetScancodeFromKey(SDLK_MINUS);
	xbox_keymap[VK_equal] = SDL_GetScancodeFromKey(SDLK_EQUALS);
	xbox_keymap[VK_BACK] = SDL_GetScancodeFromKey(SDLK_BACKSPACE);
	xbox_keymap[VK_TAB] = SDL_GetScancodeFromKey(SDLK_TAB);
	xbox_keymap['Q'] = SDL_GetScancodeFromKey(SDLK_q);
	xbox_keymap['W'] = SDL_GetScancodeFromKey(SDLK_w);
	xbox_keymap['E'] = SDL_GetScancodeFromKey(SDLK_e);
	xbox_keymap['R'] = SDL_GetScancodeFromKey(SDLK_r);
	xbox_keymap['T'] = SDL_GetScancodeFromKey(SDLK_t);
	xbox_keymap['Y'] = SDL_GetScancodeFromKey(SDLK_y);
	xbox_keymap['U'] = SDL_GetScancodeFromKey(SDLK_u);
	xbox_keymap['I'] = SDL_GetScancodeFromKey(SDLK_i);
	xbox_keymap['O'] = SDL_GetScancodeFromKey(SDLK_o);
	xbox_keymap['P'] = SDL_GetScancodeFromKey(SDLK_p);
	xbox_keymap[VK_bracketleft] = SDL_GetScancodeFromKey(SDLK_LEFTBRACKET);
	xbox_keymap[VK_bracketright] = SDL_GetScancodeFromKey(SDLK_RIGHTBRACKET);
	xbox_keymap[VK_RETURN] = SDL_GetScancodeFromKey(SDLK_RETURN);
	xbox_keymap[VK_LCONTROL] = SDL_GetScancodeFromKey(SDLK_LCTRL);
	xbox_keymap['A'] = SDL_GetScancodeFromKey(SDLK_a);
	xbox_keymap['S'] = SDL_GetScancodeFromKey(SDLK_s);
	xbox_keymap['D'] = SDL_GetScancodeFromKey(SDLK_d);
	xbox_keymap['F'] = SDL_GetScancodeFromKey(SDLK_f);
	xbox_keymap['G'] = SDL_GetScancodeFromKey(SDLK_g);
	xbox_keymap['H'] = SDL_GetScancodeFromKey(SDLK_h);
	xbox_keymap['J'] = SDL_GetScancodeFromKey(SDLK_j);
	xbox_keymap['K'] = SDL_GetScancodeFromKey(SDLK_k);
	xbox_keymap['L'] = SDL_GetScancodeFromKey(SDLK_l);
	xbox_keymap[VK_OEM_1] = SDL_GetScancodeFromKey(SDLK_SEMICOLON);
	xbox_keymap[VK_OEM_7] = SDL_GetScancodeFromKey(SDLK_QUOTE);
	xbox_keymap[VK_OEM_3] = SDL_GetScancodeFromKey(SDLK_BACKQUOTE);
	xbox_keymap[VK_LSHIFT] = SDL_GetScancodeFromKey(SDLK_LSHIFT);
	xbox_keymap[VK_backslash] = SDL_GetScancodeFromKey(SDLK_BACKSLASH);
	xbox_keymap['Z'] = SDL_GetScancodeFromKey(SDLK_z);
	xbox_keymap['X'] = SDL_GetScancodeFromKey(SDLK_x);
	xbox_keymap['C'] = SDL_GetScancodeFromKey(SDLK_c);
	xbox_keymap['V'] = SDL_GetScancodeFromKey(SDLK_v);
	xbox_keymap['B'] = SDL_GetScancodeFromKey(SDLK_b);
	xbox_keymap['N'] = SDL_GetScancodeFromKey(SDLK_n);
	xbox_keymap['M'] = SDL_GetScancodeFromKey(SDLK_m);
	xbox_keymap[VK_OEM_COMMA] = SDL_GetScancodeFromKey(SDLK_COMMA);
	xbox_keymap[VK_OEM_PERIOD] = SDL_GetScancodeFromKey(SDLK_PERIOD);
	xbox_keymap[VK_OEM_PLUS] = SDL_GetScancodeFromKey(SDLK_PLUS);
	xbox_keymap[VK_OEM_MINUS] = SDL_GetScancodeFromKey(SDLK_MINUS);
	xbox_keymap[VK_slash] = SDL_GetScancodeFromKey(SDLK_SLASH);
	xbox_keymap[VK_RSHIFT] = SDL_GetScancodeFromKey(SDLK_RSHIFT);
	xbox_keymap[VK_MULTIPLY] = SDL_GetScancodeFromKey(SDLK_KP_MULTIPLY);
	xbox_keymap[VK_LMENU] = SDL_GetScancodeFromKey(SDLK_LALT);
	xbox_keymap[VK_SPACE] = SDL_GetScancodeFromKey(SDLK_SPACE);
	xbox_keymap[VK_CAPITAL] = SDL_GetScancodeFromKey(SDLK_CAPSLOCK);
	xbox_keymap[VK_F1] = SDL_GetScancodeFromKey(SDLK_F1);
	xbox_keymap[VK_F2] = SDL_GetScancodeFromKey(SDLK_F2);
	xbox_keymap[VK_F3] = SDL_GetScancodeFromKey(SDLK_F3);
	xbox_keymap[VK_F4] = SDL_GetScancodeFromKey(SDLK_F4);
	xbox_keymap[VK_F5] = SDL_GetScancodeFromKey(SDLK_F5);
	xbox_keymap[VK_F6] = SDL_GetScancodeFromKey(SDLK_F6);
	xbox_keymap[VK_F7] = SDL_GetScancodeFromKey(SDLK_F7);
	xbox_keymap[VK_F8] = SDL_GetScancodeFromKey(SDLK_F8);
	xbox_keymap[VK_F9] = SDL_GetScancodeFromKey(SDLK_F9);
	xbox_keymap[VK_F10] = SDL_GetScancodeFromKey(SDLK_F10);
	xbox_keymap[VK_NUMLOCK] = SDL_GetScancodeFromKey(SDLK_NUMLOCKCLEAR);
	xbox_keymap[VK_SCROLL] = SDL_GetScancodeFromKey(SDLK_SCROLLLOCK);
	xbox_keymap[VK_NUMPAD7] = SDL_GetScancodeFromKey(SDLK_KP_7);
	xbox_keymap[VK_NUMPAD8] = SDL_GetScancodeFromKey(SDLK_KP_8);
	xbox_keymap[VK_NUMPAD9] = SDL_GetScancodeFromKey(SDLK_KP_9);
	xbox_keymap[VK_ADD] = SDL_GetScancodeFromKey(SDLK_KP_PLUS);
	xbox_keymap[VK_SUBTRACT] = SDL_GetScancodeFromKey(SDLK_KP_MINUS);
	xbox_keymap[VK_NUMPAD4] = SDL_GetScancodeFromKey(SDLK_KP_4);
	xbox_keymap[VK_NUMPAD5] = SDL_GetScancodeFromKey(SDLK_KP_5);
	xbox_keymap[VK_NUMPAD6] = SDL_GetScancodeFromKey(SDLK_KP_6);
	xbox_keymap[VK_NUMPAD1] = SDL_GetScancodeFromKey(SDLK_KP_1);
	xbox_keymap[VK_NUMPAD2] = SDL_GetScancodeFromKey(SDLK_KP_2);
	xbox_keymap[VK_NUMPAD3] = SDL_GetScancodeFromKey(SDLK_KP_3);
	xbox_keymap[VK_NUMPAD0] = SDL_GetScancodeFromKey(SDLK_KP_0);
	xbox_keymap[VK_DECIMAL] = SDL_GetScancodeFromKey(SDLK_KP_PERIOD);
	xbox_keymap[VK_F11] = SDL_GetScancodeFromKey(SDLK_F11);
	xbox_keymap[VK_F12] = SDL_GetScancodeFromKey(SDLK_F12);

	xbox_keymap[VK_F13] = SDL_GetScancodeFromKey(SDLK_F13);
	xbox_keymap[VK_F14] = SDL_GetScancodeFromKey(SDLK_F14);
	xbox_keymap[VK_F15] = SDL_GetScancodeFromKey(SDLK_F15);

	xbox_keymap[VK_equal] = SDL_GetScancodeFromKey(SDLK_F15);
	xbox_keymap[VK_RCONTROL] = SDL_GetScancodeFromKey(SDLK_RCTRL);
	xbox_keymap[VK_DIVIDE] = SDL_GetScancodeFromKey(SDLK_KP_DIVIDE);
	xbox_keymap[VK_RMENU] = SDL_GetScancodeFromKey(SDLK_RALT);
	xbox_keymap[VK_PAUSE] = SDL_GetScancodeFromKey(SDLK_PAUSE);
	xbox_keymap[VK_HOME] = SDL_GetScancodeFromKey(SDLK_HOME);
	xbox_keymap[VK_UP] = SDL_GetScancodeFromKey(SDLK_UP);
	xbox_keymap[VK_PRIOR] = SDL_GetScancodeFromKey(SDLK_PAGEUP);
	xbox_keymap[VK_LEFT] = SDL_GetScancodeFromKey(SDLK_LEFT);
	xbox_keymap[VK_RIGHT] = SDL_GetScancodeFromKey(SDLK_RIGHT);
	xbox_keymap[VK_END] = SDL_GetScancodeFromKey(SDLK_END);
	xbox_keymap[VK_DOWN] = SDL_GetScancodeFromKey(SDLK_DOWN);
	xbox_keymap[VK_NEXT] = SDL_GetScancodeFromKey(SDLK_PAGEDOWN);
	xbox_keymap[VK_INSERT] = SDL_GetScancodeFromKey(SDLK_INSERT);
	xbox_keymap[VK_DELETE] = SDL_GetScancodeFromKey(SDLK_DELETE);
	xbox_keymap[VK_LWIN] = SDL_GetScancodeFromKey(SDLK_LGUI);
	xbox_keymap[VK_RWIN] = SDL_GetScancodeFromKey(SDLK_RGUI);
	xbox_keymap[VK_APPS] = SDL_GetScancodeFromKey(SDLK_MENU);

	if (!g_bDevicesInitialized)
		XInitDevices(0, NULL);

	Sleep(1000);

	keyboardSettings.dwFlags = XINPUT_DEBUG_KEYQUEUE_FLAG_KEYDOWN | XINPUT_DEBUG_KEYQUEUE_FLAG_KEYREPEAT | XINPUT_DEBUG_KEYQUEUE_FLAG_KEYUP;
	keyboardSettings.dwQueueSize = 25;
	keyboardSettings.dwRepeatDelay = 500;
	keyboardSettings.dwRepeatInterval = 50;

	if (ERROR_SUCCESS != XInputDebugInitKeyboardQueue(&keyboardSettings))
		return;

	g_bDevicesInitialized = TRUE;

	// Now find the keyboard device, in this case we shall loop indefinitely, although
	// it would be better to monitor the time taken and to time out if necessary
	// in case the keyboard has been unplugged

	dwDeviceMask = XGetDevices(XDEVICE_TYPE_DEBUG_KEYBOARD);

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

			g_hKeyboardDevice[i] = XInputOpen(XDEVICE_TYPE_DEBUG_KEYBOARD, i,
				XDEVICE_NO_SLOT, &pollValues);
		}
	}
}

#endif //SDL_VIDEO_DRIVER_XBOX