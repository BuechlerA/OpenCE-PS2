/* Host test of src/fp_controller.c against the exported map (src/map_assets.c): the original's numbers come out, the player never
   leaves the ground the map has, and cliffs stop it.

     cc -O1 -I src -o /tmp/fp_controller_test tools/fp_controller_test.c src/map_assets.c src/fp_weapon.c src/weapon_assets.c -lm && /tmp/fp_controller_test

   (the controller is included, not linked, so that the test can look at the pill's spheres)
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/fp_controller.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(fp_player *p, const fp_pad *pad, float seconds)
{
	float dt = 1.f / 60.f, t;

	for (t = 0; t < seconds; t += dt) fp_update(p, pad, dt);
}

/* how far the pill is into the front of a collision surface: the deepest of its spheres, against every triangle of the map (not the tree's candidates) */
static float penetration(const fp_player *p)
{
	float centres[3][3], deepest = 0.f;
	int spheres = pill_spheres(p->pos, p->crouch, centres), s, i;

	for (i = 0; i < MAP_COL_TRIS; i++)
	{
		const unsigned short *ix = &map_col_tri[i * 3];
		const float *a = &map_col_pos[ix[0] * 3], *b = &map_col_pos[ix[1] * 3], *c = &map_col_pos[ix[2] * 3];
		float e1[3], e2[3], nrm[3], length;

		if (fmaxf(a[0], fmaxf(b[0], c[0])) < p->pos[0] - 0.3f || fminf(a[0], fminf(b[0], c[0])) > p->pos[0] + 0.3f) continue;
		if (fmaxf(a[1], fmaxf(b[1], c[1])) < p->pos[1] - 0.3f || fminf(a[1], fminf(b[1], c[1])) > p->pos[1] + 0.3f) continue;
		sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, nrm);
		length = sqrtf(dot3(nrm, nrm));
		if (length < 1e-12f) continue;
		for (s = 0; s < spheres; s++)
		{
			float to[3], closest[3], d[3];

			sub3(centres[s], a, to);
			if (dot3(to, nrm) / length < 0.f) continue;           /* behind it: the surface does not touch from there */
			closest_on_triangle(centres[s], a, b, c, closest);
			sub3(centres[s], closest, d);
			length = sqrtf(dot3(d, d));
			if (MAP_RADIUS - length > deepest) deepest = MAP_RADIUS - length;
			length = sqrtf(dot3(nrm, nrm));
		}
	}
	return deepest;
}

static float wrap_angle(float a) { while (a > (float)M_PI) a -= 2 * (float)M_PI; while (a < -(float)M_PI) a += 2 * (float)M_PI; return a; }
static unsigned seed = 12345;
static float rand01(void) { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 8) & 0xffff) / 65535.f; }

