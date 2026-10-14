/*
FP_CONTROLLER.C

Halo's first-person controller on the PS2 sample's map. What is ported, and from where:

  - fp_shape_input: input_xbox.c's dead zone (fix_dead_zone, 9000 of 32767 on each axis) and input_abstraction.c's
    default joystick preset (the left stick moves, the right stick looks; the stick's circle is stretched to the
    square the original's game_input_state has; the d-pad overrides the left stick);
  - look: player_control.c's get_local_player_input_blob and player_control_modify_desired_angles: the globals' look
    function, the yaw and pitch rates, the acceleration of a pegged stick, the pitch limits;
  - tick: bipeds.c's biped_update_physics: 30 Hz, the player's speeds and acceleration from the globals, the slope
    speed scale, the airborne acceleration, gravity, the jump, the crouch and its camera height;
  - fp_camera: first_person_camera.c and bipeds.c's biped_get_sight_position: the eye is the feet plus the camera
    height (blended by the crouch), looking along the facing, with the original's 70 degree lens.

All the numbers come from the map's tags (tools/export_map.py, MAP_* in map_assets.h). What is not ported: the
collision model (the original sweeps a pill through the collision BSP; this walks on the render BSP's triangles,
asking for the ground under nine points of a circle the pill's width, and does not collide with the scenery or
the Chief), the pitch autolevel, the unit camera's own pitch limits, the first-person weapon and the stun penalties.
The speeds the original gets from the animation graph (the walk speed of the alert pose) are not here either.
*/

#include <math.h>
#include <string.h>

#include "map_assets.h"
#include "fp_controller.h"

#define TICKS_PER_SECOND 30.f
#define GRAVITY 0.0035651792f              /* units a tick squared (physics.c: global_gravity) */
#define STICK_DEAD_RANGE 9000              /* input_xbox.c: GAMEPAD_STICK_DEAD_RANGE */
#define PITCH_LIMIT (85.5f * (float)M_PI / 180.f)
#define PI_F ((float)M_PI)
#define STEP_UP 0.3f                       /* the most a point of the pill's circle may rise above the feet: the radius times a 45 degree slope, and the stride */
#define STICK_TO_GROUND 0.12f              /* a drop smaller than this is walked down, not fallen */
#define FALL_RESPAWN_Z (-60.f)
#define COLLISION_POINTS 8

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------- input ---------- */

/* input_xbox.c's fix_dead_zone */
static int fix_dead_zone(int value, int dead_range)
{
	if (value > dead_range) return (value - dead_range) * 32767 / (32767 - dead_range);
	if (value < -dead_range) return (value + dead_range) * -32768 / (-32768 + dead_range);
	return 0;
}

/* input_abstraction.c: the stick's circle stretched out to the square (the corner is 1, 1, not .7, .7) */
static void stick_to_square(short raw_x, short raw_y, float *x, float *y)
{
	int dx = fix_dead_zone(raw_x, STICK_DEAD_RANGE), dy = fix_dead_zone(raw_y, STICK_DEAD_RANGE);
	float angle = atan2f((float)dy, (float)dx);
	float scale = 1.f / fmaxf(fabsf(sinf(angle)), fabsf(cosf(angle)));

	*x = clampf(dx * (1.f / 32767.f) * scale, -1.f, 1.f);
	*y = clampf(dy * (1.f / 32767.f) * scale, -1.f, 1.f);
}

void fp_shape_input(const fp_pad *pad, fp_input *input)
{
	float lx, ly, rx, ry;

	stick_to_square(pad->left_x, pad->left_y, &lx, &ly);
	stick_to_square(pad->right_x, pad->right_y, &rx, &ry);
	/* _joystick_controls_default */
	input->strafe = (pad->buttons & FP_DPAD_LEFT) ? 1.f : (pad->buttons & FP_DPAD_RIGHT) ? -1.f : -lx;
	input->forward_movement = (pad->buttons & FP_DPAD_UP) ? 1.f : (pad->buttons & FP_DPAD_DOWN) ? -1.f : ly;
	input->yaw = -rx;
	input->pitch = ry;
	input->jump = (pad->buttons & FP_JUMP) != 0;
	input->crouch = (pad->buttons & FP_CROUCH) != 0;
}

