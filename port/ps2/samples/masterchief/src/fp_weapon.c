/*
FP_WEAPON.C

The player's weapons, ported from source/items/weapons.c, the weapon parts of source/units/units.c and the flight of source/items/projectiles.c.

What is ported: the weapon_update loop (the weapon's ready timer; the triggers down and the secondary trigger's modes; the reload request; each
magazine's reloading and chambering; each trigger's state machine: idle, overloading, charging, charged, recovering, spewing, locked,
tracking), the rate of fire and error ramps, weapon_trigger_can_fire_again's cadence, firing (rounds out of the magazine, heat, age, misfire,
the shot's error cone, the horizontal fan of a shotgun's pellets, the alternate shots of the plasma pistol), reloads and the ready and put-away
times taken from the first-person animation frame counts, heat and overheating, zoom (weapon_rotate_zoom_level, magnification, field of
view), the unit's two weapons, switching, melee and the flight of a projectile through the map's collision geometry.

What is not: the effects (the firing, reloading and impact effects, sound, light), damage (the Chief does not take any), the third-person
weapon's own animations (they time only the recoil state, which only the weapons that have a graph of their own use, and only block a
reload for a few ticks), vehicles' weapons, picking weapons up, the AI's use of a weapon, and a projectile's detonation (the rocket's
blast, a grenade's fuse): a projectile that hits something is an event.
*/

#include <math.h>
#include <string.h>

#include "map_assets.h"
#include "fp_controller.h"
#include "fp_weapon.h"

#define TICKS_PER_SECOND 30.f
#define GRAVITY 0.0035651792f
#define MAGAZINE_WASTES_ROUNDS 1u
#define MAGAZINE_CHAMBERED_EVERY_SHOT 2u
#define OVERCHARGED_EXPLODES 1
#define OVERCHARGED_FIRE 2
#define SECONDARY_SLAVED_TO_PRIMARY 1
#define SECONDARY_INHIBITS_PRIMARY 2
#define SECONDARY_LOADS_ALTERNATE 3
#define SECONDARY_LOADS_MULTIPLE_PRIMARY 4
#define WEAPON_TYPE_SHOTGUN 1
#define WEAPON_TYPE_PLASMA_PISTOL 3
#define TRIGGER_RELEASED_SINCE_LAST_SHOT 1u
#define TRIGGER_FIRED_BEFORE_CHARGING 2u
#define TRIGGER_WAS_DOWN 4u
#define TRIGGER_TOGGLED 8u
#define MELEE_RANGE 0.9f
#define MAX_FLIGHT_TICKS 300

