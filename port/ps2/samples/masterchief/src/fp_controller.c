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

  - move_pill: the collision: the pill swept through the map's collision surfaces (the structure BSP's collision BSP,
    triangulated by tools/export_map.py), sliding along what stops it, standing on the walkable surfaces it meets.

All the numbers come from the map's tags (tools/export_map.py, MAP_* in map_assets.h). What is not ported: the BSP's
own tree (the surfaces are found through a grid over x, y, not the bsp3d nodes: the same surfaces), the collision of the
scenery and the Chief, the pitch autolevel, the unit camera's own pitch limits, the first-person weapon and the stun
penalties. The speeds the original gets from the animation graph (the walk speed of the alert pose) are not here either.
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
#define FALL_RESPAWN_Z (-60.f)

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
	input->trigger = pad->trigger * (1.f / 255.f);
}

/* ---------- what is collided with ---------- */

/* The map's collision: the structure BSP's surfaces, found through its own tree (the bsp3d: a node is a plane and two children,
   a leaf the surfaces on its boundary), and the objects' (the scenery placed in the world, the Chief), found through a grid over
   x, y. Both are triangles in one array; a query gathers the triangles near a box, each once. */

#define MAX_CANDIDATES 2048
#define MAX_TREE_DEPTH 128

static unsigned short candidates[MAX_CANDIDATES];
static unsigned stamp[MAP_COL_TRIS], stamp_now;

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(const float *a, const float *b, float *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

static int add_candidate(int count, unsigned tri)
{
	if (stamp[tri] == stamp_now) return count;
	stamp[tri] = stamp_now;
	if (count < MAX_CANDIDATES) candidates[count++] = (unsigned short)tri;
	return count;
}

/* the triangles of a box: down the BSP's tree to the leaves it reaches (a node's plane is crossed by the box, or it is wholly to one
   side: only that side's child), the surfaces of those leaves, then the objects in the grid cells the box touches */
static int gather(const float *lo, const float *hi)
{
	unsigned stack[MAX_TREE_DEPTH * 2];
	float centre[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) }, half[3] = { 0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]), 0.5f * (hi[2] - lo[2]) };
	int count = 0, depth = 0, cx, cy;

	stamp_now++;
	stack[depth++] = 0;
	while (depth)
	{
		unsigned node = stack[--depth];

		while (node < MAP_COL_NODES)                       /* (a child that is no node of the tree, nor a leaf, ends the walk) */
		{
			const unsigned *n = &map_col_nodes[node * 3];
			const float *plane = &map_col_planes[n[0] * 4];
			float distance = plane[0] * centre[0] + plane[1] * centre[1] + plane[2] * centre[2] - plane[3];
			float reach = fabsf(plane[0]) * half[0] + fabsf(plane[1]) * half[1] + fabsf(plane[2]) * half[2];
			unsigned child;

			if (distance - reach >= 0.f) child = n[2];            /* in front of it */
			else if (distance + reach < 0.f) child = n[1];        /* behind it */
			else { if (depth < MAX_TREE_DEPTH * 2) stack[depth++] = n[1]; child = n[2]; }
			if (child & 0x80000000u)
			{
				unsigned leaf = child & 0x7FFFFFFFu, i;

				if (leaf < MAP_COL_LEAVES)
					for (i = map_col_leaf_start[leaf]; i < map_col_leaf_start[leaf + 1]; i++)
					{
						unsigned surface = map_col_leaf_items[i], t;

						for (t = map_col_surface_tri[surface]; t < map_col_surface_tri[surface + 1]; t++) count = add_candidate(count, t);
					}
				break;
			}
			node = child;
		}
	}
	/* the objects */
	{
		int x0 = (int)floorf((lo[0] - MAP_OBJ_X0) / MAP_OBJ_CELL), x1 = (int)floorf((hi[0] - MAP_OBJ_X0) / MAP_OBJ_CELL);
		int y0 = (int)floorf((lo[1] - MAP_OBJ_Y0) / MAP_OBJ_CELL), y1 = (int)floorf((hi[1] - MAP_OBJ_Y0) / MAP_OBJ_CELL);

		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 >= MAP_OBJ_W) x1 = MAP_OBJ_W - 1;
		if (y1 >= MAP_OBJ_H) y1 = MAP_OBJ_H - 1;
		for (cy = y0; cy <= y1; cy++)
			for (cx = x0; cx <= x1; cx++)
			{
				unsigned i, end = map_obj_start[cy * MAP_OBJ_W + cx + 1];

				for (i = map_obj_start[cy * MAP_OBJ_W + cx]; i < end; i++) count = add_candidate(count, map_obj_items[i]);
			}
	}
	return count;
}

