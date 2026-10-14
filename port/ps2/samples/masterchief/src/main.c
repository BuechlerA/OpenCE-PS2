/*
 * Master Chief, running with the assault rifle, turning on the spot: a PS2 (EE + GS) port of the
 * rotation scene rendered from a Halo Xbox map.
 *
 * The meshes, the 20-frame run cycle (as one skinning matrix per node per frame), the two base
 * maps (palettised, the Chief's with its colour-change already applied) and the visor's reflection
 * cube come from assets.c, written by tools/export_assets.py. Vertices are skinned, lit and
 * projected on the EE; the GS draws Gouraud-shaded textured triangles with a 16-bit Z buffer.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include <kernel.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsToolkit.h>

#include "assets.h"

/* ---------- small matrix type: p' = p * r + t (row vectors, as the exporter writes them) ---------- */

typedef struct { float r[3][3]; float t[3]; } xform;

static void xform_from_array(xform *m, const float *a)
{
	memcpy(m->r, a, 9 * sizeof(float));
	memcpy(m->t, a + 9, 3 * sizeof(float));
}

/* apply a, then b */
static void xform_then(const xform *a, const xform *b, xform *out)
{
	xform o;
	int i, j, k;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
		{
			o.r[i][j] = 0.f;
			for (k = 0; k < 3; k++)
				o.r[i][j] += a->r[i][k] * b->r[k][j];
		}
	for (j = 0; j < 3; j++)
	{
		o.t[j] = b->t[j];
		for (k = 0; k < 3; k++)
			o.t[j] += a->t[k] * b->r[k][j];
	}
	*out = o;
}

static void apply_point(const xform *m, const float *p, float *o)
{
	o[0] = p[0] * m->r[0][0] + p[1] * m->r[1][0] + p[2] * m->r[2][0] + m->t[0];
	o[1] = p[0] * m->r[0][1] + p[1] * m->r[1][1] + p[2] * m->r[2][1] + m->t[1];
	o[2] = p[0] * m->r[0][2] + p[1] * m->r[1][2] + p[2] * m->r[2][2] + m->t[2];
}

static void apply_vector(const xform *m, const float *p, float *o)
{
	o[0] = p[0] * m->r[0][0] + p[1] * m->r[1][0] + p[2] * m->r[2][0];
	o[1] = p[0] * m->r[0][1] + p[1] * m->r[1][1] + p[2] * m->r[2][1];
	o[2] = p[0] * m->r[0][2] + p[1] * m->r[1][2] + p[2] * m->r[2][2];
}

