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

#if !defined(SDL_JOYSTICK_DISABLED) && defined(__XBOX__) /* Use your own build flag here */

#include "SDL_joystick.h"
#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "SDL_events.h"

/* Include the Xbox specific headers */
#include <xtl.h>
#include <math.h>
#include <stdio.h>		/* For the definition of NULL */

#ifndef XUSER_MAX_COUNT
#define XUSER_MAX_COUNT 4
#endif
#define IS_BUTTON_PRESSED(buttons, index) ((buttons) & (1 << (index)))

extern BOOL g_bDevicesInitialized;
// Global instance of XInput polling parameters
XINPUT_POLLING_PARAMETERS g_PollingParameters =
{
	TRUE,
	TRUE,
	0,
	8,
	8,
	0,
};

typedef struct {
	HANDLE device_handle;      /* Handle from XInputOpen */
	DWORD port;                /* Controller port number: 0-3 */
	BOOL connected;            /* Is the device currently connected? */
	XINPUT_CAPABILITIES caps;  /* Capabilities for this device */
	XINPUT_FEEDBACK feedback;  /* Persistent feedback structure */
	/* Rumble Tracking */
	Uint32 rumble_end_time;    /* Time when rumble should stop */
	BOOL rumble_active;        /* Is rumble currently active? */
	Uint16 usb_vendor_id;
	Uint16 usb_product_id;
} XboxControllerDevice;

const int XBOX_JOYSTICK_A = 0;
const int XBOX_JOYSTICK_B = 1;
const int XBOX_JOYSTICK_X = 2;
const int XBOX_JOYSTICK_Y = 3;
const int XBOX_JOYSTICK_BLACK = 4;
const int XBOX_JOYSTICK_WHITE = 5;
const int XBOX_JOYSTICK_START = 6;
const int XBOX_JOYSTICK_BACK = 7;
const int XBOX_JOYSTICK_LEFT_THUMB = 8;
const int XBOX_JOYSTICK_RIGHT_THUMB = 9;

const int XBOX_JOYSTICK_STICKTHUMB_LEFT_X = 0;
const int XBOX_JOYSTICK_STICKTHUMB_LEFT_Y = 1;
const int XBOX_JOYSTICK_STICKTHUMB_RIGHT_X = 2;
const int XBOX_JOYSTICK_STICKTHUMB_RIGHT_Y = 3;
const int XBOX_JOYSTICK_LEFT_TRIGGER = 4;
const int XBOX_JOYSTICK_RIGHT_TRIGGER = 5;

static XboxControllerDevice g_Controllers[XUSER_MAX_COUNT];
static int g_NumControllers = 0;

static int XBOX_JoystickInit(void) {
	SDL_Log("Initializing XBOX Joystick driver\n");

	// Initialize devices once
	if (!g_bDevicesInitialized) {
		XDEVICE_PREALLOC_TYPE deviceTypes[] = { {XDEVICE_TYPE_GAMEPAD, 4} };
		XInitDevices(sizeof(deviceTypes) / sizeof(XDEVICE_PREALLOC_TYPE), deviceTypes);
		g_bDevicesInitialized = TRUE;
		SDL_Log("XInitDevices completed\n");
	}

	// Check connected devices
	DWORD dwDevices = XGetDevices(XDEVICE_TYPE_GAMEPAD);
	SDL_Log("Device mask: %08X\n", dwDevices);

	for (DWORD port = 0; port < XUSER_MAX_COUNT; port++) {
		if (!(dwDevices & (1 << port))) {
			SDL_Log("No controller detected at port %d\n", port);
			g_Controllers[port].connected = FALSE;
			continue;
		}

		SDL_Log("Attempting to open controller at port %d\n", port);
		XINPUT_POLLING_PARAMETERS pollParams = g_PollingParameters;
		HANDLE h = XInputOpen(XDEVICE_TYPE_GAMEPAD, port, XDEVICE_NO_SLOT, &pollParams);

		if (!h) {
			SDL_Log("XInputOpen failed for port %d\n", port);
			g_Controllers[port].connected = FALSE;
			continue;
		}

		XINPUT_DEVICE_DESCRIPTION pDescription;
		DWORD usb_desc = XInputGetDeviceDescription(h, &pDescription);
		if (!usb_desc) {
			g_Controllers[port].usb_product_id = 0xBEEF;
			g_Controllers[port].usb_vendor_id = 0xDEAD;
		}
		else {
			g_Controllers[port].usb_product_id = pDescription.wProductID;
			g_Controllers[port].usb_vendor_id = pDescription.wVendorID;
		}
		SDL_Log("Controller connected in port %d, VID 0x%.4x PID 0x%.4x\n", port, g_Controllers[port].usb_vendor_id, g_Controllers[port].usb_product_id);

		g_Controllers[port].device_handle = h;
		g_Controllers[port].connected = TRUE;
		g_Controllers[port].port = port;

		// Retrieve capabilities
		if (XInputGetCapabilities(h, &g_Controllers[port].caps) != ERROR_SUCCESS) {
			SDL_Log("Failed to get capabilities for port %d\n", port);
			XInputClose(h);
			g_Controllers[port].device_handle = NULL;
			g_Controllers[port].connected = FALSE;
			continue;
		}

		g_Controllers[port].rumble_active = FALSE;
		g_Controllers[port].rumble_end_time = 0;
	}

	g_NumControllers = 0;
	for (DWORD port = 0; port < XUSER_MAX_COUNT; port++) {
		if (g_Controllers[port].connected) {
			SDL_Log("Controller at port %d is connected\n", port);
			g_NumControllers++;
		}
	}
	SDL_Log("Number of connected controllers: %d\n", g_NumControllers);

	return 0;
}

