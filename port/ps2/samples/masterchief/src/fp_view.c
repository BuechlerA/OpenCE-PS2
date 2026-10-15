/*
FP_VIEW.C

The first-person weapon's pose, from source/interface/first_person_weapons.c (first_person_weapon_update, _set_state, _next_state,
_message, _build_node_matrices) and source/models/model_animations.c (animation_update_internal, animation_get_node_orientations,
overlay_animation_apply[_scaled], interpolate_node_orientations, animation_graph_node_matrices_from_orientations), on the graphs of
fpm_assets.h.

Ported: the states (idle, posing after 10 to 15 s of nothing, the fires, the reloads, the shotgun's, the melee, the ready and the put-away,
the grenade throw, the overheating ones), each state's animation and its frame a tick, the next state at the end of an animation, the
interpolation of 3 to 6 ticks between animations, the moving overlay, the 9-frame sway overlay (position, turning and push-back, which
accelerate toward the unit's throttle, its turning and the shots), the ammunition overlay (the frame is the rounds loaded), and the node
matrices in the camera's space.

Not: the sounds the animations trigger, the plasma weapons' overcharge jitter and the weapons' own effects, the flashlight.
*/

#include <math.h>
#include <string.h>

#include "map_assets.h"
#include "fp_view.h"

#define OVERLAY_FRAMES 9
#define FIRING_PUSH_BACK_VELOCITY 0.05f
#define PI_F ((float)M_PI)

/* ---------- animation data ---------- */

const fpm_animation *fpv_animation_of_type(const fpv_state *v, int type)
{
	const fpm_graph *g;

	if (v->weapon < 0 || !fpm_weapons[v->weapon].present || type < 0 || type >= FPM_ANIMATION_TYPES) return NULL;
	g = fpm_weapons[v->weapon].graph;
	return g->animation_of_type[type] >= 0 ? &g->animations[g->animation_of_type[type]] : NULL;
}

static int slot_of_type(const fpv_state *v, int type)
{
	if (v->weapon < 0 || !fpm_weapons[v->weapon].present || type < 0 || type >= FPM_ANIMATION_TYPES) return -1;
	return fpm_weapons[v->weapon].graph->animation_of_type[type];
}

static const fpm_animation *animation_at(const fpv_state *v, int slot)
{
	return slot >= 0 ? &fpm_weapons[v->weapon].graph->animations[slot] : NULL;
}

static float read_float(const unsigned char *p) { float f; memcpy(&f, p, 4); return f; }

static void read_quaternion(const unsigned char *p, float *q)
{
	short s[4];
	float l;
	int i;

	memcpy(s, p, 8);
	for (i = 0; i < 4; i++) q[i] = s[i] / 32767.f;
	l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 1e-9f) for (i = 0; i < 4; i++) q[i] /= l;
}

/* animation_get_node_orientations: a base animation's frame: each node's rotation, translation and scale from the frame where the animation has one for
   it, else from the animation's defaults */
static void sample_base(const fpm_animation *a, int frame, fpv_orientation *out, int count)
{
	const unsigned char *p = a->data + (size_t)frame * a->frame_size, *dp = a->defaults;
	int n;

	for (n = 0; n < a->node_count && n < count; n++)
	{
		unsigned bit = 1u << (n & 31);
		int word = n >> 5;

		if (a->rotation_flags[word] & bit) { read_quaternion(p, out[n].q); p += 8; } else { read_quaternion(dp, out[n].q); dp += 8; }
		if (a->translation_flags[word] & bit) { memcpy(out[n].t, p, 12); p += 12; } else { memcpy(out[n].t, dp, 12); dp += 12; }
		if (a->scale_flags[word] & bit) { out[n].s = read_float(p); p += 4; } else { out[n].s = read_float(dp); dp += 4; }
	}
}

