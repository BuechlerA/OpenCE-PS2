/* Host test of src/fp_weapon.c against the numbers the map's tags give: the assault rifle's cadence and magazine, reload and ready times from the
   first-person animations, the pistol's, the sniper rifle's zoom, switching, and shots against the map and the Chief.

     cc -O1 -I src -o /tmp/fp_weapon_test tools/fp_weapon_test.c src/fp_weapon.c src/fp_controller.c src/map_assets.c src/weapon_assets.c -lm && /tmp/fp_weapon_test
*/
#include <math.h>
#include <stdio.h>

#include "../src/fp_controller.h"
#include "../src/fp_weapon.h"
#include "../src/map_assets.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fpw_events events;

static int count_events(int type)
{
	int i, n = 0;

	for (i = 0; i < events.count; i++) if (events.list[i].type == type) n++;
	return n;
}

static void tick(fpw_inventory *inv, float trigger, int reload, int switch_weapon, int melee, int zoom)
{
	fpw_input in = { trigger, reload, switch_weapon, melee, zoom, 0 };

	events.count = 0;
	fpw_inventory_tick(inv, &in, &events);
}

/* a player aiming along +x from 0.62 above the ground at the start */
static void aim_at(fpw_inventory *inv, const float *from, const float *to)
{
	float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	int k;

	for (k = 0; k < 3; k++) { inv->aim.origin[k] = from[k]; inv->aim.forward[k] = d[k] / l; inv->aim.velocity[k] = 0.f; }
}