static void XBOX_JoystickDetect(void) {
	DWORD dwDevices = XGetDevices(XDEVICE_TYPE_GAMEPAD);
	//SDL_Log("Device mask: %08X\n", dwDevices);

	for (DWORD port = 0; port < XUSER_MAX_COUNT; port++) {
		if (!(dwDevices & (1 << port))) {
			if (g_Controllers[port].connected) {
				SDL_Log("Controller disconnected at port %d\n", port);
				XInputClose(g_Controllers[port].device_handle);
				g_Controllers[port].device_handle = NULL;
				g_Controllers[port].connected = FALSE;
				g_NumControllers--;
				SDL_PrivateJoystickRemoved(port);
			}
			continue;
		}

		if (!g_Controllers[port].connected) {
			SDL_Log("Attempting to open controller at port %d\n", port);
			XINPUT_POLLING_PARAMETERS pollParams = g_PollingParameters;
			HANDLE h = XInputOpen(XDEVICE_TYPE_GAMEPAD, port, XDEVICE_NO_SLOT, &pollParams);

			if (!h) {
				SDL_Log("XInputOpen failed for port %d\n", port);
				continue;
			}

			XINPUT_DEVICE_DESCRIPTION pDescription;
			DWORD usb_desc = XInputGetDeviceDescription(h, &pDescription); // Seems this doesnt work
			if (!usb_desc) {
				g_Controllers[port].usb_product_id = 0xBEEF;
				g_Controllers[port].usb_vendor_id = 0xDEAD;
			}
			else {
				g_Controllers[port].usb_product_id = pDescription.wProductID;
				g_Controllers[port].usb_vendor_id = pDescription.wVendorID;
			}
			SDL_Log("Controller connected in port %d, VID 0x%.4x PID 0x%.4x\n", port, g_Controllers[port].usb_vendor_id, g_Controllers[port].usb_product_id);

			g_Controllers[port].device_handle = h;
			g_Controllers[port].connected = TRUE;
			g_Controllers[port].port = port;

			if (XInputGetCapabilities(h, &g_Controllers[port].caps) != ERROR_SUCCESS) {
				SDL_Log("Failed to get capabilities for port %d\n", port);
				XInputClose(h);
				g_Controllers[port].device_handle = NULL;
				g_Controllers[port].connected = FALSE;
				continue;
			}

			g_Controllers[port].rumble_active = FALSE;
			g_Controllers[port].rumble_end_time = 0;
			g_NumControllers++;
			SDL_PrivateJoystickAdded(port);
		}
	}

	//SDL_Log("Number of connected controllers: %d\n", g_NumControllers);
}

static int
XBOX_JoystickGetCount(void)
{
	// SDL_Log("XBOX_JoystickGetCount\n");
	return g_NumControllers;
}

