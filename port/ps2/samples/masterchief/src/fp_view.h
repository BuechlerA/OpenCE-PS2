/*
FP_VIEW.H

The first-person weapon's pose: source/interface/first_person_weapons.c (the state the weapon's animation is in, the animation's frame a tick,
the moving, sway, turning and firing push-back overlays, the interpolation between animations) on the animation graphs and models of
fpm_assets.h. It makes, each 30 Hz tick, the node matrices of the weapon's animation graph in the camera's own space (x forward, y left, z up,
the origin the eye): the arms and the gun are drawn with them (skin_matrix: a model node's matrix is its inverse bind matrix and its graph
node's). No PS2 or GS code in it.
*/
#ifndef FP_VIEW_H
#define FP_VIEW_H

#include "fp_weapon.h"
#include "fpm_assets.h"

#define FPV_MAX_NODES 64

typedef struct { float q[4], t[3], s; } fpv_orientation;      /* rotation (i, j, k, w), translation, scale */

/* source/interface/first_person_weapons.c: first_person_weapon_state */
enum { FPV_IDLE = 0, FPV_OVERHEATING, FPV_OVERHEATING_AGAIN, FPV_OVERHEATED, FPV_CHARGED, FPV_POSING, FPV_PRIMARY_FIRE, FPV_SECONDARY_FIRE, FPV_PRIMARY_MISFIRE,
       FPV_SECONDARY_MISFIRE, FPV_MELEE, FPV_LIGHT_ON, FPV_LIGHT_OFF, FPV_RELOAD_WHILE_EMPTY, FPV_RELOAD_WHILE_FULL, FPV_SHOTGUN_ENTER_RELOAD,
       FPV_SHOTGUN_EXIT_RELOAD_EMPTY, FPV_SHOTGUN_EXIT_RELOAD_FULL, FPV_PUT_AWAY, FPV_READY, FPV_THROW_GRENADE, FPV_THROW_GRENADE_OVERHEATED,
       FPV_OVERHEATED_EXIT, FPV_OVERHEATING_SUPER_RECOIL };

typedef struct
{
	int weapon;                              /* index into fpw_weapons / fpm_weapons: -1 none */
	int state, animation, frame;             /* the state and its animation (an index into the graph's animations) and frame */
	int moving_animation, moving_frame;
	int overlay_animation;                   /* the sway overlay (9 frames: position, turning, push-back), or -1 */
	int interpolation_index, interpolation_count;
	float push_back, push_back_velocity, position[2], position_velocity[2], turning[2], turning_velocity[2];
	int ticks_idle, ticks_until_pose;
	float last_yaw, last_pitch;
	int rendered, built;
	int shotgun_empty, shotgun_reload_type;
	unsigned random_seed;
	fpv_orientation original[FPV_MAX_NODES], orientation[FPV_MAX_NODES];
	float matrices[FPV_MAX_NODES][12];       /* the graph's nodes in the camera's space: a 3x3 and a translation, for row vectors (p * m) */
	float previous_matrices[FPV_MAX_NODES][12];
} fpv_state;

/* what the unit is doing this tick (units.c, player_control.c) */
typedef struct
{
	float throttle[2];                       /* forward and left, as the pad gives them (the unit's throttle) */
	float yaw, pitch;                        /* where the player is looking, radians */
	int rounds_loaded;                       /* in the first magazine */
	int zoomed, flying, overheated;
} fpv_input;

void fpv_init(fpv_state *v);

/* the weapon in hand: the weapon switch (it is readied from idle) */
void fpv_set_weapon(fpv_state *v, int weapon);

/* an event of the weapon (fpw_event) as the first-person weapon's message; `w` is the weapon's state (for the rounds loaded) */
void fpv_event(fpv_state *v, const fpw_event *e, const fpw_state *w);

/* one 30 Hz tick: the animation's frame, the overlays, the pose and its node matrices */
void fpv_tick(fpv_state *v, const fpv_input *input, const fpw_state *w);

/* the skin matrix (12 floats, p * m) of a model's node i, between the previous tick's pose and this one's by `t` in 0..1 */
void fpv_skin_matrix(const fpv_state *v, const fpm_model *model, const short *graph_node, int node, float t, float *out);

/* the node of the graph's animation of a kind (weapons.h: _first_person_weapon_animation_*), or NULL */
const fpm_animation *fpv_animation_of_type(const fpv_state *v, int type);

#endif