static void normalise(float *v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* ---------- scene constants ---------- */

#define ORBIT_SECONDS 4.0f          /* one turn */
#define CYCLE_SECONDS (2.0f / 3.0f) /* the run cycle's length: 20 frames at 30 fps */
#define FOV_HALF_DEGREES 14.0f
#define CAMERA_DISTANCE (MODEL_RADIUS * 4.3f)
#define CAMERA_HEIGHT (MODEL_RADIUS * 0.55f)
#define GRID_CELL 0.25f
#define GAMMA (1.0f / 2.2f)
#define CULL_SIGN (-1.0f)           /* front faces have this sign of screen-space area (y down) */

#define MAX_VERTS 2200

static float cam_pos[3], cam_right[3], cam_up[3], cam_fwd[3];
static float focal;                 /* pixels per unit of x/z */
static float screen_cx, screen_cy;
static float z_near, z_far;
static float light_dir[3][3];
static const float light_color[3][3] = { { 0.90f, 0.855f, 0.765f }, { 0.2025f, 0.2475f, 0.36f }, { 0.45f, 0.475f, 0.5f } };
static const float light_scale[3] = { 1.f, 1.f, 1.f };
static const float ambient[3] = { 0.14f, 0.15f, 0.18f };

/* ---------- textures ---------- */

static GSTEXTURE chief_tex, rifle_tex, mask_tex, rifle_mask_tex, display_tex;

static void texture_setup(GSGLOBAL *gs, GSTEXTURE *t, int size, const unsigned char *pixels, const unsigned int *clut)
{
	memset(t, 0, sizeof(*t));
	t->Width = size;
	t->Height = size;
	t->PSM = GS_PSM_T8;
	t->ClutPSM = GS_PSM_CT32;
	t->TBW = size / 64;
	t->Mem = (u32 *)pixels;
	t->Clut = (u32 *)clut;
	t->Filter = GS_FILTER_LINEAR;
	t->Vram = gsKit_vram_alloc(gs, gsKit_texture_size(t->Width, t->Height, t->PSM), GSKIT_ALLOC_USERBUFFER);
	t->VramClut = gsKit_vram_alloc(gs, gsKit_texture_size(16, 16, t->ClutPSM), GSKIT_ALLOC_USERBUFFER);
	gsKit_texture_upload(gs, t);
}

/* ---------- vertex lighting ---------- */

static unsigned char to_byte(float v)
{
	if (v < 0.f) v = 0.f;
	if (v > 1.f) v = 1.f;
	return (unsigned char)(v * 255.f + 0.5f);
}

/* the summed light on a vertex with normal n (before any gamma) */
static void light_sum(const float *n, float *sum)
{
	int l, c;

	for (c = 0; c < 3; c++) sum[c] = ambient[c];
	for (l = 0; l < 3; l++)
	{
		float d = n[0] * light_dir[l][0] + n[1] * light_dir[l][1] + n[2] * light_dir[l][2];

		if (d > 0.f)
			for (c = 0; c < 3; c++)
				sum[c] += d * light_color[l][c] * light_scale[l];
	}
}

/* diffuse light for a textured vertex: the GS modulates by colour / 128, and the texture is gamma
   encoded, so the light is too */
static void light_vertex(const float *n, unsigned char *rgb)
{
	float sum[3];
	int c;

	light_sum(n, sum);
	for (c = 0; c < 3; c++)
	{
		float v = powf(sum[c], GAMMA) * 128.f;

		rgb[c] = (unsigned char)(v > 255.f ? 255.f : v);
	}
}

/* what a cube map shows in direction r (given in the model's axes) */
static void cube_sample(const unsigned char *cube, const float *r, float *rgb)
{
	float d[3], a[3], u, v;
	int k, o1, o2, face, c, px, py;
	const unsigned char *texel;

	d[0] = r[1]; d[1] = r[2]; d[2] = r[0];            /* the cube's axes */
	for (c = 0; c < 3; c++) a[c] = fabsf(d[c]);
	k = a[0] >= a[1] && a[0] >= a[2] ? 0 : (a[1] >= a[2] ? 1 : 2);
	o1 = k == 0 ? 1 : 0;
	o2 = k == 2 ? 1 : 2;
	face = 2 * k + (d[k] > 0.f ? 0 : 1);
	u = 0.5f * (d[o1] / a[k] + 1.f);
	v = 0.5f * (d[o2] / a[k] + 1.f);
	px = (int)(u * 64.f); if (px > 63) px = 63; if (px < 0) px = 0;
	py = (int)(v * 64.f); if (py > 63) py = 63; if (py < 0) py = 0;
	texel = cube + ((face * 64 + py) * 64 + px) * 3;
	for (c = 0; c < 3; c++) rgb[c] = texel[c] * (1.f / 255.f);
}

/* a vertex's view of itself: n . v, the mirror direction, n . h for the key light */
static void shade_terms(const float *n, const float *pos, float *nv, float *refl, float *ndh)
{
	float v[3] = { cam_pos[0] - pos[0], cam_pos[1] - pos[1], cam_pos[2] - pos[2] }, h[3];
	int c;

	normalise(v);
	*nv = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
	if (*nv < 0.f) *nv = 0.f;
	for (c = 0; c < 3; c++) refl[c] = 2.f * *nv * n[c] - v[c];
	for (c = 0; c < 3; c++) h[c] = v[c] + light_dir[0][c];
	normalise(h);
	*ndh = n[0] * h[0] + n[1] * h[1] + n[2] * h[2];
	if (*ndh < 0.f) *ndh = 0.f;
}

/* the visor: the cube map it reflects, tinted between its perpendicular and parallel colours, and
   the key light's highlight */
static void visor_color(const float *n, const float *pos, float spec_mask, unsigned char *rgb)
{
	float nv, r[3], ndh, cube[3], fres;
	int c;

	shade_terms(n, pos, &nv, r, &ndh);
	cube_sample(visor_cube, r, cube);
	fres = 1.f - nv;
	{
		float spec = powf(ndh, 60.f) * (0.15f + 1.1f * spec_mask);

		for (c = 0; c < 3; c++)
		{
			float tint = visor_tint[c] * (1.f - fres) + visor_tint[3 + c] * fres;
			float val = cube[c] * tint * 1.35f + spec * (c == 0 ? 1.f : (c == 1 ? 0.97f : 0.9f));

			rgb[c] = to_byte(powf(val > 1.f ? 1.f : val, GAMMA));
		}
	}
}

/* a masked part's second pass (the armour's, the rifle's), added over the lit and textured first: the shader's reflection
   (0.06 + 0.40 m) * cube * (0.5 + fresnel) and highlight (0.15 + 1.1 m) * n.h^18, where the mask m
   comes from the texture (see tools/export_assets.py). What is added here is the part that scales
   with the mask; the GS multiplies it by the mask texel. The sum is in linear light but the frame
   is gamma encoded, so what is added is the encoded difference it makes on a typical armour pixel. */
static void mask_specular(const unsigned char *cube_map, const float *n, const float *pos, const float *lit_sum, unsigned char *rgb)
{
	float nv, r[3], ndh, cube[3], fres, spec;
	int c;

	shade_terms(n, pos, &nv, r, &ndh);
	cube_sample(cube_map, r, cube);
	fres = 1.f - nv;
	spec = powf(ndh, 18.f);
	for (c = 0; c < 3; c++)
	{
		float add = 1.14f * (0.40f * cube[c] * (0.5f + fres) + 1.1f * spec * (c == 0 ? 1.f : (c == 1 ? 0.97f : 0.9f)));
		float typical = 0.25f * lit_sum[c];                      /* a mid-tone armour pixel, lit */
		float enc = powf(typical + add, GAMMA) - powf(typical, GAMMA);

		rgb[c] = (unsigned char)(enc <= 0.f ? 0.f : (enc * 128.f > 255.f ? 255.f : enc * 128.f));
	}
}

/* ---------- drawing ---------- */

typedef struct { float x, y; int z; unsigned char rgb[3]; float u, v; } svert;

static svert chief_v[MAX_VERTS], chief_spec_v[MAX_VERTS], rifle_spec_v[MAX_VERTS], display_v[DISPLAY_VERTS];
static svert rifle_v[MAX_VERTS];

static int project(const float *p, svert *o)
{
	float zc = p[2];
	int iz;

	if (zc < 0.01f) return 0;
	o->x = screen_cx + p[0] / zc * focal;
	o->y = screen_cy - p[1] / zc * focal;
	iz = (int)(65535.f * (z_far - zc) / (z_far - z_near));
	o->z = iz < 1 ? 1 : (iz > 65534 ? 65534 : iz);
	return 1;
}

static u64 rgbaq(const unsigned char *c)
{
	return GS_SETREG_RGBAQ(c[0], c[1], c[2], 0x80, 0x00);
}

static void draw_textured(GSGLOBAL *gs, GSTEXTURE *tex, const svert *v, const unsigned short *tri, int first, int count, float tex_size)
{
	int i;

	for (i = first; i < first + count; i++)
	{
		const svert *a = &v[tri[i * 3 + 0]], *b = &v[tri[i * 3 + 1]], *c = &v[tri[i * 3 + 2]];
		float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);

		if (area * CULL_SIGN <= 0.f) continue;
		gsKit_prim_triangle_goraud_texture_3d(gs, tex,
			a->x, a->y, a->z, a->u * tex_size, a->v * tex_size,
			b->x, b->y, b->z, b->u * tex_size, b->v * tex_size,
			c->x, c->y, c->z, c->u * tex_size, c->v * tex_size,
			rgbaq(a->rgb), rgbaq(b->rgb), rgbaq(c->rgb));
	}
}

