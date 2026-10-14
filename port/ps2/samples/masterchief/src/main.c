/*
 * Master Chief, running with the assault rifle, turning on the spot: a PS2 (EE + GS) port of the
 * rotation scene rendered from a Halo Xbox map.
 *
 * The meshes, the 20-frame run cycle (as one skinning matrix per node per frame), the two base
 * maps (palettised, the Chief's with its colour-change already applied) and the visor's reflection
 * cube come from assets.c, written by tools/export_assets.py. Vertices are skinned, lit and
 * projected on the EE; the GS draws Gouraud-shaded textured triangles with a 16-bit Z buffer.
 *
 * The EE's budget is one vsync, so the per-vertex path avoids what the EE does slowly: powf
 * (about 540 cycles) is replaced by a gamma table and repeated squaring, double arithmetic (software,
 * the EE's FPU is single precision) is kept out, each vertex is transformed three times rather than
 * four, and the lit vertices go to the GS as one REGLIST packet per mesh instead of one gsKit call
 * (with its own TEX0, TEX1 and CLUT load) per triangle. The transforms themselves run on VU0 in
 * macro mode (-DNO_VU0 keeps them on the FPU, to compare).
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include <kernel.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsInline.h>
#include <gsToolkit.h>

#include "assets.h"

#ifdef PROFILE                      /* -DPROFILE: time the frame's phases with the EE's cycle counter */
#include <timer.h>
enum { P_SETUP, P_GROUND, P_SKIN, P_RIFLE, P_SUBMIT, P_EXEC, P_FLIP, P_COUNT };
static const char *prof_name[P_COUNT] = { "setup", "ground", "skin_chief", "rifle+disp", "submit", "queue_exec", "sync_flip" };
static u32 prof_sum[P_COUNT], prof_last, prof_tris, prof_tris_culled;
static void prof_mark(int phase) { u32 t = cpu_ticks(); prof_sum[phase] += t - prof_last; prof_last = t; }
#define PROF_MARK(p) prof_mark(p)
#else
#define PROF_MARK(p) ((void)0)
#endif

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

static inline void apply_point(const xform *m, const float *p, float *o)
{
	o[0] = p[0] * m->r[0][0] + p[1] * m->r[1][0] + p[2] * m->r[2][0] + m->t[0];
	o[1] = p[0] * m->r[0][1] + p[1] * m->r[1][1] + p[2] * m->r[2][1] + m->t[1];
	o[2] = p[0] * m->r[0][2] + p[1] * m->r[1][2] + p[2] * m->r[2][2] + m->t[2];
}

static inline void apply_vector(const xform *m, const float *p, float *o)
{
	o[0] = p[0] * m->r[0][0] + p[1] * m->r[1][0] + p[2] * m->r[2][0];
	o[1] = p[0] * m->r[0][1] + p[1] * m->r[1][1] + p[2] * m->r[2][1];
	o[2] = p[0] * m->r[0][2] + p[1] * m->r[1][2] + p[2] * m->r[2][2];
}

/* one square root and one divide, then three multiplies (three divides cost as much again) */
static inline void normalise(float *v)
{
	float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];

	if (l2 > 1e-18f)
	{
		float inv = 1.f / sqrtf(l2);

		v[0] *= inv; v[1] *= inv; v[2] *= inv;
	}
}

static inline float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

#define MAX_VERTS 2200
#define MAX_TRIS 2000

/* ---------- VU0 in macro mode: the transforms ---------- */

#ifndef NO_VU0
typedef struct { float v[4]; } __attribute__((aligned(16))) vec4;
typedef struct { vec4 row[4]; } mat4;   /* rows r0, r1, r2 (w 0) and t (w 1): p' = p * m, p.w = 1 */

static void mat4_from_xform(mat4 *m, const xform *x)
{
	int i;

	for (i = 0; i < 3; i++)
	{
		m->row[i].v[0] = x->r[i][0]; m->row[i].v[1] = x->r[i][1]; m->row[i].v[2] = x->r[i][2]; m->row[i].v[3] = 0.f;
	}
	m->row[3].v[0] = x->t[0]; m->row[3].v[1] = x->t[1]; m->row[3].v[2] = x->t[2]; m->row[3].v[3] = 1.f;
}

/* the view matrix stays in vf20-vf23 for the frame: nothing else uses VU0's registers */
static inline void vu0_load_view(const mat4 *m)
{
	__asm__ __volatile__(
		"lqc2 $vf20, 0(%0)\n" "lqc2 $vf21, 16(%0)\n" "lqc2 $vf22, 32(%0)\n" "lqc2 $vf23, 48(%0)\n"
		: : "r"(m));
}