static void quaternion_multiply(const float *a, const float *b, float *out)
{
	float r[4];

	r[0] = a[1] * b[2] - a[2] * b[1] + a[0] * b[3] + a[3] * b[0];       /* math/real_math.c: quaternions_multiply */
	r[1] = a[2] * b[0] - a[0] * b[2] + a[1] * b[3] + a[3] * b[1];
	r[2] = a[0] * b[1] - a[1] * b[0] + a[2] * b[3] + a[3] * b[2];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, r, sizeof(r));
}

/* an overlay's frame laid over the orientations, scaled by `amount` (1: overlay_animation_apply, else _scaled: the rotation interpolated from the
   identity, the translation and scale by the amount) */
static void apply_overlay(const fpm_animation *a, int frame, float amount, fpv_orientation *out, int count)
{
	const unsigned char *p;
	int n;

	if (!a || a->type != 1 || frame < 0 || frame >= a->frames) return;
	p = a->data + (size_t)frame * a->frame_size;
	for (n = 0; n < a->node_count && n < count; n++)
	{
		unsigned bit = 1u << (n & 31);
		int word = n >> 5;

		if (a->rotation_flags[word] & bit)
		{
			float q[4], identity[4] = { 0, 0, 0, 1 }, dot, t = amount;
			int i;

			read_quaternion(p, q); p += 8;
			if (amount != 1.f)
			{
				dot = q[3];                                                  /* identity . q */
				if (dot < 0.f) t = -amount;
				for (i = 0; i < 4; i++) q[i] = identity[i] * (1.f - amount) + q[i] * t;
			}
			quaternion_multiply(q, out[n].q, out[n].q);
		}
		if (a->translation_flags[word] & bit)
		{
			float t[3];

			memcpy(t, p, 12); p += 12;
			out[n].t[0] += t[0] * amount; out[n].t[1] += t[1] * amount; out[n].t[2] += t[2] * amount;
		}
		if (a->scale_flags[word] & bit) { float s = read_float(p); p += 4; out[n].s *= amount == 1.f ? s : s * amount + (1.f - amount); }
	}
}

/* ---------- the matrices ---------- */

/* the 4x3 of a quaternion (the rows as the sample's other models', tools/skel.py: quat_rows) scaled, and a translation */
static void matrix_from_orientation(const fpv_orientation *o, float *m)
{
	float i = o->q[0], j = o->q[1], k = o->q[2], w = o->q[3], x = 2 * i, y = 2 * j, z = 2 * k;
	float xx = i * x, xy = i * y, xz = i * z, yy = j * y, yz = j * z, zz = k * z, wx = w * x, wy = w * y, wz = w * z, s = o->s;

	m[0] = (1 - (yy + zz)) * s; m[1] = (xy - wz) * s; m[2] = (xz + wy) * s;
	m[3] = (xy + wz) * s; m[4] = (1 - (xx + zz)) * s; m[5] = (yz - wx) * s;
	m[6] = (xz - wy) * s; m[7] = (yz + wx) * s; m[8] = (1 - (xx + yy)) * s;
	m[9] = o->t[0]; m[10] = o->t[1]; m[11] = o->t[2];
}

/* a * b for row vectors (p * a * b) */
static void matrix_multiply(const float *a, const float *b, float *out)
{
	float r[12];
	int i, j;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
	for (j = 0; j < 3; j++) r[9 + j] = a[9] * b[0 * 3 + j] + a[10] * b[1 * 3 + j] + a[11] * b[2 * 3 + j] + b[9 + j];
	memcpy(out, r, sizeof(r));
}

void fpv_skin_matrix(const fpv_state *v, const fpm_model *model, const short *graph_node, int node, float t, float *out)
{
	float world[12];
	int g = graph_node[node], k;

	if (g < 0 || g >= FPV_MAX_NODES) { memset(out, 0, 12 * sizeof(float)); out[0] = out[4] = out[8] = 1.f; return; }
	for (k = 0; k < 12; k++) world[k] = v->previous_matrices[g][k] * (1.f - t) + v->matrices[g][k] * t;
	matrix_multiply(&model->inverse_bind[node * 12], world, out);
}

