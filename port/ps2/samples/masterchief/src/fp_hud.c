/*
FP_HUD.C

The first-person HUD, ported from source/interface/hud_weapon.c (the weapon HUD's states, flashing, statics, meters, numbers, overlays and crosshairs, the grenade HUD),
hud_unit.c (the shield and health panels, the motion sensor's backgrounds) and hud_draw.c (a corner's placement, a flash's colour, a number's layout, a meter's parameters)
on the tags tools/export_hud.py baked into hud_assets.h. It makes a list of quads (fp_hud.h); main.c draws them.

Not ported: the multitexture overlays of the static elements (the scan lines and fades some of them have), the motion sensor's blips and sweep (the Chief does not move: the
sensor's backgrounds are drawn), the unit's integrated light meter (the flashlight), the damage indicators, the messages and the waypoints, the multiplayer and split screen
elements (an element's use_on_map_type of 2 is left out by the exporter), the autoaim's red reticle (there is no target) and the HUD's sounds.
*/

#include <math.h>
#include <string.h>

#include "fp_hud.h"

#define TICKS_PER_SECOND 30.f

enum { DRAW_FLASHING = 1, DRAW_DISABLED = 2 };
enum { ANCHOR_TOP_LEFT = 0, ANCHOR_TOP_RIGHT, ANCHOR_BOTTOM_LEFT, ANCHOR_BOTTOM_RIGHT, ANCHOR_CENTER };
enum
{
	CROSSHAIR_AIM = 0, CROSSHAIR_ZOOM, CROSSHAIR_CHARGE, CROSSHAIR_FLASH_AMMO, CROSSHAIR_FLASH_HEAT, CROSSHAIR_FLASH_TOTAL_AMMO, CROSSHAIR_FLASH_TOTAL_BATTERY, CROSSHAIR_RELOAD,
	CROSSHAIR_FIRED_WITH_NO_AMMO, CROSSHAIR_THREW_WITH_NO_GRENADE, CROSSHAIR_FLASH_AMMO_NONE_FOR_RELOAD, CROSSHAIR_FLASH_SECONDARY_AMMO, CROSSHAIR_FLASH_SECONDARY_TOTAL_AMMO,
	CROSSHAIR_SECONDARY_RELOAD, CROSSHAIR_FIRED_SECONDARY_WITH_NO_AMMO, CROSSHAIR_FLASH_SECONDARY_AMMO_NONE_FOR_RELOAD, CROSSHAIR_PRIMARY_TRIGGER_READY,
	CROSSHAIR_SECONDARY_TRIGGER_READY, CROSSHAIR_FLASH_FIRED_BATTERY_DEPLETED
};
enum { OVERLAY_FLASHING = 1 << 0, OVERLAY_EMPTY = 1 << 1, OVERLAY_RELOAD = 1 << 2, OVERLAY_DEFAULT = 1 << 3, OVERLAY_ALWAYS = 1 << 4 };
enum { LEVEL_USE_PARENT_FLASH = 1 << 0 };                          /* weapon_flash_state_definition.flags */
enum { OVERLAY_FLASHES = 1 << 0 };                                 /* hud_weapon_overlay_flags */
enum { NUMBER_SHOW_ALL_LEADING_ZEROS = 1 << 0, NUMBER_SHOW_ONLY_WHEN_ZOOMED = 1 << 1, NUMBER_SHOW_TRAILING_M = 1 << 2 };
enum { NUMBER_DIVIDE_BY_CLIP_SIZE = 1 << 0 };                      /* weapon_hud_number_element.weapon_flags */
enum
{
	CH_FLASHES = 1 << 0, CH_NOT_A_SPRITE = 1 << 1, CH_NOT_ON_DEFAULT_ZOOM = 1 << 2, CH_SHOW_SNIPER_DATA = 1 << 3, CH_HIDE_OUTSIDE_AREA = 1 << 4, CH_ONE_ZOOM_LEVEL = 1 << 5,
	CH_ONLY_ON_DEFAULT_ZOOM = 1 << 6
};
enum { METER_SWITCH_COLOR_ON_STATE_CHANGE = 1 << 0, METER_INTERPOLATES_BETWEEN_MIN_MAX = 1 << 1, METER_INVERT_INTERPOLATION_VALUE = 1 << 4 };
enum { FLASH_REVERSE_COLORS = 1 << 0 };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------- colours ---------- */

typedef struct { float a, r, g, b; } argb;

static argb unpack(unsigned c) { argb x = { (float)(c >> 24) / 255.f, (float)((c >> 16) & 255) / 255.f, (float)((c >> 8) & 255) / 255.f, (float)(c & 255) / 255.f }; return x; }

static unsigned pack(argb c)
{
	return ((unsigned)(clampf(c.a, 0.f, 1.f) * 255.f + 0.5f) << 24) | ((unsigned)(clampf(c.r, 0.f, 1.f) * 255.f + 0.5f) << 16) |
	       ((unsigned)(clampf(c.g, 0.f, 1.f) * 255.f + 0.5f) << 8) | (unsigned)(clampf(c.b, 0.f, 1.f) * 255.f + 0.5f);
}

