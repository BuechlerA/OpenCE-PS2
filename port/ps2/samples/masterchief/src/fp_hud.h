/*
FP_HUD.H

The first-person HUD of Halo (source/interface/hud_weapon.c, hud_unit.c, hud_draw.c) for the Master Chief sample: what it draws, as a list of quads, from the HUD tags
tools/export_hud.py baked (hud_assets.h) and the state of the player's weapon. No PS2 or GS code in it: main.c draws the quads (and builds a meter's palette).

A frame's quads are, in the original's order: the crosshairs, the weapon's HUD (its statics, meters, numbers and overlays, parent first), the grenade HUD, the unit HUD (the
shield and health panels, the motion sensor's backgrounds). Positions are in the display's pixels (640 wide).
*/
#ifndef FP_HUD_H
#define FP_HUD_H

#include "hud_assets.h"

#define FPH_MAX_QUADS 160
#define FPH_NONE (-1)
#define FPH_CROSSHAIR_STATES 19

/* the window the HUD is laid out in (the Xbox's title safe frame, 48, 36 to 592, 444 of 640 x 480, here scaled to the display's lines) */
typedef struct { int x0, y0, x1, y1; } fph_window;

/* what the HUD reads of the player: the weapon's interface state (weapons.c: weapon_build_weapon_interface_state), the zoom, the grenades and the unit's vitality */
typedef struct
{
	int weapon;                                  /* an index of hud_weapons, or -1 for none (the default crosshair) */
	int magazine_count;
	struct { int reloading, can_fire, rounds_loaded, rounds_loaded_maximum, rounds_remaining; } magazines[2];
	float heat, age;
	int overheated;
	int zoom_level;                              /* -1: none */
	int primary_trigger;                         /* the unit's control flag */
	int grenade_count;                           /* of the current type */
	float shield, health;                        /* 0..1 */
} fph_player;

typedef struct
{
	int time;                                    /* game ticks (30 a second) */
	int last_weapon_flash_time[8];               /* when each state began to flash, or FPH_NONE */
	int last_grenade_flash_time;
	int last_weapon;
	int crosshair_value[FPH_CROSSHAIR_STATES];   /* hud_weapon.c's crosshair_state: the tick a state began, a frame index for the aim and zoom, or FPH_NONE */
	unsigned crosshair_render_flags;
} fph_state;

enum { FPH_IMAGE = 0, FPH_METER = 1 };

/* a quad: the image's rectangle drawn at (x0, y0)-(x1, y1) tinted by `color` (alpha, red, green, blue; the colour a meter's pixels have is the meter's, below) */
typedef struct
{
	int kind, image;
	float x0, y0, x1, y1;
	unsigned color;
	/* a meter (hud_draw.c: hud_draw_meter): a texel whose fill order (0..255) is at most `threshold` is drawn in the gradient from gradient_min to gradient_max (by the fill order),
	   one above it and at most `flash_threshold` in the flash colour, the rest in `empty`; `fade` scales the alpha */
	int threshold, flash_threshold;
	unsigned gradient_min, gradient_max, flash, empty;
	float fade;
} fph_quad;

void fph_init(fph_state *state);

/* one 30 Hz tick: the crosshair states (hud_update_weapon_local_player) and the game time */
void fph_tick(fph_state *state, const fph_player *player);

/* one frame: the quads (at most `max`; the number written) */
int fph_render(fph_state *state, const fph_player *player, const fph_window *window, fph_quad *quads, int max);

/* a meter quad's palette for its texture (hud_assets.h kind 1): 256 entries r | g << 8 | b << 16 | a << 24 (alpha 0..255), in the order of the texel values */
void fph_meter_palette(const fph_quad *quad, unsigned *palette);

/* the flash colour of a hud colour at a time (hud_draw.c: get_flash_color), ARGB */
unsigned fph_flash_color(const hud_element *e, int time, int reference_time);

#endif