/* ---------- random ---------- */

static float random01(fpv_state *v)
{
	v->random_seed = v->random_seed * 1664525u + 1013904223u;
	return (float)((v->random_seed >> 8) & 0xffff) / 65535.f;
}

/* ---------- the states ---------- */

/* first_person_animation_type_from_weapon_state */
static int animation_type_of_state(int state)
{
	switch (state)
	{
	case FPV_IDLE: return FPW_ANIMATION_IDLE;
	case FPV_OVERHEATED: return FPW_ANIMATION_OVERHEATED;
	case FPV_CHARGED: return FPW_ANIMATION_OVERCHARGED;
	case FPV_POSING: return FPW_ANIMATION_POSING;
	case FPV_PRIMARY_FIRE: return FPW_ANIMATION_PRIMARY_FIRE;
	case FPV_SECONDARY_FIRE: return FPW_ANIMATION_SECONDARY_FIRE;
	case FPV_PRIMARY_MISFIRE: return FPW_ANIMATION_PRIMARY_MISFIRE;
	case FPV_SECONDARY_MISFIRE: return FPW_ANIMATION_SECONDARY_MISFIRE;
	case FPV_MELEE: return FPW_ANIMATION_MELEE;
	case FPV_LIGHT_ON: return FPW_ANIMATION_LIGHT_ON;
	case FPV_LIGHT_OFF: return FPW_ANIMATION_LIGHT_OFF;
	case FPV_RELOAD_WHILE_EMPTY: return FPW_ANIMATION_RELOAD_WHILE_EMPTY;
	case FPV_RELOAD_WHILE_FULL: return FPW_ANIMATION_RELOAD_WHILE_FULL;
	case FPV_PUT_AWAY: return FPW_ANIMATION_PUT_AWAY;
	case FPV_READY: return FPW_ANIMATION_READY;
	case FPV_THROW_GRENADE: return FPW_ANIMATION_THROW_GRENADE;
	case FPV_THROW_GRENADE_OVERHEATED: return FPW_ANIMATION_THROW_GRENADE_OVERHEATED;
	case FPV_OVERHEATING: return FPW_ANIMATION_OVERHEATING;
	case FPV_OVERHEATING_AGAIN: return FPW_ANIMATION_OVERHEATING_AGAIN;
	case FPV_SHOTGUN_ENTER_RELOAD: return FPW_ANIMATION_SHOTGUN_ENTER;
	case FPV_SHOTGUN_EXIT_RELOAD_EMPTY: return FPW_ANIMATION_SHOTGUN_EXIT_EMPTY;
	case FPV_SHOTGUN_EXIT_RELOAD_FULL: return FPW_ANIMATION_SHOTGUN_EXIT_FULL;
	case FPV_OVERHEATED_EXIT: return FPW_ANIMATION_OVERHEATED_EXIT;
	case FPV_OVERHEATING_SUPER_RECOIL: return FPW_ANIMATION_OVERHEATED_SUPERCHARGE_ENTER;
	}
	return -1;
}

static int node_count(const fpv_state *v)
{
	return fpm_weapons[v->weapon].graph->node_count < FPV_MAX_NODES ? fpm_weapons[v->weapon].graph->node_count : FPV_MAX_NODES;
}

/* first_person_weapon_start_interpolation: the pose now is the one the next animation eases from */
static void start_interpolation(fpv_state *v, int frames)
{
	memcpy(v->original, v->orientation, sizeof(v->original));
	v->interpolation_index = 0;
	v->interpolation_count = frames;
}