static void draw_flat_gouraud(GSGLOBAL *gs, const svert *v, const unsigned short *tri, int first, int count)
{
	int i;

	for (i = first; i < first + count; i++)
	{
		const svert *a = &v[tri[i * 3 + 0]], *b = &v[tri[i * 3 + 1]], *c = &v[tri[i * 3 + 2]];
		float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);

		if (area * CULL_SIGN <= 0.f) continue;
		gsKit_prim_triangle_gouraud_3d(gs, a->x, a->y, a->z, b->x, b->y, b->z, c->x, c->y, c->z,
			rgbaq(a->rgb), rgbaq(b->rgb), rgbaq(c->rgb));
	}
}

/* the ground grid, in model space, sliding back at the cycle's own stride */
static void draw_ground(GSGLOBAL *gs, const xform *world_to_view, float theta, float scroll)
{
	const float extent = MODEL_RADIUS * 3.2f;
	const int segments = 10;
	float c = cosf(theta), s = sinf(theta);
	int axis, k, seg;

	for (axis = 0; axis < 2; axis++)
	{
		float offset = axis == 0 ? -fmodf(scroll, GRID_CELL) : 0.f;
		int first = (int)floorf((-extent - offset) / GRID_CELL), last = (int)ceilf((extent - offset) / GRID_CELL);

		for (k = first; k <= last; k++)
		{
			float line = k * GRID_CELL + offset;       /* offset from the model's centre */

			for (seg = 0; seg < segments; seg++)
			{
				float t0 = -extent + 2.f * extent * seg / segments, t1 = -extent + 2.f * extent * (seg + 1) / segments;
				float m0[3], m1[3], w0[3], w1[3], p0[3], p1[3], f0, f1;
				svert s0, s1;
				unsigned char c0[3], c1[3];
				int i;

				/* model-space points about the centre, then the orbit, then the view */
				m0[0] = axis == 0 ? line : t0; m0[1] = axis == 0 ? t0 : line; m0[2] = 0.f;
				m1[0] = axis == 0 ? line : t1; m1[1] = axis == 0 ? t1 : line; m1[2] = 0.f;
				f0 = 1.f - sqrtf(m0[0] * m0[0] + m0[1] * m0[1]) / (extent * 1.0f);
				f1 = 1.f - sqrtf(m1[0] * m1[0] + m1[1] * m1[1]) / (extent * 1.0f);
				if (f0 < 0.f) f0 = 0.f;
				if (f1 < 0.f) f1 = 0.f;
				/* rotate (row-vector convention [[c,s],[-s,c]]) */
				w0[0] = m0[0] * c - m0[1] * s + MODEL_CENTER_X; w0[1] = m0[0] * s + m0[1] * c + MODEL_CENTER_Y; w0[2] = MODEL_FLOOR_Z;
				w1[0] = m1[0] * c - m1[1] * s + MODEL_CENTER_X; w1[1] = m1[0] * s + m1[1] * c + MODEL_CENTER_Y; w1[2] = MODEL_FLOOR_Z;
				apply_point(world_to_view, w0, p0);
				apply_point(world_to_view, w1, p1);
				if (!project(p0, &s0) || !project(p1, &s1)) continue;
				for (i = 0; i < 3; i++)
				{
					float base[3] = { 0.075f, 0.09f, 0.11f }, add[3] = { 0.10f, 0.14f, 0.17f };

					c0[i] = to_byte(powf(base[i] + add[i], GAMMA) * powf(f0, 1.2f) + 0.0f);
					c1[i] = to_byte(powf(base[i] + add[i], GAMMA) * powf(f1, 1.2f) + 0.0f);
				}
				gsKit_prim_line_goraud_3d(gs, s0.x, s0.y, 1, s1.x, s1.y, 1, rgbaq(c0), rgbaq(c1));
			}
		}
	}
}

