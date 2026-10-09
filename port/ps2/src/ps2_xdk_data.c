/*
PS2_XDK_DATA.C

The XDK's data that the game reads directly, which the Linux build defines
with the layers that use it (port/linux/src/d3d8_gl.c, xinput_sdl.c): the
render and texture stage states its inline Direct3D functions keep, the
index buffer they draw from, and the device types XInput identifies devices
by (their addresses). The PS2's renderer and input will own them; until
then they are plain storage.
*/

#include "platform.h"

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;