/* hud_draw.c: get_flash_color */
unsigned fph_flash_color(const hud_element *e, int now, int reference)
{
	float period = e->flash_period, phase;
	argb base = unpack(e->color), flash = unpack(e->flash_color), result;

	if (period <= 0.f) return e->color;
	phase = fmodf((float)(now - reference) * (1.f / TICKS_PER_SECOND), period);
	if (phase < (float)e->flashes * (e->flash_delay + e->flash_length))
	{
		phase = fmodf(phase, e->flash_delay + e->flash_length);
		if (!reference) result = (e->color_flags & FLASH_REVERSE_COLORS) ? base : flash;
		else if (phase < e->flash_length)
		{
			float fraction = sqrtf(clampf(1.f - (cosf(phase / e->flash_length * 6.283f) + 1.f) * 0.5f, 0.f, 1.f));
			argb from = (e->color_flags & FLASH_REVERSE_COLORS) ? flash : base, to = (e->color_flags & FLASH_REVERSE_COLORS) ? base : flash;

			result.a = from.a + (to.a - from.a) * fraction; result.r = from.r + (to.r - from.r) * fraction;
			result.g = from.g + (to.g - from.g) * fraction; result.b = from.b + (to.b - from.b) * fraction;
		}
		else result = (e->color_flags & FLASH_REVERSE_COLORS) ? flash : base;
	}
	else result = base;
	return pack(result);
}

static int flash_duration(const hud_element *e) { return (int)(e->flash_period * 30.f); }

/* the colour a static element, number or overlay draws in for its draw flags (hud_draw_static_element) */
static unsigned element_color(const hud_element *e, int draw_flags, int now, int flash_reference)
{
	if (draw_flags & DRAW_DISABLED) return e->disabled_color;
	if (draw_flags & DRAW_FLASHING) return fph_flash_color(e, now, flash_reference);
	return e->color;
}

/* ---------- placement ---------- */

/* hud_draw.c: hud_calculate_point (the scale is 1): the corner's point of the window plus the offset, the bounds of the image on the corner's side */
static void place(const fph_window *w, int corner, int x, int y, float *px, float *py)
{
	if (corner < ANCHOR_CENTER)
	{
		*px = (corner & 1) ? (float)(w->x1 - x) : (float)(w->x0 + x);
		*py = (corner & 2) ? (float)(w->y1 - y) : (float)(w->y0 + y);
	}
	else
	{
		*px = (float)((w->x0 + w->x1) / 2 + x);
		*py = (float)((w->y0 + w->y1) / 2 + y);
	}
}

static void bounds(int corner, float width, float height, float *x0, float *y0, float *x1, float *y1)
{
	switch (corner)
	{
	case ANCHOR_TOP_RIGHT: *x0 = -width; *x1 = 0.f; *y0 = 0.f; *y1 = height; break;
	case ANCHOR_BOTTOM_LEFT: *x0 = 0.f; *x1 = width; *y0 = -height; *y1 = 0.f; break;
	case ANCHOR_BOTTOM_RIGHT: *x0 = -width; *x1 = 0.f; *y0 = -height; *y1 = 0.f; break;
	case ANCHOR_CENTER: *x0 = width * -0.5f; *x1 = width * 0.5f; *y0 = height * -0.5f; *y1 = height * 0.5f; break;
	default: *x0 = 0.f; *x1 = width; *y0 = 0.f; *y1 = height; break;
	}
}

typedef struct { fph_quad *out; int count, max; } sink;

static fph_quad *new_quad(sink *s)
{
	fph_quad *q;

	if (s->count >= s->max) return 0;
	q = &s->out[s->count++];
	memset(q, 0, sizeof(*q));
	return q;
}

/* an image at a point of its corner (hud_draw_bitmap_with_meter) */
static fph_quad *draw_image(sink *s, int image, int corner, float px, float py, float scale_x, float scale_y, unsigned color)
{
	const hud_image *im = &hud_images[image];
	float w = (float)(im->x1 - im->x0), h = (float)(im->y1 - im->y0), x0, y0, x1, y1;
	fph_quad *q;

	bounds(corner, w, h, &x0, &y0, &x1, &y1);
	q = new_quad(s);
	if (!q) return 0;
	q->kind = FPH_IMAGE; q->image = image; q->color = color;
	q->x0 = px + x0 * scale_x; q->y0 = py + y0 * scale_y; q->x1 = px + x1 * scale_x; q->y1 = py + y1 * scale_y;
	return q;
}

/* ---------- the weapon's state, per tick ---------- */