/* p (w 1) and n (w 0) by one matrix (a rotation, so n keeps its length), then p into the view */
static inline void vu0_transform(const mat4 *m, const vec4 *p, const vec4 *n, vec4 *pw, vec4 *nw, vec4 *pv)
{
	__asm__ __volatile__(
		"lqc2 $vf1, 0(%1)\n" "lqc2 $vf2, 0(%2)\n"
		"lqc2 $vf4, 0(%0)\n" "lqc2 $vf5, 16(%0)\n" "lqc2 $vf6, 32(%0)\n" "lqc2 $vf7, 48(%0)\n"
		"vmulax.xyz $ACC, $vf4, $vf1x\n" "vmadday.xyz $ACC, $vf5, $vf1y\n" "vmaddaz.xyz $ACC, $vf6, $vf1z\n" "vmaddw.xyz $vf12, $vf7, $vf1w\n"
		"vmove.w $vf12, $vf0\n"
		"vmulax.xyz $ACC, $vf4, $vf2x\n" "vmadday.xyz $ACC, $vf5, $vf2y\n" "vmaddz.xyz $vf15, $vf6, $vf2z\n"
		"vmulax.xyz $ACC, $vf20, $vf12x\n" "vmadday.xyz $ACC, $vf21, $vf12y\n" "vmaddaz.xyz $ACC, $vf22, $vf12z\n" "vmaddw.xyz $vf19, $vf23, $vf12w\n"
		"sqc2 $vf12, 0(%3)\n" "sqc2 $vf15, 0(%4)\n" "sqc2 $vf19, 0(%5)\n"
		: : "r"(m), "r"(p), "r"(n), "r"(pw), "r"(nw), "r"(pv) : "memory");
}

/* the same by two bones, blended by w = (w0, w1, 0, 0); the blended normal is normalised */
static inline void vu0_skin(const mat4 *b0, const mat4 *b1, const vec4 *p, const vec4 *n, const vec4 *w, vec4 *pw, vec4 *nw, vec4 *pv)
{
	__asm__ __volatile__(
		"lqc2 $vf1, 0(%2)\n" "lqc2 $vf2, 0(%3)\n" "lqc2 $vf3, 0(%4)\n"
		"lqc2 $vf4, 0(%0)\n" "lqc2 $vf5, 16(%0)\n" "lqc2 $vf6, 32(%0)\n" "lqc2 $vf7, 48(%0)\n"
		"lqc2 $vf8, 0(%1)\n" "lqc2 $vf9, 16(%1)\n" "lqc2 $vf10, 32(%1)\n" "lqc2 $vf11, 48(%1)\n"
		"vmulax.xyz $ACC, $vf4, $vf1x\n" "vmadday.xyz $ACC, $vf5, $vf1y\n" "vmaddaz.xyz $ACC, $vf6, $vf1z\n" "vmaddw.xyz $vf12, $vf7, $vf1w\n"
		"vmulax.xyz $ACC, $vf8, $vf1x\n" "vmadday.xyz $ACC, $vf9, $vf1y\n" "vmaddaz.xyz $ACC, $vf10, $vf1z\n" "vmaddw.xyz $vf13, $vf11, $vf1w\n"
		"vmulax.xyz $ACC, $vf12, $vf3x\n" "vmaddy.xyz $vf14, $vf13, $vf3y\n"
		"vmove.w $vf14, $vf0\n"
		"vmulax.xyz $ACC, $vf4, $vf2x\n" "vmadday.xyz $ACC, $vf5, $vf2y\n" "vmaddz.xyz $vf15, $vf6, $vf2z\n"
		"vmulax.xyz $ACC, $vf8, $vf2x\n" "vmadday.xyz $ACC, $vf9, $vf2y\n" "vmaddz.xyz $vf16, $vf10, $vf2z\n"
		"vmulax.xyz $ACC, $vf15, $vf3x\n" "vmaddy.xyz $vf17, $vf16, $vf3y\n"
		"vmul.xyz $vf18, $vf17, $vf17\n" "vaddy.x $vf18, $vf18, $vf18y\n" "vaddz.x $vf18, $vf18, $vf18z\n"
		"vrsqrt $Q, $vf0w, $vf18x\n"
		"vmulax.xyz $ACC, $vf20, $vf14x\n" "vmadday.xyz $ACC, $vf21, $vf14y\n" "vmaddaz.xyz $ACC, $vf22, $vf14z\n" "vmaddw.xyz $vf19, $vf23, $vf14w\n"
		"vwaitq\n" "vmulq.xyz $vf17, $vf17, $Q\n"
		"sqc2 $vf14, 0(%5)\n" "sqc2 $vf17, 0(%6)\n" "sqc2 $vf19, 0(%7)\n"
		: : "r"(b0), "r"(b1), "r"(p), "r"(n), "r"(w), "r"(pw), "r"(nw), "r"(pv) : "memory");
}

/* the meshes' vertices as the VU reads them: 16-byte quads, the position's w 1, the normal's 0,
   and the skinning weights as (w, 1 - w) */
static vec4 chief_p4[MAX_VERTS], chief_n4[MAX_VERTS], chief_w4[MAX_VERTS], rifle_p4[MAX_VERTS], rifle_n4[MAX_VERTS];

