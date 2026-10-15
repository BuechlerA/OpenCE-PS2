/*
PS2_DEBUG.C

Debug text for the PS2 build, on the screen and on ps2link's console.

ps2sdk's libdebug draws text straight to the GS (init_scr, scr_printf),
which the game does not need while its Direct3D is null
(ps2_d3d_null.c). Everything this file prints goes to two places:

- the screen, so a person at the console sees how far the boot got, and
  ps2client's "scrdump" (it reads the display back) can save it as a
  picture without a capture card;
- stderr, which ps2link sends to ps2client's terminal (as platform_log's always went), so the same lines
  are in the host's scrollback after the screen has moved on.

platform_log (xbox_kernel.c) and the game's error file (errors.c) both call
ps2_debug_log, so assertions and "not implemented" notes appear without a
call of their own. ps2_debug_frame puts a heartbeat line on the screen every
few seconds: while it counts up, the main loop is alive.

This takes over the display (init_scr sets the GS's CRT mode). It goes when
the renderer owns the GS (docs/ps2_renderer_design.md): ps2_debug_init is
then called only for the lines before the renderer starts.

Called from the game (shell_xbox.c, main.c, errors.c).
*/

#include <debug.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_port_capacity.h"

/* the screen is 640 pixels wide in 8-pixel cells; a longer line wraps */
#define LINE_LENGTH 160
/* the heartbeat's period, in frames of the main loop (5 s at 60 Hz) */
#define FRAMES_PER_HEARTBEAT 300

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized;

static void debug_initialize(void)
{
	if (!initialized)
	{
		initialized = 1;
		init_scr();
		scr_setCursor(0);
	}
}

/* prints one line, with the lock held */
static void debug_line(char const *text)
{
	debug_initialize();
	scr_printf("%s\n", text);
	fputs(text, stderr);
	fputc('\n', stderr);
}

void ps2_debug_init(void)
{
	char line[LINE_LENGTH];

	pthread_mutex_lock(&lock);
	debug_initialize();
	scr_clear();
	snprintf(line, sizeof(line), "Halo PS2  built %s %s", __DATE__, __TIME__);
	debug_line(line);
	snprintf(line, sizeof(line), "tag cache at %08lx", (unsigned long)HALO_PORT_TAG_CACHE_ADDRESS);
	debug_line(line);
	pthread_mutex_unlock(&lock);
}

void ps2_debug_log(char const *format, ...)
{
	char line[LINE_LENGTH];
	va_list arguments;
	size_t length;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	/* (the game's lines end with a newline of their own) */
	length = strlen(line);
	while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
		line[--length] = '\0';
	pthread_mutex_lock(&lock);
	debug_line(line);
	pthread_mutex_unlock(&lock);
}

/* called each frame of the main loop */
void ps2_debug_frame(unsigned char main_menu_scenario_loaded, unsigned char game_in_progress)
{
	static unsigned long frames;

	if (++frames % FRAMES_PER_HEARTBEAT == 0)
		ps2_debug_log("main loop: frame %lu, menu scenario %d, game %d", frames, main_menu_scenario_loaded,
			game_in_progress);
}