void fph_init(fph_state *state)
{
	int i;

	memset(state, 0, sizeof(*state));
	for (i = 0; i < 8; i++) state->last_weapon_flash_time[i] = FPH_NONE;
	state->last_grenade_flash_time = FPH_NONE;
	state->last_weapon = FPH_NONE;
	for (i = 0; i < FPH_CROSSHAIR_STATES; i++) state->crosshair_value[i] = FPH_NONE;
}

/* the chain of definitions of a weapon's hud (the weapon's own first) */
static const hud_weapon *weapon_hud(const fph_player *p) { return p->weapon >= 0 && p->weapon < HUD_WEAPONS ? &hud_weapons[p->weapon] : 0; }

/* hud_weapon.c: hud_update_weapon_local_player: which crosshairs are on, and the state each is in */
void fph_tick(fph_state *state, const fph_player *p)
{
	const hud_weapon *hw = weapon_hud(p);
	unsigned valid = 0, render_flags = 0;
	int i, k;

	state->time++;
	if (!hw) return;
	for (i = 0; i < hw->level_count; i++) valid |= hw->levels[i].valid_crosshairs;
	if (p->weapon != state->last_weapon && p->weapon < 0) memset(state->crosshair_value, 0, sizeof(state->crosshair_value));
	for (k = 0; k < FPH_CROSSHAIR_STATES; k++)
	{
		const hud_level *root = &hw->levels[0];
		int result = 0;

		if (!(valid & (1u << k))) continue;
		switch (k)
		{
		case CROSSHAIR_PRIMARY_TRIGGER_READY: result = p->magazines[0].can_fire; break;
		case CROSSHAIR_SECONDARY_TRIGGER_READY: result = p->magazines[1].can_fire; break;
		case CROSSHAIR_AIM: result = 0; break;                                             /* the autoaim level is 1 when it has a target: none here */
		case CROSSHAIR_ZOOM: result = p->zoom_level >= 0 ? p->zoom_level + 2 : 1; break;
		case CROSSHAIR_RELOAD: result = p->magazines[0].reloading; break;
		case CROSSHAIR_FLASH_AMMO:
			result = (p->magazines[0].rounds_remaining || p->magazines[0].rounds_loaded) && p->magazines[0].rounds_remaining && p->magazines[0].rounds_loaded <= root->loaded_ammo; break;
		case CROSSHAIR_FLASH_HEAT: result = p->heat * 100.f >= (float)root->heat; break;
		case CROSSHAIR_FLASH_TOTAL_AMMO: result = p->magazines[0].rounds_remaining <= root->total_ammo && !p->magazines[0].reloading; break;
		case CROSSHAIR_FLASH_TOTAL_BATTERY: result = p->age < 1.f && (1.f - p->age) * 100.f <= (float)root->age; break;
		case CROSSHAIR_CHARGE: result = 0; break;
		case CROSSHAIR_FLASH_FIRED_BATTERY_DEPLETED: result = (p->age == 1.f && p->primary_trigger) || state->crosshair_value[k] != FPH_NONE; break;
		case CROSSHAIR_FIRED_WITH_NO_AMMO:
			result = (!p->magazines[0].rounds_loaded && !p->magazines[0].rounds_remaining && p->primary_trigger) || state->crosshair_value[k] != FPH_NONE; break;
		case CROSSHAIR_THREW_WITH_NO_GRENADE: result = 0; break;                          /* (the grenade button is read, the throw is not done) */
		case CROSSHAIR_FLASH_AMMO_NONE_FOR_RELOAD:
			result = (p->magazines[0].rounds_remaining || p->magazines[0].rounds_loaded) && !p->magazines[0].rounds_remaining && p->magazines[0].rounds_loaded <= root->loaded_ammo; break;
		case CROSSHAIR_FLASH_SECONDARY_AMMO:
			result = (p->magazines[1].rounds_remaining || p->magazines[1].rounds_loaded) && p->magazines[1].rounds_remaining && p->magazines[1].rounds_loaded <= root->loaded_ammo; break;
		case CROSSHAIR_FLASH_SECONDARY_TOTAL_AMMO:
			result = p->magazine_count > 1 && p->magazines[1].rounds_remaining <= root->total_ammo && !p->magazines[1].reloading; break;
		case CROSSHAIR_SECONDARY_RELOAD: result = p->magazines[1].reloading; break;
		case CROSSHAIR_FIRED_SECONDARY_WITH_NO_AMMO:
			result = p->magazine_count > 1 && ((!p->magazines[1].rounds_loaded && !p->magazines[1].rounds_remaining && p->primary_trigger) || state->crosshair_value[k] != FPH_NONE); break;
		case CROSSHAIR_FLASH_SECONDARY_AMMO_NONE_FOR_RELOAD:
			result = (p->magazines[1].rounds_remaining || p->magazines[1].rounds_loaded) && !p->magazines[1].rounds_remaining && p->magazines[1].rounds_loaded <= root->loaded_ammo; break;
		}
		if (result > 0 || k == CROSSHAIR_AIM) render_flags |= 1u << k;
		if (k == CROSSHAIR_AIM) state->crosshair_value[k] = result;
		else if (k == CROSSHAIR_ZOOM) state->crosshair_value[k] = result - 1;
		else if (!result) state->crosshair_value[k] = FPH_NONE;
		else if (state->crosshair_value[k] == FPH_NONE) state->crosshair_value[k] = state->time;
	}
	state->crosshair_render_flags = render_flags;
}