static const char*
XBOX_JoystickGetDeviceName(int device_index) {
	SDL_Log("XBOX_JoystickGetDeviceName called for device index %d\n", device_index);
	int count = 0;
	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_Controllers[i].connected) {
			if (count == device_index) {
				return "Xbox Controller";
			}
			count++;
		}
	}
	return NULL;
}

static int
XBOX_JoystickGetDevicePlayerIndex(int device_index)
{
	SDL_Log("XBOX_JoystickGetDevicePlayerIndex called for device index %d\n", device_index);
	// Player index matches the order or port number.
	// Here we just return the port as player index.
	int count = 0;
	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_Controllers[i].connected) {
			if (count == device_index) {
				return g_Controllers[i].port;
			}
			count++;
		}
	}
	return -1;
}

static SDL_JoystickGUID
XBOX_JoystickGetDeviceGUID(int device_index)
{
	SDL_JoystickGUID guid;
	SDL_zero(guid);
	int count = 0;
	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_Controllers[i].connected) {
			// Search for controller
			if (count == device_index) {
				guid.data[0] = 'O';
				guid.data[1] = 'G';
				guid.data[2] = ' ';
				guid.data[3] = 'X';
				guid.data[4] = 'b';
				guid.data[5] = 'o';
				guid.data[6] = 'x';
				guid.data[7] = ' ';
				guid.data[8] = 'P';
				guid.data[9] = 'a';
				guid.data[10] = 'd';
				guid.data[11] = ' ';
				guid.data[12] = 'N';
				guid.data[13] = 'o';
				guid.data[14] = ' ';
				guid.data[15] = (Uint8)g_Controllers[i].port;
				break;
			}
			count++;
		}
	}

	return guid;
}

static SDL_JoystickID
XBOX_JoystickGetDeviceInstanceID(int device_index)
{
	SDL_Log("XBOX_JoystickGetDeviceInstanceID called for device index %d\n", device_index);
	// Instance ID can be the device_index itself or something stable.
	return (SDL_JoystickID) device_index;
}

static int
XBOX_JoystickOpen(SDL_Joystick* joystick, int device_index)
{
	SDL_Log("XBOX_JoystickOpen called for device index %d\n", device_index);
	int count = 0;
	int foundPort = -1;

	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_Controllers[i].connected) {
			if (count == device_index) {
				foundPort = i;
				break;
			}
			count++;
		}
	}

	if (foundPort == -1) {
		SDL_Log("Invalid device index: %d\n", device_index);
		return SDL_SetError("Invalid device index");
	}

	joystick->instance_id = XBOX_JoystickGetDeviceInstanceID(device_index);

	// For standard Xbox controller:
	// Typically: 2 analog sticks = 4 axes, triggers can be axes if desired.
	// We'll expose leftX, leftY, rightX, rightY axes = 4 axes
	joystick->naxes = 6;

	// For buttons: A,B,X,Y,Black,White,Start,Back,DPad(U,D,L,R), L/R thumb press
	// The original Xbox controller had A,B,X,Y, Black, White, Start, Back, LThumbPress, RThumbPress and a D-Pad.
	// We'll treat D-Pad as a hat, so exclude that from the button count.
	// That's 8 buttons (A,B,X,Y,Black,White,Start,Back) + 2 thumb presses = 10 buttons.
	joystick->nbuttons = 10;
	joystick->nhats = 1; // for D-Pad

	joystick->hwdata = &g_Controllers[foundPort];

	SDL_Log("Joystick opened successfully for port %d\n", foundPort);

	return 0;
}

