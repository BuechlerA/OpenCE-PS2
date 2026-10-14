/* Host test of src/fp_controller.c against the exported map (src/map_assets.c): the original's numbers come out, the player never
   leaves the ground the map has, and cliffs stop it.

     cc -O1 -I src -o /tmp/fp_controller_test tools/fp_controller_test.c src/fp_controller.c src/map_assets.c -lm && /tmp/fp_controller_test
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/fp_controller.h"
#include "../src/map_assets.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(fp_player *p, const fp_pad *pad, float seconds)
{
	float dt = 1.f / 60.f, t;

	for (t = 0; t < seconds; t += dt) fp_update(p, pad, dt);
}

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
	CHECK(fp_ground(p.pos[0], p.pos[1], p.pos[2] + 1.f, &ground, NULL, NULL) && fabsf(ground - p.pos[2]) < 1e-4f, "not on the ground");
	CHECK(fabsf(eye[2] - p.pos[2] - MAP_CAMERA_STANDING) < 1e-4f, "eye height %f", eye[2] - p.pos[2]);
	CHECK(fabsf(target[0] - eye[0] + 1.f) < 1e-3f, "not looking along -x: %f", target[0] - eye[0]);
	printf("spawn (%.2f, %.2f, %.3f)\n", p.pos[0], p.pos[1], p.pos[2]);

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
		CHECK(worst_below > -1e-3f && highest_above < 0.13f, "left the ground");
	}
	printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
	return failures != 0;
}