/* ---------- the crosshairs ---------- */

/* hud_weapon.c: crosshairs_draw: from the weapon's own definition up its parents, each crosshair element of a state that is on */
static void draw_crosshairs(sink *s, fph_state *state, const fph_player *p, const fph_window *w)
{
	const hud_weapon *hw = weapon_hud(p);
	int level, i, firing_active = 0;

	if (!hw) return;
	for (level = 0; level < hw->level_count; level++)
	{
		const hud_level *lv = &hw->levels[level];

		for (i = 0; i < lv->crosshair_count; i++)
		{
			const hud_element *e = &lv->crosshairs[i];
			int st = e->state, frame, flashes = (e->flags & CH_FLASHES) != 0, count = e->image_count;
			unsigned color;
			float px, py;

			if (st < 0 || st >= FPH_CROSSHAIR_STATES || !(state->crosshair_render_flags & (1u << st))) continue;
			if ((e->flags & CH_NOT_ON_DEFAULT_ZOOM) && !(state->crosshair_value[CROSSHAIR_ZOOM] > 0)) continue;
			if ((e->flags & CH_ONLY_ON_DEFAULT_ZOOM) && state->crosshair_value[CROSSHAIR_ZOOM] != 0) continue;
			if (!count) continue;                                                         /* (not a sprite: none of the weapons' that are carried) */
			switch (st)
			{
			case CROSSHAIR_AIM:
				if (flashes)
				{
					frame = 0;
					if (state->crosshair_value[st] > 0) { color = fph_flash_color(e, state->time, 0); break; }
				}
				else frame = state->crosshair_value[st];
				color = e->color;
				break;
			case CROSSHAIR_ZOOM:
				if (e->flags & CH_ONE_ZOOM_LEVEL)
				{
					if (!state->crosshair_value[st]) continue;
					frame = 0;
				}
				else frame = state->crosshair_value[st] - ((e->flags & CH_NOT_ON_DEFAULT_ZOOM) != 0);
				if (flashes && state->crosshair_value[CROSSHAIR_AIM] > 0) { color = fph_flash_color(e, state->time, 0); break; }
				color = e->color;
				break;
			default:
				if (st == CROSSHAIR_FLASH_FIRED_BATTERY_DEPLETED) firing_active = p->age == 0.f && p->primary_trigger;
				else if (st == CROSSHAIR_FIRED_WITH_NO_AMMO) firing_active = !p->magazines[0].rounds_loaded && !p->magazines[0].rounds_remaining && p->primary_trigger;
				if (st == CROSSHAIR_FLASH_FIRED_BATTERY_DEPLETED || st == CROSSHAIR_FIRED_WITH_NO_AMMO || st == CROSSHAIR_THREW_WITH_NO_GRENADE || st == CROSSHAIR_FIRED_SECONDARY_WITH_NO_AMMO)
				{
					if (!firing_active && state->time - state->crosshair_value[st] >= flash_duration(e)) state->crosshair_value[st] = FPH_NONE;
					if (state->crosshair_value[st] == FPH_NONE) continue;
				}
				frame = e->frame_rate > 0 && count > 0 ? (int)(((state->time - state->crosshair_value[st]) / e->frame_rate / (int)TICKS_PER_SECOND) % count) : 0;
				if (flashes && state->crosshair_value[st] != FPH_NONE) { color = fph_flash_color(e, state->time, state->crosshair_value[st]); break; }
				color = e->color;
				break;
			}
			if (frame < 0 || frame >= count) continue;
			if (!(color >> 24)) color |= 0xFF000000u;                                    /* (a reticle's colours have no alpha in the tag, and it is not drawn clear) */
			place(w, ANCHOR_CENTER, e->x, e->y, &px, &py);
			draw_image(s, hud_refs[e->image_first + frame], ANCHOR_CENTER, px, py, e->scale_x, e->scale_y, color);
		}
	}
}

/* ---------- numbers ---------- */