/* ---------- frame ---------- */

static volatile int vsync_count;

static int vsync_handler(int cause)
{
	(void)cause;
	vsync_count++;
	return 0;
}

static void skin_chief(const xform *node_world[NODE_COUNT], const xform *node_view[NODE_COUNT], const xform *node_normal[NODE_COUNT], int visor_first_vert)
{
	int i, c;

	for (i = 0; i < CHIEF_VERTS; i++)
	{
		const float *p = &chief_pos[i * 3], *n = &chief_nrm[i * 3];
		int n0 = chief_node[i * 2], n1 = chief_node[i * 2 + 1];
		float w = chief_weight[i], a[3], b[3], pv[3], na[3], nb[3], nw[3], pw[3], pb[3];
		svert *o = &chief_v[i];

		apply_point(node_view[n0], p, a);
		apply_point(node_view[n1], p, b);
		for (c = 0; c < 3; c++) pv[c] = a[c] * w + b[c] * (1.f - w);
		apply_vector(node_normal[n0], n, na);
		apply_vector(node_normal[n1], n, nb);
		for (c = 0; c < 3; c++) nw[c] = na[c] * w + nb[c] * (1.f - w);
		normalise(nw);
		project(pv, o);
		o->u = chief_uv[i * 2]; o->v = chief_uv[i * 2 + 1];
		apply_point(node_world[n0], p, pw);
		apply_point(node_world[n1], p, pb);
		for (c = 0; c < 3; c++) pw[c] = pw[c] * w + pb[c] * (1.f - w);
		if (i >= visor_first_vert)
			visor_color(nw, pw, visor_spec[i - visor_first_vert], o->rgb);
		else
		{
			float sum[3];

			light_vertex(nw, o->rgb);
			light_sum(nw, sum);
			chief_spec_v[i] = *o;
			mask_specular(armor_cube, nw, pw, sum, chief_spec_v[i].rgb);
		}
	}
}

