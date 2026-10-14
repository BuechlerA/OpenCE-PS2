/*
FP_CONTROLLER.H

The first-person controller of Halo (input_abstraction.c, player_control.c, bipeds.c and first_person_camera.c in
source/) for the Master Chief sample: a gamepad in, an eye position and a facing out, with the player walking on the
map's structure BSP. No PS2 or GS code in it: src/pad.c reads the DualShock, main.c draws.
*/
#ifndef FP_CONTROLLER_H
#define FP_CONTROLLER_H

#include "fp_weapon.h"
#include "fp_view.h"

/* what a gamepad gave this frame, as the Xbox's XInput state: sticks in -32768..32767 (right and up positive) */
enum
{
	FP_DPAD_UP = 1 << 0,
	FP_DPAD_DOWN = 1 << 1,
	FP_DPAD_LEFT = 1 << 2,
	FP_DPAD_RIGHT = 1 << 3,
	FP_JUMP = 1 << 4,       /* the Xbox's A */
	FP_CROUCH = 1 << 5,     /* its left stick click */
	FP_START = 1 << 6,      /* its Start: not the controller's, the demo's (fp_demo.h) */
	FP_RELOAD = 1 << 7,     /* X */
	FP_SWITCH_WEAPON = 1 << 8,   /* Y */
	FP_MELEE = 1 << 9,      /* B */
	FP_ZOOM = 1 << 10,      /* the right stick's click */
	FP_GRENADE = 1 << 11,   /* the left trigger */
	FP_AUTOLEVEL = 1 << 12  /* Back: the look autocenter option (a menu setting in the original) */
};

typedef struct
{
	short left_x, left_y, right_x, right_y;
	unsigned buttons;
	unsigned char trigger;       /* the right trigger, 0..255: fires */
} fp_pad;

/* the pad's state shaped as the original's game_input_state: -1..1, forward and left positive, yaw positive counter-clockwise (left), pitch positive up */
typedef struct
{
	float forward_movement, strafe, yaw, pitch;
	int jump, crouch;
	float trigger;              /* 0..1 */
} fp_input;

typedef struct
{
	/* the biped, at 30 Hz as in the game: feet position, velocity in units a tick */
	float pos[3], prev_pos[3], vel[3];
	float crouch, prev_crouch;
	int airborne;
	float ground_n[3];          /* the surface it stands on: its normal (valid when not airborne) */
	float tick_time;            /* the seconds since the last tick, for the camera's interpolation */
	/* the player_control: where it looks, every frame */
	float yaw, pitch;           /* radians; yaw 0 looks along +x, positive turns left */
	float look_accel_time;      /* seconds the stick has been pegged */
	float time;
	float spawn[2];             /* where a fall into the void puts the player back */
	/* the weapons (fp_weapon.h), the lens and the look's autolevel */
	fpw_inventory weapons;
	fpw_events events;          /* what the weapons did this frame */
	fpv_state view;             /* the first-person weapon's pose (the arms and the gun) */
	unsigned previous_buttons, pending_edges;
	float trigger;
	float field_of_view, fov_from, fov_to, fov_time;   /* horizontal, radians: the unit's, over the zoom's magnification, eased in over 0.18 s */
	int autolevel_enabled, autolevel_ticks, use_autolevel;
	float autolevel_fraction;
} fp_player;

/* xinput-style pad to the original's shaped input (dead zone, square stick, d-pad overrides) */
void fp_shape_input(const fp_pad *pad, fp_input *input);

/* the player on the ground at (x, y), looking along yaw */
void fp_init(fp_player *player, float x, float y, float yaw);

/* one frame: the look at this frame's rate, the physics in 30 Hz ticks for the seconds it lasted */
void fp_update(fp_player *player, const fp_pad *pad, float seconds);

/* the camera: eye and the point it looks at (a unit in front), interpolated between the last two ticks */
void fp_camera(const fp_player *player, float eye[3], float target[3]);

/* the lens: a horizontal field of view (the original's 70 degrees, less when zoomed) as the vertical half angle of a 4:3 picture, in degrees */
float fp_vertical_half_fov_degrees(float horizontal_radians);

/* the player's field of view now, horizontal, radians */
float fp_field_of_view(const fp_player *player);

/* the height of the walkable surface under (x, y), the highest one at or below z_max (and no more than 8 units under it); returns 0 if there is none.
   normal_z gets the surface's slope (the cosine of its angle from the vertical) if not NULL */
int fp_ground(float x, float y, float z_max, float *z, float *normal_z, float normal_xy[2]);

/* the same, looking no deeper than `depth` below z_max (a query that wants the ground near a height is much cheaper: fewer leaves of the tree) */
int fp_ground_in(float x, float y, float z_max, float depth, float *z, float *normal_z, float normal_xy[2]);


/* the first surface (from its front) a segment from a to b crosses in the map's collision geometry (the structure's, the scenery and the Chief):
   returns 1 with the fraction of the segment, the surface's normal and the triangle's index, 0 if it crosses none */
int fp_trace(const float a[3], const float b[3], float *t, float normal[3], unsigned *triangle);

#endif
