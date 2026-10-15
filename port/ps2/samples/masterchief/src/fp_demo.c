/*
FP_DEMO.C

A player that walks by itself, through the same gamepad state a person would give the controller, so everything it does
(the dead zone, the look function that makes a turn slow to start, the acceleration, the slope's speed, the jump's arc,
the walls) is the controller's. Two parts:

  - the plan: A* over a grid of the ground (fp_ground), 0.5 units a cell, on the cells the controller walks comfortably
    (a gentle slope, no edge or wall within the pill's reach), then the corners cut where the straight line is as walkable;
  - the pilot: it chases a point 1.5 units ahead on the path (pure pursuit), turning the right stick by the heading's error
    (through the inverse of the look function, so the turn is as fast as the error asks, never so fast the stick pegs and
    the turn accelerates), running the left stick forward (less in a sharp turn), jumping at marks along the path, and at
    the end standing and looking at the Chief. The weapons go through the pad too, as a person presses them: two bursts of the
    assault rifle on the way, then at the Chief a burst, a melee blow, a reload, the switch to the pistol, the zoom and single shots, the
    zoom out, before starting over.
*/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "map_assets.h"
#include "fp_demo.h"

#define PI_F ((float)M_PI)
#define CELL 0.5f
#define GRID 128                              /* cells a side: 64 units around the start and the goal */
#define STEP_UP 0.3f                          /* the controller's step */
#define MIN_NORMAL_Z 0.79f                    /* the slopes the plan uses: under 38 degrees (the controller climbs 45) */
#define CLEARANCE 0.5f                        /* ground must be there, and level, this far around a cell */
#define LOOKAHEAD 1.5f
#define ARRIVE_DISTANCE 0.7f
#define DWELL_SECONDS 15.f
#define STUCK_SECONDS 4.f
#define DEAD_RANGE 9000
#define STICK_MAX 32767
#define CHIEF_CHEST 0.4f                      /* the height above the Chief's feet that the demo aims at */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float wrap_angle(float a) { while (a > PI_F) a -= 2.f * PI_F; while (a < -PI_F) a += 2.f * PI_F; return a; }

/* ---------- the plan ---------- */

/* the controller can walk from a point at height az to (bx, by), and there is room around b. Only ground within a step of az counts (a
   drop or a rise of more is no way), so the queries look no deeper than that, which is far less of the tree. `cache` (a byte, or NULL) holds
   the answer to the room around b, which does not depend on where the walk comes from */