static void render_frame(GSGLOBAL *gs, float seconds)
{
	float theta = 2.f * (float)M_PI * seconds / ORBIT_SECONDS;
	float cycles = seconds / CYCLE_SECONDS;
	int frame = (int)(fmodf(cycles, 1.f) * ANIM_FRAMES) % ANIM_FRAMES;
	float scroll = -ANIM_STRIDE * cycles;
	float c = cosf(theta), s = sinf(theta);
	xform orbit, view, world_to_view, bone[NODE_COUNT], bone_world[NODE_COUNT], bone_view[NODE_COUNT], bone_normal[NODE_COUNT], rifle_m, rifle_view, rifle_world, rifle_normal;
	const xform *pw[NODE_COUNT], *pv[NODE_COUNT], *pn[NODE_COUNT];
	int i, j, k;

	/* orbit about the model's centre; row-vector rotation [[c,s,0],[-s,c,0],[0,0,1]] */
	memset(&orbit, 0, sizeof(orbit));
	orbit.r[0][0] = c; orbit.r[0][1] = s; orbit.r[1][0] = -s; orbit.r[1][1] = c; orbit.r[2][2] = 1.f;
	{
		const float ctr[3] = { MODEL_CENTER_X, MODEL_CENTER_Y, MODEL_CENTER_Z };
		float rc[3];

		apply_vector(&orbit, ctr, rc);
		for (i = 0; i < 3; i++) orbit.t[i] = ctr[i] - rc[i];
	}
	/* the view: the camera's right, up and forward as the columns */
	for (i = 0; i < 3; i++)
	{
		view.r[i][0] = cam_right[i]; view.r[i][1] = cam_up[i]; view.r[i][2] = cam_fwd[i];
	}
	for (j = 0; j < 3; j++)
		view.t[j] = -(cam_pos[0] * view.r[0][j] + cam_pos[1] * view.r[1][j] + cam_pos[2] * view.r[2][j]);
	xform_then(&orbit, &view, &world_to_view);

	for (i = 0; i < NODE_COUNT; i++)
	{
		xform_from_array(&bone[i], &anim_skin[(frame * NODE_COUNT + i) * 12]);
		xform_then(&bone[i], &orbit, &bone_world[i]);
		xform_then(&bone[i], &world_to_view, &bone_view[i]);
		bone_normal[i] = bone_world[i];
		pw[i] = &bone_world[i]; pv[i] = &bone_view[i]; pn[i] = &bone_normal[i];
	}
	xform_from_array(&rifle_m, &anim_rifle[frame * 12]);
	xform_then(&rifle_m, &orbit, &rifle_world);
	xform_then(&rifle_m, &world_to_view, &rifle_view);
	rifle_normal = rifle_world;

	/* backdrop */
	gsKit_clear(gs, GS_SETREG_RGBAQ(0x20, 0x26, 0x2c, 0x00, 0x00));
	gsKit_set_test(gs, GS_ZTEST_OFF);
	draw_ground(gs, &world_to_view, theta, scroll);
	gsKit_set_test(gs, GS_ZTEST_ON);

	skin_chief(pw, pv, pn, CHIEF_VERTS - 69);
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		float pp[3], pw[3], nn[3], sum[3];
		svert *o = &rifle_v[i];

		apply_point(&rifle_view, &rifle_pos[i * 3], pp);
		apply_point(&rifle_world, &rifle_pos[i * 3], pw);
		apply_vector(&rifle_normal, &rifle_nrm[i * 3], nn);
		normalise(nn);
		project(pp, o);
		o->u = rifle_uv[i * 2]; o->v = rifle_uv[i * 2 + 1];
		light_vertex(nn, o->rgb);
		light_sum(nn, sum);
		rifle_spec_v[i] = *o;
		mask_specular(i < RIFLE_GRIP_VERTS ? rifle_grip_cube : rifle_gun_cube, nn, pw, sum, rifle_spec_v[i].rgb);
	}
	/* the ammo display sits on the rifle's top, a hair above it: unlit (it glows), and nudged toward the
	   camera so the 16-bit Z buffer doesn't let the receiver through */
	for (i = 0; i < DISPLAY_VERTS; i++)
	{
		float pp[3];
		svert *o = &display_v[i];

		apply_point(&rifle_view, &display_pos[i * 3], pp);
		project(pp, o);
		o->z = o->z + 120 > 65534 ? 65534 : o->z + 120;
		o->u = display_uv[i * 2]; o->v = display_uv[i * 2 + 1];
		o->rgb[0] = o->rgb[1] = o->rgb[2] = 128;
	}
	(void)k;

	draw_textured(gs, &chief_tex, chief_v, chief_tri, CHIEF_ARMOR_FIRST_TRI, CHIEF_ARMOR_TRIS, 512.f);
	draw_flat_gouraud(gs, chief_v, chief_tri, CHIEF_VISOR_FIRST_TRI, CHIEF_VISOR_TRIS);
	/* the armour's reflection and highlight: its triangles again, the mask texture added on */