int main(void)
{
	fp_player p;
	fp_pad idle = { 0, 0, 0, 0, 0 }, forward = { 0, 32767, 0, 0, 0 };
	float ground, eye[3], target[3], peak;
	int heading, i;

	/* standing: on the ground, the eye .62 above the feet */
	fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, (float)M_PI);
	run(&p, &idle, 1.f);
	fp_camera(&p, eye, target);
	CHECK(fp_ground(p.pos[0], p.pos[1], p.pos[2] + 1.f, &ground, NULL, NULL) && fabsf(ground - p.pos[2]) < 0.01f, "not on the ground");
	CHECK(fabsf(eye[2] - p.pos[2] - MAP_CAMERA_STANDING) < 1e-3f, "eye height %f", eye[2] - p.pos[2]);
	CHECK(fabsf(target[0] - eye[0] + 1.f) < 1e-3f, "not looking along -x: %f", target[0] - eye[0]);
	printf("spawn (%.2f, %.2f, %.3f)\n", p.pos[0], p.pos[1], p.pos[2]);

	/* standing still stays still, on flat ground and on slopes: a grid of places over the valley, each 3 s with no input */
	{
		float x, y, worst = 0.f, wx = 0, wy = 0, g, nz;
		int places = 0, slopes = 0;

		for (x = 10.f; x < 130.f; x += 3.7f)
			for (y = -185.f; y < -45.f; y += 3.9f)
			{
				float drift, settled[3];

				if (!fp_ground(x, y, 3.f, &g, &nz, NULL) || nz < 0.8f) continue;

				fp_init(&p, x, y, 0.f);
				run(&p, &idle, 1.f);                      /* a place the pill does not fit (against a wall) is let go of, or fallen from, first */
				settled[0] = p.pos[0]; settled[1] = p.pos[1]; settled[2] = p.pos[2];
				run(&p, &idle, 3.f);
				drift = sqrtf((p.pos[0] - settled[0]) * (p.pos[0] - settled[0]) + (p.pos[1] - settled[1]) * (p.pos[1] - settled[1]) + (p.pos[2] - settled[2]) * (p.pos[2] - settled[2]));
				places++; if (nz < 0.97f) slopes++;
				if (drift > worst) { worst = drift; wx = x; wy = y; }
			}
		printf("standing still 3 s (after 1 s to settle) at %d places (%d on slopes): the most drift %.5f units (at %.1f, %.1f)\n", places, slopes, worst, wx, wy);
		CHECK(worst < 1e-4f, "slides %.5f units standing still at (%.1f, %.1f)", worst, wx, wy);
	}

	/* the pill never goes into the map: 40 players, each 40 s of random sticks, jumps and crouches from random places */
	{
		float deepest = 0.f, lowest = 1e9f, start_depth_worst = 0.f;
		int deepest_run = -1, deepest_tick = -1;
		int run_index, tick_index, rolls = 0, skipped = 0;

		for (run_index = 0; run_index < 40; run_index++)
		{
			float x, y, g, nz;
			fp_pad pad = { 0, 0, 0, 0, 0 };

			do   /* (a place against a wall is pushed out of it; one in a crevice that the push cannot clear is not a place to start) */
			{
				do { x = 10.f + rand01() * 120.f; y = -185.f + rand01() * 140.f; } while (!fp_ground(x, y, 3.f, &g, &nz, NULL));
				fp_init(&p, x, y, rand01() * 6.28f);
				if (penetration(&p) > 0.005f) skipped++;
			} while (penetration(&p) > 0.005f);
			if (penetration(&p) > start_depth_worst) start_depth_worst = penetration(&p);
			for (tick_index = 0; tick_index < 40 * 60; tick_index++)
			{
				float depth;

				if (tick_index % 45 == 0)
				{
					pad.left_x = (short)((rand01() - 0.5f) * 65000); pad.left_y = (short)((rand01() - 0.2f) * 50000);
					pad.right_x = (short)((rand01() - 0.5f) * 40000); pad.right_y = (short)((rand01() - 0.5f) * 8000);
					pad.buttons = (rand01() < 0.3f ? FP_JUMP : 0) | (rand01() < 0.15f ? FP_CROUCH : 0);
					rolls++;
				}
				fp_update(&p, &pad, 1.f / 60.f);
				depth = penetration(&p);
				if (depth > deepest) { deepest = depth; deepest_run = run_index; deepest_tick = tick_index; }
				if (p.pos[2] < lowest) lowest = p.pos[2];
			}
		}
		printf("40 random players x 40 s (%d inputs): deepest into a surface %.4f units (run %d, tick %d; the worst place to start was %.4f deep, %d places in crevices skipped), lowest z %.2f\n", rolls, deepest, deepest_run, deepest_tick, start_depth_worst, skipped, lowest);
		CHECK(deepest < 0.02f, "the pill went %.4f units into a surface", deepest);
		CHECK(lowest > -5.f, "fell through the map to z %.2f", lowest);
	}

	/* the BSP's tree and the objects' grid give every triangle that is ground: the highest up-facing triangle under 4,000 random points,
	   by the tree and by a look at every triangle */
	{
		int k, wrong = 0, found = 0;

		for (k = 0; k < 4000; k++)
		{
			float x = -5.f + rand01() * 170.f, y = -215.f + rand01() * 180.f, zmax = -0.5f + rand01() * 12.f, z = 0.f, brute = -1e9f;
			int i, have = 0, tree = fp_ground(x, y, zmax, &z, NULL, NULL);

			for (i = 0; i < MAP_COL_TRIS; i++)
			{
				const unsigned short *ix = &map_col_tri[i * 3];
				const float *a = &map_col_pos[ix[0] * 3], *b = &map_col_pos[ix[1] * 3], *c = &map_col_pos[ix[2] * 3];
				float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]), w0, w1, w2, zz, e1[3], e2[3], nrm[3];

				if (fabsf(d) < 1e-12f) continue;
				w0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d; w1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d; w2 = 1.f - w0 - w1;
				if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
				zz = w0 * a[2] + w1 * b[2] + w2 * c[2];
				sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, nrm);
				if (zz > zmax || zz < zmax - 100.f || nrm[2] <= 1e-4f * sqrtf(dot3(nrm, nrm))) continue;
				if (!have || zz > brute) { brute = zz; have = 1; }
			}
			if (tree) found++;
			/* the tree must find the ground within 8 units under the point, and anything it finds must be the highest there is (it may also find
			   ground deeper than 8, from a leaf it reached) */
			if ((have && brute >= zmax - 8.f && !tree) || (tree && (!have || fabsf(brute - z) > 1e-3f))) { if (wrong++ < 3) printf("  tree %d (%.4f) brute %d (%.4f) at %.2f %.2f under %.2f\n", tree, z, have, brute, x, y, zmax); }
		}
		printf("ground by the tree against every triangle: %d points, %d with ground, %d differ\n", k, found, wrong);
		CHECK(wrong == 0, "the tree missed ground at %d points", wrong);
	}

	/* the Chief is solid: walking straight at him from 2.4 units stops at the sum of two radii */
	{
		fp_pad forward = { 0, 32767, 0, 0, 0 };
		float closest = 1e9f;
		int k;

		fp_init(&p, MAP_DEMO_CHIEF_X + 2.4f, MAP_DEMO_CHIEF_Y, (float)M_PI);
		for (k = 0; k < 60 * 12; k++)
		{
			fp_update(&p, &forward, 1.f / 60.f);
			closest = fminf(closest, hypotf(p.pos[0] - MAP_DEMO_CHIEF_X, p.pos[1] - MAP_DEMO_CHIEF_Y));
		}
		printf("walking at the Chief for 12 s: never nearer than %.3f units (his capsule's radius and the player's make 0.4)\n", closest);
		CHECK(closest > 0.36f && closest < 0.6f, "stopped %.3f from the Chief", closest);
	}

	/* scenery is solid: for the steep surfaces of scenery within the pill's height, the player starts a unit in front of one and walks into it: it does not get through */
	{
		int tried = 0, through = 0, tri, k;
		fp_pad forward = { 0, 32767, 0, 0, 0 };

		for (tri = MAP_COL_STRUCTURE_TRIS; tri < MAP_COL_TRIS - 80 && tried < 60; tri += 5)
		{
			const unsigned short *ix = &map_col_tri[tri * 3];
			const float *a = &map_col_pos[ix[0] * 3], *b = &map_col_pos[ix[1] * 3], *c = &map_col_pos[ix[2] * 3];
			float e1[3], e2[3], nrm[3], length, centre[3], g, start[2], depth_in;

			sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, nrm);
			length = sqrtf(dot3(nrm, nrm));
			if (length < 1e-9f) continue;
			nrm[0] /= length; nrm[1] /= length; nrm[2] /= length;
			if (fabsf(nrm[2]) > 0.3f) continue;                                /* a wall of it, not a top */
			centre[0] = (a[0] + b[0] + c[0]) / 3.f; centre[1] = (a[1] + b[1] + c[1]) / 3.f; centre[2] = (a[2] + b[2] + c[2]) / 3.f;
			length = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1]);
			start[0] = centre[0] + nrm[0] / length * 1.2f; start[1] = centre[1] + nrm[1] / length * 1.2f;
			if (!fp_ground(start[0], start[1], centre[2] + 0.3f, &g, NULL, NULL) || centre[2] - g < 0.1f || centre[2] - g > 0.6f) continue;   /* the wall's middle is within the pill's height */
			fp_init(&p, start[0], start[1], atan2f(-nrm[1], -nrm[0]));
			if (penetration(&p) > 0.01f) continue;
			tried++;
			for (k = 0; k < 60 * 3 / 2; k++) fp_update(&p, &forward, 1.f / 60.f);      /* 1.5 s: it reaches the wall in half a second and presses on, not long enough to go round */
			depth_in = (p.pos[0] - centre[0]) * nrm[0] + (p.pos[1] - centre[1]) * nrm[1] + (p.pos[2] - centre[2]) * nrm[2];
			{
				float at[3] = { p.pos[0], p.pos[1], centre[2] }, near_point[3], gap[3];

				closest_on_triangle(at, a, b, c, near_point); sub3(at, near_point, gap);
				/* behind its plane and close to it: through. (Behind it and far from it: round the object, which a small rock allows.) */
				if (depth_in >= 0.f || sqrtf(dot3(gap, gap)) > 0.5f) depth_in = 1.f;
			}
			if (depth_in < 0.f) { through++; printf("  got through scenery triangle %d (%.1f, %.1f, %.2f), %.3f behind its plane\n", tri, centre[0], centre[1], centre[2], depth_in); }
		}
		printf("walking into %d walls of scenery for 1.5 s each: %d got through\n", tried, through);
		CHECK(tried >= 10 && through == 0, "tried %d, through %d", tried, through);
	}

	/* standing up under a ceiling is refused: a place where the pill fits crouched but not standing, found over the map */
	{
		float x, y, z, nz, push[3], found_x = 0, found_y = 0, found_z = 0;
		int found = 0, k;
		fp_pad crouch = { 0, 0, 0, 0, FP_CROUCH }, idle_pad = { 0, 0, 0, 0, 0 };

		for (x = -5.f; x < 165.f && !found; x += 0.4f)
			for (y = -215.f; y < -35.f && !found; y += 0.4f)
			{
				float feet[3];

				if (!fp_ground(x, y, 60.f, &z, &nz, NULL) || nz < 0.95f) continue;
				feet[0] = x; feet[1] = y; feet[2] = z + 0.002f;
				if (deepest_overlap(feet, 0.f, push) > 0.01f && deepest_overlap(feet, 1.f, push) < 0.001f) { found = 1; found_x = x; found_y = y; found_z = z; }
			}
		if (!found) printf("no place in the map where the pill fits crouched but not standing (the refusal is not exercised)\n");
		else
		{
			fp_init(&p, found_x, found_y, 0.f);
			p.pos[0] = found_x; p.pos[1] = found_y; p.pos[2] = found_z + 0.002f;        /* where the scan found it (the start pushed the standing pill out) */
			p.crouch = 1.f;
			for (k = 0; k < 60; k++) fp_update(&p, &idle_pad, 1.f / 60.f);
			printf("under a ceiling at (%.1f, %.1f): crouched 1 s and let go, the crouch is %.2f\n", found_x, found_y, p.crouch);
			CHECK(p.crouch > 0.3f && penetration(&p) < 0.005f, "stood up into a ceiling (crouch %.2f, %.4f into it)", p.crouch, penetration(&p));
			(void)crouch;
		}
	}

	/* the weapons through the pad: the trigger fires the assault rifle, a reload takes the first-person animation, Y switches to the pistol,
	   the click of the right stick zooms it (2x: the lens narrows over 0.18 s, the look slows to half) */
	{
		fp_pad trigger = { 0, 0, 0, 0, 0, 255 }, none_pad = { 0, 0, 0, 0, 0, 0 }, reload = { 0, 0, 0, 0, FP_RELOAD, 0 }, swap = { 0, 0, 0, 0, FP_SWITCH_WEAPON, 0 }, zoom = { 0, 0, 0, 0, FP_ZOOM, 0 };
		fp_pad turn = { 0, 0, 20000, 0, 0, 0 };
		int loaded_before, k, reloads = 0, fires = 0;
		float fov_samples[4], yaw0, unzoomed_turn, zoomed_turn;

		fp_init(&p, MAP_DEMO_START_X, MAP_DEMO_START_Y, 0.f);
		run(&p, &none_pad, 1.f);
		loaded_before = p.weapons.slots[0].magazines[0].rounds_loaded;
		for (k = 0; k < 60; k++) { fp_update(&p, &trigger, 1.f / 60.f); fires += 0; }
		printf("a second of the right trigger: %d rounds fired (15)\n", loaded_before - p.weapons.slots[0].magazines[0].rounds_loaded);
		CHECK(loaded_before - p.weapons.slots[0].magazines[0].rounds_loaded >= 14 && loaded_before - p.weapons.slots[0].magazines[0].rounds_loaded <= 16, "fired %d", loaded_before - p.weapons.slots[0].magazines[0].rounds_loaded);
		fp_update(&p, &none_pad, 1.f / 60.f);
		fp_update(&p, &reload, 1.f / 60.f);                  /* one frame of the button: the weapon must see it once */
		for (k = 0; k < 240; k++) { fp_update(&p, &none_pad, 1.f / 60.f); for (int e = 0; e < p.events.count; e++) if (p.events.list[e].type == FPW_EVENT_RELOAD_FINISH) reloads++; }
		CHECK(p.weapons.slots[0].magazines[0].rounds_loaded == 60 && reloads == 1, "reloaded to %d, %d times", p.weapons.slots[0].magazines[0].rounds_loaded, reloads);

		fp_update(&p, &swap, 1.f / 60.f);
		for (k = 0; k < 180; k++) fp_update(&p, &none_pad, 1.f / 60.f);
		CHECK(p.weapons.current == 1, "did not switch");
		yaw0 = p.yaw; run(&p, &turn, 0.2f); unzoomed_turn = fabsf(wrap_angle(p.yaw - yaw0));
		fp_update(&p, &zoom, 1.f / 60.f);
		for (k = 0; k < 4; k++) { run(&p, &none_pad, 0.06f); fov_samples[k] = p.field_of_view * 180.f / (float)M_PI; }
		printf("the pistol zoomed: the lens %.1f %.1f %.1f %.1f degrees at 60 ms steps (70 to 35 in 0.18 s)\n", fov_samples[0], fov_samples[1], fov_samples[2], fov_samples[3]);
		CHECK(fov_samples[0] < 70.f && fov_samples[0] > 35.f && fabsf(fov_samples[2] - 35.f) < 0.5f && fabsf(fov_samples[3] - 35.f) < 0.5f, "lens %.1f %.1f %.1f %.1f", fov_samples[0], fov_samples[1], fov_samples[2], fov_samples[3]);
		yaw0 = p.yaw; run(&p, &turn, 0.2f); zoomed_turn = fabsf(wrap_angle(p.yaw - yaw0));
		printf("a turn of 0.2 s: %.2f degrees unzoomed, %.2f zoomed\n", unzoomed_turn * 180.f / (float)M_PI, zoomed_turn * 180.f / (float)M_PI);
		CHECK(fabsf(zoomed_turn / unzoomed_turn - 0.5f) < 0.06f, "the zoomed turn is %.2f of the unzoomed", zoomed_turn / unzoomed_turn);
		fp_update(&p, &zoom, 1.f / 60.f); fp_update(&p, &none_pad, 1.f / 60.f); fp_update(&p, &zoom, 1.f / 60.f);
		CHECK(p.weapons.zoom_level == -1 || p.weapons.zoom_level == 0, "zoom level %d", p.weapons.zoom_level);
	}

	/* autolevel: off, the pitch stays; on (Back), walking forward levels it out */
	{
		fp_pad walk = { 0, 32767, 0, 0, 0, 0 }, toggle = { 0, 0, 0, 0, FP_AUTOLEVEL, 0 }, none_pad = { 0, 0, 0, 0, 0, 0 };
		float before;

		fp_init(&p, MAP_DEMO_START_X, MAP_DEMO_START_Y, (float)M_PI);
		p.pitch = 0.5f; before = p.pitch;
		run(&p, &walk, 3.f);
		CHECK(fabsf(p.pitch - before) < 1e-4f, "the pitch moved with autolevel off: %.3f", p.pitch);
		fp_init(&p, MAP_DEMO_START_X, MAP_DEMO_START_Y, (float)M_PI);
		p.pitch = 0.5f;
		fp_update(&p, &toggle, 1.f / 60.f); fp_update(&p, &none_pad, 1.f / 60.f);
		CHECK(p.autolevel_enabled, "autolevel did not turn on");
		run(&p, &walk, 3.f);
		printf("autolevel: walking forward 3 s with the pitch at 28.6 degrees: %.1f degrees\n", p.pitch * 180.f / (float)M_PI);
		CHECK(p.pitch < 0.35f && p.pitch >= 0.f, "pitch %.3f after autolevel", p.pitch);
	}

	/* the dead zone: a stick at 9000 does nothing, at full is 1.0 */
	{
		fp_pad small = { 9000, 9000, 9000, -9000, 0 }; fp_input in;
		fp_shape_input(&small, &in);
		CHECK(in.forward_movement == 0 && in.strafe == 0 && in.yaw == 0 && in.pitch == 0, "dead zone: %f %f %f %f", in.forward_movement, in.strafe, in.yaw, in.pitch);
		small.left_x = 32767; small.left_y = 32767;
		fp_shape_input(&small, &in);
		CHECK(in.forward_movement > 0.99f && in.strafe < -0.99f, "square stick: %f %f", in.forward_movement, in.strafe);
	}

	/* the look: 120 degrees a second at full stick, three times that after a second pegged */
	{
		fp_pad turn = { 0, 0, 32767, 0, 0 };
		float before;

		fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, 0.f);
		before = p.yaw;
		run(&p, &turn, 0.5f);
		printf("0.5 s of full right stick: %.1f degrees (yaw %.3f)\n", fmodf(before - p.yaw + 2 * (float)M_PI, 2 * (float)M_PI) * 180 / (float)M_PI, p.yaw);
		CHECK(fabsf(fmodf(before - p.yaw + 2 * (float)M_PI, 2 * (float)M_PI) * 180 / (float)M_PI - 90.f) < 5.f, "the first half second: 120 degrees a second, accelerating 1 to 2 times");
		run(&p, &turn, 3.f);
		before = p.yaw; run(&p, &turn, 0.25f);
		printf("a quarter second at full acceleration: %.1f degrees\n", fmodf(before - p.yaw + 2 * (float)M_PI, 2 * (float)M_PI) * 180 / (float)M_PI);
		CHECK(fabsf(fmodf(before - p.yaw + 2 * (float)M_PI, 2 * (float)M_PI) * 180 / (float)M_PI - 90.f) < 10.f, "accelerated turn is 360 degrees a second");
	}

	/* pitch stops at 85.5 degrees */
	{
		fp_pad up = { 0, 0, 0, 32767, 0 };
		fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, 0.f);
		run(&p, &up, 3.f);
		CHECK(fabsf(p.pitch - 85.5f * (float)M_PI / 180.f) < 1e-4f, "pitch %f", p.pitch);
	}

	/* running on flat ground: 2.25 units a second */
	{
		float x0, y0, speed;
		fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, (float)M_PI);
		run(&p, &forward, 0.6f);
		x0 = p.pos[0]; y0 = p.pos[1];
		run(&p, &forward, 1.f);
		speed = hypotf(p.pos[0] - x0, p.pos[1] - y0);
		printf("forward speed %.3f units/s (the globals say %.2f; the ground slopes)\n", speed, MAP_RUN_FORWARD);
		CHECK(speed > 0.8f * MAP_RUN_FORWARD && speed < 1.3f * MAP_RUN_FORWARD, "speed %f", speed);
	}

	/* a jump on flat ground: v^2 / 2g = 0.687 units */
	{
		fp_pad jump = { 0, 0, 0, 0, FP_JUMP };
		fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, 0.f);
		run(&p, &idle, 0.5f);
		ground = p.pos[2]; peak = 0;
		for (i = 0; i < 90; i++) { fp_update(&p, i < 2 ? &jump : &idle, 1.f / 60.f); if (p.pos[2] - ground > peak) peak = p.pos[2] - ground; }
		printf("jump peak %.3f units, airborne after 1.5 s: %d\n", peak, p.airborne);
		CHECK(peak > 0.5f && peak < 0.9f, "jump peak %f", peak);
		CHECK(!p.airborne, "did not land");
	}

	/* crouch: the camera drops to .35 in .2 s */
	{
		fp_pad crouch = { 0, 0, 0, 0, FP_CROUCH };
		fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, 0.f);
		run(&p, &crouch, 0.5f);
		fp_camera(&p, eye, target);
		CHECK(fabsf(eye[2] - p.pos[2] - MAP_CAMERA_CROUCHING) < 1e-3f, "crouched eye %f", eye[2] - p.pos[2]);
	}

	/* walk every way for a minute: always finite, on the ground the map has, never in the sky */
	{
		float worst_below = 0, highest_above = 0, farthest = 0, min_z = 1e9f, max_z = -1e9f;
		int stuck = 0;

		for (heading = 0; heading < 36; heading++)
		{
			fp_init(&p, MAP_CHIEF_X + 2.2f, MAP_CHIEF_Y, heading * (float)M_PI / 18.f);
			for (i = 0; i < 60 * 60; i++)
			{
				float g, d;

				fp_update(&p, &forward, 1.f / 60.f);
				CHECK(isfinite(p.pos[0]) && isfinite(p.pos[1]) && isfinite(p.pos[2]), "not finite");
				if (!p.airborne && fp_ground(p.pos[0], p.pos[1], p.pos[2] + 1.f, &g, NULL, NULL))
				{
					if (p.pos[2] - g < worst_below) worst_below = p.pos[2] - g;
					if (p.pos[2] - g > highest_above) highest_above = p.pos[2] - g;
				}
				d = hypotf(p.pos[0] - MAP_CHIEF_X, p.pos[1] - MAP_CHIEF_Y);
				if (d > farthest) farthest = d;
				if (p.pos[2] < min_z) min_z = p.pos[2];
				if (p.pos[2] > max_z) max_z = p.pos[2];
			}
			if (hypotf(p.vel[0], p.vel[1]) < 1e-4f) stuck++;
		}
		printf("36 headings x 60 s: farthest %.1f units from the start, z %.2f..%.2f, ground offset %.4f..%.4f, %d ended against something\n", farthest, min_z, max_z, worst_below, highest_above, stuck);
		CHECK(min_z > -50.f, "fell out of the map (z %f)", min_z);
		/* a round-bottomed pill on a slope or an edge is a little above the ground under its feet: r / cos(slope) - r at most (0.083 at 45 degrees), more across a crease */
		CHECK(worst_below > -0.02f && highest_above < 0.25f, "left the ground: %.4f..%.4f", worst_below, highest_above);
	}
	printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
	return failures != 0;
}