/* hud_draw.c: hud_draw_numbers: the digits of `value` (as many as the element has, leading zeros as it says), the least at the cursor, each a screen width to its left */
static void draw_number(sink *s, const hud_element *e, int corner, const fph_window *w, int value, int draw_flags, int now, int flash_reference)
{
	float origin_x, origin_y, cursor;
	unsigned color = element_color(e, draw_flags, now, flash_reference);
	int digit_index;
	float screen_width = (float)hud_counter.screen_width, digit_count = (float)e->digits;

	if (e->number_flags & NUMBER_SHOW_ONLY_WHEN_ZOOMED) return;                         /* (the zoomed view's numbers: not here) */
	place(w, corner, e->x, e->y, &origin_x, &origin_y);
	switch (corner)
	{
	case ANCHOR_TOP_LEFT: case ANCHOR_BOTTOM_LEFT: cursor = (digit_count - 2.f) * screen_width + origin_x; break;
	case ANCHOR_CENTER: cursor = (digit_count - 1.f) * screen_width * 0.5f + origin_x; break;
	default: cursor = origin_x; break;
	}
	if (value < 0) value = -value;
	for (digit_index = 0; digit_index < e->digits; digit_index++)
	{
		int digit = value % 10;

		if (!value && !(e->number_flags & NUMBER_SHOW_ALL_LEADING_ZEROS)) break;
		draw_image(s, hud_counter.images[digit], corner, cursor, origin_y, 1.f, 1.f, color);
		cursor -= screen_width;
		value /= 10;
	}
}

/* ---------- meters ---------- */

/* hud_draw.c: hud_draw_meter's parameters for a value: the threshold and the colours (the flags that change the colours as the state does are ported) */
static void draw_meter(sink *s, const hud_element *e, int corner, const fph_window *w, int min_value, int max_value, int draw_flags, float reference_time, float reference_value)
{
	int min_alpha = e->minimum_value > clampi((int)(e->alpha_multiplier * min_value + e->alpha_bias), 0, 255) ? e->minimum_value : clampi((int)(e->alpha_multiplier * min_value + e->alpha_bias), 0, 255);
	int max_alpha = e->minimum_value > clampi((int)(e->alpha_multiplier * max_value + e->alpha_bias), 0, 255) ? e->minimum_value : clampi((int)(e->alpha_multiplier * max_value + e->alpha_bias), 0, 255);
	unsigned gradient_min, gradient_max, flash;
	float px, py;
	fph_quad *q;

	if (draw_flags & DRAW_DISABLED) { gradient_min = gradient_max = flash = 0; }
	else if (!(e->meter_flags & METER_SWITCH_COLOR_ON_STATE_CHANGE))
	{
		float fade = reference_time < 0.f ? 0.f : clampf(1.f - reference_time, 0.f, 1.f);
		argb f = unpack(e->meter_flash_color);

		f.r *= fade; f.g *= fade; f.b *= fade;
		gradient_min = (e->min_color & 0xFFFFFF) | ((unsigned)min_alpha << 24);
		gradient_max = e->max_color & 0xFFFFFF;
		flash = (pack(f) & 0xFFFFFF) | ((unsigned)max_alpha << 24);
	}
	else if (draw_flags & DRAW_FLASHING)
	{
		if (e->meter_flags & METER_INTERPOLATES_BETWEEN_MIN_MAX)
		{
			argb a = unpack(e->min_color), b = unpack(e->max_color), c;
			float t = (e->meter_flags & METER_INVERT_INTERPOLATION_VALUE) ? 1.f - reference_value : reference_value;

			c.a = 0.f; c.r = a.r + (b.r - a.r) * t; c.g = a.g + (b.g - a.g) * t; c.b = a.b + (b.b - a.b) * t;
			gradient_min = pack(c) | ((unsigned)min_alpha << 24); gradient_max = pack(c) & 0xFFFFFF; flash = (unsigned)min_alpha << 24;
		}
		else { gradient_min = (e->max_color & 0xFFFFFF) | ((unsigned)min_alpha << 24); flash = (unsigned)min_alpha << 24; gradient_max = e->max_color & 0xFFFFFF; }
	}
	else { gradient_min = ((unsigned)min_alpha << 24) | (e->min_color & 0xFFFFFF); flash = (unsigned)min_alpha << 24; gradient_max = e->min_color & 0xFFFFFF; }
	place(w, corner, e->x, e->y, &px, &py);
	q = draw_image(s, hud_refs[e->image_first], corner, px, py, e->scale_x, e->scale_y, 0xFFFFFFFFu);
	if (!q) return;
	q->kind = FPH_METER;
	q->threshold = min_alpha; q->flash_threshold = max_alpha;
	q->gradient_min = gradient_min; q->gradient_max = gradient_max; q->flash = flash;
	q->empty = ((255u - (e->empty_color >> 24)) << 24) | (e->empty_color & 0xFFFFFF);
	q->fade = 1.f - clampf(e->opacity, 0.f, 1.f);      /* the blend's constant colour: (the tint's alpha, e->fade, is not in the blend equation) */
}

/* ---------- the weapon's hud ---------- */