int main(void)
{
	fpw_inventory inv;
	int ar[2] = { FPW_ASSAULT_RIFLE, FPW_PISTOL }, i, fired, shots, ticks;
	float eye[3] = { MAP_DEMO_CHIEF_X + 2.4f, MAP_DEMO_CHIEF_Y, 0.f };

	{ float g; fp_ground(eye[0], eye[1], 5.f, &g, NULL, NULL); eye[2] = g + 0.62f; }

	/* the assault rifle: readied (1.3 s of ready_time is not the ready animation: 29 frames), 60 rounds loaded of 240 */
	fpw_inventory_init(&inv, ar, 2, 7u);
	aim_at(&inv, eye, (float[3]) { MAP_DEMO_CHIEF_X, MAP_DEMO_CHIEF_Y, eye[2] - 0.2f });
	CHECK(inv.slots[0].magazines[0].rounds_loaded == 60 && inv.slots[0].magazines[0].rounds_total == 180, "loaded %d total %d", inv.slots[0].magazines[0].rounds_loaded, inv.slots[0].magazines[0].rounds_total);
	for (i = 0; i < 10; i++) tick(&inv, 1.f, 0, 0, 0, 0);
	CHECK(inv.slots[0].magazines[0].rounds_loaded == 60, "fired while it was being readied (%d loaded)", inv.slots[0].magazines[0].rounds_loaded);
	ticks = 0;
	do { tick(&inv, 0.f, 0, 0, 0, 0); ticks++; } while (inv.slots[0].state_timer > 0 && ticks < 100);
	printf("ready took %d ticks in all (the animation has %d frames)\n", 10 + ticks, fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_READY));
	CHECK(10 + ticks == fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_READY) || 10 + ticks == fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_READY) + 1, "ready ticks");

	/* 2 seconds of the trigger: 15 rounds a second (the rate is the tag's initial and final, 15) */
	fired = 0;
	for (i = 0; i < 60; i++) { tick(&inv, 1.f, 0, 0, 0, 0); fired += count_events(FPW_EVENT_FIRE); }
	printf("assault rifle: %d rounds in 2 s (15 a second: 30)\n", fired);
	CHECK(fired >= 29 && fired <= 31, "fired %d", fired);
	CHECK(inv.slots[0].magazines[0].rounds_loaded == 60 - fired, "rounds loaded %d", inv.slots[0].magazines[0].rounds_loaded);

	/* on to the empty magazine: the reload starts by itself, takes the first-person animation's frames, loads 60 */
	shots = fired; ticks = 0;
	while (inv.slots[0].magazines[0].rounds_loaded > 0 && ticks++ < 200) { tick(&inv, 1.f, 0, 0, 0, 0); shots += count_events(FPW_EVENT_FIRE); }
	CHECK(shots == 60, "the magazine had %d rounds", shots);
	ticks = 0;
	while (inv.slots[0].magazines[0].state != FPW_MAGAZINE_RELOADING && ticks++ < 10) tick(&inv, 0.f, 0, 0, 0, 0);
	ticks = 0;
	while (inv.slots[0].magazines[0].state == FPW_MAGAZINE_RELOADING && ticks < 400) { tick(&inv, 0.f, 0, 0, 0, 0); ticks++; }
	printf("reload took %d ticks (the animation has %d frames); loaded %d, in reserve %d\n", ticks, fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_RELOAD_WHILE_EMPTY), inv.slots[0].magazines[0].rounds_loaded, inv.slots[0].magazines[0].rounds_total);
	CHECK(ticks >= fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_RELOAD_WHILE_EMPTY) - 2 && ticks <= fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_RELOAD_WHILE_EMPTY) + 2, "reload ticks %d", ticks);
	CHECK(inv.slots[0].magazines[0].rounds_loaded == 60 && inv.slots[0].magazines[0].rounds_total == 120, "after reload %d / %d", inv.slots[0].magazines[0].rounds_loaded, inv.slots[0].magazines[0].rounds_total);

	/* shots reach the map, or the Chief */
	{
		int impacts = 0, on_chief = 0, objects = 0;

		for (i = 0; i < 4; i++) tick(&inv, 0.f, 0, 0, 0, 0);
		for (i = 0; i < 40; i++)
		{
			int k;

			tick(&inv, 1.f, 0, 0, 0, 0);
			for (k = 0; k < events.count; k++) if (events.list[k].type == FPW_EVENT_IMPACT) { impacts++; if (events.list[k].b == 2) on_chief++; if (events.list[k].b == 1) objects++; }
		}
		for (i = 0; i < 10; i++) { int k; tick(&inv, 0.f, 0, 0, 0, 0); for (k = 0; k < events.count; k++) if (events.list[k].type == FPW_EVENT_IMPACT) { impacts++; if (events.list[k].b == 2) on_chief++; } }
		printf("40 ticks of fire at the Chief from 2.4 units: %d impacts, %d on him (the error cone of up to 6.5 degrees spreads the rest)\n", impacts, on_chief);
		CHECK(impacts >= 15 && on_chief >= 8, "impacts %d, on the Chief %d", impacts, on_chief);
	}

	/* switching to the pistol: put away, ready, then 3.5 rounds a second */
	{
		int put_away = 0, ready = 0, first_fire = -1;

		fpw_inventory_init(&inv, ar, 2, 7u);
		aim_at(&inv, eye, (float[3]) { MAP_DEMO_CHIEF_X, MAP_DEMO_CHIEF_Y, eye[2] - 0.2f });
		for (i = 0; i < 40; i++) tick(&inv, 0.f, 0, 0, 0, 0);
		for (i = 0; i < 200; i++)
		{
			tick(&inv, 1.f, 0, i == 0, 0, 0);
			if (count_events(FPW_EVENT_PUT_AWAY)) put_away = i;
			if (count_events(FPW_EVENT_READY)) ready = i;
			if (count_events(FPW_EVENT_FIRE) && first_fire < 0) first_fire = i;
		}
		printf("switch: put away at tick %d, readied at %d (the animations: %d and %d frames), first round at %d\n", put_away, ready,
		       fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_PUT_AWAY), fpw_animation_ticks(FPW_PISTOL, FPW_ANIMATION_READY), first_fire);
		CHECK(inv.current == 1 && fpw_weapons[inv.slots[1].weapon].zoom_level_count == 1, "not on the pistol");
		CHECK(ready - put_away == fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_PUT_AWAY) + 1 || ready - put_away == fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_PUT_AWAY), "readied %d ticks after the put-away", ready - put_away);
		CHECK(first_fire >= ready + fpw_animation_ticks(FPW_PISTOL, FPW_ANIMATION_READY) - 1, "fired during the ready animation");
	}

	/* the pistol's cadence, and the sniper rifle's zoom */
	{
		int sniper[2] = { FPW_SNIPER_RIFLE, FPW_PISTOL };
		float fov[4], unit_fov = 70.f * (float)M_PI / 180.f;
		int level[4];

		fpw_inventory_init(&inv, sniper, 2, 3u);
		for (i = 0; i < 60; i++) tick(&inv, 0.f, 0, 0, 0, 0);
		for (i = 0; i < 3; i++) { tick(&inv, 0.f, 0, 0, 0, 1); level[i] = inv.zoom_level; fov[i] = fpw_field_of_view(&inv, unit_fov) * 180.f / (float)M_PI; }
		tick(&inv, 0.f, 0, 0, 0, 1); level[3] = inv.zoom_level; fov[3] = fpw_field_of_view(&inv, unit_fov) * 180.f / (float)M_PI;
		printf("sniper zoom: levels %d %d %d %d, fields of view %.1f %.1f %.1f %.1f degrees (magnifications 2 and 8 of 70)\n", level[0], level[1], level[2], level[3], fov[0], fov[1], fov[2], fov[3]);
		CHECK(level[0] == 0 && level[1] == 1 && level[2] == -1 && level[3] == 0, "zoom levels %d %d %d %d", level[0], level[1], level[2], level[3]);
		CHECK(fabsf(fov[0] - 35.f) < 0.1f && fabsf(fov[1] - 8.75f) < 0.1f && fabsf(fov[2] - 70.f) < 0.1f, "fields of view %.2f %.2f %.2f", fov[0], fov[1], fov[2]);
		/* unzoomed on a switch */
		for (i = 0; i < 4; i++) tick(&inv, 0.f, 0, 0, 0, 0);
		tick(&inv, 0.f, 0, 1, 0, 0);
		CHECK(inv.zoom_level == -1, "still zoomed after a switch");
	}

	/* melee: busies the weapon for the animation, the blow at its key frame */
	{
		int blows = 0;

		fpw_inventory_init(&inv, ar, 2, 7u);
		aim_at(&inv, eye, (float[3]) { MAP_DEMO_CHIEF_X, MAP_DEMO_CHIEF_Y, eye[2] - 0.2f });
		for (i = 0; i < 60; i++) tick(&inv, 0.f, 0, 0, 0, 0);
		tick(&inv, 0.f, 0, 0, 1, 0);
		CHECK(count_events(FPW_EVENT_MELEE) == 1, "no melee");
		for (i = 0; i < 40; i++) { tick(&inv, 1.f, 0, 0, 0, 0); blows += count_events(FPW_EVENT_FIRE); }
		CHECK(blows <= 3, "fired %d rounds in the middle of a melee", blows);
	}
	printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
	return failures != 0;
}
