/*
PS2_BENCH.C

The PS2 build's benchmark (debug.bench_map in config.toml, HALO_BENCH_MAP):
the measurement of Phase 1, "the simulation on hardware"
(docs/ps2_port_plan.md). It loads a level from the main menu without a
person, plays it with a scripted player (the Linux build's debug.test_input
"bot" pattern, by game time so each run plays the same) for
debug.bench_ticks ticks, and logs how long the ticks take: the game's
simulation only (game_tick), not the null renderer's or the waits between
frames. Every 150 ticks it logs the average and the longest of them, and at
the end the totals and how many ticks took longer than the 15 ms of Gate 1.

The time is the EE's cycle counter, so on a PS2 it is real; in PCSX2 it is
the emulated cycles, which leave out the caches' and the memory's waits and
so read low. The result is for comparing builds, and for finding the ticks
that are much longer than the rest.

Called from the game (main.c, game_time.c).
*/

#include "platform.h"
#include "port_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <timer.h>
#include <unistd.h>

/* the game's */
void main_set_map_name(char const *map_name);
long game_time_get(void);

/* the EE's CPU clock, the rate of cpu_ticks */
#define CYCLES_PER_MILLISECOND 294912UL
#define TICKS_PER_REPORT 150
/* Gate 1's budget for a typical campaign tick */
#define BUDGET_MILLISECONDS 15.0

enum
{
	_bench_off,
	_bench_waiting_for_menu,
	_bench_loading,
	_bench_running,
	_bench_done,
};

static struct
{
	int state;
	unsigned long frames;
	unsigned long menu_frames;
	unsigned long load_frames;
	unsigned long tick_start;
	unsigned long window_ticks;
	unsigned long window_cycles;
	unsigned long window_longest;
	unsigned long total_ticks;
	double total_cycles;
	unsigned long total_longest;
	unsigned long over_budget;
	unsigned long first_game_time;
} bench;

static int bench_enabled(void)
{
	static int checked, enabled;

	if (!checked)
	{
		checked = TRUE;
		enabled = config_string("debug.bench_map")[0] != '\0';
	}
	return enabled;
}

static double milliseconds(double cycles)
{
	return cycles / (double)CYCLES_PER_MILLISECOND;
}

static void bench_report(void)
{
	platform_log("bench: tick %ld: %lu ticks, %.2f ms on average, %.2f ms the longest",
		game_time_get(), bench.window_ticks, milliseconds((double)bench.window_cycles / bench.window_ticks),
		milliseconds((double)bench.window_longest));
	bench.window_ticks = 0;
	bench.window_cycles = 0;
	bench.window_longest = 0;
}

void ps2_bench_tick_begin(void)
{
	if (bench.state == _bench_running)
		bench.tick_start = cpu_ticks();
}

void ps2_bench_tick_end(void)
{
	unsigned long cycles;

	if (bench.state != _bench_running)
		return;
	cycles = cpu_ticks() - bench.tick_start;
	bench.window_ticks++;
	bench.window_cycles += cycles;
	if (cycles > bench.window_longest)
		bench.window_longest = cycles;
	bench.total_ticks++;
	bench.total_cycles += (double)cycles;
	if (cycles > bench.total_longest)
		bench.total_longest = cycles;
	if (milliseconds((double)cycles) > BUDGET_MILLISECONDS)
		bench.over_budget++;
	if (bench.window_ticks >= TICKS_PER_REPORT)
		bench_report();
}

/* called each frame of the main loop */
void ps2_bench_update(unsigned char main_menu_scenario_loaded, unsigned char game_in_progress)
{
	if (!bench_enabled() || bench.state == _bench_done)
		return;
	/* (the loop is alive, and where the game is, while it waits) */
	if (bench.state != _bench_running && ++bench.frames % 300 == 0)
		platform_log("bench: frame %lu, state %d, menu scenario %d, game %d", bench.frames, bench.state,
			main_menu_scenario_loaded, game_in_progress);
	switch (bench.state)
	{
	case _bench_off:
		bench.state = _bench_waiting_for_menu;
		break;
	case _bench_waiting_for_menu:
		/* (a second of the menu first, as a player would) */
		if (main_menu_scenario_loaded && ++bench.menu_frames >= 60)
		{
			platform_log("bench: loading %s", config_string("debug.bench_map"));
			main_set_map_name(config_string("debug.bench_map"));
			bench.state = _bench_loading;
		}
		break;
	case _bench_loading:
		/* (the level is in once the game runs it, not the menu's scenario) */
		if (game_in_progress && !main_menu_scenario_loaded)
		{
			if (++bench.load_frames >= 30)
			{
				platform_log("bench: %s is loaded; %ld ticks to play", config_string("debug.bench_map"),
					config_integer("debug.bench_ticks"));
				bench.first_game_time = (unsigned long)game_time_get();
				bench.state = _bench_running;
			}
		}
		break;
	case _bench_running:
		if (bench.total_ticks >= (unsigned long)config_integer("debug.bench_ticks"))
		{
			if (bench.window_ticks)
				bench_report();
			platform_log("bench: %lu ticks, %.2f ms on average, %.2f ms the longest, %lu over %.0f ms",
				bench.total_ticks, milliseconds(bench.total_cycles / bench.total_ticks),
				milliseconds((double)bench.total_longest), bench.over_budget, BUDGET_MILLISECONDS);
			/* the memory the heap has not taken yet: from its end (malloc's
			sbrk) to the main thread's stack, 128 KB below the 32 MB */
			platform_log("bench: heap: %lu bytes never taken (of 32 MB), its end at 0x%lx",
				0x2000000UL - 0x20000UL - (unsigned long)sbrk(0), (unsigned long)sbrk(0));
			platform_log("bench: done");
			bench.state = _bench_done;
		}
		break;
	}
}

/* ---------- the scripted player (xinput_sdl.c, test_input_gamepad) */

int ps2_bench_player_active(void)
{
	return bench.state == _bench_running;
}

void ps2_bench_player_gamepad(XINPUT_GAMEPAD *gamepad)
{
	double t = (double)(game_time_get() - (long)bench.first_game_time) / 30.0;

	gamepad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
	gamepad->sThumbLX = (SHORT)(cos(t * 0.6) * 20000.0);
	gamepad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
	if (fmod(t, 3.0) < 0.3)
		gamepad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
	if (fmod(t, 5.0) < 0.1)
		gamepad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
	if (fmod(t, 7.0) < 0.2)
		gamepad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
}