static void draw_overlays(sink *s, const hud_element *e, int corner, const fph_window *w, int type_flags, int reference_time, int draw_flags, int now)
{
	int frame = 0;
	unsigned color;
	float px, py;

	if (!(e->type & type_flags)) return;
	if ((e->flags & OVERLAY_FLASHES) && (draw_flags & DRAW_FLASHING)) color = fph_flash_color(e, now, reference_time); else color = e->color;
	if ((e->flags & OVERLAY_FLASHES) && (draw_flags & DRAW_FLASHING) && e->frame_rate > 0 && e->image_count > 0)
		frame = (int)(((now - reference_time) / e->frame_rate / (int)TICKS_PER_SECOND) % e->image_count);
	if (frame < 0 || frame >= e->image_count) return;
	place(w, corner, e->x, e->y, &px, &py);
	draw_image(s, hud_refs[e->image_first + frame], corner, px, py, e->scale_x, e->scale_y, color);
}

typedef struct { short flags[8], overlay[8], number[8]; } level_values;

/* hud_weapon.c: render_weapon_hud: the state flags (flashing, disabled), overlay flags and numbers of a definition, or its child's when it uses its parent's parameters; then the
   parent's elements, then its own */
static void render_level(sink *s, fph_state *state, const fph_player *p, const fph_window *w, const hud_weapon *hw, int level, const level_values *given)
{
	const hud_level *lv = &hw->levels[level];
	level_values v;
	int i;

	if ((lv->flags & LEVEL_USE_PARENT_FLASH) && given) v = *given;
	else
	{
		short f;
		int age_percentage = clampi((int)(p->age * 100.f), 0, 100);

		memset(&v, 0, sizeof(v));
		f = 0; if (p->magazines[0].rounds_remaining <= lv->total_ammo) f |= DRAW_FLASHING; if (p->magazines[0].rounds_remaining == 0) f |= DRAW_DISABLED; v.flags[0] = f;
		f = 0; if (p->magazines[0].rounds_loaded <= lv->loaded_ammo && !p->magazines[0].reloading) f |= DRAW_FLASHING; v.flags[1] = f;
		f = 0; if (p->heat * 100.f >= (float)lv->heat) f |= DRAW_FLASHING; v.flags[2] = f;
		f = 0; if ((1.f - p->age) * 100.f <= (float)lv->age) f |= DRAW_FLASHING; if (100 - age_percentage == 0) f |= DRAW_DISABLED; v.flags[3] = f;
		f = 0; if (p->magazines[1].rounds_remaining <= lv->total_ammo) f |= DRAW_FLASHING; if (p->magazines[1].rounds_remaining == 0) f |= DRAW_DISABLED; v.flags[4] = f;
		f = 0; if (p->magazines[1].rounds_loaded <= lv->loaded_ammo && !p->magazines[1].reloading) f |= DRAW_FLASHING; v.flags[5] = f;
		for (i = 0; i < 8; i++)
		{
			if (v.flags[i] & DRAW_FLASHING) { if (state->last_weapon_flash_time[i] == FPH_NONE) state->last_weapon_flash_time[i] = state->time; }
			else state->last_weapon_flash_time[i] = FPH_NONE;
		}
		f = 0; if (p->magazines[0].rounds_remaining <= lv->total_ammo && !p->magazines[0].reloading) f |= OVERLAY_FLASHING; if (p->magazines[0].reloading) f |= OVERLAY_RELOAD;
		if (p->magazines[0].rounds_remaining == 0) f |= OVERLAY_EMPTY; if (!f) f |= OVERLAY_DEFAULT; f |= OVERLAY_ALWAYS; v.overlay[0] = f;
		f = 0; if (p->magazines[0].rounds_loaded <= lv->loaded_ammo) f |= OVERLAY_FLASHING; if (p->magazines[0].reloading) f |= OVERLAY_RELOAD;
		if (p->magazines[0].rounds_loaded == 0) f |= OVERLAY_EMPTY; if (!f) f |= OVERLAY_DEFAULT; f |= OVERLAY_ALWAYS; v.overlay[1] = f;
		f = 0; if (p->heat * 100.f >= (float)lv->heat) f |= OVERLAY_FLASHING; if (p->overheated) f |= OVERLAY_RELOAD; if (100 - age_percentage == 0) f |= OVERLAY_EMPTY;
		if (!f) f |= OVERLAY_DEFAULT; f |= OVERLAY_ALWAYS; v.overlay[2] = f;
		f = 0; if ((1.f - p->age) * 100.f <= (float)lv->age) f |= OVERLAY_FLASHING; if (p->overheated) f |= OVERLAY_RELOAD; if (100 - age_percentage == 0) f |= OVERLAY_EMPTY;
		if (!f) f |= OVERLAY_DEFAULT; f |= OVERLAY_ALWAYS; v.overlay[3] = f;
		f = 0; if (p->magazines[1].rounds_remaining <= lv->total_ammo && !p->magazines[1].reloading) f |= OVERLAY_FLASHING; if (p->magazines[1].reloading) f |= OVERLAY_RELOAD;
		if (p->magazines[1].rounds_remaining == 0) f |= OVERLAY_EMPTY; if (!f) f |= OVERLAY_DEFAULT; f |= OVERLAY_ALWAYS; v.overlay[4] = f;
		f = 0; if (p->magazines[1].rounds_loaded <= lv->loaded_ammo) f |= OVERLAY_FLASHING; if (p->magazines[1].reloading) f |= OVERLAY_RELOAD;
		if (p->magazines[1].rounds_loaded == 0) f |= OVERLAY_EMPTY; if (!f) f |= OVERLAY_DEFAULT; f |= OVERLAY_ALWAYS; v.overlay[5] = f;
		if (p->magazine_count < 2) v.overlay[4] = v.overlay[5] = 0;                       /* a weapon without a second magazine draws none of its overlays */
		v.number[0] = (short)p->magazines[0].rounds_remaining;
		v.number[1] = (short)p->magazines[0].rounds_loaded;
		v.number[2] = (short)(p->heat * 255.f);
		v.number[3] = (short)((1.f - p->age) * 100.f);
		v.number[4] = (short)p->magazines[1].rounds_remaining;
		v.number[5] = (short)p->magazines[1].rounds_loaded;
	}
	if (level + 1 < hw->level_count) render_level(s, state, p, w, hw, level + 1, &v);
	for (i = 0; i < lv->element_count; i++)
	{
		const hud_element *e = &lv->elements[i];
		int st = e->state;

		if (st < 0 || st >= 8) continue;
		switch (e->kind)
		{
		case HUD_STATIC:
		{
			float px, py;

			place(w, lv->corner, e->x, e->y, &px, &py);
			draw_image(s, hud_refs[e->image_first], lv->corner, px, py, e->scale_x, e->scale_y, element_color(e, v.flags[st], state->time, state->last_weapon_flash_time[st]));
			break;
		}
		case HUD_METER:
		{
			int value = (unsigned char)v.number[st];

			draw_meter(s, e, lv->corner, w, value, value, v.flags[st], (float)state->last_weapon_flash_time[st], 0.f);
			break;
		}
		case HUD_NUMBER:
		{
			int magazine_size = 1, value;

			if (e->weapon_flags & NUMBER_DIVIDE_BY_CLIP_SIZE) { magazine_size = p->magazines[0].rounds_loaded_maximum; if (magazine_size == 0) magazine_size = 1; }
			value = v.number[st] / magazine_size;
			draw_number(s, e, lv->corner, w, value, v.flags[st], state->time, state->last_weapon_flash_time[st]);
			break;
		}
		case HUD_OVERLAY:
			draw_overlays(s, e, lv->corner, w, v.overlay[st], state->last_weapon_flash_time[st], v.flags[st], state->time);
			break;
		}
	}
}

