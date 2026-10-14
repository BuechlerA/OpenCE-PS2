/*
FP_WEAPON.H

The player's weapons: source/items/weapons.c (the trigger and magazine state machines, firing, heat and age, reload and ready times from the
first-person animations, the error cone of a shot, zoom), the unit's weapon handling in units.c (the two weapons carried, switching, melee,
zoom levels) and projectiles.c (a projectile's flight and impact), on the numbers weapon_assets.h has from the map's tags. 30 Hz, like the
controller (fp_controller.h); no PS2 or GS code in it.
*/
#ifndef FP_WEAPON_H
#define FP_WEAPON_H

#include "weapon_assets.h"

enum { FPW_TRIGGER_IDLE = 0, FPW_TRIGGER_OVERLOADING, FPW_TRIGGER_CHARGING, FPW_TRIGGER_CHARGED, FPW_TRIGGER_RECOVERING, FPW_TRIGGER_TRACKING, FPW_TRIGGER_SPEWING,
       FPW_TRIGGER_LOCKED, FPW_TRIGGER_UNINITIALIZED };
enum { FPW_MAGAZINE_IDLE = 0, FPW_MAGAZINE_RELOADING, FPW_MAGAZINE_UNCHAMBERED, FPW_MAGAZINE_CHAMBERING };
enum { FPW_CONTROL_PRIMARY_TRIGGER = 1 << 0, FPW_CONTROL_SECONDARY_TRIGGER = 1 << 1, FPW_CONTROL_RELOAD = 1 << 2, FPW_CONTROL_USER_BUSY = 1 << 3, FPW_CONTROL_ZOOMED = 1 << 4 };
enum { FPW_FLAG_OVERHEATED = 1 << 0, FPW_FLAG_OVERHEATED_EXIT = 1 << 1, FPW_FLAG_NEEDS_TO_RELOAD = 1 << 2 };

/* the trigger definition's flags (weapon_trigger_definition_flags) and the weapon's (weapon_definition flags) that the logic reads */
enum { FPW_TRIGGER_TRACKS_PROJECTILE = 1 << 0, FPW_TRIGGER_CAN_FIRE_WITH_PARTIAL_AMMUNITION = 1 << 2, FPW_TRIGGER_LATCHED = 1 << 3, FPW_TRIGGER_TOGGLES = 1 << 4,
       FPW_TRIGGER_ANALOG_RATE_OF_FIRE = 1 << 9, FPW_TRIGGER_USE_ERROR_WHEN_UNZOOMED = 1 << 10, FPW_TRIGGER_PROJECTILES_HAVE_IDENTICAL_ERROR = 1 << 12 };
enum { FPW_WEAPON_CANNOT_FIRE_AT_MAXIMUM_AGE = 1 << 11, FPW_WEAPON_PREVENTS_GRENADE_THROWING = 1 << 6, FPW_WEAPON_PREVENTS_MELEE_ATTACK = 1 << 9,
       FPW_WEAPON_SECONDARY_TRIGGER_OVERRIDES_GRENADES = 1 << 12 };

typedef struct { int state, timer, idle_ticks; float rate_of_fire, error; unsigned flags; int sequential_non_tracer_rounds; float ejection_port, illumination; } fpw_trigger_state;
typedef struct { int state, timer, rounds_loaded, rounds_total, fractional_recharged, original_time; } fpw_magazine_state;

typedef struct
{
	int weapon;                          /* index into fpw_weapons */
	int state_timer;                     /* ticks the weapon can not fire for (it was just readied) */
	float heat, age, overcharged, primary_trigger;
	unsigned flags, control_flags;
	int alternate_shots_loaded;
	fpw_magazine_state magazines[2];
	fpw_trigger_state triggers[2];
} fpw_state;

/* what happened this tick, for what draws and sounds it (and for the projectiles' effects) */
enum { FPW_EVENT_FIRE = 1, FPW_EVENT_MISFIRE, FPW_EVENT_EMPTY, FPW_EVENT_RELOAD_START, FPW_EVENT_RELOAD_FINISH, FPW_EVENT_CHAMBER, FPW_EVENT_OVERHEATING, FPW_EVENT_CHARGED,
       FPW_EVENT_READY, FPW_EVENT_PUT_AWAY, FPW_EVENT_MELEE, FPW_EVENT_MELEE_HIT, FPW_EVENT_ZOOM, FPW_EVENT_IMPACT, FPW_EVENT_DETONATE, FPW_EVENT_THROW, FPW_EVENT_DRY };
typedef struct { int type, a, b; float position[3], normal[3]; } fpw_event;
#define FPW_MAX_EVENTS 48
typedef struct { fpw_event list[FPW_MAX_EVENTS]; int count; } fpw_events;

/* a projectile in flight (projectiles.c): units a tick */
typedef struct { int active, projectile, age_ticks; float position[3], velocity[3], travelled; } fpw_projectile_state;
#define FPW_MAX_PROJECTILES 48

/* where a shot starts and which way it goes: the player's eye and its aim */
typedef struct { float origin[3], forward[3], velocity[3]; } fpw_aim;

typedef struct
{
	fpw_state slots[2];
	int slot_count, current, desired;
	int switch_timer;                    /* ticks left of putting the current weapon away (-1: not switching) */
	int zoom_level;                      /* -1: none */
	int melee_timer, melee_key_timer;
	fpw_aim aim;
	fpw_projectile_state projectiles[FPW_MAX_PROJECTILES];
	unsigned random_seed;
	unsigned previous_buttons;
} fpw_inventory;

/* the weapons the unit carries (at most two), the first readied */
void fpw_inventory_init(fpw_inventory *inv, const int *weapons, int count, unsigned seed);

/* what a player asks of the weapon in one tick: trigger pressure (0..1), pressed this tick (an edge): reload, switch weapons, melee, zoom */
typedef struct { float trigger; int reload, switch_weapon, melee, zoom; int grenade; } fpw_input;

/* one 30 Hz tick: the unit's controls to the weapon, the weapon's update, the projectiles' flight; events are appended */
void fpw_inventory_tick(fpw_inventory *inv, const fpw_input *input, fpw_events *events);

/* the zoom: the current level's magnification (1 if none) and the lens it gives (the unit's field of view, in radians, over the magnification) */
float fpw_zoom_magnification(const fpw_inventory *inv);
float fpw_field_of_view(const fpw_inventory *inv, float unit_field_of_view);

/* the current weapon (0 if the unit has none) */
const fpw_weapon *fpw_current(const fpw_inventory *inv);
fpw_state *fpw_current_state(fpw_inventory *inv);

/* ticks of one of a weapon's first-person animations (its frames at 30 a second; 0 if it has none) */
int fpw_animation_ticks(int weapon, int type);

#endif