static int ticks_of(float seconds) { return (int)(seconds * TICKS_PER_SECOND); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------- random numbers: a seeded sequence, the same every run ---------- */

static float random01(fpw_inventory *inv)
{
	inv->random_seed = inv->random_seed * 1664525u + 1013904223u;
	return (float)((inv->random_seed >> 8) & 0xffff) / 65535.f;
}

static float random_range(fpw_inventory *inv, float lo, float hi) { return lo + (hi - lo) * random01(inv); }

/* ---------- events ---------- */

static void add_event(fpw_events *events, int type, int a, int b, const float *position, const float *normal)
{
	fpw_event *e;

	if (!events || events->count >= FPW_MAX_EVENTS) return;
	e = &events->list[events->count++];
	e->type = type; e->a = a; e->b = b;
	if (position) memcpy(e->position, position, sizeof(e->position)); else memset(e->position, 0, sizeof(e->position));
	if (normal) memcpy(e->normal, normal, sizeof(e->normal)); else memset(e->normal, 0, sizeof(e->normal));
}

/* ---------- the weapon ---------- */

int fpw_animation_ticks(int weapon, int type)
{
	if (weapon < 0 || weapon >= FPW_WEAPONS || type < 0 || type >= FPW_ANIMATION_TYPES) return 0;
	return fpw_weapons[weapon].animations[type].frames;
}

/* weapons.c: weapon_get_first_person_animation_time. A shotgun's first round of a reload takes the enter animation's frames. */
static int reload_ticks(const fpw_state *w, int shotgun_reload_type)
{
	int ticks = fpw_animation_ticks(w->weapon, FPW_ANIMATION_RELOAD_WHILE_EMPTY);

	if (fpw_weapons[w->weapon].weapon_type == WEAPON_TYPE_SHOTGUN && (shotgun_reload_type == 0 || shotgun_reload_type == 2))
		ticks = fpw_animation_ticks(w->weapon, FPW_ANIMATION_SHOTGUN_ENTER);
	return ticks;
}

/* weapons.c: weapon_new */
static void weapon_new(fpw_state *w, int weapon)
{
	const fpw_weapon *def = &fpw_weapons[weapon];
	int i;

	memset(w, 0, sizeof(*w));
	w->weapon = weapon;
	for (i = 0; i < def->magazine_count; i++)
	{
		int loaded = def->magazines[i].total_initial < def->magazines[i].loaded_maximum ? def->magazines[i].total_initial : def->magazines[i].loaded_maximum;

		w->magazines[i].rounds_loaded = loaded;
		w->magazines[i].rounds_total = def->magazines[i].total_initial - loaded;
	}
	for (i = 0; i < def->trigger_count; i++) w->triggers[i].idle_ticks = 127;
}

static int magazine_state_change_ok(const fpw_state *w)
{
	return w->triggers[0].state == FPW_TRIGGER_IDLE && w->triggers[1].state == FPW_TRIGGER_IDLE;
}

static int weapon_busy(const fpw_state *w)
{
	return w->triggers[0].state != FPW_TRIGGER_IDLE || w->triggers[1].state != FPW_TRIGGER_IDLE || w->magazines[0].state != FPW_MAGAZINE_IDLE || w->magazines[1].state != FPW_MAGAZINE_IDLE;
}

static void magazine_finish_reload(fpw_state *w, int m, fpw_events *events);

/* weapons.c: weapon_reset */
static void weapon_reset(fpw_state *w, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	int i;

	for (i = 0; i < def->trigger_count; i++) { w->triggers[i].state = FPW_TRIGGER_UNINITIALIZED; w->triggers[i].timer = 0; }
	for (i = 0; i < def->magazine_count; i++)
	{
		if (w->magazines[i].state == FPW_MAGAZINE_RELOADING && 2 * w->magazines[i].timer < fpw_animation_ticks(w->weapon, FPW_ANIMATION_RELOAD_WHILE_EMPTY))
			magazine_finish_reload(w, i, events);       /* put away over half way through a reload: it finished */
		w->magazines[i].state = FPW_MAGAZINE_IDLE;
		w->magazines[i].timer = 0;
	}
}

/* weapons.c: weapon_magazine_start_reload */
static void magazine_start_reload(fpw_state *w, int m, int unknown, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	fpw_magazine_state *mag = &w->magazines[m];
	const fpw_magazine *md = &def->magazines[m];

	if (m >= def->magazine_count) return;
	if ((mag->state == FPW_MAGAZINE_IDLE || mag->state == FPW_MAGAZINE_UNCHAMBERED) && magazine_state_change_ok(w))
	{
		if (mag->rounds_total > 0 && mag->rounds_loaded < md->loaded_maximum)
		{
			int reload_type = -1;

			if (def->weapon_type == WEAPON_TYPE_SHOTGUN)
			{
				if (unknown) reload_type = md->loaded_maximum - mag->rounds_loaded == 1 ? 2 : 0;
				else reload_type = md->loaded_maximum - mag->rounds_loaded == 1 ? 1 : -1;
			}
			mag->state = FPW_MAGAZINE_RELOADING;
			mag->original_time = mag->timer = reload_ticks(w, reload_type);
			add_event(events, FPW_EVENT_RELOAD_START, mag->rounds_loaded != 0, m, NULL, NULL);
		}
		w->flags &= ~FPW_FLAG_NEEDS_TO_RELOAD;
	}
}

/* weapons.c: weapon_magazine_finish_reload */
static void magazine_finish_reload(fpw_state *w, int m, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	fpw_magazine_state *mag = &w->magazines[m];
	const fpw_magazine *md = &def->magazines[m];
	int to_load, loaded;

	if (md->flags & MAGAZINE_WASTES_ROUNDS) mag->rounds_loaded = 0;
	to_load = md->rounds_reloaded > mag->rounds_total ? mag->rounds_total : md->rounds_reloaded;
	loaded = mag->rounds_loaded + to_load;
	if (loaded > md->loaded_maximum) loaded = md->loaded_maximum;
	mag->rounds_total = mag->rounds_total - loaded + mag->rounds_loaded;
	mag->rounds_loaded = loaded;
	mag->state = FPW_MAGAZINE_UNCHAMBERED;
	mag->timer = 0;
	add_event(events, FPW_EVENT_RELOAD_FINISH, loaded, m, NULL, NULL);
	if (mag->rounds_total > 0 && mag->rounds_loaded < md->loaded_maximum && !(md->flags & MAGAZINE_WASTES_ROUNDS) && !(w->control_flags & (FPW_CONTROL_PRIMARY_TRIGGER | FPW_CONTROL_SECONDARY_TRIGGER)))
		magazine_start_reload(w, m, 0, events);       /* a shotgun loads another shell */
}

/* weapons.c: weapon_magazine_start_chamber / finish_chamber */
static void magazine_start_chamber(fpw_state *w, int m, fpw_events *events)
{
	fpw_magazine_state *mag = &w->magazines[m];

	if ((mag->state == FPW_MAGAZINE_IDLE || mag->state == FPW_MAGAZINE_UNCHAMBERED) && magazine_state_change_ok(w))
	{
		mag->state = FPW_MAGAZINE_CHAMBERING;
		mag->timer = ticks_of(fpw_weapons[w->weapon].magazines[m].chamber_time);
		add_event(events, FPW_EVENT_CHAMBER, m, 0, NULL, NULL);
	}
}

static void trigger_change_state(fpw_state *w, int t, int state, int timer) { w->triggers[t].state = state; w->triggers[t].timer = timer; }
static void trigger_idle(fpw_state *w, int t) { trigger_change_state(w, t, FPW_TRIGGER_IDLE, 0); }
static void trigger_recover(fpw_state *w, int t) { w->triggers[t].idle_ticks = 0; trigger_idle(w, t); }

/* ---------- firing ---------- */

/* a unit vector rotated about an axis (Rodrigues) */
static void rotate_about_axis(float *v, const float *axis, float s, float c)
{
	float cross[3] = { axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0] };
	float along = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2], out[3];
	int i;

	for (i = 0; i < 3; i++) out[i] = v[i] * c + cross[i] * s + axis[i] * along * (1.f - c);
	memcpy(v, out, sizeof(out));
}