#ifndef NO_SPECULAR_PASS
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	draw_textured(gs, &mask_tex, chief_spec_v, chief_tri, CHIEF_ARMOR_FIRST_TRI, CHIEF_ARMOR_TRIS, 512.f);
	gs->PrimAlphaEnable = GS_SETTING_OFF;
#endif
	draw_textured(gs, &rifle_tex, rifle_v, rifle_tri, 0, RIFLE_TRIS, 512.f);
	draw_textured(gs, &display_tex, display_v, display_tri, 0, DISPLAY_TRIS, 256.f);
#ifndef NO_SPECULAR_PASS
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	draw_textured(gs, &rifle_mask_tex, rifle_spec_v, rifle_tri, 0, RIFLE_TRIS, 256.f);
	gs->PrimAlphaEnable = GS_SETTING_OFF;
#endif
}

int main(void)
{
	GSGLOBAL *gs;
	float refresh;
	int i;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gs = gsKit_init_global_custom(1024 * 1024, 16 * 1024);
	gs->PSM = GS_PSM_CT32;
	gs->PSMZ = GS_PSMZ_16S;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->ZBuffering = GS_SETTING_ON;
	gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_init_screen(gs);
	gsKit_mode_switch(gs, GS_ONESHOT);
	gsKit_set_test(gs, GS_ZTEST_ON);

	FlushCache(0);
	texture_setup(gs, &chief_tex, 512, chief_tex_pixels, chief_tex_clut);
	texture_setup(gs, &rifle_tex, 512, rifle_tex_pixels, rifle_tex_clut);
	texture_setup(gs, &mask_tex, 512, mask_tex_pixels, mask_tex_clut);
	texture_setup(gs, &rifle_mask_tex, 256, rifle_mask_pixels, rifle_mask_clut);
	texture_setup(gs, &display_tex, 256, display_tex_pixels, display_tex_clut);

	/* camera: in front of the model (+x), a little above its middle, looking at the centre */
	{
		float centre[3] = { MODEL_CENTER_X, MODEL_CENTER_Y, MODEL_CENTER_Z }, up[3] = { 0.f, 0.f, 1.f };
		static const float key[3] = { 0.6f, -0.5f, 0.7f }, fill[3] = { 0.3f, 0.8f, 0.2f }, back[3] = { -0.8f, -0.1f, 0.3f };

		cam_pos[0] = centre[0] + CAMERA_DISTANCE; cam_pos[1] = centre[1]; cam_pos[2] = centre[2] + CAMERA_HEIGHT;
		for (i = 0; i < 3; i++) cam_fwd[i] = centre[i] - cam_pos[i];
		normalise(cam_fwd);
		cam_right[0] = cam_fwd[1] * up[2] - cam_fwd[2] * up[1];
		cam_right[1] = cam_fwd[2] * up[0] - cam_fwd[0] * up[2];
		cam_right[2] = cam_fwd[0] * up[1] - cam_fwd[1] * up[0];
		normalise(cam_right);
		cam_up[0] = cam_right[1] * cam_fwd[2] - cam_right[2] * cam_fwd[1];
		cam_up[1] = cam_right[2] * cam_fwd[0] - cam_right[0] * cam_fwd[2];
		cam_up[2] = cam_right[0] * cam_fwd[1] - cam_right[1] * cam_fwd[0];
		memcpy(light_dir[0], key, sizeof(key)); memcpy(light_dir[1], fill, sizeof(fill)); memcpy(light_dir[2], back, sizeof(back));
		for (i = 0; i < 3; i++) normalise(light_dir[i]);
	}
	screen_cx = gs->Width * 0.5f;
	screen_cy = gs->Height * 0.5f;
	focal = (1.f / tanf(FOV_HALF_DEGREES * (float)M_PI / 180.f)) * gs->Height * 0.5f;
	z_near = CAMERA_DISTANCE - MODEL_RADIUS * 1.5f;
	z_far = CAMERA_DISTANCE + MODEL_RADIUS * 2.5f;

	refresh = gs->Mode == GS_MODE_PAL ? 50.f : 60.f;
	gsKit_add_vsync_handler(vsync_handler);

	for (;;)
	{
#ifdef DEBUG_SECONDS                 /* a still frame, for comparing builds */
		render_frame(gs, DEBUG_SECONDS);
#else
		render_frame(gs, vsync_count / refresh);
#endif
		gsKit_queue_exec(gs);
		gsKit_sync_flip(gs);
	}
	return 0;
}