static void pack_vertices(void)
{
	int i, c;

	for (i = 0; i < CHIEF_VERTS; i++)
	{
		for (c = 0; c < 3; c++) { chief_p4[i].v[c] = chief_pos[i * 3 + c]; chief_n4[i].v[c] = chief_nrm[i * 3 + c]; }
		chief_p4[i].v[3] = 1.f; chief_n4[i].v[3] = 0.f;
		chief_w4[i].v[0] = chief_weight[i]; chief_w4[i].v[1] = 1.f - chief_weight[i]; chief_w4[i].v[2] = chief_w4[i].v[3] = 0.f;
	}
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		for (c = 0; c < 3; c++) { rifle_p4[i].v[c] = rifle_pos[i * 3 + c]; rifle_n4[i].v[c] = rifle_nrm[i * 3 + c]; }
		rifle_p4[i].v[3] = 1.f; rifle_n4[i].v[3] = 0.f;
	}
}
#endif

/* ---------- scene constants ---------- */

#define ORBIT_SECONDS 4.0f          /* one turn */
#define CYCLE_SECONDS (2.0f / 3.0f) /* the run cycle's length: 20 frames at 30 fps */
#define FOV_HALF_DEGREES 14.0f
#define CAMERA_DISTANCE (MODEL_RADIUS * 4.3f)
#define CAMERA_HEIGHT (MODEL_RADIUS * 0.55f)
#define GRID_CELL 0.25f
#define GAMMA (1.0f / 2.2f)
#define CULL_SIGN (-1.0f)           /* front faces have this sign of screen-space area (y down) */

static float cam_pos[3], cam_right[3], cam_up[3], cam_fwd[3];
static float focal;                 /* pixels per unit of x/z */
static float screen_cx, screen_cy;
static float z_near, z_far, z_scale; /* z_scale: 65535 / (z_far - z_near) */
static float light_dir[3][3];
static const float light_color[3][3] = { { 0.90f, 0.855f, 0.765f }, { 0.2025f, 0.2475f, 0.36f }, { 0.45f, 0.475f, 0.5f } };
static const float ambient[3] = { 0.14f, 0.15f, 0.18f };
static const float spec_tint[3] = { 1.f, 0.97f, 0.9f };   /* the highlight's colour */

/* ---------- textures ---------- */

typedef struct { GSTEXTURE t; int tw, th; } texture;   /* tw, th: log2 of the size, for TEX0 */

static texture chief_tex, rifle_tex, mask_tex, rifle_mask_tex, display_tex;

static void texture_setup(GSGLOBAL *gs, texture *x, int size, const unsigned char *pixels, const unsigned int *clut)
{
	GSTEXTURE *t = &x->t;

	memset(x, 0, sizeof(*x));
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
	for (x->tw = 0; (1 << x->tw) < size; x->tw++) ;   /* log2, for TEX0 */
	x->th = x->tw;
}

/* ---------- the slow functions, tabulated ---------- */

/* x^(1/2.2) for x in [0, 4), by a table and linear interpolation: within the 8 bits the GS gets */
#define GAMMA_LUT_SCALE 512.f
#define GAMMA_LUT_LAST 2047
static float gamma_lut[GAMMA_LUT_LAST + 2];
static float pow02_lut[257];         /* f^0.2 for f in [0, 1]: the grid's f^1.2 is f times this */

static inline float gamma_enc(float x)
{
	float f = x * GAMMA_LUT_SCALE, t;
	int i;

	if (f < 0.f) f = 0.f;
	if (f > (float)GAMMA_LUT_LAST) f = (float)GAMMA_LUT_LAST;
	i = (int)f;
	t = f - (float)i;
	return gamma_lut[i] + (gamma_lut[i + 1] - gamma_lut[i]) * t;
}

/* the same for an x known to lie in [0, 4): the lit sums are at least the ambient and at most 2.3 */
static inline float gamma_enc_in_range(float x)
{
	float f = x * GAMMA_LUT_SCALE;
	int i = (int)f;

	return gamma_lut[i] + (gamma_lut[i + 1] - gamma_lut[i]) * (f - (float)i);
}
#define QUARTER_GAMMA 0.53252f       /* 0.25^(1/2.2): (x / 4)^(1/2.2) = this times x^(1/2.2) */

static inline float pow18(float a) { float b = a * a, c = b * b, d = c * c, e = d * d; return e * b; }
static inline float pow60(float a) { float b = a * a, c = b * b, d = c * c, e = d * d, f = e * e; return f * e * d * c; }

static void tables_setup(void)
{
	int i;

	for (i = 0; i <= GAMMA_LUT_LAST + 1; i++) gamma_lut[i] = powf(i / GAMMA_LUT_SCALE, GAMMA);
	for (i = 0; i <= 256; i++) pow02_lut[i] = powf(i / 256.f, 0.2f);
}

/* ---------- vertex lighting ---------- */

static inline unsigned char to_byte(float v)
{
	if (v < 0.f) v = 0.f;
	if (v > 1.f) v = 1.f;
	return (unsigned char)(v * 255.f + 0.5f);
}

/* the GS modulates by colour / 128; v is an encoded value in [0, 1.88), so no clamp is needed */
static inline unsigned char to_half_byte(float v)
{
	return (unsigned char)(int)(v * 128.f);
}