/* ---------- the grenade hud and the unit hud ---------- */

/* hud_weapon.c: render_grenade_hud */
static void draw_grenades(sink *s, fph_state *state, const fph_player *p, const fph_window *w)
{
	short draw_flags = 0, overlay_flags;
	int i, count = p->grenade_count;

	if (!hud_grenade_count) return;
	if (count <= hud_grenade_flash_cutoff) draw_flags |= DRAW_FLASHING;
	if (count == 0) draw_flags |= DRAW_DISABLED;
	if (draw_flags & DRAW_FLASHING) { if (state->last_grenade_flash_time == FPH_NONE) state->last_grenade_flash_time = state->time; }
	else state->last_grenade_flash_time = FPH_NONE;
	overlay_flags = count <= hud_grenade_flash_cutoff ? 1 : 0;
	if (count == 0) overlay_flags |= 2;
	if (!overlay_flags) overlay_flags |= 4;
	overlay_flags |= 8;
	for (i = 0; i < hud_grenade_count; i++)
	{
		const hud_element *e = &hud_grenade[i];
		float px, py;

		if (e->kind == HUD_STATIC)
		{
			place(w, e->corner, e->x, e->y, &px, &py);
			draw_image(s, hud_refs[e->image_first], e->corner, px, py, e->scale_x, e->scale_y, element_color(e, draw_flags, state->time, state->last_grenade_flash_time));
		}
		else if (e->kind == HUD_NUMBER && e->digits) draw_number(s, e, e->corner, w, count, draw_flags, state->time, state->last_grenade_flash_time);
		else if (e->kind == HUD_OVERLAY) draw_overlays(s, e, e->corner, w, overlay_flags, state->last_grenade_flash_time, draw_flags, state->time);
	}
}

/* the unit hud's element of a part (hud_assets.h: 0 background, 1 shield background, 2 shield meter, 3 health background, 4 health meter, 5 and 6 the motion sensor's) */
static const hud_element *unit_part(int part)
{
	int i;

	for (i = 0; i < hud_unit_count; i++) if (hud_unit[i].state == part) return &hud_unit[i];
	return 0;
}