/* first_person_weapon_set_state */
static void set_state(fpv_state *v, int new_state, const fpv_input *in_overheated_hint)
{
	int type, frames, slot;
	int overheated = in_overheated_hint ? in_overheated_hint->overheated : 0;

	if (overheated)
	{
		if (new_state == FPV_READY) new_state = FPV_OVERHEATING_AGAIN;
		else if (new_state == FPV_THROW_GRENADE) new_state = FPV_THROW_GRENADE_OVERHEATED;
	}
	switch (new_state)
	{
	case FPV_READY: if (v->state == FPV_READY) new_state = -1; break;
	case FPV_LIGHT_OFF: case FPV_LIGHT_ON: if (v->state != FPV_IDLE && v->state != FPV_POSING) new_state = -1; break;
	case FPV_PRIMARY_FIRE: case FPV_SECONDARY_FIRE: case FPV_PRIMARY_MISFIRE: case FPV_SECONDARY_MISFIRE:
		if (v->state != FPV_IDLE && v->state != FPV_POSING && v->state != FPV_PRIMARY_FIRE && v->state != FPV_CHARGED && v->state != FPV_SHOTGUN_ENTER_RELOAD &&
		    v->state != FPV_OVERHEATED_EXIT && v->state != FPV_SHOTGUN_EXIT_RELOAD_EMPTY && v->state != FPV_SHOTGUN_EXIT_RELOAD_FULL &&
		    v->state != FPV_RELOAD_WHILE_EMPTY && v->state != FPV_RELOAD_WHILE_FULL) new_state = -1;
		break;
	}
	if (new_state < 0 || v->weapon < 0) return;
	type = animation_type_of_state(new_state);
	if (fpw_weapons[v->weapon].weapon_type == 1 && v->state == FPV_SHOTGUN_EXIT_RELOAD_EMPTY) frames = 0;
	else switch (new_state)
	{
	case FPV_OVERHEATED: case FPV_MELEE: case FPV_READY: frames = 0; break;
	case FPV_PRIMARY_FIRE: case FPV_SECONDARY_FIRE: case FPV_PRIMARY_MISFIRE: case FPV_SECONDARY_MISFIRE: frames = 3; break;
	default: frames = 6; break;
	}
	slot = slot_of_type(v, type);
	if (slot >= 0)
	{
		if (frames > 0) start_interpolation(v, frames);
		v->state = new_state;
		v->animation = slot;
		v->frame = 0;
	}
}

/* first_person_weapon_next_state: an animation has run its length */
static void next_state(fpv_state *v, const fpv_input *in)
{
	int new_state = -1;

	switch (v->state)
	{
	case FPV_IDLE: case FPV_POSING: case FPV_PRIMARY_FIRE: case FPV_SECONDARY_FIRE: case FPV_PRIMARY_MISFIRE: case FPV_SECONDARY_MISFIRE: case FPV_MELEE: case FPV_LIGHT_OFF:
	case FPV_LIGHT_ON: case FPV_SHOTGUN_EXIT_RELOAD_EMPTY: case FPV_SHOTGUN_EXIT_RELOAD_FULL: case FPV_READY: case FPV_THROW_GRENADE: case FPV_OVERHEATED_EXIT:
		new_state = FPV_IDLE; break;
	case FPV_OVERHEATING: case FPV_OVERHEATING_AGAIN: case FPV_THROW_GRENADE_OVERHEATED: case FPV_OVERHEATING_SUPER_RECOIL:
		new_state = FPV_OVERHEATED; break;
	case FPV_PUT_AWAY: v->frame--; break;                                  /* held on the last frame until the next weapon is readied */
	case FPV_SHOTGUN_ENTER_RELOAD:
		if (fpw_weapons[v->weapon].weapon_type == 1 && v->shotgun_reload_type == 2) new_state = v->shotgun_empty ? FPV_SHOTGUN_EXIT_RELOAD_EMPTY : FPV_SHOTGUN_EXIT_RELOAD_FULL;
		else new_state = FPV_IDLE;
		break;
	case FPV_RELOAD_WHILE_EMPTY: case FPV_RELOAD_WHILE_FULL:
		if (fpw_weapons[v->weapon].weapon_type != 1 || !v->shotgun_reload_type || v->shotgun_reload_type == -1) new_state = FPV_IDLE;
		else new_state = v->shotgun_empty ? FPV_SHOTGUN_EXIT_RELOAD_EMPTY : FPV_SHOTGUN_EXIT_RELOAD_FULL;
		break;
	}
	set_state(v, new_state, in);
}