/* the summed light on a vertex with normal n (before any gamma) */
static inline void light_sum(const float *n, float *sum)
{
	int l, c;

	for (c = 0; c < 3; c++) sum[c] = ambient[c];
	for (l = 0; l < 3; l++)
	{
		float d = dot(n, light_dir[l]);

		if (d > 0.f)
			for (c = 0; c < 3; c++)
				sum[c] += d * light_color[l][c];
	}
}

/* what a cube map shows in direction r (given in the model's axes) */
static inline void cube_sample(const unsigned char *cube, const float *r, float *rgb)
{
	float d[3], a[3], u, v, inv;
	int k, o1, o2, face, c, px, py;
	const unsigned char *texel;

	d[0] = r[1]; d[1] = r[2]; d[2] = r[0];            /* the cube's axes */
	for (c = 0; c < 3; c++) a[c] = fabsf(d[c]);
	k = a[0] >= a[1] && a[0] >= a[2] ? 0 : (a[1] >= a[2] ? 1 : 2);
	o1 = k == 0 ? 1 : 0;
	o2 = k == 2 ? 1 : 2;
	face = 2 * k + (d[k] > 0.f ? 0 : 1);
	inv = a[k] > 1e-18f ? 1.f / a[k] : 0.f;
	u = 0.5f * (d[o1] * inv + 1.f);
	v = 0.5f * (d[o2] * inv + 1.f);
	px = (int)(u * 64.f); if (px > 63) px = 63; if (px < 0) px = 0;
	py = (int)(v * 64.f); if (py > 63) py = 63; if (py < 0) py = 0;
	texel = cube + ((face * 64 + py) * 64 + px) * 3;
	for (c = 0; c < 3; c++) rgb[c] = texel[c] * (1.f / 255.f);
}

/* a vertex's view of itself: n . v, the mirror direction, n . h for the key light */
static inline void shade_terms(const float *n, const float *pos, float *nv, float *refl, float *ndh)
{
	float v[3] = { cam_pos[0] - pos[0], cam_pos[1] - pos[1], cam_pos[2] - pos[2] }, h[3];
	int c;

	normalise(v);
	*nv = dot(n, v);
	if (*nv < 0.f) *nv = 0.f;
	for (c = 0; c < 3; c++) refl[c] = 2.f * *nv * n[c] - v[c];
	for (c = 0; c < 3; c++) h[c] = v[c] + light_dir[0][c];
	normalise(h);
	*ndh = dot(n, h);
	if (*ndh < 0.f) *ndh = 0.f;
}

/* the visor: the cube map it reflects, tinted between its perpendicular and parallel colours, and
   the key light's highlight */
static u64 visor_color(const float *n, const float *pos, float spec_mask)
{
	float nv, r[3], ndh, cube[3], fres, spec;
	unsigned char rgb[3];
	int c;

	shade_terms(n, pos, &nv, r, &ndh);
	cube_sample(visor_cube, r, cube);
	fres = 1.f - nv;
	spec = pow60(ndh) * (0.15f + 1.1f * spec_mask);
	for (c = 0; c < 3; c++)
	{
		float tint = visor_tint[c] * (1.f - fres) + visor_tint[3 + c] * fres;
		float val = cube[c] * tint * 1.35f + spec * spec_tint[c];

		rgb[c] = to_byte(gamma_enc(val > 1.f ? 1.f : val));
	}
	return GS_SETREG_RGBAQ(rgb[0], rgb[1], rgb[2], 0x80, 0x00);
}

/* a lit, textured vertex: the diffuse colour the GS modulates the base map by, and the colour of its
   masked second pass (the armour's, the rifle's), added over the first: the shader's reflection
   (0.06 + 0.40 m) * cube * (0.5 + fresnel) and highlight (0.15 + 1.1 m) * n.h^18, where the mask m
   comes from the texture (see tools/export_assets.py). What is added is the part that scales with
   the mask; the GS multiplies it by the mask texel. The sum is in linear light but the frame is
   gamma encoded, so what is added is the encoded difference it makes on a typical armour pixel. */
static inline void lit_colors(const unsigned char *cube_map, const float *n, const float *pos, u64 *diffuse, u64 *specular)
{
	float sum[3], nv, r[3], ndh, cube[3], fres, spec;
	unsigned char d[3], s[3];
	int c;

	light_sum(n, sum);
	shade_terms(n, pos, &nv, r, &ndh);
	cube_sample(cube_map, r, cube);
	fres = 1.f - nv;
	spec = pow18(ndh);
	for (c = 0; c < 3; c++)
	{
		float enc = gamma_enc_in_range(sum[c]);
		float add = 1.14f * (0.40f * cube[c] * (0.5f + fres) + 1.1f * spec * spec_tint[c]);
		float typical = 0.25f * sum[c];                      /* a mid-tone armour pixel, lit: encoded, a quarter's worth of enc */

		d[c] = to_half_byte(enc);
		s[c] = to_half_byte(gamma_enc_in_range(typical + add) - QUARTER_GAMMA * enc);
	}
	*diffuse = GS_SETREG_RGBAQ(d[0], d[1], d[2], 0x80, 0x00);
	*specular = GS_SETREG_RGBAQ(s[0], s[1], s[2], 0x80, 0x00);
}

