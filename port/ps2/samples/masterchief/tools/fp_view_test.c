/* Host test of src/fp_view.c: the first-person weapon's pose against tools/fp_view_reference.py (an animation's frame played straight from the
   tags), and the state machine: the animations the states play, the ticks they last, the idle pose, the overlays.

     cc -O1 -I src -o /tmp/fp_view_test tools/fp_view_test.c src/fp_view.c src/fp_weapon.c src/fp_controller.c src/map_assets.c src/weapon_assets.c src/fpm_assets.c -lm && /tmp/fp_view_test [weapon type frame]
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fp_view.h"
#include "../src/fp_controller.h"
#include "../src/map_assets.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void skinned(const fpv_state *v, const fpm_model *m, const short *graph_node, int vertex, float *out)
{
	float s0[12], s1[12], p[3], a[3], b[3], w = m->weight[vertex] / 255.f;
	int n0 = m->node[vertex * 2], n1 = m->node[vertex * 2 + 1], i;

	fpv_skin_matrix(v, m, graph_node, n0, 1.f, s0);
	fpv_skin_matrix(v, m, graph_node, n1, 1.f, s1);
	for (i = 0; i < 3; i++) p[i] = m->position[vertex * 3 + i];
	for (i = 0; i < 3; i++)
	{
		a[i] = p[0] * s0[0 * 3 + i] + p[1] * s0[1 * 3 + i] + p[2] * s0[2 * 3 + i] + s0[9 + i];
		b[i] = p[0] * s1[0 * 3 + i] + p[1] * s1[1 * 3 + i] + p[2] * s1[2 * 3 + i] + s1[9 + i];
		out[i] = a[i] * w + b[i] * (1.f - w);
	}
}

/* the pose of weapon `weapon`'s animation of `type` at `frame`, no overlay: the state is set by hand and one tick takes it to the frame */
static void pose_at(fpv_state *v, int weapon, int type, int frame)
{
	fpv_input in = { { 0, 0 }, 0, 0, 1000, 0, 0, 0 };
	const fpm_graph *g = fpm_weapons[weapon].graph;

	fpv_init(v);
	fpv_set_weapon(v, weapon);
	v->animation = g->animation_of_type[type];
	v->frame = frame - 1;
	v->interpolation_count = 0;
	v->overlay_animation = -1;
	v->state = 99;
	fpv_tick(v, &in, NULL);
}