void fpv_init(fpv_state *v)
{
	memset(v, 0, sizeof(*v));
	v->weapon = -1; v->animation = v->moving_animation = v->overlay_animation = -1; v->state = -1;
	v->random_seed = 987654321u;
}

/* first_person_weapon_switch_weapons: the weapon in hand, idle */
void fpv_set_weapon(fpv_state *v, int weapon)
{
	fpv_input none;

	memset(&none, 0, sizeof(none));
	v->weapon = weapon >= 0 && weapon < FPW_COUNT && fpm_weapons[weapon].present ? weapon : -1;
	v->state = -1; v->animation = -1; v->moving_animation = -1; v->overlay_animation = -1;
	v->push_back = v->push_back_velocity = 0.f;
	v->ticks_idle = 0; v->ticks_until_pose = 0;
	v->interpolation_count = 0;
	if (v->weapon < 0) return;
	{
		const fpm_animation *overlay = fpv_animation_of_type(v, FPW_ANIMATION_OVERLAYS);

		if (overlay && overlay->frames >= OVERLAY_FRAMES) v->overlay_animation = slot_of_type(v, FPW_ANIMATION_OVERLAYS);
	}
	set_state(v, FPV_IDLE, &none);
	v->interpolation_count = 0;
	v->built = 0;
}

/* first_person_weapon_message: what the weapon says to its first-person model */
void fpv_event(fpv_state *v, const fpw_event *e, const fpw_state *w)
{
	fpv_input in;
	int state = -1;

	memset(&in, 0, sizeof(in));
	in.overheated = w && (w->flags & FPW_FLAG_OVERHEATED) != 0;
	switch (e->type)
	{
	case FPW_EVENT_FIRE: v->push_back_velocity += FIRING_PUSH_BACK_VELOCITY; state = e->a ? FPV_SECONDARY_FIRE : FPV_PRIMARY_FIRE; break;
	case FPW_EVENT_MISFIRE: state = e->a ? FPV_SECONDARY_MISFIRE : FPV_PRIMARY_MISFIRE; break;
	case FPW_EVENT_RELOAD_START:
		state = e->a ? FPV_RELOAD_WHILE_FULL : FPV_RELOAD_WHILE_EMPTY;
		if (v->weapon >= 0 && fpw_weapons[v->weapon].weapon_type == 1 && w)
		{
			const fpw_magazine *md = &fpw_weapons[v->weapon].magazines[0];
			int loaded = w->magazines[0].rounds_loaded, total = w->magazines[0].rounds_total, shells = md->loaded_maximum - loaded < total ? md->loaded_maximum - loaded : total;

			if (v->state == FPV_SHOTGUN_ENTER_RELOAD || v->state == FPV_RELOAD_WHILE_EMPTY || v->state == FPV_RELOAD_WHILE_FULL || w->magazines[0].state)
				v->shotgun_reload_type = (shells == 1) ? 1 : -1;
			else
			{
				v->shotgun_empty = loaded == 0;
				v->shotgun_reload_type = shells != 1 ? 0 : 2;
			}
			state = v->shotgun_reload_type == 0 || v->shotgun_reload_type == 2 ? FPV_SHOTGUN_ENTER_RELOAD : FPV_RELOAD_WHILE_EMPTY;
		}
		break;
	case FPW_EVENT_PUT_AWAY: state = FPV_PUT_AWAY; break;
	case FPW_EVENT_READY: fpv_set_weapon(v, e->a); v->interpolation_count = 0; state = FPV_READY; break;
	case FPW_EVENT_MELEE: state = FPV_MELEE; break;
	case FPW_EVENT_CHARGED: state = FPV_CHARGED; break;
	case FPW_EVENT_OVERHEATING: state = FPV_OVERHEATING; break;
	case FPW_EVENT_THROW: state = FPV_THROW_GRENADE; break;
	}
	if (state >= 0) set_state(v, state, &in);
	if (e->type == FPW_EVENT_READY) v->interpolation_count = 0;
}