static void draw_unit_static(sink *s, const hud_element *e, const fph_window *w, int draw_flags, int now)
{
	float px, py;

	if (!e) return;
	place(w, e->corner, e->x, e->y, &px, &py);
	draw_image(s, hud_refs[e->image_first], e->corner, px, py, e->scale_x, e->scale_y, element_color(e, draw_flags, now, FPH_NONE));
}

/* hud_unit.c: hud_render_unit_interface: the unit's background, the shield panel (the meter, then the background over it: value_scale 198 times the vitality, flashing under a
   quarter), the health panel (its meter coloured by the cutoffs, flashing when the shield is gone), the motion sensor's two backgrounds */
static void draw_unit(sink *s, fph_state *state, const fph_player *p, const fph_window *w)
{
	const hud_element *shield = unit_part(2), *health = unit_part(4);

	draw_unit_static(s, unit_part(0), w, 0, state->time);
	if (shield)
	{
		int value_scale = shield->value_scale ? shield->value_scale : 255, value = clampi((int)((float)value_scale * p->shield), 0, 255);
		float px = 0.f, py = 0.f;

		(void)px; (void)py;
		draw_meter(s, shield, shield->corner, w, value, value, p->shield < 0.25f ? DRAW_FLASHING : 0, -1.f, p->shield);
	}
	draw_unit_static(s, unit_part(1), w, p->shield < 0.25f ? DRAW_FLASHING : 0, state->time);
	if (health)
	{
		hud_element m = *health;
		int value_scale = health->value_scale ? health->value_scale : 8, value = clampi((int)((float)value_scale * p->health), 0, 255);

		if (p->health >= health->max_cutoff) m.min_color = m.max_color;
		else if (p->health <= health->min_cutoff) m.max_color = m.min_color;
		else { m.max_color = health->mid_color; m.min_color = m.max_color; }
		draw_meter(s, &m, health->corner, w, value, value, p->shield <= 0.f ? DRAW_FLASHING : 0, -1.f, p->health);
	}
	draw_unit_static(s, unit_part(3), w, p->shield <= 0.f ? DRAW_FLASHING : 0, state->time);
	draw_unit_static(s, unit_part(5), w, 0, state->time);
	draw_unit_static(s, unit_part(6), w, 0, state->time);
}

/* ---------- a frame ---------- */

int fph_render(fph_state *state, const fph_player *p, const fph_window *w, fph_quad *quads, int max)
{
	sink s = { quads, 0, max };
	const hud_weapon *hw = weapon_hud(p);

	draw_crosshairs(&s, state, p, w);
	if (hw) render_level(&s, state, p, w, hw, 0, 0);
	draw_grenades(&s, state, p, w);
	draw_unit(&s, state, p, w);
	state->last_weapon = p->weapon;
	return s.count;
}

/* ---------- a meter's palette ---------- */

/* The Xbox's meter blend reads Cd * (the shader's alpha) + Cs * (1 - opacity) by the constants it sets, which I could not tie to the texture's alpha; here the shape is the
   alpha of an ordinary blend (half for a faint edge, whole inside), so the meter reads on a bright ground as well as a dark one. The palette of a meter's texture (hud_assets.h: texel = 64 * the shape's alpha in 4 levels + the fill order / 4) for a meter quad: 256 entries in the order of the texel values, each
   r | g << 8 | b << 16 | a << 24 with the alpha 0..255 (main.c puts them in the GS's order and scale). The Xbox's meter pixel shader draws a texel filled (the gradient colour by
   its fill order) while its fill order is at most the quad's threshold, in the flash colour up to the flash threshold, else in the empty colour */
void fph_meter_palette(const fph_quad *q, unsigned *palette)
{
	argb lo = unpack(q->gradient_min), hi = unpack(q->gradient_max), fl = unpack(q->flash), em = unpack(q->empty);
	int texel;

	for (texel = 0; texel < 256; texel++)
	{
		int shape = texel >> 6, order = (texel & 63) * 4 + 2;
		float alpha = shape == 0 ? 0.f : shape == 1 ? 0.5f : 1.f, r, g, b;

		if (!shape) { palette[texel] = 0; continue; }
		if (order <= q->threshold)
		{
			float t = (float)order / 255.f;

			r = lo.r + (hi.r - lo.r) * t; g = lo.g + (hi.g - lo.g) * t; b = lo.b + (hi.b - lo.b) * t;
		}
		else if (order <= q->flash_threshold) { r = fl.r; g = fl.g; b = fl.b; }
		else { r = em.r; g = em.g; b = em.b; alpha *= em.a; }
		r *= q->fade; g *= q->fade; b *= q->fade;
		palette[texel] = ((unsigned)(clampf(alpha, 0.f, 1.f) * 255.f + 0.5f) << 24) | ((unsigned)(clampf(b, 0.f, 1.f) * 255.f + 0.5f) << 16) |
		                 ((unsigned)(clampf(g, 0.f, 1.f) * 255.f + 0.5f) << 8) | (unsigned)(clampf(r, 0.f, 1.f) * 255.f + 0.5f);
	}
}