static void normalise3(float *v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* math/random_math.c: seed_random_vector_in_cone3d: a random axis across the cone's axis, the angle between the inner and outer */
static void random_vector_in_cone(fpw_inventory *inv, float *forward, float inner, float outer)
{
	float direction[3], axis[3], angle;

	do
	{
		direction[0] = random_range(inv, -1.f, 1.f); direction[1] = random_range(inv, -1.f, 1.f); direction[2] = random_range(inv, -1.f, 1.f);
	} while (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2] > 1.f || direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2] < 1e-4f);
	normalise3(direction);
	axis[0] = forward[1] * direction[2] - forward[2] * direction[1];
	axis[1] = forward[2] * direction[0] - forward[0] * direction[2];
	axis[2] = forward[0] * direction[1] - forward[1] * direction[0];
	if (sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) > 1e-6f)
	{
		normalise3(axis);
		angle = random_range(inv, inner, outer);
		rotate_about_axis(forward, axis, sinf(angle), cosf(angle));
	}
}

static void spawn_projectile(fpw_inventory *inv, int projectile, const float *origin, const float *direction)
{
	int i;
	const fpw_projectile *def;

	if (projectile < 0) return;
	def = &fpw_projectiles[projectile];
	for (i = 0; i < FPW_MAX_PROJECTILES; i++)
		if (!inv->projectiles[i].active)
		{
			fpw_projectile_state *p = &inv->projectiles[i];
			int k;

			memset(p, 0, sizeof(*p));
			p->active = 1; p->projectile = projectile;
			for (k = 0; k < 3; k++) { p->position[k] = origin[k]; p->velocity[k] = direction[k] * def->initial_velocity + inv->aim.velocity[k]; }
			return;
		}
}

/* weapons.c: trigger_create_projectiles: the shot's projectiles from the player's eye along its aim, within the error cone */
static void trigger_create_projectiles(fpw_inventory *inv, fpw_state *w, int t)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	const fpw_trigger *td = &def->triggers[t];
	fpw_trigger_state *trigger = &w->triggers[t];
	int projectile = td->projectile, count = td->projectiles_per_shot, i;
	float first_forward[3] = { 0, 0, 0 };

	if (t == 0 && w->alternate_shots_loaded > 0)
	{
		int alternate = w->alternate_shots_loaded;

		projectile = def->triggers[1].projectile;
		if (def->secondary_trigger_mode == SECONDARY_LOADS_MULTIPLE_PRIMARY) alternate++;
		count = td->projectiles_per_shot * alternate;
		w->alternate_shots_loaded = 0;
	}
	for (i = 0; i < count; i++)
	{
		float forward[3] = { inv->aim.forward[0], inv->aim.forward[1], inv->aim.forward[2] }, origin[3], right[3], up[3], fraction, error;
		int k;

		if (trigger->rate_of_fire == 0.f || trigger->sequential_non_tracer_rounds++ >= td->rounds_between_tracers) trigger->sequential_non_tracer_rounds = 0;
		fraction = (td->flags & FPW_TRIGGER_ANALOG_RATE_OF_FIRE) ? w->primary_trigger : trigger->error;
		error = (1.f - fraction) * td->error_lower + fraction * td->error_upper;
		if (!(td->flags & FPW_TRIGGER_USE_ERROR_WHEN_UNZOOMED) || !(w->control_flags & FPW_CONTROL_ZOOMED))
			random_vector_in_cone(inv, forward, td->error_inner_cone, error);
		if (i == 0) memcpy(first_forward, forward, sizeof(forward));
		if (td->flags & FPW_TRIGGER_PROJECTILES_HAVE_IDENTICAL_ERROR) memcpy(forward, first_forward, sizeof(forward));
		/* the distribution: a horizontal fan about an up perpendicular to the aim */
		right[0] = inv->aim.forward[1]; right[1] = -inv->aim.forward[0]; right[2] = 0.f;
		if (sqrtf(right[0] * right[0] + right[1] * right[1]) < 1e-6f) { right[0] = 0.f; right[1] = -1.f; }
		normalise3(right);
		up[0] = right[1] * inv->aim.forward[2] - right[2] * inv->aim.forward[1]; up[1] = right[2] * inv->aim.forward[0] - right[0] * inv->aim.forward[2]; up[2] = right[0] * inv->aim.forward[1] - right[1] * inv->aim.forward[0];
		normalise3(up);
		if (td->projectile_distribution_function == 1)
		{
			float offset;

			if (count & 1)
			{
				if (i == 0) offset = 0.f;
				else { int step = i - 1; step = (step & 1) ? step >> 1 : -(step >> 1); offset = (float)step; }
			}
			else { offset = (float)(i >> 1) - 0.5f; if (i & 1) offset = -offset; }
			rotate_about_axis(forward, up, sinf(offset * td->projectile_distribution_angle), cosf(offset * td->projectile_distribution_angle));
		}
		for (k = 0; k < 3; k++) origin[k] = inv->aim.origin[k] + inv->aim.forward[k] * td->first_person_offset[0] + right[k] * td->first_person_offset[1] + up[k] * td->first_person_offset[2];
		spawn_projectile(inv, projectile, origin, forward);
	}
}

