/*
PS2_UNIMPLEMENTED.C

What the PS2 build's generated stubs call (tools/ps2/unimplemented_stubs.py):
a function the game needs and the PS2 platform layer does not have yet. It
prints the function's name on the EE console and stops the program, so a run
in PCSX2 (tools/ps2/pcsx2_run.py --until unimplemented:) names the next
function to write.
*/

#include <stdio.h>
#include <kernel.h>

void halo_ps2_unimplemented(const char *name)
{
	printf("unimplemented: %s\n", name);
	for (;;)
	{
		SleepThread();
	}
}
