/*
FP_DEMO.H

The first-person demo: a player that walks by itself, through the gamepad the controller (fp_controller.h) reads, from the
front of one fortress to the middle of the valley, turning where the path bends and jumping on the way, then stands
looking at the Chief and starts again. The path is planned on the map's ground, at start-up. Touching the real pad
takes the player over; Start gives it back to the demo from the beginning.
*/
#ifndef FP_DEMO_H
#define FP_DEMO_H

#include "fp_controller.h"

#define FP_DEMO_MAX_POINTS 64
#define FP_DEMO_MAX_JUMPS 4

typedef struct
{
	int autopilot;                                     /* the demo drives (else the pad does) */
	int planned;
	float start[2], goal[2], face[2];
	float point[FP_DEMO_MAX_POINTS][2];                /* the path, start to goal */
	float arc[FP_DEMO_MAX_POINTS];                     /* the distance along it of each point */
	int count;
	float length;
	float jump_at[FP_DEMO_MAX_JUMPS];                  /* distances along the path where the player jumps */
	int jump_count, next_jump;
	float jump_pulse;                                  /* seconds the jump button is still held */
	float progress;                                    /* the distance along the path the player has got to */
	float arrived;                                     /* seconds at the goal */
	float stuck;                                       /* seconds without progress */
} fp_demo;

/* plans a walk from (start) through the via points (in order; the walk may pass them at some distance) to (goal), ending turned toward (face); returns 0 if there is no way (the player then stands) */
int fp_demo_init(fp_demo *demo, fp_player *player, float start_x, float start_y, float goal_x, float goal_y, float face_x, float face_y, const float via[][2], int via_count);

/* one frame: the pad the player should be given. `user` is what the real pad gave: any stick or button takes over, Start restarts the demo */
void fp_demo_pad(fp_demo *demo, fp_player *player, const fp_pad *user, fp_pad *out, float seconds);

#endif