/* math/real_math.c: accelerate_to_position (not periodic) */
static void accelerate_to_position(float *position, float *velocity, float target, float maximum_velocity, float acceleration, float minimum, float maximum)
{
	float delta = target - *position, limit = maximum_velocity < acceleration ? maximum_velocity : acceleration, speed, step, braking = 2.f * maximum_velocity * fabsf(delta), np;

	if (fabsf(delta - *velocity) <= limit)
	{
		*velocity = 0.f;
		np = target < minimum ? minimum : target > maximum ? maximum : target;
		*position = np;
		return;
	}
	speed = braking >= acceleration * acceleration ? acceleration : sqrtf(braking);
	if (delta < 0.f) speed = -speed;
	step = speed - *velocity;
	if (fabsf(step) > maximum_velocity) step = step < 0.f ? -maximum_velocity : maximum_velocity;
	*velocity += step;
	np = step * 0.5f + *velocity + *position;
	*position = np < minimum ? minimum : np > maximum ? maximum : np;
}

static float signed_angular_difference(float a, float b)
{
	float d = b - a;

	while (d > PI_F) d -= 2.f * PI_F;
	while (d < -PI_F) d += 2.f * PI_F;
	return d;
}

/* animation_update_internal: a frame on. Returns 1 if the animation will restart on its next frame (the state is over), 0 otherwise */
static int advance(const fpm_animation *a, int *frame)
{
	(*frame)++;
	if (*frame >= a->frames)
	{
		if (a->loop_frame > 0) *frame = a->loop_frame < a->frames - 1 ? a->loop_frame : a->frames - 1;
		else *frame = 0;
		return 0;
	}
	return *frame + 1 == a->frames && a->loop_frame == 0;
}