static int
XBOX_JoystickRumble(SDL_Joystick* joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
	SDL_Log("XBOX_JoystickRumble called with low: %u, high: %u, duration: %u ms\n", low_frequency_rumble, high_frequency_rumble, duration_ms);

	XboxControllerDevice* dev = (XboxControllerDevice*)joystick->hwdata;
	if (!dev || !dev->connected || !dev->device_handle) {
		SDL_Log("Rumble failed: device not connected or handle invalid.\n");
		return SDL_Unsupported();
	}

	// Clear feedback
	SDL_zero(dev->feedback);

	// Set rumble motor speeds
	dev->feedback.Rumble.wLeftMotorSpeed = (low_frequency_rumble * 65535) / SDL_MAX_UINT16;
	dev->feedback.Rumble.wRightMotorSpeed = (high_frequency_rumble * 65535) / SDL_MAX_UINT16;

	// Send rumble command
	DWORD res = XInputSetState(dev->device_handle, &dev->feedback);
	SDL_Log("XInputSetState called. Result: %lu\n", res);

	if (res == ERROR_SUCCESS) {
		SDL_Log("Rumble started successfully.\n");
		dev->rumble_active = TRUE;
	}
	else if (res == ERROR_IO_PENDING) {
		SDL_Log("Rumble operation is pending. It will complete asynchronously.\n");
		dev->rumble_active = TRUE;
	}
	else {
		SDL_Log("Rumble command failed with error: %lu\n", res);
		return SDL_Unsupported();
	}

	// Set end time for stopping rumble
	if (duration_ms > 0) {
		dev->rumble_end_time = SDL_GetTicks() + duration_ms;
		SDL_Log("Rumble will stop in %u ms.\n", duration_ms);
	}
	else {
		dev->rumble_end_time = 0; // No auto-stop
		SDL_Log("Rumble set to run indefinitely.\n");
	}

	return 0;
}

static void XBOX_JoystickUpdate(SDL_Joystick* joystick) {
	XboxControllerDevice* dev = (XboxControllerDevice*)joystick->hwdata;

	// Validate handle and connection status
	if (!dev || !dev->connected || !dev->device_handle) {
		return;
	}

	// Poll for disconnection or rumble stop
	XInputPoll(dev->device_handle);

	Uint32 current_time = SDL_GetTicks();
	if (dev->rumble_active && current_time >= dev->rumble_end_time) {
		SDL_Log("XBOX_JoystickUpdate: Stopping rumble motors.\n");
		SDL_zero(dev->feedback);
		XInputSetState(dev->device_handle, &dev->feedback);
		dev->rumble_active = FALSE;
	}

	// Get state from controller
	XINPUT_STATE state;
	DWORD res = XInputGetState(dev->device_handle, &state);

	// Handle disconnection
	if (res != ERROR_SUCCESS) {
		SDL_Log("XInputGetState failed for port %d. Disconnecting controller.\n", dev->port);

		// Notify SDL that the joystick has been removed
		SDL_PrivateJoystickRemoved(joystick->instance_id);

		// Close the handle and mark as disconnected
		XInputClose(dev->device_handle);
		dev->device_handle = NULL;
		dev->connected = FALSE;

		return;
	}

	// Apply dead zones to thumbsticks
#define DEAD_ZONE 7849
	SHORT sThumbLX = (abs(state.Gamepad.sThumbLX) > DEAD_ZONE) ? state.Gamepad.sThumbLX : 0;
	SHORT sThumbLY = (abs(state.Gamepad.sThumbLY) > DEAD_ZONE) ? state.Gamepad.sThumbLY : 0;
	SHORT sThumbRX = (abs(state.Gamepad.sThumbRX) > DEAD_ZONE) ? state.Gamepad.sThumbRX : 0;
	SHORT sThumbRY = (abs(state.Gamepad.sThumbRY) > DEAD_ZONE) ? state.Gamepad.sThumbRY : 0;

	// Map thumbstick axes
	SDL_PrivateJoystickAxis(joystick, XBOX_JOYSTICK_STICKTHUMB_LEFT_X, sThumbLX);
	SDL_PrivateJoystickAxis(joystick, XBOX_JOYSTICK_STICKTHUMB_LEFT_Y, sThumbLY);
	SDL_PrivateJoystickAxis(joystick, XBOX_JOYSTICK_STICKTHUMB_RIGHT_X, sThumbRX);
	SDL_PrivateJoystickAxis(joystick, XBOX_JOYSTICK_STICKTHUMB_RIGHT_Y, sThumbRY);

	WORD Digital_Buttons = state.Gamepad.wButtons;
	BYTE* Analog_Buttons = state.Gamepad.bAnalogButtons;

	// Map face buttons
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_A, (Analog_Buttons[XINPUT_GAMEPAD_A] > 0) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_B, (Analog_Buttons[XINPUT_GAMEPAD_B] > 0) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_X, (Analog_Buttons[XINPUT_GAMEPAD_X] > 0) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_Y, (Analog_Buttons[XINPUT_GAMEPAD_Y] > 0) ? SDL_PRESSED : SDL_RELEASED);

	// Map shoulder buttons
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_BLACK, (Analog_Buttons[XINPUT_GAMEPAD_BLACK] > 0) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_WHITE, (Analog_Buttons[XINPUT_GAMEPAD_WHITE] > 0) ? SDL_PRESSED : SDL_RELEASED);

	// Map triggers as buttons with threshold
	SDL_PrivateJoystickAxis(joystick, XBOX_JOYSTICK_LEFT_TRIGGER, (Analog_Buttons[XINPUT_GAMEPAD_LEFT_TRIGGER] << 8) - 0x7FFF);
	SDL_PrivateJoystickAxis(joystick, XBOX_JOYSTICK_RIGHT_TRIGGER, (Analog_Buttons[XINPUT_GAMEPAD_RIGHT_TRIGGER] << 8) - 0x7FFF);

	// Map start/back/thumbstick buttons
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_START, (Digital_Buttons & XINPUT_GAMEPAD_START) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_BACK, (Digital_Buttons & XINPUT_GAMEPAD_BACK) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_LEFT_THUMB, (Digital_Buttons & XINPUT_GAMEPAD_LEFT_THUMB) ? SDL_PRESSED : SDL_RELEASED);
	SDL_PrivateJoystickButton(joystick, XBOX_JOYSTICK_RIGHT_THUMB, (Digital_Buttons & XINPUT_GAMEPAD_RIGHT_THUMB) ? SDL_PRESSED : SDL_RELEASED);

	// Map D-Pad as hat
	Uint8 hat = SDL_HAT_CENTERED;
	if (Digital_Buttons & XINPUT_GAMEPAD_DPAD_UP)    hat |= SDL_HAT_UP;
	if (Digital_Buttons & XINPUT_GAMEPAD_DPAD_DOWN)  hat |= SDL_HAT_DOWN;
	if (Digital_Buttons & XINPUT_GAMEPAD_DPAD_LEFT)  hat |= SDL_HAT_LEFT;
	if (Digital_Buttons & XINPUT_GAMEPAD_DPAD_RIGHT) hat |= SDL_HAT_RIGHT;

	SDL_PrivateJoystickHat(joystick, 0, hat);
}

