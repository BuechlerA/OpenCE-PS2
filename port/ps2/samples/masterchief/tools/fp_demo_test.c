/* Host test of src/fp_demo.c: the demo plans a walk from the front of the fortress to the Chief in the middle of the valley, and
   the controller, given only the pad the demo makes, gets there, turning and jumping on the way, again and again.

     cc -O1 -I src -o /tmp/fp_demo_test tools/fp_demo_test.c src/fp_demo.c src/fp_controller.c src/map_assets.c -lm && /tmp/fp_demo_test
*/
#include <math.h>
#include <stdio.h>

#include "../src/fp_demo.h"
#include "../src/map_assets.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float wrap(float a) { while (a > (float)M_PI) a -= 2 * (float)M_PI; while (a < -(float)M_PI) a += 2 * (float)M_PI; return a; }

int main(void)
{
	static fp_demo demo;
	static const float via[][2] = { MAP_DEMO_VIA_POINTS };
	fp_player p;
	fp_pad none = { 0, 0, 0, 0, 0 }, pad;
	float dt = 1.f / 60.f, t, goal[2], dir[2], len, turned = 0.f, last_yaw, last_progress = 0.f, run_start = 0.f, longest = 0.f, closest = 1e9f, worst_error = 0.f;
	int i, jumps = 0, was_airborne = 0, restarts = 0, arrivals = 0, was_arrived = 0;

	dir[0] = MAP_DEMO_START_X - MAP_DEMO_CHIEF_X; dir[1] = MAP_DEMO_START_Y - MAP_DEMO_CHIEF_Y;
	len = hypotf(dir[0], dir[1]);
	goal[0] = MAP_DEMO_CHIEF_X + dir[0] / len * 2.4f; goal[1] = MAP_DEMO_CHIEF_Y + dir[1] / len * 2.4f;
	CHECK(fp_demo_init(&demo, &p, MAP_DEMO_START_X, MAP_DEMO_START_Y, goal[0], goal[1], MAP_DEMO_CHIEF_X, MAP_DEMO_CHIEF_Y, via, MAP_DEMO_VIA_COUNT), "no path");
	printf("path: %d points, %.1f units, %d jumps\n", demo.count, demo.length, demo.jump_count);
	for (i = 0; i < demo.count; i++) printf("  (%.1f, %.1f) at %.1f\n", demo.point[i][0], demo.point[i][1], demo.arc[i]);

	last_yaw = p.yaw;
	for (t = 0; t < 100.f; t += dt)
	{
		fp_demo_pad(&demo, &p, &none, &pad, dt);
		if (demo.progress < last_progress - 1.f)       /* started over */
		{
			restarts++;
			if (!was_arrived) { CHECK(0, "restarted at %.1f s without arriving (progress %.1f of %.1f, at %.1f, %.1f)", t, last_progress, demo.length, p.pos[0], p.pos[1]); }
			if (t - run_start > longest) longest = t - run_start;
			run_start = t; last_yaw = p.yaw;
		}
		last_progress = demo.progress;
		fp_update(&p, &pad, dt);
		turned += fabsf(wrap(p.yaw - last_yaw)); last_yaw = p.yaw;
		if (p.airborne && !was_airborne) jumps++;
		was_airborne = p.airborne;
		if (demo.arrived > 0.f && !was_arrived) { arrivals++; closest = fminf(closest, hypotf(p.pos[0] - goal[0], p.pos[1] - goal[1])); }
		if (demo.arrived > 0.f && demo.arrived > 3.f) worst_error = fmaxf(worst_error, fabsf(wrap(atan2f(MAP_DEMO_CHIEF_Y - p.pos[1], MAP_DEMO_CHIEF_X - p.pos[0]) - p.yaw)));
		was_arrived = demo.arrived > 0.f;
	}
	printf("100 s: %d arrivals, %d restarts, a walk takes up to %.1f s, %d jumps, %.0f degrees turned, ends %.2f units from the goal, looking within %.0f degrees of the Chief\n",
	       arrivals, restarts, longest, jumps, turned * 180 / (float)M_PI, closest, worst_error * 180 / (float)M_PI);
	CHECK(arrivals >= 3, "arrived %d times", arrivals);
	CHECK(jumps >= 2 * arrivals, "jumped %d times in %d walks", jumps, arrivals);
	CHECK(closest < 0.8f, "arrived %.2f from the goal", closest);
	CHECK(worst_error < 0.25f, "not looking at the Chief at the end: %.2f rad", worst_error);

	/* a person's hand on the pad takes over, and Start gives the demo back from the start */
	fp_demo_init(&demo, &p, MAP_DEMO_START_X, MAP_DEMO_START_Y, goal[0], goal[1], MAP_DEMO_CHIEF_X, MAP_DEMO_CHIEF_Y, via, MAP_DEMO_VIA_COUNT);
	for (i = 0; i < 120; i++) { fp_demo_pad(&demo, &p, &none, &pad, dt); fp_update(&p, &pad, dt); }
	CHECK(demo.autopilot && demo.progress > 0.5f, "the demo did not walk");
	{
		fp_pad touch = { 0, 0, 20000, 0, 0 }, start = { 0, 0, 0, 0, FP_START };
		fp_demo_pad(&demo, &p, &touch, &pad, dt);
		CHECK(!demo.autopilot && pad.right_x == 20000, "the pad did not take over");
		fp_demo_pad(&demo, &p, &none, &pad, dt);
		CHECK(!demo.autopilot && pad.right_x == 0 && pad.left_y == 0, "the demo came back by itself");
		fp_demo_pad(&demo, &p, &start, &pad, dt);
		CHECK(demo.autopilot && demo.progress == 0.f && fabsf(p.pos[0] - MAP_DEMO_START_X) < 1e-3f, "Start did not restart the demo");
	}
	printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
	return failures != 0;
}
