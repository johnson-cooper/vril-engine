/*
Copyright (C) 2026 NZ:P Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// ps2_input.c -- DualShock 2 input (libpad / padman)
//
// Buttons are translated to the engine's generic gamepad keys and go
// through the normal binding system, so everything stays configurable.
// Both analog sticks are exposed (the PSP "one stick" limitation does not
// apply). In menus the left stick also generates D-pad key events with
// auto-repeat, so the whole UI works with either the stick or the D-pad.

#include "../../nzportable_def.h"
#include "../../menu/menu_defs.h"
#include "ps2_iop.h"

#include <kernel.h>
#include <libpad.h>

extern void PS2_Log(const char *fmt, ...);

static char pad_area[256] __attribute__((aligned(64)));

static cvar_t in_ps2_deadzone = {"in_ps2_deadzone", "0.10", true};
static cvar_t in_ps2_menustick = {"in_ps2_menustick", "1", true};

typedef enum { PAD_S_CLOSED, PAD_S_WAIT, PAD_S_CONFIG, PAD_S_READY } pad_phase_t;

static pad_phase_t pad_phase = PAD_S_CLOSED;
static int pad_opened;
static int pad_has_analog;
static int pad_has_actuators;
static u16 pad_last_btns;       // active high
static u8  pad_lx = 128, pad_ly = 128, pad_rx = 128, pad_ry = 128;

// rumble
static int    rumble_small, rumble_large;
static double rumble_until;
static int    rumble_active;

typedef struct { u16 mask; int key; } button_map_t;

static const button_map_t button_map[] = {
	{ PAD_CROSS,    K_BOTTOMFACE },
	{ PAD_CIRCLE,   K_RIGHTFACE },
	{ PAD_SQUARE,   K_LEFTFACE },
	{ PAD_TRIANGLE, K_TOPFACE },
	{ PAD_L1,       K_LTRIGGER },
	{ PAD_R1,       K_RTRIGGER },
	{ PAD_L2,       K_ZLTRIGGER },
	{ PAD_R2,       K_ZRTRIGGER },
	{ PAD_L3,       K_LTHUMB },
	{ PAD_R3,       K_RTHUMB },
	{ PAD_START,    K_START },
	{ PAD_SELECT,   K_SELECT },
	{ PAD_UP,       K_DPAD_UP },
	{ PAD_DOWN,     K_DPAD_DOWN },
	{ PAD_LEFT,     K_DPAD_LEFT },
	{ PAD_RIGHT,    K_DPAD_RIGHT },
};
#define BUTTON_MAP_COUNT ((int)(sizeof(button_map) / sizeof(button_map[0])))

qboolean IN_PlatformHasMouse(void) { return false; }
qboolean IN_PlatformHasGamepad(void) { return true; }
void IN_SetMouseToRelative(bool relative) { (void)relative; }
void IN_PlatformMouseMove(usercmd_t *cmd) { (void)cmd; }
void IN_PlatformMove(usercmd_t *cmd) { (void)cmd; }

static void pad_release_all(void)
{
	int i;
	for (i = 0; i < BUTTON_MAP_COUNT; i++)
		if (pad_last_btns & button_map[i].mask)
			Key_Event(button_map[i].key, false);
	pad_last_btns = 0;
	pad_lx = pad_ly = pad_rx = pad_ry = 128;
}

void IN_PlatformClearPendingInput(void)
{
	pad_release_all();
}

void IN_PlatformInit(void)
{
	Cvar_RegisterVariable(&in_ps2_deadzone);
	Cvar_RegisterVariable(&in_ps2_menustick);

	if (!PS2_IOP_GroupReady(PS2_IOP_PAD)) {
		Con_Printf("PS2 pad: padman not loaded, controller unavailable\n");
		return;
	}
	if (padInit(0) != 1) {
		Con_Printf("PS2 pad: padInit failed\n");
		return;
	}
	if (padPortOpen(0, 0, pad_area) == 0) {
		Con_Printf("PS2 pad: padPortOpen failed\n");
		return;
	}
	pad_opened = 1;
	pad_phase = PAD_S_WAIT;
	Con_Printf("PS2 pad: port 1 opened\n");
}

void IN_PlatformShutdown(void)
{
	if (pad_opened) {
		padPortClose(0, 0);
		padEnd();
		pad_opened = 0;
	}
}

// Non-blocking controller state machine: detect, switch to locked analog
// (DualShock) mode, discover actuators.
static int pad_update_phase(void)
{
	int state = padGetState(0, 0);

	if (state == PAD_STATE_DISCONN) {
		if (pad_phase == PAD_S_READY) {
			Con_Printf("PS2 pad: controller disconnected\n");
			pad_release_all();
		}
		pad_phase = PAD_S_WAIT;
		return 0;
	}
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
		return 0;

	switch (pad_phase) {
	case PAD_S_WAIT: {
		int modes = padInfoMode(0, 0, PAD_MODETABLE, -1);
		int i;
		pad_has_analog = 0;
		for (i = 0; i < modes; i++)
			if (padInfoMode(0, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK)
				pad_has_analog = 1;
		if (pad_has_analog) {
			padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
			pad_phase = PAD_S_CONFIG;
			return 0;
		}
		Con_Printf("PS2 pad: digital controller detected (no analog sticks)\n");
		pad_phase = PAD_S_READY;
		return 1;
	}
	case PAD_S_CONFIG:
		if (padInfoMode(0, 0, PAD_MODECURID, 0) != PAD_TYPE_DUALSHOCK)
			return 0;  // mode switch still in progress
		pad_has_actuators = padInfoAct(0, 0, -1, 0) > 0;
		if (pad_has_actuators) {
			char align[6] = { 0, 1, 0xff, 0xff, 0xff, 0xff };
			padSetActAlign(0, 0, align);
		}
		Con_Printf("PS2 pad: DualShock ready (analog, %s)\n", pad_has_actuators ? "vibration" : "no vibration");
		pad_phase = PAD_S_READY;
		return 1;
	case PAD_S_READY:
		return 1;
	default:
		return 0;
	}
}

static void menu_stick_repeat(float x, float y)
{
	static int held_key;
	static double next_time;
	int key = 0;

	if (y > 0.6f) key = K_DPAD_UP;
	else if (y < -0.6f) key = K_DPAD_DOWN;
	else if (x < -0.6f) key = K_DPAD_LEFT;
	else if (x > 0.6f) key = K_DPAD_RIGHT;

	if (key != held_key) {
		if (held_key)
			Key_Event(held_key, false);
		held_key = key;
		if (key) {
			Key_Event(key, true);
			next_time = realtime + 0.35;
		}
		return;
	}
	if (key && realtime >= next_time) {
		Key_Event(key, false);
		Key_Event(key, true);
		next_time = realtime + 0.12;
	}
}

static void rumble_apply(int small_on, int large)
{
	char act[6];
	if (!pad_has_actuators || pad_phase != PAD_S_READY)
		return;
	act[0] = small_on ? 1 : 0;
	act[1] = (char)large;
	act[2] = act[3] = act[4] = act[5] = 0;
	padSetActDirect(0, 0, act);
}

void IN_PlatformRumble(unsigned short low_frequency, unsigned short high_frequency, unsigned int duration)
{
	// low frequency -> large motor (strength), high frequency -> small motor (on/off)
	rumble_large = low_frequency >> 8;
	rumble_small = high_frequency > 0x4000;
	rumble_until = realtime + duration / 1000.0;
	rumble_active = 1;
	rumble_apply(rumble_small, rumble_large);
}

void IN_PlatformCommands(void)
{
	struct padButtonStatus buttons;
	u16 btns, changed;
	int i;

	if (!pad_opened)
		return;
	if (!pad_update_phase())
		return;
	if (padRead(0, 0, &buttons) == 0)
		return;

	btns = 0xffff ^ buttons.btns;   // active low -> active high
	changed = btns ^ pad_last_btns;
	for (i = 0; i < BUTTON_MAP_COUNT; i++) {
		if (changed & button_map[i].mask) {
			IN_SetActiveDevice(IN_DEVICE_GAMEPAD);
			Key_Event(button_map[i].key, (btns & button_map[i].mask) ? true : false);
		}
	}
	pad_last_btns = btns;

	if (pad_has_analog) {
		pad_lx = buttons.ljoy_h;
		pad_ly = buttons.ljoy_v;
		pad_rx = buttons.rjoy_h;
		pad_ry = buttons.rjoy_v;
	}

	if (in_ps2_menustick.value && (key_dest == key_menu || key_dest == key_menu_pause)) {
		in_analog_stick_t l;
		IN_GetAnalogStick(IN_STICK_LEFT, &l);
		menu_stick_repeat(l.x, l.y);
	}

	if (rumble_active && realtime >= rumble_until) {
		rumble_active = 0;
		rumble_apply(0, 0);
	}
}

static float axis(u8 v, int invert)
{
	float f = ((float)v - 127.5f) / 127.5f;
	float dz = in_ps2_deadzone.value;
	if (invert)
		f = -f;
	if (f > -dz && f < dz)
		return 0.0f;
	if (f > 1.0f) f = 1.0f;
	if (f < -1.0f) f = -1.0f;
	return f;
}

void IN_GetAnalogStick(in_analog_stick_id_t stick, in_analog_stick_t *value)
{
	if (!pad_has_analog || pad_phase != PAD_S_READY) {
		value->x = value->y = 0.0f;
		return;
	}
	if (stick == IN_STICK_LEFT) {
		value->x = axis(pad_lx, 0);
		value->y = axis(pad_ly, 1);   // positive Y up
	} else {
		value->x = axis(pad_rx, 0);
		value->y = axis(pad_ry, 1);
	}
}