static void weapon_detonate(fpw_state *w, fpw_events *events)
{
	add_event(events, FPW_EVENT_DETONATE, w->weapon, 0, NULL, NULL);
	w->heat = 0.f;
}

/* weapons.c: weapon_trigger_fire */
static void trigger_fire(fpw_inventory *inv, fpw_state *w, int t, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	const fpw_trigger *td = &def->triggers[t];
	fpw_trigger_state *trigger = &w->triggers[t];
	int fired = 0, misfired = 0, loads_alternate = t == 1 && (def->secondary_trigger_mode == SECONDARY_LOADS_ALTERNATE || def->secondary_trigger_mode == SECONDARY_LOADS_MULTIPLE_PRIMARY);

	if (td->magazine >= 0 && td->magazine < def->magazine_count)
	{
		fpw_magazine_state *mag = &w->magazines[td->magazine];

		if (!loads_alternate || w->alternate_shots_loaded < def->maximum_alternate_shots_loaded)
		{
			if ((mag->rounds_loaded >= td->rounds_per_shot || (td->flags & FPW_TRIGGER_CAN_FIRE_WITH_PARTIAL_AMMUNITION)) &&
			    !((def->flags & FPW_WEAPON_CANNOT_FIRE_AT_MAXIMUM_AGE) && w->age >= 1.f) &&
			    (mag->rounds_loaded >= td->minimum_rounds_loaded || !(trigger->flags & TRIGGER_RELEASED_SINCE_LAST_SHOT)))
			{
				mag->rounds_loaded -= td->rounds_per_shot;
				if (mag->rounds_loaded <= 0) mag->rounds_loaded = 0;
				else if (def->magazines[td->magazine].flags & MAGAZINE_CHAMBERED_EVERY_SHOT) { mag->state = FPW_MAGAZINE_UNCHAMBERED; mag->timer = 0; }
				fired = 1;
			}
		}
	}
	else fired = 1;

	/* the weapon's age makes it misfire (the plasma weapons') */
	if (def->age[2] > 0.f && def->age[2] < 1.f && w->age > def->age[2])
	{
		float chance = (w->age - def->age[2]) * def->age[3] / (1.f - def->age[2]);

		if (trigger->state == FPW_TRIGGER_SPEWING) chance *= 2.f;
		if (random01(inv) < chance) misfired = 1;
	}

	if (fired)
	{
		add_event(events, misfired ? FPW_EVENT_MISFIRE : FPW_EVENT_FIRE, t, w->weapon, inv->aim.origin, inv->aim.forward);
		if (td->illumination_recovery_time > 0.f) trigger->illumination = 1.f;
		trigger->ejection_port = 1.f;
		w->heat += td->heat_per_round;
		if (w->heat > 1.f) w->heat = 1.f;
		w->age += td->age_per_round;
		if (w->age > 1.f) w->age = 1.f;
		if (!misfired)
		{
			if (loads_alternate) w->alternate_shots_loaded++;
			else trigger_create_projectiles(inv, w, t);
		}
	}
	if (w->heat > def->heat[2] && def->heat[2] > 0.f && random01(inv) < def->heat[3]) weapon_detonate(w, events);

	if (!fired)
	{
		trigger_change_state(w, t, FPW_TRIGGER_LOCKED, -1);
		add_event(events, FPW_EVENT_EMPTY, t, 0, NULL, NULL);
	}
	else if (trigger->state != FPW_TRIGGER_SPEWING || misfired)
	{
		if (td->flags & FPW_TRIGGER_TRACKS_PROJECTILE) trigger_change_state(w, t, FPW_TRIGGER_TRACKING, -1);
		else trigger_recover(w, t);
	}
	trigger->flags &= ~TRIGGER_RELEASED_SINCE_LAST_SHOT;
}

/* weapons.c: weapon_trigger_can_fire_again: the ticks since the last shot against the cadence of the rate of fire */
static int trigger_can_fire_again(const fpw_state *w, int t)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	const fpw_trigger *td = &def->triggers[t];
	const fpw_trigger_state *trigger = &w->triggers[t];
	float fraction = (td->flags & FPW_TRIGGER_ANALOG_RATE_OF_FIRE) ? w->primary_trigger : trigger->rate_of_fire;
	float rate = (td->rate_of_fire[1] - td->rate_of_fire[0]) * fraction + td->rate_of_fire[0];
	float required = rate > 0.0001f ? TICKS_PER_SECOND / rate : 0.f;

	if (def->age[1] > 0.f) required = (w->age * def->age[1] + 1.f) * required;
	if ((td->flags & FPW_TRIGGER_LATCHED) && !(trigger->flags & TRIGGER_RELEASED_SINCE_LAST_SHOT)) return 0;
	return (float)trigger->idle_ticks + 1.f >= required;
}