static int walk_ok(float az, float bx, float by, unsigned char *cache, float *bz)
{
	static const float around[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
	float nz, z;
	int i;

	if (!fp_ground_in(bx, by, az + STEP_UP, 2.f * STEP_UP, bz, &nz, NULL) || nz < MIN_NORMAL_Z || fabsf(*bz - az) > STEP_UP) return 0;
	if (cache && *cache) return *cache == 1;
	for (i = 0; i < 4; i++)
		if (!fp_ground_in(bx + around[i][0] * CLEARANCE, by + around[i][1] * CLEARANCE, *bz + 1.5f * STEP_UP, 3.f * STEP_UP, &z, NULL, NULL) || fabsf(z - *bz) > 1.5f * STEP_UP)
		{
			if (cache) *cache = 2;
			return 0;
		}
	if (cache) *cache = 1;
	return 1;
}

/* the straight line a to b can be walked, in steps of a quarter unit */
static int line_ok(const float *a, const float *b, float az)
{
	float dx = b[0] - a[0], dy = b[1] - a[1], length = sqrtf(dx * dx + dy * dy), z = az;
	int steps = (int)ceilf(length / 0.25f), i;

	for (i = 1; i <= steps; i++)
		if (!walk_ok(z, a[0] + dx * i / steps, a[1] + dy * i / steps, NULL, &z)) return 0;
	return 1;
}

static float node_z[GRID * GRID], node_cost[GRID * GRID];
static short node_parent[GRID * GRID];
static unsigned char node_state[GRID * GRID];         /* 0 new, 1 open, 2 closed */
static unsigned char node_room[GRID * GRID];          /* 0 unknown, 1 room around the cell, 2 none */
typedef struct { float f; short node; } heap_item;
static heap_item heap[GRID * GRID * 2];
static int heap_count;

static void heap_push(float f, int node)
{
	int i = heap_count++;

	heap[i].f = f; heap[i].node = (short)node;
	while (i > 0 && heap[(i - 1) / 2].f > heap[i].f)
	{
		heap_item t = heap[i]; heap[i] = heap[(i - 1) / 2]; heap[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
}

static heap_item heap_pop(void)
{
	heap_item top = heap[0];
	int i = 0;

	heap[0] = heap[--heap_count];
	for (;;)
	{
		int l = 2 * i + 1, r = l + 1, m = i;

		if (l < heap_count && heap[l].f < heap[m].f) m = l;
		if (r < heap_count && heap[r].f < heap[m].f) m = r;
		if (m == i) break;
		{ heap_item t = heap[i]; heap[i] = heap[m]; heap[m] = t; }
		i = m;
	}
	return top;
}

/* appends the way from a to b to the path (a is its last point already, or the first) */
static int plan_leg(fp_demo *d, const float *a, const float *b)
{
	static short chain[GRID * GRID];
	/* the grid is centred between the two ends */
	const float ox = 0.5f * (a[0] + b[0]) - 0.5f * GRID * CELL, oy = 0.5f * (a[1] + b[1]) - 0.5f * GRID * CELL;
	int sx, sy, gx, gy, s, g, n, count = 0, i;
	float sz, from_z;

	sx = (int)((a[0] - ox) / CELL + 0.5f); sy = (int)((a[1] - oy) / CELL + 0.5f);
	gx = (int)((b[0] - ox) / CELL + 0.5f); gy = (int)((b[1] - oy) / CELL + 0.5f);
	if (sx < 1 || sy < 1 || gx < 1 || gy < 1 || sx >= GRID - 1 || sy >= GRID - 1 || gx >= GRID - 1 || gy >= GRID - 1) return 0;
	if (!fp_ground(a[0], a[1], 6.f, &sz, NULL, NULL)) return 0;
	s = sy * GRID + sx; g = gy * GRID + gx;
	memset(node_state, 0, sizeof(node_state));
	memset(node_room, 0, sizeof(node_room));
	node_z[s] = sz; node_cost[s] = 0.f; node_parent[s] = -1; node_state[s] = 1;
	heap_count = 0;
	heap_push(hypotf((float)(gx - sx), (float)(gy - sy)) * CELL, s);
	while (heap_count)
	{
		int node = heap_pop().node, nx = node % GRID, ny = node / GRID, dx, dy;

		if (node_state[node] == 2) continue;
		node_state[node] = 2;
		if (node == g) break;
		for (dy = -1; dy <= 1; dy++) for (dx = -1; dx <= 1; dx++)
		{
			int mx = nx + dx, my = ny + dy, m = my * GRID + mx;
			float z, step, cost;

			if ((!dx && !dy) || mx < 0 || my < 0 || mx >= GRID || my >= GRID || node_state[m] == 2) continue;
			if (!walk_ok(node_z[node], ox + mx * CELL, oy + my * CELL, &node_room[m], &z)) continue;
			step = (dx && dy ? 1.4142f : 1.f) * CELL;
			cost = node_cost[node] + step + 2.f * fabsf(z - node_z[node]);       /* a climb costs more than the ground it covers */
			if (node_state[m] == 1 && cost >= node_cost[m]) continue;
			node_state[m] = 1; node_cost[m] = cost; node_z[m] = z; node_parent[m] = (short)node;
			if (heap_count < GRID * GRID * 2 - 1) heap_push(cost + hypotf((float)(gx - mx), (float)(gy - my)) * CELL, m);
		}
	}
	if (node_state[g] != 2) return 0;

	/* the cells of the way, goal to start */
	for (n = g; n >= 0 && count < GRID * GRID; n = node_parent[n]) chain[count++] = (short)n;
	/* corners cut: from each point the farthest one the straight line to reaches */
	if (!d->count) { d->point[0][0] = a[0]; d->point[0][1] = a[1]; d->count = 1; }
	i = count - 1;                                          /* chain[count - 1] is the start */
	from_z = sz;
	while (i > 0)
	{
		const float *from = d->point[d->count - 1];
		int j, best = i - 1;

		for (j = 0; j < i; j++)
		{
			float to[2] = { ox + (chain[j] % GRID) * CELL, oy + (chain[j] / GRID) * CELL };

			if (line_ok(from, to, from_z)) { best = j; break; }
		}
		from_z = node_z[chain[best]];
		if (d->count >= FP_DEMO_MAX_POINTS) return 0;
		d->point[d->count][0] = ox + (chain[best] % GRID) * CELL;
		d->point[d->count][1] = oy + (chain[best] / GRID) * CELL;
		d->count++;
		i = best;
	}
	/* the end itself, where the grid's cell is a little off it */
	d->point[d->count - 1][0] = b[0]; d->point[d->count - 1][1] = b[1];
	return 1;
}

static int plan(fp_demo *d, const float via[][2], int via_count)
{
	float from[2] = { d->start[0], d->start[1] };
	int i;

	d->count = 0;
	for (i = 0; i <= via_count; i++)
	{
		const float *to = i < via_count ? via[i] : d->goal;

		if (!plan_leg(d, from, to)) return 0;
		from[0] = to[0]; from[1] = to[1];
	}
	d->arc[0] = 0.f;
	for (i = 1; i < d->count; i++) d->arc[i] = d->arc[i - 1] + hypotf(d->point[i][0] - d->point[i - 1][0], d->point[i][1] - d->point[i - 1][1]);
	d->length = d->arc[d->count - 1];
	return d->count >= 2;
}

/* ---------- the pilot ---------- */

static void restart(fp_demo *d, fp_player *player)
{
	fp_init(player, d->start[0], d->start[1], atan2f(d->point[1][1] - d->point[0][1], d->point[1][0] - d->point[0][0]));
	d->autopilot = 1;
	d->next_jump = 0; d->jump_pulse = 0.f; d->next_burst = 0; d->burst_left = 0.f; d->progress = 0.f; d->arrived = 0.f; d->stuck = 0.f;
}

int fp_demo_init(fp_demo *d, fp_player *player, float start_x, float start_y, float goal_x, float goal_y, float face_x, float face_y, const float via[][2], int via_count)
{
	static const float jump_fractions[FP_DEMO_MAX_JUMPS] = { 0.2f, 0.45f, 0.7f, 0.85f };
	static const float burst_fractions[FP_DEMO_BURSTS] = { 0.33f, 0.8f };
	int i;

	memset(d, 0, sizeof(*d));
	d->start[0] = start_x; d->start[1] = start_y; d->goal[0] = goal_x; d->goal[1] = goal_y; d->face[0] = face_x; d->face[1] = face_y;
	fp_init(player, start_x, start_y, 0.f);
	if (!plan(d, via, via_count)) return 0;
	d->planned = 1;
	d->jump_count = d->length > 12.f ? FP_DEMO_MAX_JUMPS : d->length > 6.f ? 2 : 0;
	for (i = 0; i < d->jump_count; i++) d->jump_at[i] = d->length * jump_fractions[i];
	for (i = 0; i < FP_DEMO_BURSTS; i++) d->burst_at[i] = d->length * burst_fractions[i];
	restart(d, player);
	return 1;
}

/* the stick value (as the Xbox's raw 16 bits, past the dead zone) that gives a fraction c of the full deflection after the dead zone */
static short raw_stick(float c)
{
	return (short)(c <= 0.f ? 0 : DEAD_RANGE + clampf(c, 0.f, 1.f) * (STICK_MAX - DEAD_RANGE));
}

/* the deflection (0..1) whose look-function value is `fraction` of the full rate: the look function backwards */
static float deflection_for(float fraction)
{
	int i;

	fraction = clampf(fraction, 0.f, 0.58f);                 /* below the pegging threshold: no acceleration */
	for (i = 1; i < MAP_LOOK_FUNCTION_COUNT; i++)
		if (fraction <= map_look_function[i])
		{
			float span = map_look_function[i] - map_look_function[i - 1];

			return ((i - 1) + (span > 0.f ? (fraction - map_look_function[i - 1]) / span : 0.f)) / (MAP_LOOK_FUNCTION_COUNT - 1);
		}
	return 1.f;
}

/* the right stick that turns `error` radians at the rate that closes it in about a quarter second */
static short turn_stick(float error, float rate_radians)
{
	float want = clampf(error * 4.f, -3.f, 3.f) / rate_radians;      /* a fraction of the full rate */

	return (short)(want < 0.f ? -raw_stick(deflection_for(-want)) : raw_stick(deflection_for(want)));
}

static void point_at(const fp_demo *d, float s, float p[2])
{
	int i = 1;

	s = clampf(s, 0.f, d->length);
	while (i < d->count - 1 && d->arc[i] < s) i++;
	{
		float span = d->arc[i] - d->arc[i - 1], t = span > 0.f ? (s - d->arc[i - 1]) / span : 1.f;

		p[0] = d->point[i - 1][0] + (d->point[i][0] - d->point[i - 1][0]) * t;
		p[1] = d->point[i - 1][1] + (d->point[i][1] - d->point[i - 1][1]) * t;
	}
}

void fp_demo_pad(fp_demo *d, fp_player *player, const fp_pad *user, fp_pad *out, float seconds)
{
	static int start_held;
	float eye_z, target[2], error, pitch_error, aim_z = 0.f, distance_to_goal, speed;
	int user_active, start_pressed = (user->buttons & FP_START) != 0;

	memset(out, 0, sizeof(*out));
	user_active = abs(user->left_x) > DEAD_RANGE || abs(user->left_y) > DEAD_RANGE || abs(user->right_x) > DEAD_RANGE || abs(user->right_y) > DEAD_RANGE
		|| (user->buttons & ~FP_START);
	if (d->planned && start_pressed && !start_held) restart(d, player);             /* Start: the demo from the beginning */
	else if (user_active) d->autopilot = 0;
	start_held = start_pressed;
	if (!d->planned || !d->autopilot) { *out = *user; out->buttons &= ~FP_START; return; }

	/* where the player is along the path: the nearest point of the next few segments, never backwards */
	{
		int i;
		float best = 1e9f, at = d->progress;

		for (i = 1; i < d->count; i++)
		{
			float ax = d->point[i - 1][0], ay = d->point[i - 1][1], bx = d->point[i][0], by = d->point[i][1], dx = bx - ax, dy = by - ay;
			float l2 = dx * dx + dy * dy, t = l2 > 0.f ? clampf(((player->pos[0] - ax) * dx + (player->pos[1] - ay) * dy) / l2, 0.f, 1.f) : 0.f;
			float px = ax + dx * t - player->pos[0], py = ay + dy * t - player->pos[1], dist = px * px + py * py, s = d->arc[i - 1] + t * sqrtf(l2);

			if (s < d->progress - 0.5f || s > d->progress + 4.f) continue;
			if (dist < best) { best = dist; at = s; }
		}
		if (at > d->progress) { d->stuck = 0.f; d->progress = at; }
		else d->stuck += seconds;
	}
	distance_to_goal = hypotf(d->goal[0] - player->pos[0], d->goal[1] - player->pos[1]);
	eye_z = player->pos[2] + MAP_CAMERA_STANDING;

	if (distance_to_goal < ARRIVE_DISTANCE)          /* there: look at the Chief, then again from the start */
	{
		float aim_x = d->face[0] - player->pos[0], aim_y = d->face[1] - player->pos[1];

		d->arrived += seconds;
		error = wrap_angle(atan2f(aim_y, aim_x) - player->yaw);
		aim_z = MAP_DEMO_CHIEF_Z + CHIEF_CHEST;          /* (the ground there is the top of the Chief's capsule: aiming a little above it misses him) */
		pitch_error = atan2f(aim_z - eye_z, hypotf(aim_x, aim_y)) - player->pitch;
		out->right_x = (short)-turn_stick(error, MAP_LOOK_YAW_RATE * PI_F / 180.f);
		out->right_y = turn_stick(pitch_error, MAP_LOOK_PITCH_RATE * PI_F / 180.f);
		/* and the weapons, for as long as it stands there (the pad's buttons, as a person presses them; a press is 0.1 s so that a frame sees it):
		   a burst of the rifle at the Chief, a blow, a reload, the pistol out, zoomed (it has the one level), single shots (the pistol's trigger is latched: a press
		   for each), the zoom off */
		{
			const float t = d->arrived;
			static const float shots[] = { 10.4f, 11.0f, 11.6f, 12.2f, 12.8f };
			int k;

			if (t > 1.2f && t < 2.3f) out->trigger = 255;
			if (t > 3.0f && t < 3.1f) out->buttons |= FP_MELEE;
			if (t > 4.3f && t < 4.4f) out->buttons |= FP_RELOAD;
			if (t > 7.8f && t < 7.9f) out->buttons |= FP_SWITCH_WEAPON;
			if (t > 9.8f && t < 9.9f) out->buttons |= FP_ZOOM;
			if (t > 13.6f && t < 13.7f) out->buttons |= FP_ZOOM;
			for (k = 0; k < (int)(sizeof(shots) / sizeof(shots[0])); k++) if (t > shots[k] && t < shots[k] + 0.12f) out->trigger = 255;
		}
		if (d->arrived > DWELL_SECONDS) restart(d, player);
		return;
	}
	if (d->stuck > STUCK_SECONDS) { restart(d, player); return; }

	/* chase the point ahead on the path */
	point_at(d, d->progress + LOOKAHEAD, target);
	error = wrap_angle(atan2f(target[1] - player->pos[1], target[0] - player->pos[0]) - player->yaw);
	out->right_x = (short)-turn_stick(error, MAP_LOOK_YAW_RATE * PI_F / 180.f);       /* the stick's right turns the player right: yaw is positive to the left */
	/* the eyes follow the ground ahead a little, as a person's do */
	if (fp_ground(target[0], target[1], eye_z + 3.f, &aim_z, NULL, NULL)) pitch_error = clampf(atan2f(aim_z + 0.3f - eye_z, LOOKAHEAD + 1.f), -0.3f, 0.3f) - player->pitch;
	else pitch_error = -player->pitch;
	out->right_y = turn_stick(pitch_error, MAP_LOOK_PITCH_RATE * PI_F / 180.f);

	/* run, less in a bend, and stop to turn when the heading is far off */
	speed = fabsf(error) < 0.35f ? 1.f : fabsf(error) < 1.f ? 0.55f : 0.f;
	out->left_y = raw_stick(speed);

	/* the jumps: a short press at each mark, on the ground */
	if (d->next_jump < d->jump_count && d->progress >= d->jump_at[d->next_jump] && !player->airborne) { d->jump_pulse = 0.12f; d->next_jump++; }
	if (d->jump_pulse > 0.f) { out->buttons |= FP_JUMP; d->jump_pulse -= seconds; }

	/* the rifle's bursts on the way, a second long */
	if (d->next_burst < FP_DEMO_BURSTS && d->progress >= d->burst_at[d->next_burst]) { d->burst_left = 1.f; d->next_burst++; }
	if (d->burst_left > 0.f) { out->trigger = 255; d->burst_left -= seconds; }
}
