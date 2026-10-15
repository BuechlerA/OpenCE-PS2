/*
HALO_VARIADIC_PROTOTYPES.H

Force-included by the PS2 build (tools/ps2_build.py, "variadic_prototype_files"
of port/ps2/port.json) into the game files that call these variadic functions
without a prototype in scope. x86 passes every argument on the stack either
way, but the EE's n32 ABI passes floating-point arguments in registers and
the variadic ones on the stack, so such a call must see the real prototype.
*/

#ifndef __HALO_VARIADIC_PROTOTYPES_H
#define __HALO_VARIADIC_PROTOTYPES_H

union real_argb_color;

void error(short priority, const char *format, ...);
void console_printf(unsigned char clear, char const *format, ...);
void terminal_printf(union real_argb_color const *color, char const *format, ...);

#endif