/* weapons.c: weapon_trigger_begin_firing */
static void trigger_begin_firing(fpw_inventory *inv, fpw_state *w, int t, int force, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	const fpw_trigger *td = &def->triggers[t];
	fpw_trigger_state *trigger = &w->triggers[t];
	int can_fire = 1;

	if (td->magazine >= 0)
	{
		if (td->magazine >= def->magazine_count || w->magazines[td->magazine].state != FPW_MAGAZINE_IDLE) can_fire = 0;
	}
	if (w->flags & FPW_FLAG_OVERHEATED) can_fire = 0;
	if (!can_fire) return;
	if (!force && td->charging_time > 0.f)
	{
		if ((def->flags & FPW_WEAPON_CANNOT_FIRE_AT_MAXIMUM_AGE) && w->age >= 1.f) trigger_fire(inv, w, t, events);
		else
		{
			if (def->trigger_count <= 1 && trigger->rate_of_fire > 0.f) { trigger->flags |= TRIGGER_FIRED_BEFORE_CHARGING; trigger_fire(inv, w, t, events); }
			else if (def->trigger_count <= 1) trigger->flags &= ~TRIGGER_FIRED_BEFORE_CHARGING;
			trigger_change_state(w, t, FPW_TRIGGER_CHARGING, ticks_of(td->charging_time));
		}
	}
	else if (!force && td->overloading_time > 0.f) trigger_change_state(w, t, FPW_TRIGGER_OVERLOADING, ticks_of(td->overloading_time));
	else trigger_fire(inv, w, t, events);
}

static void trigger_release_charge(fpw_inventory *inv, fpw_state *w, int t, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	const fpw_trigger *td = &def->triggers[t];

	if (td->spew_time > 0.f) trigger_change_state(w, t, FPW_TRIGGER_SPEWING, ticks_of(td->spew_time));
	else
	{
		if (def->trigger_count > 1) trigger_fire(inv, w, 1, events);
		trigger_recover(w, t);
	}
	w->triggers[t].rate_of_fire = 0.f;
}

