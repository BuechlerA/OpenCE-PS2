/*
PS2_INPUT.C

The Xbox's input devices on the PS2, until libpad (docs/ps2_port_plan.md,
"Sound, input, saves and streaming"): no gamepad is connected, but for the
scripted player of a benchmark (debug.bench_map, ps2_bench.c), which plays
port 0 as the Linux build's debug.test_input "bot" does
(port/linux/src/xinput_sdl.c). The debug keyboard queue is empty.
*/

#include "platform.h"
#include "port_config.h"

#include <string.h>

/* ps2_bench.c's: whether the scripted player is playing, and its controls */
int ps2_bench_player_active(void);
void ps2_bench_player_gamepad(XINPUT_GAMEPAD *gamepad);

static struct
{
	int open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
} controller;

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types) { }

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	static int reported;

	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD && !reported && config_string("debug.bench_map")[0])
	{
		reported = TRUE;
		*insertions = XDEVICE_PORT0_MASK;
	}
	return *insertions != 0;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	if (device_type == XDEVICE_TYPE_GAMEPAD && port == 0 && config_string("debug.bench_map")[0])
	{
		memset(&controller, 0, sizeof(controller));
		controller.open = TRUE;
		return (HANDLE)&controller;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	if (device == (HANDLE)&controller)
		controller.open = FALSE;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	memset(state, 0, sizeof(*state));
	if (device != (HANDLE)&controller || !controller.open)
		return ERROR_DEVICE_NOT_CONNECTED;
	if (ps2_bench_player_active())
		ps2_bench_player_gamepad(&state->Gamepad);
	if (memcmp(&state->Gamepad, &controller.previous, sizeof(state->Gamepad)))
	{
		controller.packet_number++;
		controller.previous = state->Gamepad;
	}
	state->dwPacketNumber = controller.packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return device == (HANDLE)&controller ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters) { return ERROR_SUCCESS; }

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}

void test_input_hold_action(int hold) { }
