/*
FP_CONTROLLER.H

The first-person controller of Halo (input_abstraction.c, player_control.c, bipeds.c and first_person_camera.c in
source/) for the Master Chief sample: a gamepad in, an eye position and a facing out, with the player walking on the
map's structure BSP. No PS2 or GS code in it: src/pad.c reads the DualShock, main.c draws.
*/
#ifndef FP_CONTROLLER_H
#define FP_CONTROLLER_H

/* what a gamepad gave this frame, as the Xbox's XInput state: sticks in -32768..32767 (right and up positive) */
enum
{
	FP_DPAD_UP = 1 << 0,
	FP_DPAD_DOWN = 1 << 1,
	FP_DPAD_LEFT = 1 << 2,
	FP_DPAD_RIGHT = 1 << 3,
	FP_JUMP = 1 << 4,       /* the Xbox's A */
	FP_CROUCH = 1 << 5,     /* its left stick click */
	FP_START = 1 << 6       /* its Start: not the controller's, the demo's (fp_demo.h) */
};

typedef struct
{
	short left_x, left_y, right_x, right_y;
	unsigned buttons;
} fp_pad;

/* the pad's state shaped as the original's game_input_state: -1..1, forward and left positive, yaw positive counter-clockwise (left), pitch positive up */
typedef struct
{
	float forward_movement, strafe, yaw, pitch;
	int jump, crouch;
} fp_input;

typedef struct
{
	/* the biped, at 30 Hz as in the game: feet position, velocity in units a tick */
	float pos[3], prev_pos[3], vel[3];
	float crouch, prev_crouch;
	int airborne;
	int sliding;
	float tick_time;            /* the seconds since the last tick, for the camera's interpolation */
	/* the player_control: where it looks, every frame */
	float yaw, pitch;           /* radians; yaw 0 looks along +x, positive turns left */
	float look_accel_time;      /* seconds the stick has been pegged */
	float time;
	float spawn[2];             /* where a fall into the void puts the player back */
} fp_player;

/* xinput-style pad to the original's shaped input (dead zone, square stick, d-pad overrides) */
void fp_shape_input(const fp_pad *pad, fp_input *input);

/* the player on the ground at (x, y), looking along yaw */
void fp_init(fp_player *player, float x, float y, float yaw);

/* one frame: the look at this frame's rate, the physics in 30 Hz ticks for the seconds it lasted */
void fp_update(fp_player *player, const fp_pad *pad, float seconds);

/* the camera: eye and the point it looks at (a unit in front), interpolated between the last two ticks */
void fp_camera(const fp_player *player, float eye[3], float target[3]);

/* the camera's lens: the original's 70 degrees horizontal, as the vertical half angle of a 4:3 picture, in degrees */
float fp_vertical_half_fov_degrees(void);

/* the height of the walkable surface under (x, y), the highest one at or below z_max; returns 0 if there is none.
   normal_z gets the surface's slope (the cosine of its angle from the vertical) if not NULL */
int fp_ground(float x, float y, float z_max, float *z, float *normal_z, float normal_xy[2]);

#endif