/* ---------- drawing ---------- */

/* a projected vertex: the screen position for culling, and its GS registers ready to copy */
typedef struct { float x, y; u64 rgbaq, uv, xyz2; } svert;

static svert chief_v[MAX_VERTS], rifle_v[MAX_VERTS], display_v[DISPLAY_VERTS];
static u64 chief_spec_rgbaq[MAX_VERTS], rifle_spec_rgbaq[MAX_VERTS];
static int gs_offset_x, gs_offset_y;   /* the GS's coordinate origin, in its 12.4 fixed point */
static unsigned short visible[MAX_TRIS * 3];

/* the GS's 12.4 vertex and texture coordinates */
static inline int to_gs_xy(float v, int offset)
{
	int i = (int)(v * 16.f) + offset;

	return i < 0 ? 0 : (i > 65535 ? 65535 : i);
}

static inline u64 uv_reg(float u, float v, float tex_size)
{
	int iu = (int)(u * tex_size * 16.f), iv = (int)(v * tex_size * 16.f), max = (int)tex_size * 16;

	if (iu < 0) iu = 0; else if (iu > max) iu = max;
	if (iv < 0) iv = 0; else if (iv > max) iv = max;
	return GS_SETREG_UV(iu, iv);
}

/* the vertex's view-space position to the screen and the 16-bit Z buffer; z_add nudges it toward the camera */
static inline void project(const float *p, int z_add, svert *o)
{
	float zc = p[2] < 0.01f ? 0.01f : p[2], inv = 1.f / zc;
	int iz = (int)((z_far - zc) * z_scale) + z_add;

	o->x = screen_cx + p[0] * inv * focal;
	o->y = screen_cy - p[1] * inv * focal;
	o->xyz2 = GS_SETREG_XYZ2(to_gs_xy(o->x, gs_offset_x), to_gs_xy(o->y, gs_offset_y), iz < 1 ? 1 : (iz > 65534 ? 65534 : iz));
}

/* the front-facing triangles of tri[first .. first + count), as a list of vertex indices */
static int cull(const svert *v, const unsigned short *tri, int first, int count)
{
	int i, n = 0;

	for (i = first; i < first + count; i++)
	{
		unsigned short ia = tri[i * 3 + 0], ib = tri[i * 3 + 1], ic = tri[i * 3 + 2];
		const svert *a = &v[ia], *b = &v[ib], *c = &v[ic];
		float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);

		if (area * CULL_SIGN <= 0.f) continue;
		visible[n++] = ia; visible[n++] = ib; visible[n++] = ic;
	}
#ifdef PROFILE
	prof_tris += n / 3;
	prof_tris_culled += count - n / 3;
#endif
	return n;
}

/* the primitive type and, for a textured one, its TEX0 and TEX1 (filter): once per mesh, as A+D
   register writes (a PRIM carried in a REGLIST tag is not honoured by PCSX2) */
static void set_prim(GSGLOBAL *gs, const texture *x)
{
	int regs = x ? 3 : 1;
	u64 *p = gsKit_heap_alloc(gs, regs, regs * 16, GIF_AD);

	*p++ = GIF_TAG_AD(regs);
	*p++ = GIF_AD;
	*p++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, x ? 1 : 0, gs->PrimFogEnable, gs->PrimAlphaEnable, gs->PrimAAEnable, x ? 1 : 0, gs->PrimContext, 0);
	*p++ = GS_PRIM;
	if (!x) return;
	*p++ = GS_SETREG_TEX0(x->t.Vram / 256, x->t.TBW, x->t.PSM, x->tw, x->th, gs->PrimAlphaEnable, 0,
		x->t.VramClut / 256, x->t.ClutPSM, x->t.ClutStorageMode, 0, GS_CLUT_STOREMODE_LOAD);
	*p++ = GS_TEX0_1 + gs->PrimContext;
	*p++ = GS_SETREG_TEX1(0, 0, x->t.Filter, x->t.Filter, 0, 0, 0);
	*p++ = GS_TEX1_1 + gs->PrimContext;
}

/* the mesh's front-facing triangles as one REGLIST packet: RGBAQ, UV and XYZ2 per vertex (the GS
   reads 64 bits per register in this mode). rgbaq, when given, replaces the vertices' own colour
   (the second pass). */
static void draw_textured(GSGLOBAL *gs, const texture *x, const svert *v, const u64 *rgbaq, const unsigned short *tri, int first, int count)
{
	int n = cull(v, tri, first, count), i, qwords;
	u64 *p;

	if (n == 0) return;
	set_prim(gs, x);
	qwords = (n * 3 + 1) / 2;                            /* three registers per vertex, two per quadword */
	p = gsKit_heap_alloc(gs, qwords, qwords * 16, GIF_AD);
	*p++ = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 3);
	*p++ = (u64)GS_RGBAQ | ((u64)GS_UV << 4) | ((u64)GS_XYZ2 << 8);
	for (i = 0; i < n; i++)
	{
		const svert *s = &v[visible[i]];

		*p++ = rgbaq ? rgbaq[visible[i]] : s->rgbaq;
		*p++ = s->uv;
		*p++ = s->xyz2;
	}
}