/* ---------- the ground ---------- */

int fp_ground(float x, float y, float z_max, float *z_out, float *normal_z, float normal_xy[2])
{
	int cx = (int)floorf((x - MAP_COL_X0) / MAP_COL_CELL), cy = (int)floorf((y - MAP_COL_Y0) / MAP_COL_CELL);
	unsigned i, end;
	int found = 0;
	float best = 0.f, best_nz = 0.f, best_nx = 0.f, best_ny = 0.f;

	if (cx < 0 || cy < 0 || cx >= MAP_COL_W || cy >= MAP_COL_H) return 0;
	end = map_col_start[cy * MAP_COL_W + cx + 1];
	for (i = map_col_start[cy * MAP_COL_W + cx]; i < end; i++)
	{
		const unsigned short *t = &map_col_tri[map_col_items[i] * 3];
		const float *a = &map_col_pos[t[0] * 3], *b = &map_col_pos[t[1] * 3], *c = &map_col_pos[t[2] * 3];
		float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
		float w0, w1, w2, z, ux, uy, uz, vx, vy, vz, nx, ny, nz, length;

		if (fabsf(d) < 1e-12f) continue;
		w0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
		w1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
		w2 = 1.f - w0 - w1;
		if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
		z = w0 * a[2] + w1 * b[2] + w2 * c[2];
		if (z > z_max || (found && z <= best)) continue;
		ux = b[0] - a[0]; uy = b[1] - a[1]; uz = b[2] - a[2];
		vx = c[0] - a[0]; vy = c[1] - a[1]; vz = c[2] - a[2];
		nx = uy * vz - uz * vy; ny = uz * vx - ux * vz; nz = ux * vy - uy * vx;
		length = sqrtf(nx * nx + ny * ny + nz * nz);
		if (length < 1e-12f || nz <= 1e-4f * length) continue;     /* facing down or edge-on: not ground */
		found = 1; best = z; best_nx = nx / length; best_ny = ny / length; best_nz = nz / length;
	}
	if (found)
	{
		*z_out = best;
		if (normal_z) *normal_z = best_nz;
		if (normal_xy) { normal_xy[0] = best_nx; normal_xy[1] = best_ny; }
	}
	return found;
}

/* every point of the pill's circle (and its centre) has ground no higher than a step above the feet; the centre's ground goes to *centre */
static int pill_fits(float x, float y, float z, float *centre_z, float *centre_nz)
{
	int i;
	float g;

	if (!fp_ground(x, y, z + STEP_UP, centre_z, centre_nz, NULL)) return 0;
	for (i = 0; i < COLLISION_POINTS; i++)
	{
		float angle = i * (2.f * PI_F / COLLISION_POINTS);

		if (!fp_ground(x + MAP_RADIUS * cosf(angle), y + MAP_RADIUS * sinf(angle), z + STEP_UP, &g, NULL, NULL)) return 0;
	}
	return 1;
}

/* ---------- the biped, 30 times a second ---------- */

static void respawn(fp_player *p)
{
	fp_init(p, p->spawn[0], p->spawn[1], p->yaw);
}

void fp_init(fp_player *player, float x, float y, float yaw)
{
	float z = 0.f;

	memset(player, 0, sizeof(*player));
	fp_ground(x, y, 6.f, &z, NULL, NULL);
	player->pos[0] = x; player->pos[1] = y; player->pos[2] = z;
	memcpy(player->prev_pos, player->pos, sizeof(player->pos));
	player->yaw = yaw;
	player->spawn[0] = x; player->spawn[1] = y;
}