/* weapons.c: weapon_update */
static void weapon_update(fpw_inventory *inv, fpw_state *w, fpw_events *events)
{
	const fpw_weapon *def = &fpw_weapons[w->weapon];
	int triggers_down[2], m, t;

	/* heat */
	if (w->heat > 0.f)
	{
		if (w->heat >= def->heat[1] && !(w->flags & FPW_FLAG_OVERHEATED))
		{
			w->flags |= FPW_FLAG_OVERHEATED;
			add_event(events, FPW_EVENT_OVERHEATING, w->weapon, 0, NULL, NULL);
		}
		if (w->overcharged == 0.f)
		{
			float loss = def->heat[4] * (1.f / TICKS_PER_SECOND);

			if (def->age[0] > 0.f) loss *= 1.f - w->age * def->age[0];
			w->heat -= loss;
			if (w->heat < 0.f) w->heat = 0.f;
			if ((w->flags & FPW_FLAG_OVERHEATED) && !(w->flags & FPW_FLAG_OVERHEATED_EXIT) && loss > 0.f && (w->heat - def->heat[0]) / loss <= 1.f) w->flags |= FPW_FLAG_OVERHEATED_EXIT;
		}
		if ((w->flags & FPW_FLAG_OVERHEATED) && w->heat < def->heat[0]) w->flags &= ~(FPW_FLAG_OVERHEATED | FPW_FLAG_OVERHEATED_EXIT);
	}
	w->overcharged = 0.f;
	if (w->state_timer > 0) w->state_timer--;

	if (!(w->control_flags & FPW_CONTROL_USER_BUSY) && w->state_timer <= 0)
	{
		triggers_down[0] = (w->control_flags & FPW_CONTROL_PRIMARY_TRIGGER) != 0;
		triggers_down[1] = (def->flags & FPW_WEAPON_SECONDARY_TRIGGER_OVERRIDES_GRENADES) && (w->control_flags & FPW_CONTROL_SECONDARY_TRIGGER);
	}
	else triggers_down[0] = triggers_down[1] = 0;
	if (def->secondary_trigger_mode == SECONDARY_SLAVED_TO_PRIMARY) { if (triggers_down[1] && def->trigger_count > 0 && w->triggers[0].rate_of_fire != 1.f) triggers_down[1] = 0; }
	else if (def->secondary_trigger_mode == SECONDARY_INHIBITS_PRIMARY) { if (triggers_down[1]) triggers_down[0] = 0; }

	if ((w->control_flags & FPW_CONTROL_RELOAD) && def->magazine_count > 0) w->flags |= FPW_FLAG_NEEDS_TO_RELOAD;
	if (w->flags & FPW_FLAG_NEEDS_TO_RELOAD) magazine_start_reload(w, 0, 1, events);

	for (m = 0; m < def->magazine_count; m++)
	{
		fpw_magazine_state *mag = &w->magazines[m];
		const fpw_magazine *md = &def->magazines[m];

		if (md->recharged_per_second > 0 && mag->rounds_loaded < md->loaded_maximum)
		{
			mag->rounds_loaded += md->recharged_per_second / 30;
			mag->fractional_recharged += md->recharged_per_second % 30;
			if (mag->fractional_recharged >= 30) { mag->rounds_loaded++; mag->fractional_recharged -= 30; }
			if (mag->rounds_loaded > md->loaded_maximum) mag->rounds_loaded = md->loaded_maximum;
		}
		if (mag->timer) mag->timer--;
		switch (mag->state)
		{
		case FPW_MAGAZINE_RELOADING: if (mag->timer - 1 <= 0) magazine_finish_reload(w, m, events); break;
		case FPW_MAGAZINE_UNCHAMBERED: magazine_start_chamber(w, m, events); break;
		case FPW_MAGAZINE_CHAMBERING: if (!mag->timer) { mag->state = FPW_MAGAZINE_IDLE; mag->timer = 0; } break;
		}
	}

	for (t = 0; t < def->trigger_count; t++)
	{
		fpw_trigger_state *trigger = &w->triggers[t];
		const fpw_trigger *td = &def->triggers[t];

		if ((td->flags & FPW_TRIGGER_ANALOG_RATE_OF_FIRE)) triggers_down[t] = w->primary_trigger > 0.05f;
		if (trigger->timer > 0) trigger->timer--;
		if (td->flags & FPW_TRIGGER_TOGGLES)
		{
			if (!(trigger->flags & TRIGGER_WAS_DOWN) && triggers_down[t]) trigger->flags ^= TRIGGER_TOGGLED;
			if (triggers_down[t]) trigger->flags |= TRIGGER_WAS_DOWN; else trigger->flags &= ~TRIGGER_WAS_DOWN;
			triggers_down[t] = (trigger->flags & TRIGGER_TOGGLED) != 0;
		}
		if (!triggers_down[t]) trigger->flags |= TRIGGER_RELEASED_SINCE_LAST_SHOT;
		if (trigger->ejection_port > 0.f) { trigger->ejection_port -= td->runtime[1]; if (trigger->ejection_port < 0.f) trigger->ejection_port = 0.f; }
		if (trigger->illumination > 0.f) { trigger->illumination -= td->runtime[0]; if (trigger->illumination < 0.f) trigger->illumination = 0.f; }

		switch (trigger->state)
		{
		case FPW_TRIGGER_IDLE:
			if (!(w->control_flags & FPW_CONTROL_USER_BUSY) && td->magazine >= 0 && td->magazine < def->magazine_count)
			{
				const fpw_magazine_state *mag = &w->magazines[td->magazine];

				if ((mag->rounds_loaded < td->rounds_per_shot && !(td->flags & FPW_TRIGGER_CAN_FIRE_WITH_PARTIAL_AMMUNITION)) || mag->rounds_loaded < td->minimum_rounds_loaded || mag->rounds_loaded == 0)
					magazine_start_reload(w, td->magazine, 1, events);
			}
			if (triggers_down[t] && trigger_can_fire_again(w, t)) trigger_begin_firing(inv, w, t, 0, events);
			else if (trigger->idle_ticks < 127) trigger->idle_ticks++;
			break;
		case FPW_TRIGGER_SPEWING:
			if (trigger->timer) trigger_begin_firing(inv, w, t, 1, events); else trigger_recover(w, t);
			break;
		case FPW_TRIGGER_OVERLOADING:
			if (!triggers_down[t]) trigger_begin_firing(inv, w, t, 1, events);
			else if (!trigger->timer && w->alternate_shots_loaded < def->maximum_alternate_shots_loaded)
			{
				if (t + 1 < def->trigger_count) trigger_fire(inv, w, t + 1, events);
				trigger_change_state(w, t, FPW_TRIGGER_OVERLOADING, ticks_of(td->overloading_time));
			}
			break;
		case FPW_TRIGGER_CHARGING:
			if (trigger->timer)
			{
				if (!triggers_down[t])
				{
					if (t == 0 && def->trigger_count > 1 && !(trigger->flags & TRIGGER_FIRED_BEFORE_CHARGING)) trigger_begin_firing(inv, w, t, 1, events);
					else trigger_idle(w, t);
				}
			}
			else
			{
				trigger_change_state(w, t, FPW_TRIGGER_CHARGED, ticks_of(td->charged_time));
				add_event(events, FPW_EVENT_CHARGED, t, 0, NULL, NULL);
			}
			break;
		case FPW_TRIGGER_CHARGED:
			if (triggers_down[t])
			{
				w->overcharged = td->charged_time > 0.f ? 1.f - (trigger->timer * (1.f / TICKS_PER_SECOND)) / td->charged_time : 1.f;
				if (trigger->timer)
				{
					if (td->magazine >= 0 && td->magazine < def->magazine_count && w->magazines[td->magazine].rounds_loaded < td->rounds_per_shot && !(td->flags & FPW_TRIGGER_CAN_FIRE_WITH_PARTIAL_AMMUNITION))
						trigger_release_charge(inv, w, t, events);
				}
				else if (td->overcharged_action == OVERCHARGED_EXPLODES) weapon_detonate(w, events);
				else if (td->overcharged_action == OVERCHARGED_FIRE) trigger_release_charge(inv, w, t, events);
			}
			else trigger_release_charge(inv, w, t, events);
			break;
		case FPW_TRIGGER_RECOVERING:
			if (!trigger->timer)
			{
				if ((td->flags & FPW_TRIGGER_LATCHED) && !(trigger->flags & TRIGGER_RELEASED_SINCE_LAST_SHOT)) trigger_change_state(w, t, FPW_TRIGGER_LOCKED, -1);
				else trigger_idle(w, t);
			}
			break;
		case FPW_TRIGGER_TRACKING:
			if (!triggers_down[t]) { trigger_recover(w, t); }
			break;
		case FPW_TRIGGER_LOCKED:
			if (!triggers_down[t]) trigger_idle(w, t);
			break;
		case FPW_TRIGGER_UNINITIALIZED:
			if (!trigger->timer) trigger_idle(w, t);
			break;
		}

		if (triggers_down[t])
		{
			trigger->rate_of_fire += td->runtime[2];
			if (trigger->rate_of_fire > 1.f) trigger->rate_of_fire = 1.f;
		}
		else
		{
			trigger->rate_of_fire -= td->runtime[3];
			if (trigger->rate_of_fire < 0.f) trigger->rate_of_fire = 0.f;
		}
		if (trigger->state == FPW_TRIGGER_SPEWING || trigger->state == FPW_TRIGGER_RECOVERING || triggers_down[t])
		{
			trigger->error += td->runtime[4];
			if (trigger->error > 1.f) trigger->error = 1.f;
		}
		else
		{
			trigger->error -= td->runtime[5];
			if (trigger->error < 0.f) trigger->error = 0.f;
		}
	}
}