void fpv_tick(fpv_state *v, const fpv_input *in, const fpw_state *w)
{
	const fpm_weapon *fw;
	const fpm_graph *graph;
	const fpm_animation *state_animation, *a;
	float skip_fraction;
	int nodes, n, moving;

	if (v->weapon < 0) return;
	fw = &fpm_weapons[v->weapon]; graph = fw->graph; nodes = node_count(v);

	/* first_person_weapon_update: the state's animation a frame on, the next state at its end */
	if (v->state == FPV_OVERHEATED || v->state == FPV_OVERHEATING)
	{
		if (w && (w->flags & FPW_FLAG_OVERHEATED_EXIT)) set_state(v, FPV_OVERHEATED_EXIT, in);
		if (w && !(w->flags & FPW_FLAG_OVERHEATED)) set_state(v, FPV_IDLE, in);
	}
	state_animation = animation_at(v, v->animation);
	if (state_animation && advance(state_animation, &v->frame)) next_state(v, in);

	moving = (in->throttle[0] * in->throttle[0] + in->throttle[1] * in->throttle[1] > 0.01f) && !in->flying;
	if (v->moving_animation >= 0)
	{
		a = animation_at(v, v->moving_animation);
		advance(a, &v->moving_frame);
		if (!moving)
		{
			if (v->state == FPV_IDLE) start_interpolation(v, 6);
			v->moving_animation = -1;
		}
	}
	else if (moving) { v->moving_frame = 0; v->moving_animation = slot_of_type(v, FPW_ANIMATION_MOVING); }

	if (v->rendered)
	{
		float turning_i = signed_angular_difference(v->last_yaw, in->yaw) * 30.f, turning_j = signed_angular_difference(v->last_pitch, in->pitch) * -30.f;

		accelerate_to_position(&v->position[0], &v->position_velocity[0], in->throttle[0], 0.08f, 0.5f, -1.f, 1.f);
		accelerate_to_position(&v->position[1], &v->position_velocity[1], in->throttle[1], 0.08f, 0.5f, -1.f, 1.f);
		turning_i = turning_i < -1.f ? -1.f : turning_i > 1.f ? 1.f : turning_i;
		turning_j = turning_j < -1.f ? -1.f : turning_j > 1.f ? 1.f : turning_j;
		accelerate_to_position(&v->turning[0], &v->turning_velocity[0], turning_i, 0.03f, 0.2f, -1.f, 1.f);
		accelerate_to_position(&v->turning[1], &v->turning_velocity[1], turning_j, 0.03f, 0.2f, -1.f, 1.f);
	}
	v->last_yaw = in->yaw; v->last_pitch = in->pitch;
	v->rendered = 1;
	accelerate_to_position(&v->push_back, &v->push_back_velocity, 0.f, 0.01f, 0.2f, 0.f, 1.f);
	if (v->push_back == 1.f) v->push_back_velocity = 0.f;
	if (v->interpolation_count > 0 && ++v->interpolation_index >= v->interpolation_count) v->interpolation_count = 0;

	/* the weapon idles into a pose after a while of nothing happening */
	if (in->zoomed || v->push_back != 0.f || v->position[0] != 0.f || v->position[1] != 0.f || v->turning[0] != 0.f || v->turning[1] != 0.f)
	{
		v->ticks_idle = 0;
		if (v->state == FPV_POSING) set_state(v, FPV_IDLE, in);
	}
	else if (v->state == FPV_IDLE)
	{
		if (!v->ticks_until_pose) v->ticks_until_pose = (int)((MAP_FP_IDLE_LOWER + (MAP_FP_IDLE_UPPER - MAP_FP_IDLE_LOWER) * random01(v)) * 30.f);
		skip_fraction = MAP_FP_IDLE_SKIP;
		if (++v->ticks_idle > v->ticks_until_pose)
		{
			v->ticks_until_pose = 0;
			if (random01(v) >= skip_fraction) set_state(v, FPV_POSING, in);
		}
	}
	else v->ticks_idle = 0;

	/* first_person_weapon_build_node_matrices: the pose: the state's frame, the ammunition, the moving and the sway overlays, the interpolation */
	memcpy(v->previous_matrices, v->matrices, sizeof(v->matrices));
	state_animation = animation_at(v, v->animation);
	if (state_animation && state_animation->type == 0) sample_base(state_animation, v->frame < state_animation->frames ? v->frame : state_animation->frames - 1, v->orientation, nodes);
	a = fpv_animation_of_type(v, FPW_ANIMATION_AMMUNITION);
	if (a && in->rounds_loaded < a->frames) apply_overlay(a, in->rounds_loaded, 1.f, v->orientation, nodes);
	if (v->moving_animation >= 0) apply_overlay(animation_at(v, v->moving_animation), v->moving_frame, 1.f, v->orientation, nodes);
	if (v->overlay_animation >= 0)
	{
		a = animation_at(v, v->overlay_animation);
		if (v->position[0] > 0.f) apply_overlay(a, 0, v->position[0], v->orientation, nodes); else if (v->position[0] < 0.f) apply_overlay(a, 1, -v->position[0], v->orientation, nodes);
		if (v->position[1] > 0.f) apply_overlay(a, 3, v->position[1], v->orientation, nodes); else if (v->position[1] < 0.f) apply_overlay(a, 2, -v->position[1], v->orientation, nodes);
		if (v->turning[0] > 0.f) apply_overlay(a, 4, v->turning[0], v->orientation, nodes); else if (v->turning[0] < 0.f) apply_overlay(a, 5, -v->turning[0], v->orientation, nodes);
		if (v->turning[1] > 0.f) apply_overlay(a, 7, v->turning[1], v->orientation, nodes); else if (v->turning[1] < 0.f) apply_overlay(a, 6, -v->turning[1], v->orientation, nodes);
		if (v->push_back > 0.f) apply_overlay(a, 8, v->push_back, v->orientation, nodes);
	}
	if (v->interpolation_count > 0)
	{
		float fraction = (float)(v->interpolation_index + 1) / (float)v->interpolation_count, inverse = 1.f - fraction;

		for (n = 0; n < nodes; n++)
		{
			fpv_orientation *t = &v->orientation[n], *o = &v->original[n];
			float dot = o->q[0] * t->q[0] + o->q[1] * t->q[1] + o->q[2] * t->q[2] + o->q[3] * t->q[3], f = dot < 0.f ? -fraction : fraction, len;
			int i;

			t->s = o->s * inverse + t->s * fraction;
			for (i = 0; i < 4; i++) t->q[i] = o->q[i] * inverse + t->q[i] * f;
			len = sqrtf(t->q[0] * t->q[0] + t->q[1] * t->q[1] + t->q[2] * t->q[2] + t->q[3] * t->q[3]);
			if (len > 1e-9f) for (i = 0; i < 4; i++) t->q[i] /= len;
			for (i = 0; i < 3; i++) t->t[i] = o->t[i] * inverse + t->t[i] * fraction;
		}
	}
	/* animation_graph_node_matrices_from_orientations: the node's matrix after its parent's, the root at the camera */
	for (n = 0; n < nodes; n++)
	{
		float local[12];

		matrix_from_orientation(&v->orientation[n], local);
		if (n == 0 || graph->parent[n] < 0 || graph->parent[n] >= n) memcpy(v->matrices[n], local, sizeof(local));
		else matrix_multiply(local, v->matrices[graph->parent[n]], v->matrices[n]);
	}
	if (!v->built) { memcpy(v->previous_matrices, v->matrices, sizeof(v->matrices)); v->built = 1; }
}