/* ---------- the ground ---------- */

int fp_ground_in(float x, float y, float z_max, float depth, float *z_out, float *normal_z, float normal_xy[2])
{
	const float lo[3] = { x - 1e-4f, y - 1e-4f, z_max - depth }, hi[3] = { x + 1e-4f, y + 1e-4f, z_max };
	int n = gather(lo, hi), i, found = 0;
	float best = 0.f, best_nz = 0.f, best_nx = 0.f, best_ny = 0.f;

	for (i = 0; i < n; i++)
	{
		const unsigned short *t = &map_col_tri[candidates[i] * 3];
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

int fp_ground(float x, float y, float z_max, float *z_out, float *normal_z, float normal_xy[2])
{
	return fp_ground_in(x, y, z_max, 8.f, z_out, normal_z, normal_xy);
}

/* the first surface a segment crosses, from its front (Moller-Trumbore, the determinant positive for a segment against the normal) */
int fp_trace(const float a[3], const float b[3], float *t_out, float normal[3], unsigned *triangle)
{
	float lo[3], hi[3], dir[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, best = 2.f;
	int n, i, k, found = 0;

	for (k = 0; k < 3; k++) { lo[k] = fminf(a[k], b[k]) - 1e-3f; hi[k] = fmaxf(a[k], b[k]) + 1e-3f; }
	n = gather(lo, hi);
	for (i = 0; i < n; i++)
	{
		const unsigned short *t = &map_col_tri[candidates[i] * 3];
		const float *p0 = &map_col_pos[t[0] * 3], *p1 = &map_col_pos[t[1] * 3], *p2 = &map_col_pos[t[2] * 3];
		float e1[3], e2[3], pvec[3], tvec[3], qvec[3], det, u, v, f;

		sub3(p1, p0, e1); sub3(p2, p0, e2);
		cross3(dir, e2, pvec);
		det = dot3(e1, pvec);
		if (det < 1e-12f) continue;                         /* parallel, or facing away */
		sub3(a, p0, tvec);
		u = dot3(tvec, pvec);
		if (u < 0.f || u > det) continue;
		cross3(tvec, e1, qvec);
		v = dot3(dir, qvec);
		if (v < 0.f || u + v > det) continue;
		f = dot3(e2, qvec) / det;
		if (f < 0.f || f > 1.f || f >= best) continue;
		best = f; found = 1;
		if (triangle) *triangle = candidates[i];
		if (normal) { cross3(e1, e2, normal); f = sqrtf(dot3(normal, normal)); if (f > 0.f) { normal[0] /= f; normal[1] /= f; normal[2] /= f; } }
	}
	if (found && t_out) *t_out = best;
	return found;
}

/* ---------- the pill against the collision BSP ---------- */

/* The player is a pill (a capsule: radius MAP_RADIUS, MAP_HEIGHT_STANDING tall, less crouched) standing on its feet's point. It is
   swept through the map's collision surfaces as two or three spheres a radius apart, which cover it to a few millimetres, so
   what it is stopped by, what it slides along and what it stands on are the surfaces the original's pill meets. Each sphere against
   each triangle is the usual swept test (the triangle's face, then its edges and vertices as the cylinders and spheres of the
   sphere's Minkowski sum); the surfaces are one-sided, as the collision BSP's are (the player is stopped from the front only). */

#define SKIN 0.001f                        /* the gap left to a surface: the player never rests in it */
#define SLIDE_ITERATIONS 4
#define STICK_DISTANCE 0.12f               /* a drop smaller than this is walked down, not fallen */

/* the smaller root of a t^2 + b t + c in [0, limit), or -1 */
static float smallest_root(float a, float b, float c, float limit)
{
	float disc = b * b - 4.f * a * c, root;

	if (fabsf(a) < 1e-12f || disc < 0.f) return -1.f;
	root = (-b - sqrtf(disc)) / (2.f * a);
	if (a < 0.f) root = (-b + sqrtf(disc)) / (2.f * a);      /* the root that comes first for either sign of a */
	return root >= 0.f && root < limit ? root : -1.f;
}

static int in_triangle(const float *point, const float *p0, const float *p1, const float *p2, const float *n)
{
	const float *q[4] = { p0, p1, p2, p0 };
	int i;

	for (i = 0; i < 3; i++)
	{
		float edge[3], to[3], c[3];

		sub3(q[i + 1], q[i], edge); sub3(point, q[i], to); cross3(edge, to, c);
		if (dot3(c, n) < -1e-6f) return 0;
	}
	return 1;
}

/* a sphere (centre c, radius r) moving by v (all of it is t 1) against one triangle: lowers *t_best and sets normal if it is hit sooner */
static int sweep_triangle(const float *c, const float *v, float r, unsigned tri, float *t_best, float *normal)
{
	const unsigned short *ix = &map_col_tri[tri * 3];
	const float *p[3] = { &map_col_pos[ix[0] * 3], &map_col_pos[ix[1] * 3], &map_col_pos[ix[2] * 3] };
	float e1[3], e2[3], n[3], length, nd, sd, t0, tt, to[3], hit[3], vv = dot3(v, v);
	int i, found = 0;

	sub3(p[1], p[0], e1); sub3(p[2], p[0], e2); cross3(e1, e2, n);
	length = sqrtf(dot3(n, n));
	if (length < 1e-12f) return 0;
	n[0] /= length; n[1] /= length; n[2] /= length;
	nd = dot3(n, v);
	if (nd >= 0.f) return 0;                           /* moving away from the front of it, or along it */
	sub3(c, p[0], to);
	sd = dot3(n, to);                                  /* the centre's distance in front of the plane */
	if (sd < -r) return 0;                             /* behind it: inside what it bounds */
	t0 = (r - sd) / nd;
	if (t0 >= *t_best) return 0;
	tt = t0 < 0.f ? 0.f : t0;
	for (i = 0; i < 3; i++) hit[i] = c[i] + v[i] * tt - n[i] * (sd + nd * tt);   /* where the sphere's centre would touch the plane */
	if (in_triangle(hit, p[0], p[1], p[2], n))
	{
		*t_best = tt; normal[0] = n[0]; normal[1] = n[1]; normal[2] = n[2];
		return 1;
	}
	/* the vertices */
	for (i = 0; i < 3; i++)
	{
		float d[3], t, h[3];

		sub3(c, p[i], d);
		if (dot3(d, d) < r * r) continue;              /* already inside the vertex's sphere: let it out */
		t = smallest_root(vv, 2.f * dot3(v, d), dot3(d, d) - r * r, *t_best);
		if (t >= 0.f)
		{
			int k;
			float len;

			for (k = 0; k < 3; k++) h[k] = c[k] + v[k] * t - p[i][k];
			len = sqrtf(dot3(h, h));
			*t_best = t; normal[0] = h[0] / len; normal[1] = h[1] / len; normal[2] = h[2] / len; found = 1;
		}
	}
	/* the edges */
	for (i = 0; i < 3; i++)
	{
		const float *a = p[i], *b = p[(i + 1) % 3];
		float edge[3], to_a[3], edge_sq, edge_v, edge_to, t, f, h[3], len;
		int k;

		sub3(b, a, edge); sub3(a, c, to_a);
		edge_sq = dot3(edge, edge); edge_v = dot3(edge, v); edge_to = dot3(edge, to_a);
		if (edge_sq * (r * r - dot3(to_a, to_a)) + edge_to * edge_to > 0.f) continue;     /* inside the edge's cylinder already: let it out */
		t = smallest_root(edge_sq * -vv + edge_v * edge_v, edge_sq * 2.f * dot3(v, to_a) - 2.f * edge_v * edge_to,
			edge_sq * (r * r - dot3(to_a, to_a)) + edge_to * edge_to, *t_best);
		if (t < 0.f) continue;
		f = (edge_v * t - edge_to) / edge_sq;
		if (f < 0.f || f > 1.f) continue;
		for (k = 0; k < 3; k++) h[k] = c[k] + v[k] * t - (a[k] + edge[k] * f);
		len = sqrtf(dot3(h, h));
		if (len < 1e-9f) continue;
		*t_best = t; normal[0] = h[0] / len; normal[1] = h[1] / len; normal[2] = h[2] / len; found = 1;
	}
	return found;
}

/* the spheres of the pill at the feet position `feet`: how many, and their centres */
static int pill_spheres(const float *feet, float crouch, float centres[3][3])
{
	float height = lerpf(MAP_HEIGHT_STANDING, MAP_HEIGHT_CROUCHING, crouch), low = MAP_RADIUS, high = height - MAP_RADIUS;
	int n = 0, i;

	if (high < low) high = low;
	for (i = 0; i < 3; i++) { centres[0][i] = feet[i]; centres[1][i] = feet[i]; centres[2][i] = feet[i]; }
	centres[n++][2] += low;
	if (high - low > 0.8f * MAP_RADIUS) centres[n++][2] += 0.5f * (low + high);
	centres[n++][2] += high;
	return n;
}

/* the pill moving by `disp` from `feet`: the fraction of it that is free, and the surface's normal where it is stopped (t 1: nothing is) */
static float sweep_pill(const float *feet, float crouch, const float *disp, float *normal)
{
	float centres[3][3], t = 1.f, lo[3], hi[3];
	int spheres = pill_spheres(feet, crouch, centres), s, i, n;

	lo[0] = fminf(feet[0], feet[0] + disp[0]) - MAP_RADIUS - SKIN; hi[0] = fmaxf(feet[0], feet[0] + disp[0]) + MAP_RADIUS + SKIN;
	lo[1] = fminf(feet[1], feet[1] + disp[1]) - MAP_RADIUS - SKIN; hi[1] = fmaxf(feet[1], feet[1] + disp[1]) + MAP_RADIUS + SKIN;
	lo[2] = fminf(feet[2], feet[2] + disp[2]) - SKIN; hi[2] = fmaxf(feet[2], feet[2] + disp[2]) + MAP_HEIGHT_STANDING + SKIN;
	n = gather(lo, hi);
	for (s = 0; s < spheres; s++)
		for (i = 0; i < n; i++) sweep_triangle(centres[s], disp, MAP_RADIUS, candidates[i], &t, normal);
	return t;
}

/* the closest point of a triangle to a point (Ericson, Real-Time Collision Detection) */
static void closest_on_triangle(const float *p, const float *a, const float *b, const float *c, float *out)
{
	float ab[3], ac[3], ap[3], bp[3], cp[3], d1, d2, d3, d4, d5, d6, vc, vb, va, v, w, denom;
	int i;

	sub3(b, a, ab); sub3(c, a, ac); sub3(p, a, ap);
	d1 = dot3(ab, ap); d2 = dot3(ac, ap);
	if (d1 <= 0 && d2 <= 0) { for (i = 0; i < 3; i++) out[i] = a[i]; return; }
	sub3(p, b, bp); d3 = dot3(ab, bp); d4 = dot3(ac, bp);
	if (d3 >= 0 && d4 <= d3) { for (i = 0; i < 3; i++) out[i] = b[i]; return; }
	vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { v = d1 / (d1 - d3); for (i = 0; i < 3; i++) out[i] = a[i] + v * ab[i]; return; }
	sub3(p, c, cp); d5 = dot3(ab, cp); d6 = dot3(ac, cp);
	if (d6 >= 0 && d5 <= d6) { for (i = 0; i < 3; i++) out[i] = c[i]; return; }
	vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { w = d2 / (d2 - d6); for (i = 0; i < 3; i++) out[i] = a[i] + w * ac[i]; return; }
	va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (i = 0; i < 3; i++) out[i] = b[i] + w * (c[i] - b[i]); return; }
	denom = 1.f / (va + vb + vc); v = vb * denom; w = vc * denom;
	for (i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v + ac[i] * w;
}

/* how far into the front of a surface the pill's deepest sphere is at `feet` with the crouch `crouch` (0 or less: none), and the push
   that takes that sphere straight out of it */
static float deepest_overlap(const float *feet, float crouch, float *push)
{
	float centres[3][3], deepest = 0.f, lo[3], hi[3];
	int spheres = pill_spheres(feet, crouch, centres), s, i, n;

	lo[0] = feet[0] - 1.f; lo[1] = feet[1] - 1.f; lo[2] = feet[2] - 0.5f;
	hi[0] = feet[0] + 1.f; hi[1] = feet[1] + 1.f; hi[2] = feet[2] + 1.5f;
	n = gather(lo, hi);
	push[0] = push[1] = push[2] = 0.f;
	for (s = 0; s < spheres; s++)
		for (i = 0; i < n; i++)
		{
			const unsigned short *ix = &map_col_tri[candidates[i] * 3];
			const float *a = &map_col_pos[ix[0] * 3], *b = &map_col_pos[ix[1] * 3], *c = &map_col_pos[ix[2] * 3];
			float e1[3], e2[3], nrm[3], to[3], closest[3], d[3], length, depth;

			sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, nrm);
			length = sqrtf(dot3(nrm, nrm));
			if (length < 1e-12f) continue;
			sub3(centres[s], a, to);
			if (dot3(to, nrm) < 0.f) continue;                  /* behind it */
			closest_on_triangle(centres[s], a, b, c, closest);
			sub3(centres[s], closest, d);
			length = sqrtf(dot3(d, d));
			depth = MAP_RADIUS - length;
			if (depth > deepest && length > 1e-6f)
			{
				deepest = depth;
				push[0] = d[0] / length * depth; push[1] = d[1] / length * depth; push[2] = d[2] / length * depth;
			}
		}
	return deepest;
}

/* The sweep lets a sphere that is already inside an edge's cylinder or a vertex's sphere out rather than stop it, so sliding across
   the seam of two surfaces can leave the pill a little into one. After a move each sphere that is nearer a surface (from the
   front) than its radius is pushed straight out of it, a few passes, never more than a few centimetres at once. */
static void depenetrate(fp_player *p)
{
	int pass;

	for (pass = 0; pass < 3; pass++)
	{
		float push[3], deepest = deepest_overlap(p->pos, p->crouch, push);

		if (deepest < 0.5f * SKIN) return;
		if (deepest > 0.05f) { float k = 0.05f / deepest; push[0] *= k; push[1] *= k; push[2] *= k; }
		p->pos[0] += push[0]; p->pos[1] += push[1]; p->pos[2] += push[2];
	}
}

/* moves the pill by disp, sliding along what stops it (the velocity loses what goes into it too); the normals met go to contacts.
   A pill that is walking does not slide up a surface too steep to stand on (a wall leaning back a little): it slides along the level line on it */
static int move_pill(fp_player *p, const float *disp, int walking, float min_normal_k, float contacts[][3], int max_contacts)
{
	float remaining[3] = { disp[0], disp[1], disp[2] };
	int iteration, contact_count = 0;

	for (iteration = 0; iteration < SLIDE_ITERATIONS; iteration++)
	{
		float length = sqrtf(dot3(remaining, remaining)), normal[3], t, travel, into;
		int i;

		if (length < 1e-7f) break;
		t = sweep_pill(p->pos, p->crouch, remaining, normal);
		if (t >= 1.f) { for (i = 0; i < 3; i++) p->pos[i] += remaining[i]; break; }
		travel = t * length - SKIN;                    /* stop a skin short */
		if (travel > 0.f) for (i = 0; i < 3; i++) p->pos[i] += remaining[i] / length * travel;
		if (contact_count < max_contacts) { contacts[contact_count][0] = normal[0]; contacts[contact_count][1] = normal[1]; contacts[contact_count][2] = normal[2]; contact_count++; }
		/* what is left of the move, and the velocity, lose the part that goes into the surface */
		for (i = 0; i < 3; i++) remaining[i] *= 1.f - t;
		if (walking && normal[2] > 0.f && normal[2] < min_normal_k)
		{
			float level[3] = { -normal[1], normal[0], 0.f }, along = sqrtf(level[0] * level[0] + level[1] * level[1]), s;

			level[0] /= along; level[1] /= along;
			s = dot3(remaining, level);
			for (i = 0; i < 3; i++) remaining[i] = level[i] * s;
			s = dot3(p->vel, level);
			for (i = 0; i < 3; i++) p->vel[i] = level[i] * s;
			continue;
		}
		into = dot3(remaining, normal);
		if (into < 0.f) for (i = 0; i < 3; i++) remaining[i] -= normal[i] * into;
		into = dot3(p->vel, normal);
		if (into < 0.f) for (i = 0; i < 3; i++) p->vel[i] -= normal[i] * into;
	}
	depenetrate(p);
	return contact_count;
}

/* ---------- the biped, 30 times a second ---------- */

static void respawn(fp_player *p)
{
	fp_init(p, p->spawn[0], p->spawn[1], p->yaw);
}

void fp_init(fp_player *player, float x, float y, float yaw)
{
	float z = 0.f, nz = 1.f, nxy[2] = { 0.f, 0.f };

	memset(player, 0, sizeof(*player));
	fp_ground(x, y, 6.f, &z, &nz, nxy);
	player->pos[0] = x; player->pos[1] = y; player->pos[2] = z + 2.f * SKIN;
	memcpy(player->prev_pos, player->pos, sizeof(player->pos));
	player->ground_n[0] = nxy[0]; player->ground_n[1] = nxy[1]; player->ground_n[2] = nz;
	player->yaw = yaw;
	player->spawn[0] = x; player->spawn[1] = y;
	{
		static const int loadout[2] = { FPW_ASSAULT_RIFLE, FPW_PISTOL };

		fpw_inventory_init(&player->weapons, loadout, 2, 12345u);
		fpv_init(&player->view);
		{
			fpw_event ready = { FPW_EVENT_READY, loadout[0], 0, { 0 }, { 0 } };

			fpv_event(&player->view, &ready, &player->weapons.slots[0]);       /* the first weapon is readied: its animation plays */
		}
		player->field_of_view = player->fov_from = player->fov_to = MAP_UNIT_FOV;
		player->fov_time = 1.f;
	}
	depenetrate(player);                               /* a place against a wall: out of it first */
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

static void biped_tick(fp_player *p, float forward, float strafe, int jump, int crouch_held)
{
	const float min_normal_k = cosf(MAP_MAX_SLOPE);
	float fx = cosf(p->yaw), fy = sinf(p->yaw);
	float run_speed, side_speed, acceleration, mi, mj, dir[2];
	float c, magnitude, disp[3], contacts[8][3], best_nz = -1.f, best[3] = { 0.f, 0.f, 1.f };
	int jumped = 0, was_grounded = !p->airborne, i, contact_count;

	memcpy(p->prev_pos, p->pos, sizeof(p->pos));
	p->prev_crouch = p->crouch;

	/* the throttle is never longer than 1 (player_control.c) */
	magnitude = forward * forward + strafe * strafe;
	if (magnitude > 1.f) { magnitude = 1.f / sqrtf(magnitude); forward *= magnitude; strafe *= magnitude; }

	/* crouch: the camera height follows at the biped's crouch_transition_time */
	{
		float step = 1.f / (MAP_CROUCH_TIME * TICKS_PER_SECOND);
		float target = crouch_held ? fminf(p->crouch + step, 1.f) : fmaxf(p->crouch - step, 0.f), push[3];

		/* standing up is refused where the taller pill would be inside a ceiling (the crouch stays until there is room) */
		if (target < p->crouch && deepest_overlap(p->pos, target, push) > 0.004f) target = p->crouch;
		p->crouch = target;
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

	if (!p->airborne && jump)
	{
		p->vel[2] = fmaxf(p->vel[2], MAP_JUMP_VELOCITY);     /* bipeds.c: biped_jump */
		p->airborne = 1; jumped = 1;
	}

	if (p->airborne)
	{
		/* in the air only the airborne acceleration steers; gravity pulls */
		approach(p->vel, dir, 2, MAP_AIR_ACCEL / TICKS_PER_SECOND);
		p->vel[2] -= GRAVITY;
	}
	else
	{
		/* on the ground: the direction follows the slope, and the speed falls off uphill and rises downhill */
		const float *n = p->ground_n;
		float move[3], speed = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]), length, desired[3];

		move[0] = dir[0]; move[1] = dir[1];
		move[2] = -(dir[0] * n[0] + dir[1] * n[1]) / n[2];
		length = sqrtf(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
		if (length > 0.f) { move[0] /= length; move[1] /= length; move[2] /= length; }
		speed *= slope_speed_scale(move[2]);
		desired[0] = move[0] * speed; desired[1] = move[1] * speed; desired[2] = move[2] * speed;
		approach(p->vel, desired, 3, acceleration);
		if (speed == 0.f && dot3(p->vel, p->vel) < 1e-12f)
		{
			/* standing still: nothing moves it. (The push below on a surface the pill rests on at an edge or a vertex, whose normal
			   is not the stored ground's, would leave a sideways part that slides it.) */
			p->vel[0] = p->vel[1] = p->vel[2] = 0.f;
			p->airborne = 0;
			return;
		}
		/* the original presses the player into the ground (1/128 along the normal): the sweep below meets the ground each tick */
		for (i = 0; i < 3; i++) p->vel[i] -= n[i] * (1.f / 128.f);
	}

	/* the move: the pill is swept, sliding along what it meets; a walkable surface met is the ground */
	for (i = 0; i < 3; i++) disp[i] = p->vel[i];
	contact_count = move_pill(p, disp, was_grounded, min_normal_k, contacts, 8);
	for (i = 0; i < contact_count; i++)
		if (contacts[i][2] > best_nz) { best_nz = contacts[i][2]; best[0] = contacts[i][0]; best[1] = contacts[i][1]; best[2] = contacts[i][2]; }
	if (best_nz >= min_normal_k && (was_grounded || p->vel[2] <= 0.001f))
	{
		p->airborne = 0;                                   /* standing on it (a jump's rise is not a landing) */
		for (i = 0; i < 3; i++) p->ground_n[i] = best[i];
	}
	else if (was_grounded && !jumped)
	{
		/* the ground fell away under a step: follow it down if it is near, else this is a fall */
		float down[3] = { 0.f, 0.f, -STICK_DISTANCE }, normal[3] = { 0.f, 0.f, 1.f }, t = sweep_pill(p->pos, p->crouch, down, normal);

		if (t < 1.f && normal[2] >= min_normal_k)
		{
			p->pos[2] -= fmaxf(t * STICK_DISTANCE - SKIN, 0.f);
			for (i = 0; i < 3; i++) p->ground_n[i] = normal[i];
			p->vel[0] -= normal[0] * dot3(p->vel, normal); p->vel[1] -= normal[1] * dot3(p->vel, normal); p->vel[2] -= normal[2] * dot3(p->vel, normal);
		}
		else p->airborne = 1;
	}
	else p->airborne = 1;
	if (p->pos[2] < FALL_RESPAWN_Z) respawn(p);
}

/* the weapons' tick: the player's eye and aim, and what it asked of them (units.c's control flags, the edges of the buttons) */
static void weapon_tick(fp_player *p, unsigned edges, float forward, float strafe)
{
	float cp = cosf(p->pitch), speed;
	fpw_input input;
	int k, events_before;

	for (k = 0; k < 3; k++) p->weapons.aim.origin[k] = p->pos[k];
	p->weapons.aim.origin[2] += lerpf(MAP_CAMERA_STANDING, MAP_CAMERA_CROUCHING, p->crouch);
	p->weapons.aim.forward[0] = cp * cosf(p->yaw); p->weapons.aim.forward[1] = cp * sinf(p->yaw); p->weapons.aim.forward[2] = sinf(p->pitch);
	speed = dot3(p->weapons.aim.forward, p->vel);                    /* units.c: the unit's speed along the shot is added to the projectile's */
	for (k = 0; k < 3; k++) p->weapons.aim.velocity[k] = p->weapons.aim.forward[k] * (speed > 0.f ? speed : 0.f);
	input.trigger = p->trigger; input.reload = (edges & FP_RELOAD) != 0; input.switch_weapon = (edges & FP_SWITCH_WEAPON) != 0;
	input.melee = (edges & FP_MELEE) != 0; input.zoom = (edges & FP_ZOOM) != 0; input.grenade = (edges & FP_GRENADE) != 0;
	events_before = p->events.count;
	fpw_inventory_tick(&p->weapons, &input, &p->events);
	/* the first-person weapon hears what the weapon did this tick, then moves a frame on */
	{
		fpw_state *w = fpw_current_state(&p->weapons);
		fpv_input view_input;
		int e;

		for (e = events_before; e < p->events.count; e++) fpv_event(&p->view, &p->events.list[e], w);
		view_input.throttle[0] = forward; view_input.throttle[1] = strafe;
		view_input.yaw = p->yaw; view_input.pitch = p->pitch;
		view_input.rounds_loaded = w ? w->magazines[0].rounds_loaded : 0;
		view_input.zoomed = p->weapons.zoom_level >= 0;
		view_input.flying = p->airborne;
		view_input.overheated = w && (w->flags & FPW_FLAG_OVERHEATED);
		fpv_tick(&p->view, &view_input, w);
	}
}

static void tick(fp_player *p, float forward, float strafe, int jump, int crouch_held, unsigned edges)
{
	biped_tick(p, forward, strafe, jump, crouch_held);
	weapon_tick(p, edges, forward, strafe);
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
	float clamped_yaw, clamped_pitch, delta_yaw, delta_pitch, zoom_scale, frame_ticks = seconds * TICKS_PER_SECOND;

	/* both sticks' axes pushed: the stronger is stretched to 1, so a diagonal turns as fast as a straight push */
	if (absolute_pitch > 0.1f && absolute_yaw > 0.1f)
	{
		if (absolute_pitch > absolute_yaw) { absolute_yaw /= absolute_pitch; absolute_pitch = 1.f; }
		else { absolute_pitch /= absolute_yaw; absolute_yaw = 1.f; }
		look_scale = sqrtf(absolute_pitch * absolute_pitch + absolute_yaw * absolute_yaw);
	}
	clamped_yaw = clampf(input->yaw * look_scale, -1.f, 1.f);
	clamped_pitch = clampf(input->pitch * look_scale, -1.f, 1.f);

	/* the rates are degrees a second at full deflection (the original's are a tick: the rate over 30, 30 times a second); zoomed they are
	   over the magnification (player_control.c: unit_get_zoom_magnification) */
	zoom_scale = 1.f / fpw_zoom_magnification(&p->weapons);
	delta_yaw = evaluate_look_function(clamped_yaw) * (MAP_LOOK_YAW_RATE * PI_F / 180.f) * seconds * zoom_scale;
	delta_pitch = evaluate_look_function(clamped_pitch) * (MAP_LOOK_PITCH_RATE * PI_F / 180.f) * seconds * zoom_scale;

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

	/* player_control.c: the pitch autolevel (the controller's autocenter option, off unless asked for): walking forward without looking up
	   or down for MAP_MIN_AUTOLEVEL_TICKS levels the pitch out, the faster the faster the player goes and the further it is from level */
	if (p->autolevel_enabled && fabsf(input->forward_movement) > 0.5f && fabsf(delta_pitch) < 0.0001f)
	{
		p->autolevel_fraction += frame_ticks;
		{
			int whole = (int)p->autolevel_fraction;

			p->autolevel_fraction -= (float)whole;
			p->autolevel_ticks = p->autolevel_ticks + whole > 127 ? 127 : p->autolevel_ticks + whole;
		}
		p->use_autolevel = p->autolevel_ticks > (int)MAP_MIN_AUTOLEVEL_TICKS;
	}
	else { p->autolevel_fraction = 0.f; p->autolevel_ticks = 0; p->use_autolevel = 0; }
	if (p->use_autolevel)
	{
		float speed = sqrtf(dot3(p->vel, p->vel)), error = fabsf(p->pitch) * 0.63661975f * frame_ticks;
		float step = error * MAP_LOOK_AUTOLEVEL_SCALE * speed, difference = -p->pitch;

		p->pitch += clampf(difference, -step, step);                     /* interpolate_scalar toward level (the unit camera's autolevel is 0) */
	}
	p->pitch = clampf(p->pitch + delta_pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

void fp_update(fp_player *p, const fp_pad *pad, float seconds)
{
	fp_input input;
	int ticks = 0;
	unsigned edges;
	float target;

	fp_shape_input(pad, &input);
	p->events.count = 0;
	edges = pad->buttons & ~p->previous_buttons;         /* the buttons that went down this frame: the weapons see them once, on the next tick */
	p->pending_edges |= edges;
	p->previous_buttons = pad->buttons;
	p->trigger = input.trigger;
	if (edges & FP_AUTOLEVEL) p->autolevel_enabled = !p->autolevel_enabled;
	look(p, &input, seconds);
	p->tick_time += seconds;
	while (p->tick_time >= 1.f / TICKS_PER_SECOND)
	{
		p->tick_time -= 1.f / TICKS_PER_SECOND;
		if (ticks++ < 4)
		{
			tick(p, input.forward_movement, input.strafe, input.jump, input.crouch, p->pending_edges);   /* a long frame does not run the physics away */
			p->pending_edges = 0;
		}
	}
	if (ticks > 4) p->tick_time = 0.f;
	p->time += seconds;

	/* the lens eases to the weapon's field of view over 0.18 s (first_person_camera.c: transition_time) */
	target = fpw_field_of_view(&p->weapons, MAP_UNIT_FOV);
	if (p->fov_to != target) { p->fov_from = p->field_of_view; p->fov_to = target; p->fov_time = 0.f; }
	if (p->fov_time < 0.18f)
	{
		p->fov_time += seconds;
		p->field_of_view = p->fov_time >= 0.18f ? p->fov_to : lerpf(p->fov_from, p->fov_to, p->fov_time / 0.18f);
	}
	else p->field_of_view = p->fov_to;
}

/* ---------- the camera ---------- */

float fp_vertical_half_fov_degrees(float horizontal_radians)
{
	return atanf(tanf(0.5f * horizontal_radians) * 3.f / 4.f) * 180.f / PI_F;
}

float fp_field_of_view(const fp_player *p) { return p->field_of_view; }

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