/* ---------- the unit's weapons ---------- */

static void weapon_ready(fpw_state *w, fpw_events *events)
{
	weapon_reset(w, events);
	w->state_timer = fpw_animation_ticks(w->weapon, FPW_ANIMATION_READY);
	add_event(events, FPW_EVENT_READY, w->weapon, 0, NULL, NULL);
}

void fpw_inventory_init(fpw_inventory *inv, const int *weapons, int count, unsigned seed)
{
	int i;

	memset(inv, 0, sizeof(*inv));
	inv->random_seed = seed ? seed : 1u;
	inv->slot_count = count > 2 ? 2 : count;
	for (i = 0; i < inv->slot_count; i++) weapon_new(&inv->slots[i], weapons[i]);
	inv->current = inv->desired = 0;
	inv->switch_timer = -1;
	inv->zoom_level = -1;
	if (inv->slot_count) weapon_ready(&inv->slots[0], NULL);
}

const fpw_weapon *fpw_current(const fpw_inventory *inv) { return inv->slot_count ? &fpw_weapons[inv->slots[inv->current].weapon] : NULL; }
fpw_state *fpw_current_state(fpw_inventory *inv) { return inv->slot_count ? &inv->slots[inv->current] : NULL; }

float fpw_zoom_magnification(const fpw_inventory *inv)
{
	const fpw_weapon *def = fpw_current(inv);
	float magnification = 1.f;

	if (def && inv->zoom_level >= 0 && inv->zoom_level < def->zoom_level_count)
	{
		float fraction = def->zoom_level_count > 1 ? (float)inv->zoom_level / (def->zoom_level_count - 1) : 0.f;
		float lo = def->zoom_magnification_minimum > 0.f ? def->zoom_magnification_minimum : 1.f, hi = def->zoom_magnification_maximum > 0.f ? def->zoom_magnification_maximum : 1.f;

		magnification = powf(hi / lo, fraction) * lo;
	}
	return magnification;
}

/* weapons.c: weapon_get_field_of_view */
float fpw_field_of_view(const fpw_inventory *inv, float unit_field_of_view)
{
	float magnification = fpw_zoom_magnification(inv), zoomed;

	if (magnification != 1.f)
	{
		zoomed = unit_field_of_view / magnification;
		if (zoomed > (float)M_PI / 100.f && zoomed < (float)M_PI - (float)M_PI / 100.f) return zoomed;
	}
	return unit_field_of_view;
}

/* weapons.c: weapon_rotate_zoom_level: none, the first level, ... the last, none; not while the weapon reloads */
static int rotate_zoom_level(const fpw_inventory *inv)
{
	const fpw_weapon *def = fpw_current(inv);
	const fpw_state *w = &inv->slots[inv->current];
	int level = inv->zoom_level;

	if (def->magazine_count > 0 && w->magazines[0].state == FPW_MAGAZINE_RELOADING) return level;
	if (level >= 0 && level < def->zoom_level_count - 1) return level + 1;
	return level == def->zoom_level_count - 1 ? -1 : 0;
}

/* ---------- projectiles ---------- */