static void draw_flat_gouraud(GSGLOBAL *gs, const svert *v, const unsigned short *tri, int first, int count)
{
	int n = cull(v, tri, first, count), i;
	u64 *p;

	if (n == 0) return;
	set_prim(gs, NULL);
	p = gsKit_heap_alloc(gs, n, n * 16, GIF_AD);        /* two registers per vertex: one quadword */
	*p++ = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 2);
	*p++ = (u64)GS_RGBAQ | ((u64)GS_XYZ2 << 4);
	for (i = 0; i < n; i++)
	{
		const svert *s = &v[visible[i]];

		*p++ = s->rgbaq;
		*p++ = s->xyz2;
	}
}

/* the ground grid, in model space, sliding back at the cycle's own stride */
static float ground_color[3];       /* the grid's colour at the centre, gamma encoded */

static void draw_ground(GSGLOBAL *gs, const xform *world_to_view, float theta, float scroll)
{
	const float extent = MODEL_RADIUS * 3.2f, inv_extent = 1.f / extent;
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
				float m0[3], m1[3], w0[3], w1[3], p0[3], p1[3], f0, f1, g0, g1;
				svert s0, s1;
				unsigned char c0[3], c1[3];
				int i;

				/* model-space points about the centre, then the orbit, then the view */
				m0[0] = axis == 0 ? line : t0; m0[1] = axis == 0 ? t0 : line; m0[2] = 0.f;
				m1[0] = axis == 0 ? line : t1; m1[1] = axis == 0 ? t1 : line; m1[2] = 0.f;
				f0 = 1.f - sqrtf(m0[0] * m0[0] + m0[1] * m0[1]) * inv_extent;
				f1 = 1.f - sqrtf(m1[0] * m1[0] + m1[1] * m1[1]) * inv_extent;
				if (f0 < 0.f) f0 = 0.f;
				if (f1 < 0.f) f1 = 0.f;
				/* rotate (row-vector convention [[c,s],[-s,c]]) */
				w0[0] = m0[0] * c - m0[1] * s + MODEL_CENTER_X; w0[1] = m0[0] * s + m0[1] * c + MODEL_CENTER_Y; w0[2] = MODEL_FLOOR_Z;
				w1[0] = m1[0] * c - m1[1] * s + MODEL_CENTER_X; w1[1] = m1[0] * s + m1[1] * c + MODEL_CENTER_Y; w1[2] = MODEL_FLOOR_Z;
				apply_point(world_to_view, w0, p0);
				apply_point(world_to_view, w1, p1);
				if (p0[2] < 0.01f || p1[2] < 0.01f) continue;
				project(p0, 0, &s0);
				project(p1, 0, &s1);
				/* the lines fade with the distance from the centre as f^1.2 */
				g0 = f0 * pow02_lut[(int)(f0 * 256.f)];
				g1 = f1 * pow02_lut[(int)(f1 * 256.f)];
				for (i = 0; i < 3; i++)
				{
					c0[i] = to_byte(ground_color[i] * g0);
					c1[i] = to_byte(ground_color[i] * g1);
				}
				gsKit_prim_line_goraud_3d(gs, s0.x, s0.y, 1, s1.x, s1.y, 1,
					GS_SETREG_RGBAQ(c0[0], c0[1], c0[2], 0x80, 0x00), GS_SETREG_RGBAQ(c1[0], c1[1], c1[2], 0x80, 0x00));
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

/* each vertex: skinned by its two bones into the (orbited) world, where it is lit, then into the
   view and onto the screen */
#ifndef NO_VU0
static void skin_chief(const mat4 *bone_world, const mat4 *view, int visor_first_vert)
{
	int i;

	vu0_load_view(view);
	for (i = 0; i < CHIEF_VERTS; i++)
	{
		const mat4 *b0 = &bone_world[chief_node[i * 2]], *b1 = &bone_world[chief_node[i * 2 + 1]];
		vec4 pw, nw, pv;
		svert *o = &chief_v[i];

		if (chief_w4[i].v[1] == 0.f)   /* one bone: no blend, and a rotation keeps the normal's length */
			vu0_transform(b0, &chief_p4[i], &chief_n4[i], &pw, &nw, &pv);
		else
			vu0_skin(b0, b1, &chief_p4[i], &chief_n4[i], &chief_w4[i], &pw, &nw, &pv);
		project(pv.v, 0, o);
		o->uv = uv_reg(chief_uv[i * 2], chief_uv[i * 2 + 1], 512.f);
		if (i >= visor_first_vert)
			o->rgbaq = visor_color(nw.v, pw.v, visor_spec[i - visor_first_vert]);
		else
			lit_colors(armor_cube, nw.v, pw.v, &o->rgbaq, &chief_spec_rgbaq[i]);
	}
}
#else
static void skin_chief(const xform *bone_world, const xform *view, int visor_first_vert)
{
	int i, c;

	for (i = 0; i < CHIEF_VERTS; i++)
	{
		const float *p = &chief_pos[i * 3], *n = &chief_nrm[i * 3];
		const xform *b0 = &bone_world[chief_node[i * 2]], *b1 = &bone_world[chief_node[i * 2 + 1]];
		float w = chief_weight[i], w1 = 1.f - w, a[3], b[3], pw[3], pv[3], na[3], nb[3], nw[3];
		svert *o = &chief_v[i];

		if (w1 == 0.f)                 /* one bone: no blend, and a rotation keeps the normal's length */
		{
			apply_point(b0, p, pw);
			apply_vector(b0, n, nw);
		}
		else
		{
			apply_point(b0, p, a);
			apply_point(b1, p, b);
			for (c = 0; c < 3; c++) pw[c] = a[c] * w + b[c] * w1;
			apply_vector(b0, n, na);
			apply_vector(b1, n, nb);
			for (c = 0; c < 3; c++) nw[c] = na[c] * w + nb[c] * w1;
			normalise(nw);
		}
		apply_point(view, pw, pv);
		project(pv, 0, o);
		o->uv = uv_reg(chief_uv[i * 2], chief_uv[i * 2 + 1], 512.f);
		if (i >= visor_first_vert)
			o->rgbaq = visor_color(nw, pw, visor_spec[i - visor_first_vert]);
		else
			lit_colors(armor_cube, nw, pw, &o->rgbaq, &chief_spec_rgbaq[i]);
	}
}
#endif

static void render_frame(GSGLOBAL *gs, float seconds)
{
	float theta = 2.f * (float)M_PI * seconds / ORBIT_SECONDS;
	float cycles = seconds / CYCLE_SECONDS;
	int frame = (int)(fmodf(cycles, 1.f) * ANIM_FRAMES) % ANIM_FRAMES;
	float scroll = -ANIM_STRIDE * cycles;
	float c = cosf(theta), s = sinf(theta);
	xform orbit, view, world_to_view, bone, bone_world[NODE_COUNT], rifle_m, rifle_view, rifle_world;
#ifndef NO_VU0
	mat4 bone_world4[NODE_COUNT], view4, rifle_world4;
#endif
	int i, j;

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
		xform_from_array(&bone, &anim_skin[(frame * NODE_COUNT + i) * 12]);
		xform_then(&bone, &orbit, &bone_world[i]);
	}
	xform_from_array(&rifle_m, &anim_rifle[frame * 12]);
	xform_then(&rifle_m, &orbit, &rifle_world);
	xform_then(&rifle_world, &view, &rifle_view);
#ifndef NO_VU0
	for (i = 0; i < NODE_COUNT; i++) mat4_from_xform(&bone_world4[i], &bone_world[i]);
	mat4_from_xform(&view4, &view);
	mat4_from_xform(&rifle_world4, &rifle_world);
#endif

	/* backdrop */
	gsKit_clear(gs, GS_SETREG_RGBAQ(0x20, 0x26, 0x2c, 0x00, 0x00));
	gsKit_set_test(gs, GS_ZTEST_OFF);
	PROF_MARK(P_SETUP);
	draw_ground(gs, &world_to_view, theta, scroll);
	gsKit_set_test(gs, GS_ZTEST_ON);
	PROF_MARK(P_GROUND);

#ifndef NO_VU0
	skin_chief(bone_world4, &view4, CHIEF_VERTS - 69);
	PROF_MARK(P_SKIN);
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		vec4 pw, nn, pp;
		svert *o = &rifle_v[i];

		vu0_transform(&rifle_world4, &rifle_p4[i], &rifle_n4[i], &pw, &nn, &pp);
		project(pp.v, 0, o);
		o->uv = uv_reg(rifle_uv[i * 2], rifle_uv[i * 2 + 1], 512.f);
		lit_colors(i < RIFLE_GRIP_VERTS ? rifle_grip_cube : rifle_gun_cube, nn.v, pw.v, &o->rgbaq, &rifle_spec_rgbaq[i]);
	}