static void
XBOX_JoystickClose(SDL_Joystick* joystick)
{
	SDL_Log("XBOX_JoystickClose\n");
	XboxControllerDevice* dev = (XboxControllerDevice*)joystick->hwdata;
	if (dev && dev->connected && dev->device_handle) {
		XInputClose(dev->device_handle);
		dev->device_handle = NULL;
		dev->connected = FALSE;
	}
	joystick->hwdata = NULL;
}

static void
XBOX_JoystickQuit(void)
{
	// Close all open devices
	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_Controllers[i].connected && g_Controllers[i].device_handle) {
			XInputClose(g_Controllers[i].device_handle);
			g_Controllers[i].device_handle = NULL;
			g_Controllers[i].connected = FALSE;
		}
	}
	g_NumControllers = 0;
	SDL_Log("All controllers have been closed and resources released.\n");
}

static const char*
XBOX_JoystickGetDevicePath(int device_index) {
	// SDL_Log("XBOX_JoystickGetDeviceName called for device index %d\n", device_index);
	int count = 0;
	for (int i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_Controllers[i].connected) {
			if (count == device_index) {
				return "Xbox Controller";
			}
			count++;
		}
	}
	return NULL;
}

static void
XBOX_JoystickSetDevicePlayerIndex(int device_index, int player_index) {
	// Do nothing
}

static int
XBOX_RumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble) {
	return SDL_Unsupported();
}

static Uint32
XBOX_GetCapabilities(SDL_Joystick *joystick){
	return 0;
}

static int
XBOX_SetLed(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue) {
	return SDL_Unsupported();
}

static int
XBOX_SendEffect(SDL_Joystick *joystick, const void *data, int size) {
	return SDL_Unsupported();
}

static int
XBOX_SetSensorsEnabled(SDL_Joystick *joystick, static SDL_bool enabled) {
	return SDL_Unsupported();
}