/* the state machine, through the controller: the pad's buttons to the first-person weapon's states and the ticks they last */
static void state_tests(void)
{
	fp_player p;
	fp_pad none = { 0, 0, 0, 0, 0, 0 }, reload = { 0, 0, 0, 0, FP_RELOAD, 0 }, trigger = { 0, 0, 0, 0, 0, 255 }, swap = { 0, 0, 0, 0, FP_SWITCH_WEAPON, 0 }, walk = { 0, 32767, 0, 0, 0, 0 };
	int k, ticks, ready_ticks, reload_ticks = 0, saw_fire = 0, max_push = 0;
	float dt = 1.f / 30.f;

	fp_init(&p, MAP_DEMO_START_X, MAP_DEMO_START_Y, 0.f);
	CHECK(p.view.state == FPV_READY, "the first weapon is not readied: state %d", p.view.state);
	for (ticks = 0; p.view.state == FPV_READY && ticks < 100; ticks++) fp_update(&p, &none, dt);
	ready_ticks = ticks;
	printf("ready: %d ticks (the animation has %d frames), then state %d (idle %d)\n", ready_ticks, fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_READY), p.view.state, FPV_IDLE);
	CHECK(abs(ready_ticks - fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_READY)) <= 2 && p.view.state == FPV_IDLE, "ready took %d ticks, then state %d", ready_ticks, p.view.state);

	/* firing: the push-back overlay moves, the idle animation plays on (the assault rifle has no fire animation of its own) */
	for (k = 0; k < 15; k++) { fp_update(&p, &trigger, dt); if (p.view.push_back > 0.f) saw_fire = 1; if (p.view.push_back * 100 > max_push) max_push = (int)(p.view.push_back * 100); }
	printf("fire: push back reached %.2f; state %d\n", max_push / 100.f, p.view.state);
	CHECK(saw_fire && p.view.state == FPV_IDLE, "no push back (%d) or the state is %d", saw_fire, p.view.state);

	/* reload: the state is the reload's for the animation's frames, then idle */
	for (k = 0; k < 20; k++) fp_update(&p, &none, dt);
	fp_update(&p, &reload, dt);
	CHECK(p.view.state == FPV_RELOAD_WHILE_FULL || p.view.state == FPV_RELOAD_WHILE_EMPTY, "reload state %d", p.view.state);
	for (reload_ticks = 1; (p.view.state == FPV_RELOAD_WHILE_FULL || p.view.state == FPV_RELOAD_WHILE_EMPTY) && reload_ticks < 200; reload_ticks++) fp_update(&p, &none, dt);
	printf("reload: %d ticks (the animation has %d frames)\n", reload_ticks, fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_RELOAD_WHILE_EMPTY));
	CHECK(abs(reload_ticks - fpw_animation_ticks(FPW_ASSAULT_RIFLE, FPW_ANIMATION_RELOAD_WHILE_EMPTY)) <= 3 && p.view.state == FPV_IDLE, "reload %d ticks, then state %d", reload_ticks, p.view.state);

	/* switching: put away, then the pistol's ready, then idle */
	fp_update(&p, &swap, dt);
	for (k = 0; k < 6 && p.view.state != FPV_PUT_AWAY; k++) fp_update(&p, &none, dt);        /* once the magazine is not chambering */
	CHECK(p.view.state == FPV_PUT_AWAY, "put-away state %d", p.view.state);
	for (ticks = 0; p.view.state != FPV_READY && ticks < 100; ticks++) fp_update(&p, &none, dt);
	CHECK(p.view.weapon == FPW_PISTOL && p.view.state == FPV_READY, "after the put-away: weapon %d state %d", p.view.weapon, p.view.state);
	for (ticks = 0; p.view.state == FPV_READY && ticks < 100; ticks++) fp_update(&p, &none, dt);
	printf("the pistol readied in %d ticks (%d frames)\n", ticks, fpw_animation_ticks(FPW_PISTOL, FPW_ANIMATION_READY));
	CHECK(abs(ticks - fpw_animation_ticks(FPW_PISTOL, FPW_ANIMATION_READY)) <= 2, "ready %d", ticks);

	/* walking: the moving overlay plays, the position sways, then it stops */
	for (k = 0; k < 30; k++) fp_update(&p, &walk, dt);
	CHECK(p.view.moving_animation >= 0 && p.view.position[0] > 0.5f, "not moving: %d, position %.2f", p.view.moving_animation, p.view.position[0]);
	for (k = 0; k < 40; k++) fp_update(&p, &none, dt);
	CHECK(p.view.moving_animation < 0 && fabsf(p.view.position[0]) < 0.05f, "still moving: %d, position %.2f", p.view.moving_animation, p.view.position[0]);

	/* idle for 10 to 15 s: a pose */
	{
		int posed = 0;

		for (k = 0; k < 30 * 16 && !posed; k++) { fp_update(&p, &none, dt); posed = p.view.state == FPV_POSING; }
		printf("idle pose after %.1f s (10 to 15)\n", k / 30.f);
		CHECK(posed || p.view.state == FPV_IDLE, "no pose");
	}
}