/* ---------- a weapon's own display ---------- */

/* the compass part's counter value (objects.c: object_export_function_values, _object_function_compass): the weapon's heading against the scenario's local north,
   0.5 at north, a turn over 0..1. The original reads the weapon's forward (i, j) as atan2 (i, j); the player's yaw is 0 along +x, positive to the left */
float fpv_compass_value(float yaw)
{
	float angle = PI_F * 0.5f - yaw, difference = angle - FPM_LOCAL_NORTH, value;

	while (difference >= PI_F) difference -= 2.f * PI_F;
	while (difference <= -PI_F) difference += 2.f * PI_F;
	value = 0.15915494f * difference + 0.5f;
	return value < 0.f ? 0.f : value > 1.f ? 1.f : value;
}

/* the frame of the numeric map a display part shows (rasterizer_xbox_transparent_geometry.c: counter = limit * the function's value, rounded and pinned, the digit of
   its place by dividing by the frame count once for each place, the frame the remainder); -1 for a part with no frames to choose among */
int fpv_display_frame(const fpm_display_part *part, int rounds_loaded, int loaded_maximum, float compass_value)
{
	float value;
	int counter, place;

	if (part->kind == 0 || part->frames < 2) return -1;
	value = part->kind == 2 ? compass_value : (loaded_maximum > 0 ? (float)rounds_loaded / (float)loaded_maximum : 0.f);
	counter = (int)floorf((float)part->limit * value + 0.5f);
	if (counter < 0) counter = 0; else if (counter > part->limit) counter = part->limit;
	for (place = 0; place < part->place; place++) counter /= part->frames;
	return counter % part->frames;
}