static SDL_bool
XBOX_GetGamepadMapping(int device_index, SDL_GamepadMapping *out) {
	// Map thumbstick axes
	out->leftx.kind = EMappingKind_Axis;
	out->lefty.kind = EMappingKind_Axis;
	out->rightx.kind = EMappingKind_Axis;
	out->righty.kind = EMappingKind_Axis;
	out->leftx.target = XBOX_JOYSTICK_STICKTHUMB_LEFT_X;
	out->lefty.target = XBOX_JOYSTICK_STICKTHUMB_LEFT_Y;
	out->rightx.target = XBOX_JOYSTICK_STICKTHUMB_RIGHT_X;
	out->righty.target = XBOX_JOYSTICK_STICKTHUMB_RIGHT_Y;
	out->lefty.axis_reversed = SDL_TRUE;
	out->righty.axis_reversed = SDL_TRUE;

	// Map face buttons
	out->a.kind = EMappingKind_Button;
	out->b.kind = EMappingKind_Button;
	out->x.kind = EMappingKind_Button;
	out->y.kind = EMappingKind_Button;
	out->a.target = XBOX_JOYSTICK_A;
	out->b.target = XBOX_JOYSTICK_B;
	out->x.target = XBOX_JOYSTICK_X;
	out->y.target = XBOX_JOYSTICK_Y;

	// Map shoulder buttons
	out->leftshoulder.kind = EMappingKind_Button;
	out->rightshoulder.kind = EMappingKind_Button;
	out->leftshoulder.target = XBOX_JOYSTICK_WHITE;
	out->rightshoulder.target = XBOX_JOYSTICK_BLACK;

	// Map triggers as buttons with threshold
	out->lefttrigger.kind = EMappingKind_Axis;
	out->righttrigger.kind = EMappingKind_Axis;
	out->lefttrigger.target = XBOX_JOYSTICK_LEFT_TRIGGER;
	out->righttrigger.target = XBOX_JOYSTICK_RIGHT_TRIGGER;

	// Map start/back/thumbstick buttons
	out->start.kind = EMappingKind_Button;
	out->back.kind = EMappingKind_Button;
	out->leftstick.kind = EMappingKind_Button;
	out->rightstick.kind = EMappingKind_Button;
	out->start.target = XBOX_JOYSTICK_START;
	out->back.target = XBOX_JOYSTICK_BACK;
	out->leftstick.target = XBOX_JOYSTICK_LEFT_THUMB;
	out->rightstick.target = XBOX_JOYSTICK_RIGHT_THUMB;

	// Map D-Pad as hat
	out->dpup.kind = EMappingKind_Hat;
	out->dpdown.kind = EMappingKind_Hat;
	out->dpleft.kind = EMappingKind_Hat;
	out->dpright.kind = EMappingKind_Hat;
	out->dpup.target = SDL_HAT_UP;
	out->dpdown.target = SDL_HAT_DOWN;
	out->dpleft.target = SDL_HAT_LEFT;
	out->dpright.target = SDL_HAT_RIGHT;

	return SDL_TRUE;
}

SDL_JoystickDriver SDL_XBOX_JoystickDriver =
{
	XBOX_JoystickInit,
	XBOX_JoystickGetCount,
	XBOX_JoystickDetect,
	XBOX_JoystickGetDeviceName,
	XBOX_JoystickGetDevicePath,
	XBOX_JoystickGetDevicePlayerIndex,
	XBOX_JoystickGetDevicePlayerIndex,
	XBOX_JoystickSetDevicePlayerIndex,
	XBOX_JoystickGetDeviceGUID,
	XBOX_JoystickGetDeviceInstanceID,
	XBOX_JoystickOpen,
	XBOX_JoystickRumble,
	XBOX_RumbleTriggers,
	XBOX_GetCapabilities,
	XBOX_SetLed,
	XBOX_SendEffect,
	XBOX_SetSensorsEnabled,
	XBOX_JoystickUpdate,
	XBOX_JoystickClose,
	XBOX_JoystickQuit,
	XBOX_GetGamepadMapping,
};

#endif /* !SDL_JOYSTICK_DISABLED && __XBOX__ */

/* vi: set ts=4 sw=4 expandtab: */