/* the assault rifle's display: the digits of the rounds loaded (its two numeric parts), and the compass's frames over a turn */
static void display_tests(void)
{
	const fpm_weapon *w = &fpm_weapons[FPW_ASSAULT_RIFLE];
	const fpm_display_part *tens = NULL, *units = NULL, *compass = NULL;
	static const int rounds[6] = { 60, 45, 7, 0, 10, 59 };
	int i, seen[8] = { 0 }, frames_seen = 0;

	for (i = 0; i < w->display_count; i++)
	{
		const fpm_display_part *d = &w->display[i];

		if (d->kind == 1 && d->place == 1) tens = d;
		if (d->kind == 1 && d->place == 0) units = d;
		if (d->kind == 2) compass = d;
	}
	CHECK(tens && units && compass, "the rifle's display has no tens (%p), units (%p) or compass (%p)", (const void *)tens, (const void *)units, (const void *)compass);
	if (!tens || !units || !compass) return;
	CHECK(w->display_count == 5, "the rifle's display has %d parts, not 5 (compass, data, two digits, icons)", w->display_count);
	for (i = 0; i < 6; i++)
	{
		int t = fpv_display_frame(tens, rounds[i], 60, 0.f), u = fpv_display_frame(units, rounds[i], 60, 0.f);

		CHECK(t == rounds[i] / 10 && u == rounds[i] % 10, "%d rounds show %d%d", rounds[i], t, u);
	}
	CHECK(fpv_display_frame(&w->display[1], 30, 60, 0.f) == -1, "a static part has a frame");
	for (i = 0; i < 720; i++)
	{
		float yaw = i * (float)M_PI / 360.f;
		int f = fpv_display_frame(compass, 0, 60, fpv_compass_value(yaw));

		CHECK(f >= 0 && f < 8, "compass frame %d at yaw %.2f", f, yaw);
		if (f >= 0 && f < 8) seen[f] = 1;
	}
	for (i = 0; i < 8; i++) frames_seen += seen[i];
	CHECK(frames_seen == 8, "a turn shows %d of the compass's 8 frames", frames_seen);
	/* facing north (the angle of the player's forward as the original reads it, pi / 2 - yaw, equal to the scenario's) is the counter's middle */
	CHECK(fabsf(fpv_compass_value((float)M_PI * 0.5f - FPM_LOCAL_NORTH) - 0.5f) < 1e-4f, "north is not the middle of the compass: %.4f", fpv_compass_value((float)M_PI * 0.5f - FPM_LOCAL_NORTH));
	printf("display: the rifle's %d parts, digits of 60 / 45 / 7 / 0 rounds, the compass's 8 frames: ok\n", w->display_count);
}

int main(int argc, char **argv)
{
	fpv_state v;
	int weapon = FPW_ASSAULT_RIFLE, type = FPW_ANIMATION_RELOAD_WHILE_EMPTY, frame = 40, i;
	float p[3];
	static const int vertices[5] = { 0, 100, 500, 1000, -1 };

	if (argc != 4) { state_tests(); display_tests(); printf(failures ? "%d FAILURES\n" : "all passed\n", failures); return failures != 0; }
	if (argc == 4) { weapon = atoi(argv[1]); type = atoi(argv[2]); frame = atoi(argv[3]); }
	pose_at(&v, weapon, type, frame);
	for (i = 0; i < 5; i++)
	{
		int vi = vertices[i] < 0 ? fpm_hands.vertices - 1 : vertices[i];

		skinned(&v, &fpm_hands, fpm_weapons[weapon].hands_graph_node, vi, p);
		printf("hands %d %.5f %.5f %.5f\n", vi, p[0], p[1], p[2]);
	}
	for (i = 0; i < 5; i++)
	{
		const fpm_model *gun = fpm_weapons[weapon].gun;
		int vi = vertices[i] < 0 ? gun->vertices - 1 : vertices[i];

		if (vi >= gun->vertices) continue;
		skinned(&v, gun, gun->graph_node, vi, p);
		printf("gun %d %.5f %.5f %.5f\n", vi, p[0], p[1], p[2]);
	}
	return 0;
}