/* bipeds.c: the speed on a slope falls from 1 at the falloff angle to the scale at the cutoff angle, by the direction's vertical component */
static float slope_speed_scale(float k)
{
	float downhill_k0 = -sinf(MAP_DOWNHILL_FALLOFF), downhill_k1 = -sinf(MAP_DOWNHILL_CUTOFF);
	float uphill_k0 = sinf(MAP_UPHILL_FALLOFF), uphill_k1 = sinf(MAP_UPHILL_CUTOFF);

	if (k <= downhill_k1) return MAP_DOWNHILL_SCALE;
	if (k < downhill_k0) return (k - downhill_k0) * (MAP_DOWNHILL_SCALE - 1.f) / (downhill_k1 - downhill_k0) + 1.f;
	if (k >= uphill_k1) return MAP_UPHILL_SCALE;
	if (k > uphill_k0) return (k - uphill_k0) * (MAP_UPHILL_SCALE - 1.f) / (uphill_k1 - uphill_k0) + 1.f;
	return 1.f;
}

/* v moves toward target by at most limit */
static void approach(float *v, const float *target, int n, float limit)
{
	float delta[3], length = 0.f;
	int i;

	for (i = 0; i < n; i++) { delta[i] = target[i] - v[i]; length += delta[i] * delta[i]; }
	length = sqrtf(length);
	if (length > limit) for (i = 0; i < n; i++) delta[i] *= limit / length;
	for (i = 0; i < n; i++) v[i] += delta[i];
}

/* the pill fits at (x, y), and the ground there is not a slope too steep to climb */
static int can_move(const fp_player *p, float x, float y, float min_normal_k)
{
	float ground_z, ground_nz;

	if (!pill_fits(x, y, p->pos[2], &ground_z, &ground_nz)) return 0;
	return p->airborne || p->sliding || ground_z <= p->pos[2] + 0.001f || ground_nz >= min_normal_k;
}