#else
	skin_chief(bone_world, &view, CHIEF_VERTS - 69);
	PROF_MARK(P_SKIN);
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		float pp[3], pw[3], nn[3];
		svert *o = &rifle_v[i];

		apply_point(&rifle_view, &rifle_pos[i * 3], pp);
		apply_point(&rifle_world, &rifle_pos[i * 3], pw);
		apply_vector(&rifle_world, &rifle_nrm[i * 3], nn);
		normalise(nn);
		project(pp, 0, o);
		o->uv = uv_reg(rifle_uv[i * 2], rifle_uv[i * 2 + 1], 512.f);
		lit_colors(i < RIFLE_GRIP_VERTS ? rifle_grip_cube : rifle_gun_cube, nn, pw, &o->rgbaq, &rifle_spec_rgbaq[i]);
	}
#endif
	/* the ammo display sits on the rifle's top, a hair above it: unlit (it glows), and nudged toward the
	   camera so the 16-bit Z buffer doesn't let the receiver through */
	for (i = 0; i < DISPLAY_VERTS; i++)
	{
		float pp[3];
		svert *o = &display_v[i];

		apply_point(&rifle_view, &display_pos[i * 3], pp);
		project(pp, 120, o);
		o->uv = uv_reg(display_uv[i * 2], display_uv[i * 2 + 1], 256.f);
		o->rgbaq = GS_SETREG_RGBAQ(128, 128, 128, 0x80, 0x00);
	}
	PROF_MARK(P_RIFLE);

	draw_textured(gs, &chief_tex, chief_v, NULL, chief_tri, CHIEF_ARMOR_FIRST_TRI, CHIEF_ARMOR_TRIS);
	draw_flat_gouraud(gs, chief_v, chief_tri, CHIEF_VISOR_FIRST_TRI, CHIEF_VISOR_TRIS);
	/* the armour's reflection and highlight: its triangles again, the mask texture added on */
