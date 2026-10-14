/*
PAD.C

libpad on port 1, slot 0. The DualShock gives sticks as bytes (0..255, 128 at rest, down and right positive) and
the buttons with a bit cleared for a pressed one; the controller takes the Xbox's: signed 16 bits, up and right
positive. The buttons the first-person controller reads, as the Xbox's default layout has them: the d-pad, Cross (A: jump), Square (X: reload),
Triangle (Y: switch weapons), Circle (B: melee), L3 (the left stick's click: crouch), R3 (the right stick's: zoom), R2 (the right trigger: fire; its
pressure when the pad gives it), L2 (the left trigger: grenade), Select (Back: autolevel) and Start (the demo's).
*/

#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <libpad.h>

#include "pad.h"

static char pad_area[256] __attribute__((aligned(64)));
static int opened, analog_requested, pressure_requested;

int pad_init(void)
{
	SifInitRpc(0);
	SifLoadFileInit();
	/* the console's own modules (a negative return can mean they are already loaded) */
	SifLoadModule("rom0:SIO2MAN", 0, NULL);
	SifLoadModule("rom0:PADMAN", 0, NULL);
	if (padInit(0) < 0) return 0;
	opened = padPortOpen(0, 0, pad_area) != 0;
	return opened;
}

static short stick(int value, int sign)
{
	int v = (value - 128) * sign * 258;       /* 127 steps either side of the middle to the 16-bit range */

	return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}


int pad_read(fp_pad *pad, float seconds)
{
	struct padButtonStatus status;
	int state, mode;
	unsigned buttons;

	(void)seconds;
	memset(pad, 0, sizeof(*pad));
	if (!opened) return 0;
	state = padGetState(0, 0);
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) return 0;
	if (!analog_requested)       /* a DualShock starts in digital mode: ask for the sticks, and keep them whatever the Analog button says */
	{
		padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
		analog_requested = 1;
	}
	else if (!pressure_requested && padInfoMode(0, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK)    /* and the buttons' pressure, for the trigger */
	{
		padEnterPressMode(0, 0);
		pressure_requested = 1;
	}
	if (padRead(0, 0, &status) == 0) return 0;
#ifdef PAD_DEBUG                  /* -DPAD_DEBUG: what libpad sees, twice a second */
	{
		static int calls;

		if (++calls % 30 == 1)
			printf("PAD: state %d type %d buttons %04x sticks L %d,%d R %d,%d\n", state, padInfoMode(0, 0, PAD_MODECURID, 0), (unsigned)(0xffff ^ status.btns), status.ljoy_h, status.ljoy_v, status.rjoy_h, status.rjoy_v);
	}
#endif
	buttons = 0xffff ^ status.btns;
	if (buttons & PAD_UP) pad->buttons |= FP_DPAD_UP;
	if (buttons & PAD_DOWN) pad->buttons |= FP_DPAD_DOWN;
	if (buttons & PAD_LEFT) pad->buttons |= FP_DPAD_LEFT;
	if (buttons & PAD_RIGHT) pad->buttons |= FP_DPAD_RIGHT;
	if (buttons & PAD_CROSS) pad->buttons |= FP_JUMP;
	if (buttons & PAD_L3) pad->buttons |= FP_CROUCH;
	if (buttons & PAD_START) pad->buttons |= FP_START;
	if (buttons & PAD_SQUARE) pad->buttons |= FP_RELOAD;
	if (buttons & PAD_TRIANGLE) pad->buttons |= FP_SWITCH_WEAPON;
	if (buttons & PAD_CIRCLE) pad->buttons |= FP_MELEE;
	if (buttons & PAD_R3) pad->buttons |= FP_ZOOM;
	if (buttons & PAD_L2) pad->buttons |= FP_GRENADE;
	if (buttons & PAD_SELECT) pad->buttons |= FP_AUTOLEVEL;
	if (buttons & PAD_R2) pad->trigger = 255;                      /* the button; the pressure, if the pad gives it, replaces it */
	if (padInfoPressMode(0, 0) == 1 && status.r2_p) pad->trigger = status.r2_p;
	mode = padInfoMode(0, 0, PAD_MODECURID, 0);
	if (mode == PAD_TYPE_ANALOG || mode == PAD_TYPE_DUALSHOCK)      /* a digital pad has no sticks: its bytes are not at rest */
	{
		pad->left_x = stick(status.ljoy_h, 1);
		pad->left_y = stick(status.ljoy_v, -1);
		pad->right_x = stick(status.rjoy_h, 1);
		pad->right_y = stick(status.rjoy_v, -1);
	}
	return 1;
}