static void tick(fp_player *p, float forward, float strafe, int jump, int crouch_held)
{
	const float min_normal_k = cosf(MAP_MAX_SLOPE);
	float fx = cosf(p->yaw), fy = sinf(p->yaw);
	float ground_z, ground_nz = 1.f, ground_n[2] = { 0.f, 0.f };
	float run_speed, side_speed, acceleration, mi, mj, dir[2];
	float c, magnitude, nx, ny;

	memcpy(p->prev_pos, p->pos, sizeof(p->pos));
	p->prev_crouch = p->crouch;

	/* the throttle is never longer than 1 (player_control.c) */
	magnitude = forward * forward + strafe * strafe;
	if (magnitude > 1.f) { magnitude = 1.f / sqrtf(magnitude); forward *= magnitude; strafe *= magnitude; }

	/* crouch: the camera height follows at the biped's crouch_transition_time */
	{
		float step = 1.f / (MAP_CROUCH_TIME * TICKS_PER_SECOND);

		p->crouch = crouch_held ? fminf(p->crouch + step, 1.f) : fmaxf(p->crouch - step, 0.f);
	}
	c = p->crouch;

	/* the speed the globals give the player, blended by the crouch (a sneak) */
	run_speed = forward <= 0.f ? lerpf(MAP_RUN_BACKWARD, MAP_SNEAK_BACKWARD, c) : lerpf(MAP_RUN_FORWARD, MAP_SNEAK_FORWARD, c);
	side_speed = lerpf(MAP_RUN_SIDEWAYS, MAP_SNEAK_SIDEWAYS, c);
	acceleration = lerpf(MAP_RUN_ACCEL, MAP_SNEAK_ACCEL, c) / TICKS_PER_SECOND;
	mi = forward * run_speed / TICKS_PER_SECOND;
	mj = strafe * side_speed / TICKS_PER_SECOND;
	dir[0] = mi * fx - fy * mj;
	dir[1] = mj * fx + mi * fy;

	if (!p->airborne)
	{
		if (!fp_ground(p->pos[0], p->pos[1], p->pos[2] + STEP_UP, &ground_z, &ground_nz, ground_n)) p->airborne = 1;
		else if (jump)
		{
			p->vel[2] = fmaxf(p->vel[2], MAP_JUMP_VELOCITY);     /* bipeds.c: biped_jump */
			p->airborne = 1;
		}
	}

	p->sliding = 0;
	if (p->airborne)
	{
		/* in the air only the airborne acceleration steers; gravity pulls */
		approach(p->vel, dir, 2, MAP_AIR_ACCEL / TICKS_PER_SECOND);
		p->vel[2] -= GRAVITY;
	}
	else if (ground_nz < min_normal_k)
	{
		/* too steep to stand on: the player slides down it (the gravity along the slope), whatever the stick says */
		p->sliding = 1;
		p->vel[0] += GRAVITY * ground_nz * ground_n[0];
		p->vel[1] += GRAVITY * ground_nz * ground_n[1];
		p->vel[2] = 0.f;
	}
	else
	{
		/* on the ground: the direction follows the slope, and the speed falls off uphill and rises downhill */
		float move[3], speed = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]), length, desired[3];

		move[0] = dir[0]; move[1] = dir[1];
		move[2] = -(dir[0] * ground_n[0] + dir[1] * ground_n[1]) / ground_nz;
		length = sqrtf(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
		if (length > 0.f) { move[0] /= length; move[1] /= length; move[2] /= length; }
		speed *= slope_speed_scale(move[2]);
		desired[0] = move[0] * speed; desired[1] = move[1] * speed; desired[2] = move[2] * speed;
		approach(p->vel, desired, 3, acceleration);
		p->vel[0] -= ground_n[0] * (1.f / 128.f);
		p->vel[1] -= ground_n[1] * (1.f / 128.f);
	}

	/* the horizontal move: all of it, else along one axis (sliding along a wall), else none */
	nx = p->pos[0] + p->vel[0];
	ny = p->pos[1] + p->vel[1];
	if (can_move(p, nx, ny, min_normal_k)) { p->pos[0] = nx; p->pos[1] = ny; }
	else if (can_move(p, nx, p->pos[1], min_normal_k)) { p->pos[0] = nx; p->vel[1] = 0.f; }
	else if (can_move(p, p->pos[0], ny, min_normal_k)) { p->pos[1] = ny; p->vel[0] = 0.f; }
	else { p->vel[0] = p->vel[1] = 0.f; if (!p->airborne) p->vel[2] = 0.f; }

	/* the vertical: fall until the ground is reached, or follow it */
	{
		float gz;

		if (p->airborne)
		{
			p->pos[2] += p->vel[2];
			if (fp_ground(p->pos[0], p->pos[1], p->pos[2] + STEP_UP, &gz, NULL, NULL) && p->pos[2] <= gz && p->vel[2] <= 0.f)
			{
				p->pos[2] = gz; p->vel[2] = 0.f; p->airborne = 0;       /* landed */
			}
		}
		else if (fp_ground(p->pos[0], p->pos[1], p->pos[2] + STEP_UP, &gz, NULL, NULL))
		{
			if (p->pos[2] - gz > STICK_TO_GROUND) { p->airborne = 1; p->vel[2] = 0.f; }   /* walked off an edge */
			else p->pos[2] = gz;
		}
		if (p->pos[2] < FALL_RESPAWN_Z) respawn(p);
	}
}

/* ---------- looking ---------- */

/* player_control.c: evaluate_piecewise_linear_function */
static float evaluate_look_function(float input)
{
	int negate = input < 0.f, low, high;
	float position = clampf(fabsf(input) * (MAP_LOOK_FUNCTION_COUNT - 1), 0.f, (float)(MAP_LOOK_FUNCTION_COUNT - 1)), result;

	low = (int)position;
	if (low > MAP_LOOK_FUNCTION_COUNT - 1) low = MAP_LOOK_FUNCTION_COUNT - 1;
	high = low + 1 < MAP_LOOK_FUNCTION_COUNT ? low + 1 : MAP_LOOK_FUNCTION_COUNT - 1;
	result = (map_look_function[high] - map_look_function[low]) * (position - low) + map_look_function[low];
	return negate ? -result : result;
}