static void projectiles_tick(fpw_inventory *inv, fpw_events *events)
{
	int i;

	for (i = 0; i < FPW_MAX_PROJECTILES; i++)
	{
		fpw_projectile_state *p = &inv->projectiles[i];
		const fpw_projectile *def;
		float next[3], t, normal[3], speed, travel;
		unsigned tri;
		int k;

		if (!p->active) continue;
		def = &fpw_projectiles[p->projectile];
		p->velocity[2] -= GRAVITY * def->air_gravity_scale;
		for (k = 0; k < 3; k++) next[k] = p->position[k] + p->velocity[k];
		speed = sqrtf(p->velocity[0] * p->velocity[0] + p->velocity[1] * p->velocity[1] + p->velocity[2] * p->velocity[2]);
		if (fp_trace(p->position, next, &t, normal, &tri))
		{
			float hit[3];

			for (k = 0; k < 3; k++) hit[k] = p->position[k] + (next[k] - p->position[k]) * t;
			add_event(events, FPW_EVENT_IMPACT, p->projectile, tri >= MAP_COL_TRIS - MAP_COL_CHIEF_TRIS ? 2 : (tri >= MAP_COL_STRUCTURE_TRIS ? 1 : 0), hit, normal);
			p->active = 0;
			continue;
		}
		travel = speed;
		memcpy(p->position, next, sizeof(next));
		p->travelled += travel;
		p->age_ticks++;
		if ((def->maximum_range != 0.f && p->travelled > def->maximum_range) || p->age_ticks > MAX_FLIGHT_TICKS || speed < def->minimum_velocity * 0.f) p->active = 0;
	}
}

/* ---------- the tick ---------- */

void fpw_inventory_tick(fpw_inventory *inv, const fpw_input *input, fpw_events *events)
{
	fpw_state *w;
	const fpw_weapon *def;

	if (!inv->slot_count) { projectiles_tick(inv, events); return; }
	w = &inv->slots[inv->current];
	def = &fpw_weapons[w->weapon];

	/* switching: the weapon is put away (if it is not busy), then the other readied */
	if (inv->switch_timer >= 0)
	{
		if (inv->switch_timer-- == 0)
		{
			inv->current = inv->desired;
			w = &inv->slots[inv->current];
			def = &fpw_weapons[w->weapon];
			inv->switch_timer = -1;
			weapon_ready(w, events);
			inv->zoom_level = -1;
		}
	}
	else if (input->switch_weapon && inv->slot_count > 1 && !weapon_busy(w) && !inv->melee_timer)
	{
		inv->desired = 1 - inv->current;
		w->control_flags = 0;
		weapon_reset(w, events);
		inv->switch_timer = fpw_animation_ticks(w->weapon, FPW_ANIMATION_PUT_AWAY);
		add_event(events, FPW_EVENT_PUT_AWAY, w->weapon, 0, NULL, NULL);
		if (inv->zoom_level >= 0) { inv->zoom_level = -1; add_event(events, FPW_EVENT_ZOOM, -1, 0, NULL, NULL); }
	}

	/* zoom */
	if (input->zoom && def->zoom_level_count > 0 && inv->switch_timer < 0)
	{
		int level = rotate_zoom_level(inv);

		if (level != inv->zoom_level) { inv->zoom_level = level; add_event(events, FPW_EVENT_ZOOM, level, 0, NULL, NULL); }
	}

	/* melee: the weapon's melee animation busies it; its key frame is the blow */
	if (input->melee && !inv->melee_timer && inv->switch_timer < 0 && !(def->flags & FPW_WEAPON_PREVENTS_MELEE_ATTACK) && fpw_animation_ticks(w->weapon, FPW_ANIMATION_MELEE) > 0 && !weapon_busy(w))
	{
		inv->melee_timer = fpw_animation_ticks(w->weapon, FPW_ANIMATION_MELEE);
		inv->melee_key_timer = def->animations[FPW_ANIMATION_MELEE].key_frame;
		add_event(events, FPW_EVENT_MELEE, w->weapon, 0, NULL, NULL);
		if (inv->zoom_level >= 0) { inv->zoom_level = -1; add_event(events, FPW_EVENT_ZOOM, -1, 0, NULL, NULL); }
	}
	if (inv->melee_timer)
	{
		if (inv->melee_key_timer == 0)
		{
			float far_point[3], t, normal[3], hit[3];
			unsigned tri;
			int k;

			for (k = 0; k < 3; k++) far_point[k] = inv->aim.origin[k] + inv->aim.forward[k] * MELEE_RANGE;
			if (fp_trace(inv->aim.origin, far_point, &t, normal, &tri))
			{
				for (k = 0; k < 3; k++) hit[k] = inv->aim.origin[k] + (far_point[k] - inv->aim.origin[k]) * t;
				add_event(events, FPW_EVENT_MELEE_HIT, tri >= MAP_COL_TRIS - MAP_COL_CHIEF_TRIS ? 2 : 0, 0, hit, normal);
			}
		}
		inv->melee_key_timer--;
		inv->melee_timer--;
	}

	/* the unit's controls to the weapon (units.c: unit_update's weapon control flags) */
	w->control_flags = 0;
	if (inv->switch_timer < 0)
	{
		if (input->trigger > 0.f) w->control_flags |= FPW_CONTROL_PRIMARY_TRIGGER;
		if (input->reload) w->control_flags |= FPW_CONTROL_RELOAD;
		if (inv->melee_timer) w->control_flags |= FPW_CONTROL_USER_BUSY;
		if (inv->zoom_level >= 0) w->control_flags |= FPW_CONTROL_ZOOMED;
		w->primary_trigger = input->trigger;
		weapon_update(inv, w, events);
	}
	projectiles_tick(inv, events);
}
