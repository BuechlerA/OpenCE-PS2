/*
PAD.C

libpad on port 1, slot 0. The DualShock gives sticks as bytes (0..255, 128 at rest, down and right positive) and
the buttons with a bit cleared for a pressed one; the controller takes the Xbox's: signed 16 bits, up and right
positive. The buttons the first-person controller reads: the d-pad, Cross (the Xbox's A, jump) and L3 (the left
stick's click, crouch).
*/

#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <libpad.h>

#include "pad.h"

static char pad_area[256] __attribute__((aligned(64)));
static int opened, analog_requested;

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

#ifdef FP_SCRIPT
/* a walk: stand and look at the Chief, run, turn right, run on, jump, crouch, again */
static int scripted(fp_pad *pad, float seconds)
{
	float t = seconds - 10.f * (int)(seconds / 10.f);

	memset(pad, 0, sizeof(*pad));
	if (t >= 1.f && t < 3.f) pad->left_y = 32767;
	if (t >= 3.f && t < 4.f) pad->right_x = 32767;
	if (t >= 4.f && t < 6.f) { pad->left_y = 32767; pad->left_x = 20000; }
	if (t >= 6.f && t < 6.2f) pad->buttons |= FP_JUMP;
	if (t >= 6.2f && t < 7.5f) pad->left_y = 32767;
	if (t >= 7.5f && t < 9.f) pad->buttons |= FP_CROUCH;
	if (t >= 7.5f && t < 9.f) pad->right_y = 12000;
	return 1;
}
#endif

int pad_read(fp_pad *pad, float seconds)
{
	struct padButtonStatus status;
	int state, mode;
	unsigned buttons;

#ifdef FP_SCRIPT
	return scripted(pad, seconds);
#endif
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