#ifndef NO_SPECULAR_PASS
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	draw_textured(gs, &mask_tex, chief_v, chief_spec_rgbaq, chief_tri, CHIEF_ARMOR_FIRST_TRI, CHIEF_ARMOR_TRIS);
	gs->PrimAlphaEnable = GS_SETTING_OFF;
#endif
	draw_textured(gs, &rifle_tex, rifle_v, NULL, rifle_tri, 0, RIFLE_TRIS);
	draw_textured(gs, &display_tex, display_v, NULL, display_tri, 0, DISPLAY_TRIS);
#ifndef NO_SPECULAR_PASS
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	draw_textured(gs, &rifle_mask_tex, rifle_v, rifle_spec_rgbaq, rifle_tri, 0, RIFLE_TRIS);
	gs->PrimAlphaEnable = GS_SETTING_OFF;
#endif
	PROF_MARK(P_SUBMIT);
}

int main(void)
{
	GSGLOBAL *gs;
	float refresh;
	int i;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gs = gsKit_init_global_custom(1024 * 1024, 16 * 1024);
	gs->Mode = GS_MODE_NTSC;        /* 60 Hz whatever the console's region: the scene is timed for it */
	gs->PSM = GS_PSM_CT32;
	gs->PSMZ = GS_PSMZ_16S;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->ZBuffering = GS_SETTING_ON;
	gs->PrimAlphaEnable = GS_SETTING_OFF;
	gsKit_init_screen(gs);
	gsKit_mode_switch(gs, GS_ONESHOT);
	gsKit_set_test(gs, GS_ZTEST_ON);
	gs_offset_x = gs->OffsetX;
	gs_offset_y = gs->OffsetY;

	FlushCache(0);
	texture_setup(gs, &chief_tex, 512, chief_tex_pixels, chief_tex_clut);
	texture_setup(gs, &rifle_tex, 512, rifle_tex_pixels, rifle_tex_clut);
	texture_setup(gs, &mask_tex, 512, mask_tex_pixels, mask_tex_clut);
	texture_setup(gs, &rifle_mask_tex, 256, rifle_mask_pixels, rifle_mask_clut);
	texture_setup(gs, &display_tex, 256, display_tex_pixels, display_tex_clut);

	tables_setup();
#ifndef NO_VU0
	pack_vertices();
#endif
	{
		static const float base[3] = { 0.075f, 0.09f, 0.11f }, add[3] = { 0.10f, 0.14f, 0.17f };

		for (i = 0; i < 3; i++) ground_color[i] = powf(base[i] + add[i], GAMMA);
	}

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
	z_scale = 65535.f / (z_far - z_near);

	refresh = gs->Mode == GS_MODE_PAL ? 50.f : 60.f;
	gsKit_add_vsync_handler(vsync_handler);

	for (;;)
	{
#ifdef PROFILE
		static int frames, vsync_at_start;
		static u32 window_start;

		if (frames == 0) { vsync_at_start = vsync_count; window_start = cpu_ticks(); prof_last = window_start; }
#endif
#ifdef DEBUG_SECONDS                 /* a still frame, for comparing builds */
		render_frame(gs, DEBUG_SECONDS);
		(void)refresh;
#else
		render_frame(gs, vsync_count / refresh);
#endif
		gsKit_queue_exec(gs);
		PROF_MARK(P_EXEC);
		gsKit_sync_flip(gs);
		PROF_MARK(P_FLIP);
#ifdef PROFILE
		if (++frames == 60)
		{
			/* the window's length in vsyncs gives the tick rate, so the phases print in milliseconds */
			int vsyncs = vsync_count - vsync_at_start;
			u32 ticks = cpu_ticks() - window_start;
			float ms_per_tick = (vsyncs * 1000.f / refresh) / (float)ticks;
			int p;

			printf("PROFILE %d frames in %d vsyncs: %.1f fps, %.2f ms/frame; %u tris drawn + %u culled per frame\n",
				frames, vsyncs, frames * refresh / vsyncs, (vsyncs * 1000.f / refresh) / frames, prof_tris / frames, prof_tris_culled / frames);
			for (p = 0; p < P_COUNT; p++)
			{
				printf("PROFILE   %-11s %6.2f ms\n", prof_name[p], prof_sum[p] * ms_per_tick / frames);
				prof_sum[p] = 0;
			}
			prof_tris = prof_tris_culled = 0;
			frames = 0;
		}
#endif
	}
	return 0;
}