static void look(fp_player *p, const fp_input *input, float seconds)
{
	float absolute_pitch = fabsf(input->pitch), absolute_yaw = fabsf(input->yaw), look_scale = 1.f;
	float clamped_yaw, clamped_pitch, delta_yaw, delta_pitch;

	/* both sticks' axes pushed: the stronger is stretched to 1, so a diagonal turns as fast as a straight push */
	if (absolute_pitch > 0.1f && absolute_yaw > 0.1f)
	{
		if (absolute_pitch > absolute_yaw) { absolute_yaw /= absolute_pitch; absolute_pitch = 1.f; }
		else { absolute_pitch /= absolute_yaw; absolute_yaw = 1.f; }
		look_scale = sqrtf(absolute_pitch * absolute_pitch + absolute_yaw * absolute_yaw);
	}
	clamped_yaw = clampf(input->yaw * look_scale, -1.f, 1.f);
	clamped_pitch = clampf(input->pitch * look_scale, -1.f, 1.f);

	/* the rates are degrees a second at full deflection (the original's are a tick: the rate over 30, 30 times a second) */
	delta_yaw = evaluate_look_function(clamped_yaw) * (MAP_LOOK_YAW_RATE * PI_F / 180.f) * seconds;
	delta_pitch = evaluate_look_function(clamped_pitch) * (MAP_LOOK_PITCH_RATE * PI_F / 180.f) * seconds;

	/* a stick held at the stop turns faster and faster, up to look_acceleration_scale times as fast after look_acceleration_time */
	if (fabsf(clamped_yaw) >= MAP_LOOK_PEG)
	{
		float acceleration = clampf(p->look_accel_time / MAP_LOOK_ACCEL_TIME, 0.f, 1.f);

		delta_yaw *= (MAP_LOOK_ACCEL_SCALE - 1.f) * acceleration + 1.f;
		p->look_accel_time += seconds;
	}
	else p->look_accel_time = 0.f;

	p->yaw += delta_yaw;
	while (p->yaw < 0.f) p->yaw += 2.f * PI_F;
	while (p->yaw > 2.f * PI_F) p->yaw -= 2.f * PI_F;
	p->pitch = clampf(p->pitch + delta_pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

void fp_update(fp_player *p, const fp_pad *pad, float seconds)
{
	fp_input input;
	int ticks = 0;

	fp_shape_input(pad, &input);
	look(p, &input, seconds);
	p->tick_time += seconds;
	while (p->tick_time >= 1.f / TICKS_PER_SECOND)
	{
		p->tick_time -= 1.f / TICKS_PER_SECOND;
		if (ticks++ < 4) tick(p, input.forward_movement, input.strafe, input.jump, input.crouch);   /* a long frame does not run the physics away */
	}
	if (ticks > 4) p->tick_time = 0.f;
	p->time += seconds;
}

/* ---------- the camera ---------- */

float fp_vertical_half_fov_degrees(void)
{
	return atanf(tanf(35.f * PI_F / 180.f) * 3.f / 4.f) * 180.f / PI_F;
}

void fp_camera(const fp_player *p, float eye[3], float target[3])
{
	float t = clampf(p->tick_time * TICKS_PER_SECOND, 0.f, 1.f), crouch = lerpf(p->prev_crouch, p->crouch, t);
	float cp = cosf(p->pitch);
	int i;

	for (i = 0; i < 3; i++) eye[i] = lerpf(p->prev_pos[i], p->pos[i], t);
	eye[2] += lerpf(MAP_CAMERA_STANDING, MAP_CAMERA_CROUCHING, crouch);
	target[0] = eye[0] + cp * cosf(p->yaw);
	target[1] = eye[1] + cp * sinf(p->yaw);
	target[2] = eye[2] + sinf(p->pitch);
}
