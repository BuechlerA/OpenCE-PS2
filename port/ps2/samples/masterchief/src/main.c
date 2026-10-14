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
#ifndef NO_MAP
#include "map_assets.h"
#endif

#if RIFLE_MASK_SIZE != RIFLE_TEX_SIZE
#error "the rifle's second pass shares its UV registers with the first: its mask must be as big as its base map"
#endif

#ifdef PROFILE                      /* -DPROFILE: time the frame's phases with the EE's cycle counter */
#include <timer.h>
enum { P_VU1, P_SETUP, P_MAPV, P_MAPT, P_GROUND, P_SKIN, P_RIFLE, P_SUBMIT, P_EXEC, P_FLIP, P_COUNT };
static const char *prof_name[P_COUNT] = { "vu1 kick", "setup", "map verts", "map tris", "ground/passes", "skin_chief", "rifle+disp", "submit", "queue_exec", "sync_flip" };
static u32 prof_sum[P_COUNT], prof_last, prof_tris, prof_tris_culled, prof_frame_start, prof_busy_max;
static u32 prof_max[P_COUNT];
static void prof_mark(int phase) { u32 t = cpu_ticks(); u32 d = t - prof_last; prof_sum[phase] += d; if (d > prof_max[phase]) prof_max[phase] = d; prof_last = t; }
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

/* a point (w 1) into the view, by the matrix in vf20-vf23 */
static inline void vu0_view_point(const vec4 *p, vec4 *pv)
{
	__asm__ __volatile__(
		"lqc2 $vf1, 0(%0)\n"
		"vmulax.xyz $ACC, $vf20, $vf1x\n" "vmadday.xyz $ACC, $vf21, $vf1y\n" "vmaddaz.xyz $ACC, $vf22, $vf1z\n" "vmaddw.xyz $vf19, $vf23, $vf1w\n"
		"sqc2 $vf19, 0(%1)\n"
		: : "r"(p), "r"(pv) : "memory");
}

/* The rest of a map vertex on VU0, for a view position (in front of the near plane) in pv: one divide, then the screen
   position (plus the GS's origin, so that the fixed-point conversion gives the packed coordinates), the depth, Q,
   and the four texture-coordinate pairs, each the static (u, v, ...) times Q (the details' times their scales).
   Constants in vf8-vf11: (focal, -focal), (cx + 2048, cy + 2048), k1, k2. Writes the two quads of ST registers
   into the vertex's record; out gets the screen position (floats), the fixed-point x and y, the depth and Q. */
static inline void vu0_project(const vec4 *pv, const vec4 *uvlm, const vec4 *uv2, const vec4 *scale, const vec4 *bscale, vec4 *st, vec4 *d, vec4 *b, vec4 *out)
{
	__asm__ __volatile__(
		"lqc2 $vf1, 0(%0)\n"
		"vdiv $Q, $vf0w, $vf1z\n"
		"lqc2 $vf12, 0(%1)\n" "lqc2 $vf13, 0(%2)\n" "lqc2 $vf14, 0(%3)\n" "lqc2 $vf15, 0(%4)\n"
		"vwaitq\n"
		"vmulq.xy $vf2, $vf1, $Q\n" "vmul.xy $vf2, $vf2, $vf8\n" "vadd.xy $vf2, $vf2, $vf9\n"
		"vftoi4.xy $vf3, $vf2\n"
		"vmulq $vf5, $vf10, $Q\n" "vsub $vf5, $vf5, $vf11\n" "vftoi0.x $vf6, $vf5\n"
		"vaddq.x $vf4, $vf0, $Q\n"
		"vmulq $vf12, $vf12, $Q\n" "vmulq $vf13, $vf13, $Q\n" "vmul $vf16, $vf13, $vf15\n" "vmul $vf13, $vf13, $vf14\n"
		"sqc2 $vf12, 0(%5)\n" "sqc2 $vf13, 0(%6)\n" "sqc2 $vf16, 0(%7)\n"
		"sqc2 $vf2, 0(%8)\n" "sqc2 $vf3, 16(%8)\n" "sqc2 $vf6, 32(%8)\n" "sqc2 $vf4, 48(%8)\n"
		: : "r"(pv), "r"(uvlm), "r"(uv2), "r"(scale), "r"(bscale), "r"(st), "r"(d), "r"(b), "r"(out) : "memory");
}

/* a world position (w 1) all the way to the screen: the view matrix (vf20-vf23) and the constants (vf8-vf11);
   out gets the screen position (floats, the GS's origin added), the fixed-point x and y, and the depth */
static inline void vu0_world_to_screen(const vec4 *p, vec4 *out)
{
	__asm__ __volatile__(
		"lqc2 $vf1, 0(%0)\n"
		"vmulax.xyz $ACC, $vf20, $vf1x\n" "vmadday.xyz $ACC, $vf21, $vf1y\n" "vmaddaz.xyz $ACC, $vf22, $vf1z\n" "vmaddw.xyz $vf19, $vf23, $vf1w\n"
		"vdiv $Q, $vf0w, $vf19z\n" "vwaitq\n"
		"vmulq.xy $vf2, $vf19, $Q\n" "vmul.xy $vf2, $vf2, $vf8\n" "vadd.xy $vf2, $vf2, $vf9\n"
		"vftoi4.xy $vf3, $vf2\n"
		"vmulq $vf5, $vf10, $Q\n" "vsub $vf5, $vf5, $vf11\n" "vftoi0.x $vf6, $vf5\n"
		"sqc2 $vf2, 0(%1)\n" "sqc2 $vf3, 16(%1)\n" "sqc2 $vf6, 32(%1)\n"
		: : "r"(p), "r"(out) : "memory");
}

static inline void vu0_load_constants(const vec4 *c)
{
	__asm__ __volatile__("lqc2 $vf8, 0(%0)\n" "lqc2 $vf9, 16(%0)\n" "lqc2 $vf10, 32(%0)\n" "lqc2 $vf11, 48(%0)\n" : : "r"(c));
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
#define DISPLAY_NUDGE 0.0045f       /* the ammo display's depth, toward the camera, in world units */

#ifndef NO_MAP
#define MAP_FOV_HALF_DEGREES 27.5f  /* the map scene's lens */
#define MAP_CAMERA_DISTANCE 2.2f    /* the camera orbits the Chief at this distance (world units, 1 = 10 feet) ... */
#define MAP_CAMERA_HEIGHT 0.35f     /* ... this far above his centre */
#define MAP_NEAR 0.15f
#define MAP_FAR 500.f
#define CHIEF_HEADING 0.f           /* radians about the vertical */
#endif

static float cam_pos[3], cam_right[3], cam_up[3], cam_fwd[3];
/* The frame buffers are SCREEN_WIDTH pixels wide and 448 high; the display stretches them over the same 640x448 picture
   (gsKit sets the horizontal magnification from the width), so at 512 each pixel is 1.25 times as wide as it is high
   and the horizontal focal length is 512 / 640 of the vertical one: the field of view and the aspect do not change.
   A buffer is 573,440 bytes at 640 and 458,752 at 512, and the Z buffer the same: 344 KB less video memory in all. */
#ifndef SCREEN_WIDTH
#ifdef NO_MAP
#define SCREEN_WIDTH 640
#else
#define SCREEN_WIDTH 512
#endif
#endif

static float focal, focal_x;        /* pixels per unit of x/z, up and across */
static float screen_cx, screen_cy;
static float z_near, z_far, z_k1, z_k2; /* the 16-bit Z value is z_k1 / z - z_k2: affine in screen space, as the GS interpolates it */
static float light_dir[3][3];
static const float light_color[3][3] = { { 0.90f, 0.855f, 0.765f }, { 0.2025f, 0.2475f, 0.36f }, { 0.45f, 0.475f, 0.5f } };
static const float ambient[3] = { 0.14f, 0.15f, 0.18f };
static const float spec_tint[3] = { 1.f, 0.97f, 0.9f };   /* the highlight's colour */

/* ---------- textures ---------- */

/* tw, th: log2 of the size, for TEX0. levels: the mip levels of the map's textures (0 or 1: none), the
   levels after the first at video memory tbp (in 256-byte units) with buffer width tbw, for MIPTBP1 and 2 */
typedef struct { GSTEXTURE t; int tw, th, levels, alpha; unsigned mip_tbp[7], mip_tbw[7]; } texture;   /* alpha: palette entry 255 is cut out */

static texture chief_tex, rifle_tex, mask_tex, rifle_mask_tex, display_tex;

static int vram_failed;

/* shared_clut: the video memory address of a palette already uploaded (the grey ones), or 0 to allocate.
   psm: GS_PSM_T8 (256 colours) or GS_PSM_T4 (16: the specular masks, whose palette gsKit sends as the
   8x2 block the GS reads it as) */
static void texture_setup_psm(GSGLOBAL *gs, texture *x, int width, int height, int psm, const unsigned char *pixels, const unsigned int *clut, unsigned shared_clut)
{
	GSTEXTURE *t = &x->t;

	memset(x, 0, sizeof(*x));
	t->Width = width;
	t->Height = height;
	t->PSM = psm;
	t->ClutPSM = GS_PSM_CT32;
	t->TBW = (width + 63) / 64;     /* the buffer width is in units of 64 texels: at least one, for a 32-texel map */
	t->Mem = (u32 *)pixels;
	t->Clut = (u32 *)clut;
	t->Filter = GS_FILTER_LINEAR;
	t->Vram = gsKit_vram_alloc(gs, gsKit_texture_size(t->Width, t->Height, t->PSM), GSKIT_ALLOC_USERBUFFER);
	t->VramClut = shared_clut ? shared_clut
	            : gsKit_vram_alloc(gs, psm == GS_PSM_T4 ? gsKit_texture_size(8, 2, t->ClutPSM) : gsKit_texture_size(16, 16, t->ClutPSM), GSKIT_ALLOC_USERBUFFER);
	/* a failed allocation is 0 (GSKIT_ALLOC_ERROR): the frame buffer's address, so the texture would draw from it */
	if (t->Vram == GSKIT_ALLOC_ERROR || t->VramClut == GSKIT_ALLOC_ERROR)
	{
		printf("texture %dx%d: out of video memory\n", width, height);
		vram_failed = 1;
	}
	else gsKit_texture_upload(gs, t);
	for (x->tw = 0; (1 << x->tw) < width; x->tw++) ;   /* log2, for TEX0 */
	for (x->th = 0; (1 << x->th) < height; x->th++) ;
}

static void texture_setup_clut(GSGLOBAL *gs, texture *x, int width, int height, const unsigned char *pixels, const unsigned int *clut, unsigned shared_clut)
{
	texture_setup_psm(gs, x, width, height, GS_PSM_T8, pixels, clut, shared_clut);
}

#ifndef NO_MAP
/* the same with mip levels, which follow the first in pixels, each half the size of the one before */
static void texture_setup_mips(GSGLOBAL *gs, texture *x, int width, int height, int levels, const unsigned char *pixels, const unsigned int *clut, unsigned shared_clut)
{
	int l;

	texture_setup_clut(gs, x, width, height, pixels, clut, shared_clut);
	x->levels = levels;
	pixels += width * height;
	for (l = 1; l < levels; l++)
	{
		int w = width >> l, h = height >> l, tbw = (w + 63) / 64;
		u32 vram = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);

		if (vram == GSKIT_ALLOC_ERROR) { printf("mip level %dx%d: out of video memory\n", w, h); vram_failed = 1; }
		else gsKit_texture_send((u32 *)pixels, w, h, vram, GS_PSM_T8, tbw, GS_CLUT_TEXTURE);
		x->mip_tbp[l] = vram / 256; x->mip_tbw[l] = tbw;
		pixels += w * h;
	}
}
#endif

static void texture_setup(GSGLOBAL *gs, texture *x, int size, int psm, const unsigned char *pixels, const unsigned int *clut)
{
	texture_setup_psm(gs, x, size, size, psm, pixels, clut, 0);
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
static inline void cube_sample_res(const unsigned char *cube, int res, const float *r, float *rgb)
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
	px = (int)(u * (float)res); if (px > res - 1) px = res - 1; if (px < 0) px = 0;
	py = (int)(v * (float)res); if (py > res - 1) py = res - 1; if (py < 0) py = 0;
	texel = cube + ((face * res + py) * res + px) * 3;
	for (c = 0; c < 3; c++) rgb[c] = texel[c] * (1.f / 255.f);
}

static inline void cube_sample(const unsigned char *cube, const float *r, float *rgb)
{
	cube_sample_res(cube, 64, r, rgb);
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

/* the diffuse colour the GS modulates a lit, textured vertex's base map by, from the summed light */
static inline u64 diffuse_rgbaq(const float *sum)
{
	unsigned char d[3];
	int c;

	for (c = 0; c < 3; c++) d[c] = to_half_byte(gamma_enc_in_range(sum[c]));
	return GS_SETREG_RGBAQ(d[0], d[1], d[2], 0x80, 0x00);
}

/* the colour of a masked part's second pass (the armour's, the rifle's), added over the first: the shader's
   reflection (0.06 + 0.40 m) * cube * (0.5 + fresnel) and highlight (0.15 + 1.1 m) * n.h^18, where the mask m
   comes from the texture (see tools/export_assets.py). What is added is the part that scales with the mask;
   the GS multiplies it by the mask texel. The sum is in linear light but the frame is gamma encoded, so what
   is added is the encoded difference it makes on a typical armour pixel. */
static inline u64 specular_rgbaq(const unsigned char *cube_map, const float *n, const float *pos, const float *sum)
{
	float nv, r[3], ndh, cube[3], fres, spec;
	unsigned char s[3];
	int c;

	shade_terms(n, pos, &nv, r, &ndh);
	cube_sample(cube_map, r, cube);
	fres = 1.f - nv;
	spec = pow18(ndh);
	for (c = 0; c < 3; c++)
	{
		float enc = gamma_enc_in_range(sum[c]);
		float add = 1.14f * (0.40f * cube[c] * (0.5f + fres) + 1.1f * spec * spec_tint[c]);
		float typical = 0.25f * sum[c];                      /* a mid-tone armour pixel, lit: encoded, a quarter's worth of enc */

		s[c] = to_half_byte(gamma_enc_in_range(typical + add) - QUARTER_GAMMA * enc);
	}
	return GS_SETREG_RGBAQ(s[0], s[1], s[2], 0x80, 0x00);
}

static inline void lit_colors(const unsigned char *cube_map, const float *n, const float *pos, u64 *diffuse, u64 *specular)
{
	float sum[3];

	light_sum(n, sum);
	*diffuse = diffuse_rgbaq(sum);
	*specular = specular_rgbaq(cube_map, n, pos, sum);
}

/* ---------- drawing ---------- */

/* a projected vertex: the screen position for culling, and its GS registers ready to copy */
typedef struct { float x, y; u64 rgbaq, uv, xyz2; } svert;

static svert chief_v[MAX_VERTS], rifle_v[MAX_VERTS], display_v[DISPLAY_VERTS];
static u64 chief_spec_rgbaq[MAX_VERTS], rifle_spec_rgbaq[MAX_VERTS];
static u64 chief_mask_uv[MAX_VERTS];   /* the Chief's mask is smaller than his base map, so its pass has its own UV registers (texels of the mask) */
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

/* the vertex's view-space position to the screen and the 16-bit Z buffer; z_add (world units) nudges its depth toward the camera */
static inline void project_xy(const float *p, float z_add, float *x, float *y, u64 *xyz2)
{
	float zc = p[2] < 0.01f ? 0.01f : p[2], inv = 1.f / zc, zd = zc - z_add;
	int iz = (int)(z_k1 / (zd < z_near ? z_near : zd) - z_k2);

	*x = screen_cx + p[0] * inv * focal_x;
	*y = screen_cy - p[1] * inv * focal;
	*xyz2 = GS_SETREG_XYZ2(to_gs_xy(*x, gs_offset_x), to_gs_xy(*y, gs_offset_y), iz < 1 ? 1 : (iz > 65534 ? 65534 : iz));
}

static inline void project(const float *p, float z_add, svert *o)
{
	project_xy(p, z_add, &o->x, &o->y, &o->xyz2);
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
   register writes (a PRIM carried in a REGLIST tag is not honoured by PCSX2). fst: 1 when the vertices
   carry UV (texel coordinates), 0 for ST and Q (perspective-correct, and any range: tiling). clamp: 0
   repeats the texture, 1 clamps it, -1 leaves the setting as it is */
static int prim_lod_k;           /* TEX1's K for the next mipmapped texture: the level of detail's bias, in 1/16 */
static int prim_test = -1;       /* the TEST register for the next primitive: -1 leaves it, 0 the usual, 1 an alpha test (a cut-out) */

static void set_prim(GSGLOBAL *gs, const texture *x, int fst, int clamp)
{
	int mipped = x && x->levels > 1;
	int regs = (x ? (clamp < 0 ? 3 : 4) + (mipped ? 2 : 0) : 1) + (prim_test >= 0);
	u64 *p = gsKit_heap_alloc(gs, regs, regs * 16, GIF_AD);

	*p++ = GIF_TAG_AD(regs);
	*p++ = GIF_AD;
	*p++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, x ? 1 : 0, gs->PrimFogEnable, gs->PrimAlphaEnable, gs->PrimAAEnable, fst, gs->PrimContext, 0);
	*p++ = GS_PRIM;
	if (prim_test >= 0)
	{
		*p++ = prim_test ? GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2) : GS_SETREG_TEST(0, 1, 0, 0, 0, 0, 1, 2);   /* alpha >= 0x40 passes, or everything does; depth GEQUAL */
		*p++ = GS_TEST_1 + gs->PrimContext;
	}
	if (!x) return;
	*p++ = GS_SETREG_TEX0(x->t.Vram / 256, x->t.TBW, x->t.PSM, x->tw, x->th, gs->PrimAlphaEnable || prim_test == 1, 0,
		x->t.VramClut / 256, x->t.ClutPSM, x->t.ClutStorageMode, 0, GS_CLUT_STOREMODE_LOAD);
	*p++ = GS_TEX0_1 + gs->PrimContext;
	/* the level of detail comes from Q (the depth): log2 (1 / Q) + K, a trilinear filter between the levels */
	*p++ = mipped ? GS_SETREG_TEX1(0, x->levels - 1, 1, 5, 0, 0, prim_lod_k & 0xFFF) : GS_SETREG_TEX1(0, 0, x->t.Filter, x->t.Filter, 0, 0, 0);
	*p++ = GS_TEX1_1 + gs->PrimContext;
	if (clamp >= 0)
	{
		*p++ = GS_SETREG_CLAMP(clamp, clamp, 0, 0, 0, 0);
		*p++ = GS_CLAMP_1 + gs->PrimContext;
	}
	if (mipped)
	{
		*p++ = GS_SETREG_MIPTBP1(x->mip_tbp[1], x->mip_tbw[1], x->mip_tbp[2], x->mip_tbw[2], x->mip_tbp[3], x->mip_tbw[3]);
		*p++ = GS_MIPTBP1_1 + gs->PrimContext;
		*p++ = GS_SETREG_MIPTBP2(x->mip_tbp[4], x->mip_tbw[4], x->mip_tbp[5], x->mip_tbw[5], x->mip_tbp[6], x->mip_tbw[6]);
		*p++ = GS_MIPTBP2_1 + gs->PrimContext;
	}
}

/* the mesh's front-facing triangles as one REGLIST packet: RGBAQ, UV and XYZ2 per vertex (the GS
   reads 64 bits per register in this mode). rgbaq, when given, replaces the vertices' own colour
   (the second pass), and uv the same for the texture coordinates (a mask of another size than the base map). */
static void draw_textured(GSGLOBAL *gs, const texture *x, const svert *v, const u64 *rgbaq, const u64 *uv, const unsigned short *tri, int first, int count)
{
	int n = cull(v, tri, first, count), i, qwords;
	u64 *p;

	if (n == 0) return;
	set_prim(gs, x, 1, -1);
	qwords = (n * 3 + 1) / 2;                            /* three registers per vertex, two per quadword */
	p = gsKit_heap_alloc(gs, qwords, qwords * 16, GIF_AD);
	*p++ = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 3);
	*p++ = (u64)GS_RGBAQ | ((u64)GS_UV << 4) | ((u64)GS_XYZ2 << 8);
	for (i = 0; i < n; i++)
	{
		const svert *s = &v[visible[i]];

		*p++ = rgbaq ? rgbaq[visible[i]] : s->rgbaq;
		*p++ = uv ? uv[visible[i]] : s->uv;
		*p++ = s->xyz2;
	}
}

static void draw_flat_gouraud(GSGLOBAL *gs, const svert *v, const unsigned short *tri, int first, int count)
{
	int n = cull(v, tri, first, count), i;
	u64 *p;

	if (n == 0) return;
	set_prim(gs, NULL, 0, -1);
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

#ifdef NO_MAP
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

#endif

#ifndef NO_MAP
/* ---------- the map: static geometry drawn as one Gouraud mesh ---------- */

/* The camera stands inside the map, so triangles cross the near plane and run far off the screen. The
   GS's coordinates reach 2048 pixels from the screen's corner, so a triangle with a vertex behind the
   near plane or beyond CLIP_MARGIN pixels outside the screen is clipped against those five planes in
   view space (Sutherland-Hodgman) before projection; the rest go straight through. */
#define CLIP_MARGIN 1400.f      /* the GS's coordinates are 2048 pixels either side of its origin, which gsKit puts at the screen's corner */
#define CLIP_PLANES 5
#define CLIP_MAX 8                  /* a triangle gains at most one vertex a plane */
#define ATTRS 7                     /* what a vertex carries through the clip: r, g, b, u, v and the lightmap's u, v */

typedef struct { float p[3]; float a[ATTRS]; } cvert;
/* What the passes send for a vertex, eight registers (64 bytes, four quadwords) in this order: ST for the base map, ST
   for the lightmap, ST for each of the two detail maps and the bump map (the base map's times the map's scale), the
   reflection's colour as RGBAQ, the lightmap's hue as RGBAQ (Q = 1 / z, and alpha 0x80, which the blended passes
   read), and XYZ2, which kicks the vertex. A pass is
   this record sent whole with the registers it does not use listed as NOP, so a span's triangles are built
   once, whatever the number of passes. */
typedef struct { u64 st, lmst, d0st, d1st, bst, refl, rgbaq, xyz2; } mapv;
typedef struct { mapv m; float x, y, pad[2]; } mvert __attribute__((aligned(16)));   /* and the screen position, for culling */

static float clip_plane[CLIP_PLANES][4];   /* d = a x + b y + c z + e >= 0 is inside */
static float clip_length[CLIP_PLANES];     /* the length of each plane's normal, for a sphere's distance */
static float clip_x_min, clip_x_max, clip_y_min, clip_y_max;   /* the guard band on the screen */
static unsigned map_rgb32[MAP_VERTS];      /* each vertex's hue as the low half of RGBAQ: r, g, b, alpha 0x80 */
static float map_view[MAP_VERTS][4] __attribute__((aligned(16)));    /* view-space positions, w unused */
static mvert map_v[MAP_VERTS];
static unsigned char map_code[MAP_VERTS];
static unsigned map_stamp[MAP_VERTS], map_frame;       /* a vertex is transformed, projected and given its registers when a visible triangle first needs it, once a frame */
static unsigned char chunk_visible[MAP_CHUNKS];
static xform sky_xform;                 /* the view without its translation, for the sky */
static float detail_scale[MAP_TEXTURES + 1][3];        /* each base map's two detail scales and its bump scale (0: none; the last entry is for no texture) */
static mapv map_out[MAP_TRIS * 3 + 4096] __attribute__((aligned(64)));
static texture map_tex[MAP_TEXTURES], map_detail_tex[MAP_DETAILS], map_lm_tex[MAP_LIGHTMAPS];
static int span_first[MAP_SPANS], span_verts[MAP_SPANS];   /* each span's vertices in map_out */
static int lod_k_base[MAP_TEXTURES], lod_k_detail[MAP_TEXTURES][2], lod_k_bump[MAP_TEXTURES];   /* each pass's K for TEX1 (see map_lod_setup) */
#ifdef MIP_AUDIT
/* -DMIP_AUDIT: which mip levels does the orbit ever sample? The level is log2 (z) + K / 16 (z the depth of the pixel),
   so a texture whose lowest requested level is N never reads the N levels above it, and they can be left out of
   video memory without changing the picture. audit_lod[pass][texture] is the lowest level any vertex that is drawn
   asks for (a pass is a base map, its two detail maps, its bump map); audit_detail_lod[map] the lowest over every
   base map that uses a detail map. tools/mip_audit.py turns the report into tools/mip_floor.json. */
#define AUDIT_BIG 99.f
static float audit_lod[4][MAP_TEXTURES], audit_detail_lod[MAP_DETAILS];
static unsigned audit_drawn[MAP_TEXTURES];
#endif
#ifndef NO_VU0
static vec4 map_p4[MAP_VERTS];                         /* positions as quads, w 1, for the VU */
static vec4 map_uvlm[MAP_VERTS], map_uv2[MAP_VERTS];   /* (u, v, lightmap u, v) and (u, v, u, v) */
static vec4 detail_scale4[MAP_TEXTURES + 1], bump_scale4[MAP_TEXTURES + 1];   /* (k0, k0, k1, k1) and (kb, kb, kb, kb) */
static vec4 vu_const[4], vu_out[4];
static float vu_band_x0, vu_band_x1, vu_band_y0, vu_band_y1;   /* the guard band, in the VU's shifted screen coordinates */
#endif
#ifdef PROFILE
static u32 prof_map_clipped, prof_map_vertices, prof_map_chunks, prof_clip_near, prof_clip_side;
#endif

static void clip_setup(const GSGLOBAL *gs)
{
	float w = (float)gs->Width, h = (float)gs->Height;
	static float q[CLIP_PLANES][4];
	int i;

	q[0][2] = 1.f; q[0][3] = -z_near;                                    /* near */
	q[1][0] = focal_x; q[1][2] = screen_cx + CLIP_MARGIN;                /* left */
	q[2][0] = -focal_x; q[2][2] = w + CLIP_MARGIN - screen_cx;           /* right */
	q[3][1] = -focal; q[3][2] = screen_cy + CLIP_MARGIN;                 /* top */
	q[4][1] = focal; q[4][2] = h + CLIP_MARGIN - screen_cy;              /* bottom */
	memcpy(clip_plane, q, sizeof(q));
	clip_x_min = -CLIP_MARGIN; clip_x_max = w + CLIP_MARGIN; clip_y_min = -CLIP_MARGIN; clip_y_max = h + CLIP_MARGIN;
	for (i = 0; i < CLIP_PLANES; i++) clip_length[i] = sqrtf(q[i][0] * q[i][0] + q[i][1] * q[i][1] + q[i][2] * q[i][2]);
}

static inline float plane_dist(int pl, const float *p)
{
	const float *q = clip_plane[pl];

	return q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3];
}

static inline int outcode(const float *p)
{
	int pl, code = 0;

	for (pl = 0; pl < CLIP_PLANES; pl++)
		if (plane_dist(pl, p) < 0.f) code |= 1 << pl;
	return code;
}

/* planes: the bits of the vertices' codes, OR-ed (bit 0 the near plane, 1 left, 2 right, 3 top, 4 bottom: the planes' order),
   so that only the planes the triangle crosses are clipped against */
static int clip_triangle(const cvert *tri, cvert *out, int planes)
{
	cvert a[CLIP_MAX], b[CLIP_MAX], *src = a, *dst = b, *swap;
	float d[CLIP_MAX];
	int n = 3, pl, i, k, m, inside;

	memcpy(a, tri, 3 * sizeof(cvert));
	for (pl = 0; pl < CLIP_PLANES; pl++)
	{
		if (!(planes >> pl & 1)) continue;
		for (i = 0, inside = 0; i < n; i++)
		{
			d[i] = plane_dist(pl, src[i].p);
			inside += d[i] >= 0.f;
		}
		if (inside == n) continue;                      /* nothing to cut off here */
		if (inside == 0) return 0;
		m = 0;
		for (i = 0; i < n; i++)
		{
			const cvert *c = &src[i], *q = &src[i ? i - 1 : n - 1];
			float dc = d[i], dq = d[i ? i - 1 : n - 1];

			if ((dc >= 0.f) != (dq >= 0.f))
			{
				float t = dq / (dq - dc);
				cvert *o = &dst[m++];

				for (k = 0; k < 3; k++) o->p[k] = q->p[k] + (c->p[k] - q->p[k]) * t;
				for (k = 0; k < ATTRS; k++) o->a[k] = q->a[k] + (c->a[k] - q->a[k]) * t;
			}
			if (dc >= 0.f) dst[m++] = *c;
		}
		swap = src; src = dst; dst = swap;
		n = m;
	}
	memcpy(out, src, n * sizeof(cvert));
	return n;
}

/* the vertex's hue (0..128 a channel: the GS multiplies by colour / 128; alpha 0x80), Q = 1 / z, and
   S = u Q, T = v Q for the base map, the lightmap and each detail map: the GS divides by Q per pixel. u and v
   are affine in view space, so they interpolate exactly along a clipped edge. */
static inline u64 st_reg(float u, float v, float q)
{
	float st[2] = { u * q, v * q };
	u32 sbits, tbits;

	memcpy(&sbits, &st[0], 4); memcpy(&tbits, &st[1], 4);
	return (u64)sbits | ((u64)tbits << 32);
}

static inline u64 scale_st(u64 st, float k)
{
	float s, t;
	u32 sbits = (u32)st, tbits = (u32)(st >> 32);

	memcpy(&s, &sbits, 4); memcpy(&t, &tbits, 4);
	s *= k; t *= k;
	memcpy(&sbits, &s, 4); memcpy(&tbits, &t, 4);
	return (u64)sbits | ((u64)tbits << 32);
}

/* tex: the vertex's base map, or MAP_TEXTURES for none, for its detail scales */
static inline void set_attrs(mvert *o, float r, float g, float b, float u, float v, float lu, float lv, float zc, int tex)
{
	float q = 1.f / zc;
	u32 qbits;

	memcpy(&qbits, &q, 4);
	o->m.rgbaq = GS_SETREG_RGBAQ((int)(r + 0.5f), (int)(g + 0.5f), (int)(b + 0.5f), 0x80, qbits);
	o->m.st = st_reg(u, v, q);
	o->m.lmst = st_reg(lu, lv, q);
	o->m.d0st = scale_st(o->m.st, detail_scale[tex][0]);
	o->m.d1st = scale_st(o->m.st, detail_scale[tex][1]);
}

/* Fog. The Xbox fogs the environment per vertex by the view depth, linearly between the sky's start and opaque distances up to
   its maximum density, toward the sky's fog colour (rasterizer_xbox.c, the fog constants), in every pass. The GS's fog does the
   same with a coefficient F per vertex, which it interpolates and blends as F * colour + (1 - F) * FOGCOL (F 255: none). F rides
   in the top byte of a vertex's XYZF2 (bits 56 to 63, free: the depth is 16 bits), so every pass reads its vertices as XYZF2
   and ignores F unless the pass turns the fog on (PRIM's FGE).
   Fog belongs after what multiplies the picture (the detail maps, the lightmap's brightness). By default the base pass is fogged
   (it costs the EE only F, 0.5 ms) and the passes that follow multiply the haze with the surface: the lightmap's brightness runs
   up to twice the base pass's, so the haze comes out brighter in the sun and darker in a shadow, and FOG_HAZE_GAIN scales
   FOGCOL down to the average. -DFOG_EXACT_PASS does it as the Xbox does instead: one pass over the map's triangles, last, with
   per pixel dest * F + (1 - F) * FOGCOL (the GS's fog gives (1 - F) * FOGCOL for a black vertex, and the alpha F scales what
   is there already); it costs about 1.6 ms of the EE's frame. The reflection, added after the base pass, is scaled by F at its
   vertices either way. -DNO_FOG leaves the fog out. */
#ifndef FOG_HAZE_GAIN
#define FOG_HAZE_GAIN 0.5f
#endif
#define MAP_SKY_TEXTURE (MAP_TEXTURES - 1)       /* the exporter adds the sky's gradient last: the sky is not fogged */
#define FOG_SLOPE (MAP_FOG_DENSITY / (MAP_FOG_OPAQUE - MAP_FOG_START))

/* F / 255 at view depth z */
static inline float fog_factor(float z)
{
#ifdef NO_FOG
	(void)z;
	return 1.f;
#else
	float t = (z - MAP_FOG_START) * FOG_SLOPE;

	if (t < 0.f) t = 0.f; else if (t > MAP_FOG_DENSITY) t = MAP_FOG_DENSITY;
	return 1.f - t;
#endif
}

static inline u64 fog_bits(float z)
{
	return (u64)(int)(255.f * fog_factor(z) + 0.5f) << 56;
}

static inline float screen_area(const mvert *a, const mvert *b, const mvert *c)
{
	return (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
}

enum { PASS_BASE, PASS_LIGHTMAP, PASS_DETAIL0, PASS_DETAIL1, PASS_BUMP, PASS_REFLECTION, PASS_COUNT };

static void set_clamp_repeat(GSGLOBAL *gs)
{
	u64 *p = gsKit_heap_alloc(gs, 1, 16, GIF_AD);

	*p++ = GIF_TAG_AD(1);
	*p++ = GIF_AD;
	*p++ = GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0);
	*p++ = GS_CLAMP_1 + gs->PrimContext;
}

/* the state the Chief's draws expect: no alpha test (and set_prim leaves the TEST register alone again) */
static void set_test_normal(GSGLOBAL *gs)
{
	u64 *p = gsKit_heap_alloc(gs, 1, 16, GIF_AD);

	*p++ = GIF_TAG_AD(1);
	*p++ = GIF_AD;
	*p++ = GS_SETREG_TEST(0, 1, 0, 0, 0, 0, 1, 2);
	*p++ = GS_TEST_1 + gs->PrimContext;
	prim_test = -1;
}

/* a vertex record is four quadwords */
static inline void copy_record(mapv *dst, const mapv *src)
{
	u128 *d = (u128 *)dst;
	const u128 *sr = (const u128 *)src;

	d[0] = sr[0]; d[1] = sr[1]; d[2] = sr[2]; d[3] = sr[3];
}

/* a pass over a span's vertices: each record's one ST (or, for the reflection, its colour as RGBAQ), its RGBAQ and its
   XYZ2, three registers (24 bytes) a vertex as one REGLIST packet. The record has them all because the passes share it. */
static void send_map_pass(GSGLOBAL *gs, const texture *x, int pass, int lod_k, const mapv *v, int n)
{
	/* the record's 64-bit words: st, lmst, d0st, d1st, bst, refl, rgbaq, xyz2 */
	static const int field[PASS_COUNT] = { 0, 1, 2, 3, 4, 5 };
	const u64 *rec = (const u64 *)v + field[pass];
	int qwords = pass == PASS_REFLECTION ? n : (n * 3 + 1) / 2, i;
	u64 *p;

	prim_lod_k = lod_k;
	prim_test = x && pass == PASS_BASE && x->alpha ? 1 : 0;
	set_prim(gs, x, 0, pass == PASS_LIGHTMAP ? 1 : 0);
	p = gsKit_heap_alloc(gs, qwords, qwords * 16, GIF_AD);
	if (pass == PASS_REFLECTION)
	{                                                                 /* a flat colour: RGBAQ, XYZ2 */
		*p++ = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 2);
		*p++ = (u64)GS_RGBAQ | ((u64)GS_XYZF2 << 4);
		for (i = 0; i < n; i++, rec += 8) { *p++ = rec[0]; *p++ = rec[2]; }
		return;
	}
	*p++ = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 3);
	*p++ = (u64)GS_ST | ((u64)GS_RGBAQ << 4) | ((u64)GS_XYZF2 << 8);
	for (i = 0; i < n; i++, rec += 8) { *p++ = rec[0]; *p++ = rec[6 - field[pass]]; *p++ = rec[7 - field[pass]]; }
}

#ifdef FOG_EXACT_PASS
/* The fog, last of all: every vertex black with the alpha F / 255 (128 is 1), through the GS's fog unit (FGE, FOGCOL), which makes
   the fragment (1 - F) * FOGCOL; the blend (Cd * As) + Cs is then dest * F + (1 - F) * FOGCOL. A packet is two registers a
   vertex, RGBAQ and XYZF2, like the flat Gouraud one; the sky is left out. NLOOP is 15 bits, so a long span goes in pieces. */
#define FOG_PIECE 30000                          /* vertices a packet, a multiple of 3 */

static void fog_pass(GSGLOBAL *gs)
{
	u64 *p;
	int sp, i, n, done;

	gs->PrimFogEnable = GS_SETTING_ON;
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(1, 2, 0, 0, 0), 0);        /* (Cd - 0) * As + Cs */
	prim_test = 0;
	set_prim(gs, NULL, 0, -1);
	for (sp = 0; sp < MAP_SPANS; sp++)
	{
		const u64 *rec = (const u64 *)&map_out[span_first[sp]];

		if (!span_verts[sp] || map_spans[sp * 6] == MAP_SKY_TEXTURE) continue;
		for (done = 0; done < span_verts[sp]; done += n, rec += 8 * n)
		{
			n = span_verts[sp] - done < FOG_PIECE ? span_verts[sp] - done : FOG_PIECE;
			p = gsKit_heap_alloc(gs, n, n * 16, GIF_AD);
			*p++ = GIF_TAG(n, 1, 0, 0, GSKIT_GIF_FLG_REGLIST, 2);
			*p++ = (u64)GS_RGBAQ | ((u64)GS_XYZF2 << 4);
			for (i = 0; i < n; i++)
			{
				u64 xyzf = rec[i * 8 + 7];

				*p++ = (u64)(((unsigned)(xyzf >> 56) + 1u) >> 1) << 24;   /* black, alpha F / 255 (128 is 1; (F + 1) / 2, no divide) */
				*p++ = xyzf;
			}
		}
	}
	gs->PrimFogEnable = GS_SETTING_OFF;
	gs->PrimAlphaEnable = GS_SETTING_OFF;
}
#endif

#ifndef NO_FOG
/* FOGCOL's value: the sky's fog colour, scaled for the haze the later passes multiply (not in the exact pass) */
static inline u64 fog_color_word(void)
{
#ifdef FOG_EXACT_PASS
	const float g = 1.f;
#else
	const float g = FOG_HAZE_GAIN;
#endif
	return (u64)(int)(MAP_FOG_R * g + 0.5f) | ((u64)(int)(MAP_FOG_G * g + 0.5f) << 8) | ((u64)(int)(MAP_FOG_B * g + 0.5f) << 16);
}

/* FOGCOL, once a frame: what the fogged passes blend toward */
static void fog_color_setup(GSGLOBAL *gs)
{
	u64 *p = gsKit_heap_alloc(gs, 1, 16, GIF_AD);

	*p++ = GIF_TAG_AD(1);
	*p++ = GIF_AD;
	*p++ = fog_color_word();
	*p++ = GS_FOGCOL;
}
#endif

/* the passes multiply by alpha: destination * source alpha / 128, so a grey of 128 leaves it as it is */
static void map_blend_begin(GSGLOBAL *gs)
{
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(1, 2, 0, 2, 0), 0);
}

static void map_blend_end(GSGLOBAL *gs)
{
	gs->PrimAlphaEnable = GS_SETTING_OFF;
}

/* a vertex the visible triangles need: into view space and, when it is in front of the near plane and in the
   guard band (the same planes the clipper uses, as bounds on the screen position), onto the screen with its
   registers. One divide: q = 1 / z gives the position, the depth and every texture coordinate. The registers'
   floats are stored straight into the record. */

/* The colour a shader's reflection adds at a vertex: its cube map (16x16 faces) in the direction the view mirrors
   about the normal, weighted between the strengths head-on and at a graze by 1 - n.v squared. It follows the camera, so
   a vertex has it worked out again every REFLECT_REFRESH frames (a degree and a half a frame, as the Chief's
   specular colour). */
#define REFLECT_REFRESH 4
static u64 refl_cache[MAP_VERTS];
static unsigned refl_stamp[MAP_VERTS];

static u64 map_reflect(int i, int tex, float z)
{
	const signed char *nn = &map_normals[i * 4];
	float n[3] = { nn[0] * (1.f / 127.f), nn[1] * (1.f / 127.f), nn[2] * (1.f / 127.f) };
	float v[3] = { cam_pos[0] - map_pos[i * 3], cam_pos[1] - map_pos[i * 3 + 1], cam_pos[2] - map_pos[i * 3 + 2] };
	float nv, r[3], rgb[3], f, w;
	int c;

	if (refl_stamp[i] && map_frame - refl_stamp[i] < REFLECT_REFRESH) return refl_cache[i];
	normalise(v);
	nv = dot(n, v);
	if (nv < 0.f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; nv = -nv; }
	for (c = 0; c < 3; c++) r[c] = 2.f * nv * n[c] - v[c];
	cube_sample_res(map_cubes[map_reflection_of[tex].cube], 16, r, rgb);
	f = 1.f - nv;
	w = map_reflection_of[tex].perpendicular + (map_reflection_of[tex].parallel - map_reflection_of[tex].perpendicular) * f * f;
	refl_stamp[i] = map_frame;
	w *= fog_factor(z);                                   /* the fog's share of what is added: F * (lit + reflection) */
	refl_cache[i] = GS_SETREG_RGBAQ(to_byte(rgb[0] * w), to_byte(rgb[1] * w), to_byte(rgb[2] * w), 0x80, 0x00);
	return refl_cache[i];
}

/* the screen position, depth and registers of a vertex at view position pv in front of the near plane; a vertex
   outside the guard band is left alone and its code returned, unless force (a vertex the clipper made lies on the
   band's edge, give or take rounding: its position is clamped into it) */
static inline int fill_vertex(mvert *o, const float *pv, unsigned rgb32, float u, float v, float lu, float lv, const float *scale, int force, int fog)
{
	float q = 1.f / pv[2], k = q * focal, sx = screen_cx + pv[0] * q * focal_x, sy = screen_cy - pv[1] * k, s, t, *f = (float *)&o->m.st;
	int code = (sx < clip_x_min ? 2 : 0) | (sx > clip_x_max ? 4 : 0) | (sy < clip_y_min ? 8 : 0) | (sy > clip_y_max ? 16 : 0);
	int ix, iy, iz;
	u32 qbits;

	if (code && !force) return code;
	if (force)
	{
		sx = sx < clip_x_min ? clip_x_min : (sx > clip_x_max ? clip_x_max : sx);
		sy = sy < clip_y_min ? clip_y_min : (sy > clip_y_max ? clip_y_max : sy);
	}
	o->x = sx; o->y = sy;
	ix = (int)(sx * 16.f) + gs_offset_x;
	iy = (int)(sy * 16.f) + gs_offset_y;
	iz = (int)(z_k1 * q - z_k2);
	iz = iz < 1 ? 1 : (iz > 65534 ? 65534 : iz);
	o->m.xyz2 = GS_SETREG_XYZ2(ix & 0xFFFF, iy & 0xFFFF, iz) | (fog ? fog_bits(pv[2]) : (u64)255 << 56);
	memcpy(&qbits, &q, 4);
	o->m.rgbaq = (u64)rgb32 | ((u64)qbits << 32);
	s = u * q; t = v * q;
	f[0] = s; f[1] = t;                                    /* st */
	f[2] = lu * q; f[3] = lv * q;                          /* lmst */
	f[4] = s * scale[0]; f[5] = t * scale[0];              /* d0st */
	f[6] = s * scale[1]; f[7] = t * scale[1];              /* d1st */
	f[8] = s * scale[2]; f[9] = t * scale[2];              /* bst */
	o->m.refl = 0;
	return 0;
}

/* project a clipped vertex (it is inside the planes, give or take rounding) */
static inline void project_cvert(const cvert *v, mvert *o, int tex)
{
	float pv[3] = { v->p[0], v->p[1], v->p[2] < z_near ? z_near : v->p[2] };
	unsigned rgb32 = (unsigned)(v->a[0] + 0.5f) | ((unsigned)(v->a[1] + 0.5f) << 8) | ((unsigned)(v->a[2] + 0.5f) << 16) | (0x80u << 24);

	fill_vertex(o, pv, rgb32, v->a[3], v->a[4], v->a[5], v->a[6], detail_scale[tex], 1, tex != MAP_SKY_TEXTURE);
}

static inline void map_vertex(int i, int tex, const xform *view)
{
	const float *pv = map_view[i];
	mvert *o = &map_v[i];

#ifndef NO_VU0
	if (i < MAP_SKY_FIRST_VERT)
	{
		vu0_view_point(&map_p4[i], (vec4 *)map_view[i]);
		map_stamp[i] = map_frame;
		if (pv[2] < z_near) { map_code[i] = 1; return; }
		vu0_project((const vec4 *)map_view[i], &map_uvlm[i], &map_uv2[i], &detail_scale4[tex], &bump_scale4[tex], (vec4 *)&o->m.st, (vec4 *)&o->m.d0st, (vec4 *)&o->m.bst, vu_out);
		{
			float sx = vu_out[0].v[0], sy = vu_out[0].v[1];
			int code = (sx < vu_band_x0 ? 2 : 0) | (sx > vu_band_x1 ? 4 : 0) | (sy < vu_band_y0 ? 8 : 0) | (sy > vu_band_y1 ? 16 : 0);
			int iz = ((int *)vu_out)[8];
			u32 qbits = ((u32 *)vu_out)[12];

			map_code[i] = (unsigned char)code;
			if (code) return;
			o->x = sx; o->y = sy;
			iz = iz < 1 ? 1 : (iz > 65535 ? 65535 : iz);
			o->m.xyz2 = ((u64)(((int *)vu_out)[4] & 0xFFFF)) | ((u64)(((int *)vu_out)[5] & 0xFFFF) << 16) | ((u64)iz << 32) | fog_bits(pv[2]);
			o->m.rgbaq = (u64)map_rgb32[i] | ((u64)qbits << 32);
			if (tex < MAP_TEXTURES && map_reflection_of[tex].cube >= 0) o->m.refl = map_reflect(i, tex, pv[2]);
		}
#ifdef PROFILE
		prof_map_vertices++;
#endif
		return;
	}
	apply_point(&sky_xform, &map_pos[i * 3], map_view[i]);                  /* the sky surrounds the camera: no translation */
	(void)view;
#else
	apply_point(i >= MAP_SKY_FIRST_VERT ? &sky_xform : view, &map_pos[i * 3], map_view[i]);
#endif
	map_stamp[i] = map_frame;
	if (pv[2] < z_near) { map_code[i] = 1; return; }
	map_code[i] = (unsigned char)fill_vertex(o, pv, map_rgb32[i], map_uv[i * 2], map_uv[i * 2 + 1], map_lmuv[i * 2], map_lmuv[i * 2 + 1], detail_scale[tex], 0, tex != MAP_SKY_TEXTURE);
#ifdef PROFILE
	prof_map_vertices++;
#endif
}

static void draw_map(GSGLOBAL *gs, const xform *view, const void *view4)
{
	int i, sp, b, c, n = 0, cap = (int)(sizeof(map_out) / sizeof(map_out[0]));

#ifndef NO_VU0
	vu0_load_view((const mat4 *)view4);
	vu0_load_constants(vu_const);
#else
	(void)view4;
#endif
	sky_xform = *view;
	sky_xform.t[0] = sky_xform.t[1] = sky_xform.t[2] = 0.f;
	if (++map_frame == 0) { memset(map_stamp, 0, sizeof(map_stamp)); map_frame = 1; }
	/* the chunks in view: a bounding sphere against the five planes */
	for (c = 0; c < MAP_CHUNKS; c++)
	{
		const float *bound = &map_chunks[c * 8];
		float v[3], r = bound[3], reach = bound[4] + r;
		int pl, vis = 1;

		apply_point(view, bound, v);
		for (pl = 0; pl < CLIP_PLANES; pl++)
			if (plane_dist(pl, v) < -r * clip_length[pl]) { vis = 0; break; }
		if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > reach * reach) vis = 0;      /* beyond what it is worth drawing */
		chunk_visible[c] = (unsigned char)vis;
#ifdef PROFILE
		prof_map_chunks += vis;
#endif
	}
	PROF_MARK(P_MAPV);
	/* cull and clip every span once, from the chunks in view; the passes below draw what is left */
	for (sp = 0; sp < MAP_SPANS; sp++)
	{
		int tex = map_spans[sp * 6], ts = tex < 0 ? MAP_TEXTURES : tex;
		const int two_sided = tex >= 0 && map_tex[tex].alpha;          /* foliage is seen from both sides */

		span_first[sp] = n;
		for (b = map_spans[sp * 6 + 4]; b < map_spans[sp * 6 + 4] + map_spans[sp * 6 + 5]; b++)
		{
			int first = map_batches[b * 3 + 1], count = map_batches[b * 3 + 2];

			if (!chunk_visible[map_batches[b * 3]]) continue;
			for (i = first; i < first + count; i++)
			{
				unsigned short ia, ib, ic;
				int ca, cb, cc;

				if (!two_sided)        /* facing away from the camera: not even its vertices are needed */
				{
					const float *pl = &map_planes[i * 4];

					if (pl[0] * cam_pos[0] + pl[1] * cam_pos[1] + pl[2] * cam_pos[2] + pl[3] <= 0.f) continue;
				}
				ia = map_tri[i * 3]; ib = map_tri[i * 3 + 1]; ic = map_tri[i * 3 + 2];

				if (map_stamp[ia] != map_frame) map_vertex(ia, ts, view);
				if (map_stamp[ib] != map_frame) map_vertex(ib, ts, view);
				if (map_stamp[ic] != map_frame) map_vertex(ic, ts, view);
				ca = map_code[ia]; cb = map_code[ib]; cc = map_code[ic];
				if (ca & cb & cc) continue;                          /* all outside one plane */
				if ((ca | cb | cc) == 0)
				{
					const mvert *va = &map_v[ia], *vb = &map_v[ib], *vc = &map_v[ic];

					float area = screen_area(va, vb, vc);

					if (two_sided ? fabsf(area) < 0.01f : area * CULL_SIGN <= 0.f) continue;
					if (n + 3 > cap) break;
					copy_record(&map_out[n++], &va->m); copy_record(&map_out[n++], &vb->m); copy_record(&map_out[n++], &vc->m);
				}
				else
				{
					cvert in[3], poly[CLIP_MAX];
					mvert sv[CLIP_MAX];
					int m, k, v, ids[3];
#ifdef PROFILE
					if ((ca | cb | cc) & 1) prof_clip_near++; else prof_clip_side++;
#endif

					ids[0] = ia; ids[1] = ib; ids[2] = ic;
					for (v = 0; v < 3; v++)
					{
						memcpy(in[v].p, map_view[ids[v]], sizeof(in[v].p));
						for (k = 0; k < 3; k++) in[v].a[k] = (float)map_color[ids[v] * 4 + k];
						in[v].a[3] = map_uv[ids[v] * 2]; in[v].a[4] = map_uv[ids[v] * 2 + 1];
						in[v].a[5] = map_lmuv[ids[v] * 2]; in[v].a[6] = map_lmuv[ids[v] * 2 + 1];
					}
					m = clip_triangle(in, poly, (ca | cb | cc) & 1 ? 0x1F : (ca | cb | cc));       /* a vertex behind the near plane has no side bits: the cut may land outside any */
					for (v = 0; v < m; v++) project_cvert(&poly[v], &sv[v], ts);
					for (k = 1; k + 1 < m; k++)
					{
						float area = screen_area(&sv[0], &sv[k], &sv[k + 1]);

						if (two_sided ? fabsf(area) < 0.01f : area * CULL_SIGN <= 0.f) continue;
						if (n + 3 > cap) break;
						copy_record(&map_out[n++], &sv[0].m); copy_record(&map_out[n++], &sv[k].m); copy_record(&map_out[n++], &sv[k + 1].m);
					}
#ifdef PROFILE
					prof_map_clipped++;
#endif
				}
			}
		}
		span_verts[sp] = n - span_first[sp];
#ifdef PROFILE
		prof_tris += span_verts[sp] / 3;
#endif
	}
	PROF_MARK(P_MAPT);
#ifndef NO_FOG
	fog_color_setup(gs);
#endif
#ifdef MIP_AUDIT
	for (sp = 0; sp < MAP_SPANS; sp++)
	{
		int tex = map_spans[sp * 6], d;
		float zmin = 1e9f, lz;

		if (tex < 0 || !span_verts[sp]) continue;
		for (i = 0; i < span_verts[sp]; i++)
		{
			const u64 *rec = (const u64 *)&map_out[span_first[sp] + i];
			u32 qbits = (u32)(rec[6] >> 32);
			float q;

			memcpy(&q, &qbits, 4);
			if (q > 0.f && 1.f / q < zmin) zmin = 1.f / q;
		}
		if (zmin > 1e8f) continue;
		lz = log2f(zmin);
		audit_drawn[tex]++;
		if (lz + lod_k_base[tex] / 16.f < audit_lod[0][tex]) audit_lod[0][tex] = lz + lod_k_base[tex] / 16.f;
		for (d = 0; d < 2; d++)
			if (map_detail_of[tex].detail[d] >= 0)
			{
				float l = lz + lod_k_detail[tex][d] / 16.f;

				if (l < audit_lod[1 + d][tex]) audit_lod[1 + d][tex] = l;
				if (l < audit_detail_lod[map_detail_of[tex].detail[d]]) audit_detail_lod[map_detail_of[tex].detail[d]] = l;
			}
		if (map_detail_of[tex].detail[2] >= 0)
		{
			float l = lz + lod_k_bump[tex] / 16.f;

			if (l < audit_lod[3][tex]) audit_lod[3][tex] = l;
			if (l < audit_detail_lod[map_detail_of[tex].detail[2]]) audit_detail_lod[map_detail_of[tex].detail[2]] = l;
		}
	}
#endif
	/* each span's base map lit with the lightmap's hue, then its detail maps multiplied in */
	for (sp = 0; sp < MAP_SPANS; sp++)
	{
		int tex = map_spans[sp * 6];
		const mapv *v = &map_out[span_first[sp]];

		if (!span_verts[sp]) continue;
#if !defined(NO_FOG) && !defined(FOG_EXACT_PASS)
		gs->PrimFogEnable = GS_SETTING_ON;                   /* the base pass is fogged by the GS: F rides in each vertex's XYZF2 */
#endif
		send_map_pass(gs, tex < 0 ? NULL : &map_tex[tex], PASS_BASE, tex < 0 ? 0 : lod_k_base[tex], v, span_verts[sp]);
		gs->PrimFogEnable = GS_SETTING_OFF;
#ifndef NO_DETAIL_PASS
		if (tex >= 0)
		{
			int d;

			map_blend_begin(gs);
			for (d = 0; d < 2; d++)
				if (map_detail_of[tex].detail[d] >= 0)
					send_map_pass(gs, &map_detail_tex[map_detail_of[tex].detail[d]], PASS_DETAIL0 + d, lod_k_detail[tex][d], v, span_verts[sp]);
#ifndef NO_BUMP_PASS
			if (map_detail_of[tex].detail[2] >= 0)
				send_map_pass(gs, &map_detail_tex[map_detail_of[tex].detail[2]], PASS_BUMP, lod_k_bump[tex], v, span_verts[sp]);
#endif
			map_blend_end(gs);
		}
#endif
	}
	/* the lightmap's brightness, a page at a time */
#ifndef NO_LIGHTMAP_PASS
	map_blend_begin(gs);
	for (i = 0; i < MAP_LM_SPANS; i++)
	{
		sp = map_lm_order[i];
		if (!span_verts[sp]) continue;
		send_map_pass(gs, &map_lm_tex[map_spans[sp * 6 + 1]], PASS_LIGHTMAP, 0, &map_out[span_first[sp]], span_verts[sp]);
	}
	map_blend_end(gs);
#endif
	/* what the shiny surfaces reflect, added on top: a flat colour a vertex */
#ifndef NO_REFLECTION_PASS
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	for (sp = 0; sp < MAP_SPANS; sp++)
	{
		int tex = map_spans[sp * 6];

		if (tex >= 0 && map_reflection_of[tex].cube >= 0 && span_verts[sp])
			send_map_pass(gs, NULL, PASS_REFLECTION, 0, &map_out[span_first[sp]], span_verts[sp]);
	}
	map_blend_end(gs);
#endif
#ifdef FOG_EXACT_PASS
	fog_pass(gs);
#endif
	set_clamp_repeat(gs);                                        /* the Chief's draws expect the repeat */
	set_test_normal(gs);
}

/* The GS picks a mip level from the depth alone: level = log2 (1 / Q) + K. A pass's texels a pixel, at depth z,
   are density * size * scale * z / focal (density: texture repeats a world unit of surface, from the exporter;
   size: the texture's sides' geometric mean), so K = log2 (density * size * scale / focal), plus a bias toward the
   blurrier level. In 1/16, as TEX1 holds it. */
#define LOD_BIAS 0.3f

static int lod_k(float density, float size, float scale)
{
	int k = (int)floorf((log2f(density * size * scale / focal) + LOD_BIAS) * 16.f + 0.5f);

	return k < -2048 ? -2048 : (k > 2047 ? 2047 : k);
}

static void map_lod_setup(void)
{
	int t, d, i;

#ifndef NO_VU0
	for (i = 0; i < MAP_VERTS; i++)
	{
		map_p4[i].v[0] = map_pos[i * 3]; map_p4[i].v[1] = map_pos[i * 3 + 1]; map_p4[i].v[2] = map_pos[i * 3 + 2]; map_p4[i].v[3] = 1.f;
		map_uvlm[i].v[0] = map_uv[i * 2]; map_uvlm[i].v[1] = map_uv[i * 2 + 1]; map_uvlm[i].v[2] = map_lmuv[i * 2]; map_uvlm[i].v[3] = map_lmuv[i * 2 + 1];
		map_uv2[i].v[0] = map_uv[i * 2]; map_uv2[i].v[1] = map_uv[i * 2 + 1]; map_uv2[i].v[2] = map_uv[i * 2]; map_uv2[i].v[3] = map_uv[i * 2 + 1];
	}
	{
		float ox = gs_offset_x / 16.f, oy = gs_offset_y / 16.f;

		vu_const[0].v[0] = focal_x; vu_const[0].v[1] = -focal; vu_const[0].v[2] = vu_const[0].v[3] = 0.f;
		vu_const[1].v[0] = screen_cx + ox; vu_const[1].v[1] = screen_cy + oy; vu_const[1].v[2] = vu_const[1].v[3] = 0.f;
		for (i = 0; i < 4; i++) { vu_const[2].v[i] = z_k1; vu_const[3].v[i] = z_k2; }
		vu_band_x0 = clip_x_min + ox; vu_band_x1 = clip_x_max + ox; vu_band_y0 = clip_y_min + oy; vu_band_y1 = clip_y_max + oy;
	}
#endif
	for (i = 0; i < MAP_VERTS; i++)
		map_rgb32[i] = map_color[i * 4] | (map_color[i * 4 + 1] << 8) | (map_color[i * 4 + 2] << 16) | (0x80u << 24);
	for (t = 0; t < MAP_TEXTURES; t++)
		for (d = 0; d < 2; d++)
			detail_scale[t][d] = map_detail_of[t].detail[d] < 0 ? 0.f : map_detail_of[t].scale[d];
	for (t = 0; t < MAP_TEXTURES; t++) detail_scale[t][2] = map_detail_of[t].detail[2] < 0 ? 0.f : map_detail_of[t].scale[2];
#ifndef NO_VU0
	for (t = 0; t <= MAP_TEXTURES; t++)
	{
		detail_scale4[t].v[0] = detail_scale4[t].v[1] = detail_scale[t][0];
		detail_scale4[t].v[2] = detail_scale4[t].v[3] = detail_scale[t][1];
		bump_scale4[t].v[0] = bump_scale4[t].v[1] = bump_scale4[t].v[2] = bump_scale4[t].v[3] = detail_scale[t][2];
	}
#endif

	for (t = 0; t < MAP_TEXTURES; t++)
	{
		lod_k_base[t] = lod_k(map_tex_density[t], sqrtf((float)map_textures[t].width * map_textures[t].height), 1.f);
		for (d = 0; d < 2; d++)
		{
			int id = map_detail_of[t].detail[d];

			lod_k_detail[t][d] = id < 0 ? 0 : lod_k(map_tex_density[t], (float)map_details[id].width, map_detail_of[t].scale[d]);
		}
		lod_k_bump[t] = map_detail_of[t].detail[2] < 0 ? 0 : lod_k(map_tex_density[t], (float)map_details[map_detail_of[t].detail[2]].width, map_detail_of[t].scale[2]);
	}
}

/* the camera orbits the Chief; he stands where the exporter found the ground */
static xform chief_placement;

static void placement_setup(void)
{
	float c = cosf(CHIEF_HEADING), s = sinf(CHIEF_HEADING), foot[3] = { MODEL_CENTER_X, MODEL_CENTER_Y, MODEL_FLOOR_Z }, rf[3];

	memset(&chief_placement, 0, sizeof(chief_placement));
	chief_placement.r[0][0] = c; chief_placement.r[0][1] = s; chief_placement.r[1][0] = -s; chief_placement.r[1][1] = c; chief_placement.r[2][2] = 1.f;
	apply_vector(&chief_placement, foot, rf);
	chief_placement.t[0] = MAP_CHIEF_X - rf[0];
	chief_placement.t[1] = MAP_CHIEF_Y - rf[1];
	chief_placement.t[2] = MAP_CHIEF_Z - rf[2];
}
#endif

/* ---------- frame ---------- */

static volatile int vsync_count;

static int vsync_handler(int cause)
{
	(void)cause;
	vsync_count++;
	return 0;
}

#ifdef NO_MAP
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
		o->uv = uv_reg(chief_uv[i * 2], chief_uv[i * 2 + 1], (float)CHIEF_TEX_SIZE);
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
		o->uv = uv_reg(chief_uv[i * 2], chief_uv[i * 2 + 1], (float)CHIEF_TEX_SIZE);
		if (i >= visor_first_vert)
			o->rgbaq = visor_color(nw, pw, visor_spec[i - visor_first_vert]);
		else
			lit_colors(armor_cube, nw, pw, &o->rgbaq, &chief_spec_rgbaq[i]);
	}
}
#endif
#endif

#ifndef NO_MAP
/* ---------- the Chief's poses, cached ----------
   In the map scene he stands still and only the camera moves, so what the 20 frames of his run cycle do to his
   vertices (the skinned position and normal in the world, and the diffuse light, which the fixed lights give
   and the camera does not change) is worked out once at the start. A frame then projects the cached
   positions, takes the cached diffuse colour, and gives each vertex its view-dependent specular colour once in
   SPEC_SPREAD frames (the camera moves a degree and a half a frame; the visor's colour, 69 vertices, every frame). */
#define SPEC_SPREAD 4

typedef struct { float v[4]; } __attribute__((aligned(16))) qvec;
static qvec chief_wp[ANIM_FRAMES][CHIEF_VERTS], chief_wn[ANIM_FRAMES][CHIEF_VERTS], chief_ls[ANIM_FRAMES][CHIEF_VERTS];
static u64 chief_dif[ANIM_FRAMES][CHIEF_VERTS];
static qvec rifle_wp[ANIM_FRAMES][RIFLE_VERTS], rifle_wn[ANIM_FRAMES][RIFLE_VERTS], rifle_ls[ANIM_FRAMES][RIFLE_VERTS];
static u64 rifle_dif[ANIM_FRAMES][RIFLE_VERTS];
static qvec display_wp[ANIM_FRAMES][DISPLAY_VERTS];
static int spec_phase, spec_primed;

static inline void qset(qvec *q, const float *v, float w)
{
	q->v[0] = v[0]; q->v[1] = v[1]; q->v[2] = v[2]; q->v[3] = w;
}

static void pose_cache_build(void)
{
	int f, i, c;

	for (f = 0; f < ANIM_FRAMES; f++)
	{
		xform bone, bone_world[NODE_COUNT], rifle_m, rifle_world;
		float sum[3];

		for (i = 0; i < NODE_COUNT; i++)
		{
			xform_from_array(&bone, &anim_skin[(f * NODE_COUNT + i) * 12]);
			xform_then(&bone, &chief_placement, &bone_world[i]);
		}
		xform_from_array(&rifle_m, &anim_rifle[f * 12]);
		xform_then(&rifle_m, &chief_placement, &rifle_world);
		for (i = 0; i < CHIEF_VERTS; i++)
		{
			const float *p = &chief_pos[i * 3], *n = &chief_nrm[i * 3];
			const xform *b0 = &bone_world[chief_node[i * 2]], *b1 = &bone_world[chief_node[i * 2 + 1]];
			float w = chief_weight[i], w1 = 1.f - w, a[3], b[3], pw[3], na[3], nb[3], nw[3];

			apply_point(b0, p, a);
			apply_point(b1, p, b);
			for (c = 0; c < 3; c++) pw[c] = a[c] * w + b[c] * w1;
			apply_vector(b0, n, na);
			apply_vector(b1, n, nb);
			for (c = 0; c < 3; c++) nw[c] = na[c] * w + nb[c] * w1;
			normalise(nw);
			qset(&chief_wp[f][i], pw, 1.f); qset(&chief_wn[f][i], nw, 0.f);
			light_sum(nw, sum);
			qset(&chief_ls[f][i], sum, 0.f);
			chief_dif[f][i] = diffuse_rgbaq(sum);
		}
		for (i = 0; i < RIFLE_VERTS; i++)
		{
			float pw[3], nw[3];

			apply_point(&rifle_world, &rifle_pos[i * 3], pw);
			apply_vector(&rifle_world, &rifle_nrm[i * 3], nw);
			normalise(nw);
			qset(&rifle_wp[f][i], pw, 1.f); qset(&rifle_wn[f][i], nw, 0.f);
			light_sum(nw, sum);
			qset(&rifle_ls[f][i], sum, 0.f);
			rifle_dif[f][i] = diffuse_rgbaq(sum);
		}
		for (i = 0; i < DISPLAY_VERTS; i++)
		{
			float pw[3];

			apply_point(&rifle_world, &display_pos[i * 3], pw);
			qset(&display_wp[f][i], pw, 1.f);
		}
	}
	/* what a frame never changes: each vertex's texture coordinates */
	for (i = 0; i < CHIEF_VERTS; i++) chief_v[i].uv = uv_reg(chief_uv[i * 2], chief_uv[i * 2 + 1], (float)CHIEF_TEX_SIZE);
	for (i = 0; i < RIFLE_VERTS; i++) rifle_v[i].uv = uv_reg(rifle_uv[i * 2], rifle_uv[i * 2 + 1], (float)RIFLE_TEX_SIZE);
	for (i = 0; i < DISPLAY_VERTS; i++)
	{
		display_v[i].uv = uv_reg(display_uv[i * 2], display_uv[i * 2 + 1], (float)DISPLAY_TEX_SIZE);
		display_v[i].rgbaq = GS_SETREG_RGBAQ(128, 128, 128, 0x80, 0x00);
	}
}

/* a cached world position into the view and onto the screen */
static inline void view_project(const qvec *wp, float z_add, const xform *view, svert *o)
{
	qvec pv;

#ifndef NO_VU0
	if (z_add == 0.f)                      /* all on the VU (the display's nudge toward the camera takes the slow way) */
	{
		int iz;

		vu0_world_to_screen((const vec4 *)wp, vu_out);
		o->x = vu_out[0].v[0]; o->y = vu_out[0].v[1];
		iz = ((int *)vu_out)[8];
		iz = iz < 1 ? 1 : (iz > 65534 ? 65534 : iz);
		o->xyz2 = ((u64)(((int *)vu_out)[4] & 0xFFFF)) | ((u64)(((int *)vu_out)[5] & 0xFFFF) << 16) | ((u64)iz << 32);
		return;
	}
	vu0_view_point((const vec4 *)wp, (vec4 *)&pv);
	(void)view;
#else
	apply_point(view, wp->v, pv.v);
#endif
	project(pv.v, z_add, o);
}

/* the Chief, his rifle and its display for animation frame `frame` (the view's VU matrix loaded by the caller) */
static void cached_pose(int frame, const xform *view)
{
	const int visor_first = CHIEF_VERTS - 69, full = !spec_primed;
	int i, k = spec_phase;

#ifdef CHIEF_VU1                     /* VU1 draws the armour and the rifle (and keeps their specular colours): the visor and the display are left */
	(void)k; (void)full;
	for (i = visor_first; i < CHIEF_VERTS; i++)
	{
		svert *o = &chief_v[i];

		view_project(&chief_wp[frame][i], 0.f, view, o);
		o->rgbaq = visor_color(chief_wn[frame][i].v, chief_wp[frame][i].v, visor_spec[i - visor_first]);
	}
	PROF_MARK(P_SKIN);
	for (i = 0; i < DISPLAY_VERTS; i++) view_project(&display_wp[frame][i], DISPLAY_NUDGE, view, &display_v[i]);
	return;
#endif
	for (i = 0; i < CHIEF_VERTS; i++)
	{
		svert *o = &chief_v[i];

		view_project(&chief_wp[frame][i], 0.f, view, o);
		if (i >= visor_first)
			o->rgbaq = visor_color(chief_wn[frame][i].v, chief_wp[frame][i].v, visor_spec[i - visor_first]);
		else
		{
			o->rgbaq = chief_dif[frame][i];
			if (k == 0 || full) chief_spec_rgbaq[i] = specular_rgbaq(armor_cube, chief_wn[frame][i].v, chief_wp[frame][i].v, chief_ls[frame][i].v);
		}
		if (++k == SPEC_SPREAD) k = 0;
	}
	PROF_MARK(P_SKIN);
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		svert *o = &rifle_v[i];

		view_project(&rifle_wp[frame][i], 0.f, view, o);
		o->rgbaq = rifle_dif[frame][i];
		if (k == 0 || full)
			rifle_spec_rgbaq[i] = specular_rgbaq(i < RIFLE_GRIP_VERTS ? rifle_grip_cube : rifle_gun_cube, rifle_wn[frame][i].v, rifle_wp[frame][i].v, rifle_ls[frame][i].v);
		if (++k == SPEC_SPREAD) k = 0;
	}
	/* the ammo display sits on the rifle's top, a hair above it: unlit (it glows), and nudged toward the
	   camera so the 16-bit Z buffer doesn't let the receiver through */
	for (i = 0; i < DISPLAY_VERTS; i++) view_project(&display_wp[frame][i], DISPLAY_NUDGE, view, &display_v[i]);
	spec_primed = 1;
	if (++spec_phase == SPEC_SPREAD) spec_phase = 0;
}

#ifdef CHIEF_VU1
/* ---------- the Chief's textured passes on VU1 ----------
   -DCHIEF_VU1 (make CHIEF_VU1=1) draws the armour and the rifle, each its base map and then its specular mask added over it, with a
   VU1 microprogram (chief_vu1.vsm) instead of the EE: what the EE did a frame for them (projecting 3,000 vertices, culling, building
   four REGLIST packets, 3.5 ms) is now a few matrices and the specular colours of a quarter of the vertices. His animation is cached
   (every vertex's world position and diffuse colour for each of the 20 frames), so everything the VU1 reads is static except the
   view matrix and those specular colours, and a frame's whole VIF1 DMA chain (its batches of 12 triangles, each an UNPACK of
   positions, UVs and both passes' colours and an MSCAL) is built once, at the start. The triangles are listed out, three vertices
   each (VU1 has no use for a shared vertex), and the program culls the back faces itself.
   The VU1's draws go down GIF path 1, past gsKit's queue (path 3), so the frame is ordered: the queue's clear goes first and the EE
   waits for its FINISH; then the VU1 stream, which ends with a FINISH of its own, is started; the EE builds the map meanwhile; the
   map's queue (gsKit_queue_exec waits for that FINISH, too) goes last, and the Z buffer sorts the two out: nothing the map draws is
   in front of the Chief, and what it draws behind him fails the depth test. The visor and the ammo display, a few dozen vertices,
   stay on the EE, drawn with the map. */
#include <dma.h>

extern u32 ChiefVU1_CodeStart __attribute__((section(".vudata")));
extern u32 ChiefVU1_CodeEnd __attribute__((section(".vudata")));

#define VU_V 36                          /* vertices (12 triangles) a batch */
#define VU_T (VU_V / 3)
/* VU1's data memory, in quadwords, as chief_vu1.vsm has it */
#define VU_M 0
#define VU_SCALE 4
#define VU_TOGGLE 5
#define VU_PTRS 6
#define VU_IN_BASE 16
#define VU_IN_OFFSET 152
#define VU_IN_POS 3
#define VU_IN_UV (VU_IN_POS + VU_V)
#define VU_IN_DIF (VU_IN_UV + VU_V)
#define VU_IN_SPEC (VU_IN_DIF + VU_V)
#define VU_O1_SIZE (6 + 3 * VU_V)
#define VU_O2_SIZE (5 + 3 * VU_V)
#define VU_O1A 320
#define VU_O1B (VU_O1A + VU_O1_SIZE)
#define VU_O2A (VU_O1B + VU_O1_SIZE)
#define VU_O2B (VU_O2A + VU_O2_SIZE)
#if VU_IN_SPEC + VU_V + 3 > VU_IN_OFFSET
#error "a VU1 input buffer outgrows its half of the memory"
#endif

/* VIF codes and DMA tags */
#define VIF_NOP 0u
#define VIF_STCYCL(cl, wl) (0x01000000u | ((wl) << 8) | (cl))
#define VIF_BASE(a) (0x03000000u | (a))
#define VIF_OFFSET(a) (0x02000000u | (a))
#define VIF_FLUSH 0x11000000u
#define VIF_MSCAL(a) (0x14000000u | (a))
#define VIF_DIRECT(qwc) (0x50000000u | (qwc))
#define VIF_MPG(num, a) (0x4A000000u | ((num) << 16) | (a))
#define VIF_UNPACK_V4_32 0x6C
#define VIF_UNPACK_V4_8 0x6E
#define VIF_UNPACK(fmt, num, tops, usn, a) (((u32)(fmt) << 24) | ((u32)(num) << 16) | ((u32)(tops) << 15) | ((u32)(usn) << 14) | (u32)(a))
enum { DMA_ID_CNT = 1, DMA_ID_NEXT = 2, DMA_ID_REF = 3, DMA_ID_END = 7 };

#define VU_PAD(n) (((n) + VU_V - 1) / VU_V * VU_V)
#define CHIEF_SLOTS (CHIEF_ARMOR_TRIS * 3)
#define RIFLE_SLOTS (RIFLE_TRIS * 3)

typedef struct
{
	int tris, slots, pad, batches, verts;
	const unsigned short *tri;       /* the triangle list: three vertex indices each */
	qvec *pos;                       /* [ANIM_FRAMES][pad] world positions of the listed vertices (w 1) */
	u32 *dif;                        /* [ANIM_FRAMES][pad] their diffuse colour (RGBA bytes) */
	qvec *uv;                        /* [pad] base-map UV in texels (u, v, 0, 0) */
	u32 *spec;                       /* [pad] pass 2's colour: the specular colour of the vertex, kept up to date by the EE */
	int *slot_first, *slot_list;     /* the slots of each vertex (a vertex is listed once for each triangle that uses it) */
	qvec hdr_full[3], hdr_last[3];   /* the GIF tags of pass 1's and pass 2's draws and the batch's triangle count */
	qvec scale;
	u64 s1[10], s2[8];               /* the GS state each pass starts with (a GIF A+D packet) */
	const texture *base, *mask;
	u64 *chain[ANIM_FRAMES];
} vu_object;

static qvec chief_vu_pos[ANIM_FRAMES][VU_PAD(CHIEF_SLOTS)], rifle_vu_pos[ANIM_FRAMES][VU_PAD(RIFLE_SLOTS)];
static u32 chief_vu_dif[ANIM_FRAMES][VU_PAD(CHIEF_SLOTS)] __attribute__((aligned(16))), rifle_vu_dif[ANIM_FRAMES][VU_PAD(RIFLE_SLOTS)] __attribute__((aligned(16)));
static qvec chief_vu_uv[VU_PAD(CHIEF_SLOTS)], rifle_vu_uv[VU_PAD(RIFLE_SLOTS)];
static u32 chief_vu_spec[VU_PAD(CHIEF_SLOTS)] __attribute__((aligned(16))), rifle_vu_spec[VU_PAD(RIFLE_SLOTS)] __attribute__((aligned(16)));
static int chief_slot_first[CHIEF_VERTS + 1], chief_slot_list[CHIEF_SLOTS], rifle_slot_first[RIFLE_VERTS + 1], rifle_slot_list[RIFLE_SLOTS];
static vu_object vu_chief, vu_rifle;
static int vu_cursor[CHIEF_VERTS];
static u64 vu_chain_pool[ANIM_FRAMES * (2 * (VU_PAD(CHIEF_SLOTS) + VU_PAD(RIFLE_SLOTS)) / VU_V * 6 + 64) * 2 + 256] __attribute__((aligned(64)));
static u64 *vu_cp;                                   /* the chain being built */
static u64 vu_dyn[2][14] __attribute__((aligned(64)));   /* a frame's first packet: a tag, the matrix (4 quadwords), a tag that goes on to the frame's chain */
static int vu_dyn_toggle;
static u32 vu_ptrs[12] __attribute__((aligned(16)));

/* a DMA tag carries two VIF codes, sent before its data: an UNPACK or MPG goes second, so that its data follows it directly */
static inline void vu_tag(int id, int qwc, const void *data, u32 vif0, u32 vif1)
{
	vu_cp[0] = (u64)qwc | ((u64)id << 28) | ((u64)((u32)data & 0x0FFFFFFF) << 32);
	vu_cp[1] = (u64)vif0 | ((u64)vif1 << 32);
	vu_cp += 2;
}

static u64 tex0_of(const texture *x, int tcc)       /* what set_prim writes */
{
	return GS_SETREG_TEX0(x->t.Vram / 256, x->t.TBW, x->t.PSM, x->tw, x->th, tcc, 0, x->t.VramClut / 256, x->t.ClutPSM, x->t.ClutStorageMode, 0, GS_CLUT_STOREMODE_LOAD);
}

static u64 tex1_of(const texture *x)
{
	return GS_SETREG_TEX1(0, 0, x->t.Filter, x->t.Filter, 0, 0, 0);
}

/* the object's streams from the cached poses: listed out by triangle, the UVs (in the texels of the base map, clamped as uv_reg does), which
   slots each vertex has */
static void vu_object_build(vu_object *o, const unsigned short *tri, int tris, int pad, int verts, const qvec *wp, const u64 *dif, const float *uv, int uv_size,
                            qvec *pos, u32 *dif_out, qvec *uv_out, u32 *spec, int *slot_first, int *slot_list, const texture *base, const texture *mask)
{
	int f, s, v;

	memset(o, 0, sizeof(*o));
	o->tris = tris; o->slots = tris * 3; o->pad = pad; o->batches = (o->slots + VU_V - 1) / VU_V; o->verts = verts;
	o->tri = tri; o->pos = pos; o->dif = dif_out; o->uv = uv_out; o->spec = spec; o->slot_first = slot_first; o->slot_list = slot_list;
	o->base = base; o->mask = mask;
	for (v = 0; v <= verts; v++) slot_first[v] = 0;
	for (s = 0; s < o->slots; s++) slot_first[tri[s] + 1]++;
	for (v = 0; v < verts; v++) slot_first[v + 1] += slot_first[v];
	for (v = 0; v < verts; v++) vu_cursor[v] = slot_first[v];
	for (s = 0; s < o->slots; s++) slot_list[vu_cursor[tri[s]]++] = s;
	for (s = 0; s < pad; s++)
	{
		float u = 0.f, w = 0.f;

		if (s < o->slots) { u = uv[tri[s] * 2] * (float)uv_size; w = uv[tri[s] * 2 + 1] * (float)uv_size; }
		if (u < 0.f) u = 0.f; else if (u > (float)uv_size) u = (float)uv_size;
		if (w < 0.f) w = 0.f; else if (w > (float)uv_size) w = (float)uv_size;
		uv_out[s].v[0] = u; uv_out[s].v[1] = w; uv_out[s].v[2] = uv_out[s].v[3] = 0.f;
	}
	for (f = 0; f < ANIM_FRAMES; f++)
		for (s = 0; s < pad; s++)
		{
			int sv = s < o->slots ? tri[s] : 0;

			pos[f * pad + s] = wp[f * verts + sv];
			dif_out[f * pad + s] = s < o->slots ? (u32)dif[f * verts + sv] : 0;     /* the padding after the last vertex must be 0: the VIF reads the rest of a quadword of 8-bit data as a command */
		}
	for (s = 0; s < pad; s++) spec[s] = 0;
}

/* the GIF tags and the GS state of an object's passes, from its two textures */
static void vu_object_state(vu_object *o, int mask_vs_base_uv_num, int mask_vs_base_uv_den)
{
	const u64 prim1 = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1, 0, 0, 0, 1, 0, 0);
	const u64 prim2 = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1, 0, 1, 0, 1, 0, 0);
	const u64 regs = (u64)GS_RGBAQ | ((u64)GS_UV << 4) | ((u64)GS_XYZ2 << 8);        /* RGBAQ, UV, XYZ2 as PACKED qwords */
	u64 *h;
	int k;

	for (k = 0; k < 2; k++)
	{
		int tris = k == 0 ? VU_T : (o->slots - (o->batches - 1) * VU_V) / 3;

		h = (u64 *)(k == 0 ? o->hdr_full : o->hdr_last);
		h[0] = GIF_TAG(0, 1, 1, prim1, GSKIT_GIF_FLG_PACKED, 3); h[1] = regs;       /* NLOOP is the program's to patch */
		h[2] = GIF_TAG(0, 1, 1, prim2, GSKIT_GIF_FLG_PACKED, 3); h[3] = regs;
		h[4] = (u64)tris; h[5] = 0;
	}
	o->scale.v[0] = (float)mask_vs_base_uv_num / (float)mask_vs_base_uv_den; o->scale.v[1] = o->scale.v[0]; o->scale.v[2] = o->scale.v[3] = 0.f;
	o->s1[0] = GIF_TAG(4, 0, 0, 0, GSKIT_GIF_FLG_PACKED, 1); o->s1[1] = GIF_AD;
	o->s1[2] = tex0_of(o->base, 0); o->s1[3] = GS_TEX0_1;
	o->s1[4] = tex1_of(o->base);    o->s1[5] = GS_TEX1_1;
	o->s1[6] = GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0); o->s1[7] = GS_CLAMP_1;
	o->s1[8] = GS_SETREG_TEST(0, 1, 0, 0, 0, 0, 1, 2); o->s1[9] = GS_TEST_1;
	o->s2[0] = GIF_TAG(3, 0, 0, 0, GSKIT_GIF_FLG_PACKED, 1); o->s2[1] = GIF_AD;
	o->s2[2] = tex0_of(o->mask, 1); o->s2[3] = GS_TEX0_1;
	o->s2[4] = tex1_of(o->mask);    o->s2[5] = GS_TEX1_1;
	o->s2[6] = GS_SETREG_ALPHA(0, 2, 2, 1, 0x80); o->s2[7] = GS_ALPHA_1;
}

/* one object's part of a frame's chain: the output state (once the previous object's draws are done with the output buffers), and
   a batch a time: the three-quadword header, positions, UVs and the two colour lists into the half of the input memory VU1 is not
   reading, then the microprogram */
static void vu_chain_object(const vu_object *o, int f)
{
	int b;

	vu_tag(DMA_ID_CNT, 0, 0, VIF_FLUSH, VIF_NOP);
	vu_tag(DMA_ID_REF, 1, &o->scale, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 1, 0, 0, VU_SCALE));
	vu_tag(DMA_ID_REF, 5, o->s1, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 5, 0, 0, VU_O1A));
	vu_tag(DMA_ID_REF, 5, o->s1, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 5, 0, 0, VU_O1B));
	vu_tag(DMA_ID_REF, 4, o->s2, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 4, 0, 0, VU_O2A));
	vu_tag(DMA_ID_REF, 4, o->s2, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 4, 0, 0, VU_O2B));
	for (b = 0; b < o->batches; b++)
	{
		int so = b * VU_V, v = o->slots - so < VU_V ? o->slots - so : VU_V, last = b == o->batches - 1;

		vu_tag(DMA_ID_REF, 3, last ? o->hdr_last : o->hdr_full, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 3, 1, 0, 0));
		vu_tag(DMA_ID_REF, v, &o->pos[f * o->pad + so], VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, v, 1, 0, VU_IN_POS));
		vu_tag(DMA_ID_REF, v, &o->uv[so], VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, v, 1, 0, VU_IN_UV));
		vu_tag(DMA_ID_REF, (v + 3) / 4, &o->dif[f * o->pad + so], VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_8, v, 1, 1, VU_IN_DIF));
		vu_tag(DMA_ID_REF, (v + 3) / 4, &o->spec[so], VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_8, v, 1, 1, VU_IN_SPEC));
		vu_tag(DMA_ID_CNT, 0, 0, VIF_MSCAL(0), VIF_NOP);
	}
}

static void vu_wait_vif1(void)
{
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
}

static void chief_vu1_init(void)
{
	static u64 init_chain[16 * 2 + 8] __attribute__((aligned(64)));
	const u32 *code = &ChiefVU1_CodeStart;
	int f, n = (int)(&ChiefVU1_CodeEnd - &ChiefVU1_CodeStart) / 2, off;     /* instructions: two words each */

	/* the streams (the poses are cached): the chief's armour triangles, the rifle's */
	vu_object_build(&vu_chief, &chief_tri[CHIEF_ARMOR_FIRST_TRI * 3], CHIEF_ARMOR_TRIS, VU_PAD(CHIEF_SLOTS), CHIEF_VERTS,
		&chief_wp[0][0], &chief_dif[0][0], chief_uv, CHIEF_TEX_SIZE, &chief_vu_pos[0][0], &chief_vu_dif[0][0], chief_vu_uv, chief_vu_spec,
		chief_slot_first, chief_slot_list, &chief_tex, &mask_tex);
	vu_object_build(&vu_rifle, rifle_tri, RIFLE_TRIS, VU_PAD(RIFLE_SLOTS), RIFLE_VERTS,
		&rifle_wp[0][0], &rifle_dif[0][0], rifle_uv, RIFLE_TEX_SIZE, &rifle_vu_pos[0][0], &rifle_vu_dif[0][0], rifle_vu_uv, rifle_vu_spec,
		rifle_slot_first, rifle_slot_list, &rifle_tex, &rifle_mask_tex);
	vu_object_state(&vu_chief, MASK_TEX_SIZE, CHIEF_TEX_SIZE);
	vu_object_state(&vu_rifle, RIFLE_MASK_SIZE, RIFLE_TEX_SIZE);
	/* the frames' chains */
	vu_cp = vu_chain_pool;
	for (f = 0; f < ANIM_FRAMES; f++)
	{
		u64 *start = vu_cp;

		vu_chief.chain[f] = start;
		vu_chain_object(&vu_chief, f);
		vu_chain_object(&vu_rifle, f);
		vu_tag(DMA_ID_CNT, 2, 0, VIF_FLUSH, VIF_DIRECT(2));          /* the GS's FINISH, by path 2 once path 1 is quiet */
		vu_cp[0] = GIF_TAG(1, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1); vu_cp[1] = GIF_AD; vu_cp[2] = 0; vu_cp[3] = GS_FINISH; vu_cp += 4;
		vu_tag(DMA_ID_END, 0, 0, VIF_NOP, VIF_NOP);
		if ((vu_cp - vu_chain_pool) * 8 > (int)sizeof(vu_chain_pool)) { printf("VU1: the chain pool is too small\n"); for (;;) ; }
	}
	/* the program, the double buffer and the fixed words of VU1's memory */
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_VIF1);
	vu_ptrs[0] = vu_ptrs[1] = vu_ptrs[2] = vu_ptrs[3] = 0;
	vu_ptrs[4] = VU_O1A; vu_ptrs[5] = VU_O2A; vu_ptrs[6] = vu_ptrs[7] = 0;
	vu_ptrs[8] = VU_O1B; vu_ptrs[9] = VU_O2B; vu_ptrs[10] = vu_ptrs[11] = 0;
	vu_cp = init_chain;
	vu_tag(DMA_ID_CNT, 0, 0, VIF_STCYCL(1, 1), VIF_BASE(VU_IN_BASE));
	vu_tag(DMA_ID_CNT, 0, 0, VIF_OFFSET(VU_IN_OFFSET), VIF_NOP);
	for (off = 0; off < n; off += 256)
	{
		int chunk = n - off < 256 ? n - off : 256;

		vu_tag(DMA_ID_REF, chunk / 2, code + off * 2, VIF_NOP, VIF_MPG(chunk & 255, off));
	}
	vu_tag(DMA_ID_REF, 3, vu_ptrs, VIF_NOP, VIF_UNPACK(VIF_UNPACK_V4_32, 3, 0, 0, VU_TOGGLE));
	vu_tag(DMA_ID_END, 0, 0, VIF_NOP, VIF_NOP);
	FlushCache(0);
	dma_channel_send_chain(DMA_CHANNEL_VIF1, init_chain, 0, DMA_FLAG_TRANSFERTAG, 0);
	vu_wait_vif1();
	printf("VU1: microprogram %d instructions, %d + %d batches of %d triangles a frame\n", n, vu_chief.batches, vu_rifle.batches, VU_T);
}

/* world to screen: view and projection as one matrix (p * M, rows for x, y, z and 1): X/W and Y/W are pixels with the GS's origin, Z/W the
   16-bit depth, W the depth in the view, as vu0_world_to_screen has them */
static void vu_matrix(const xform *view, qvec *m)
{
	float cxo = screen_cx + gs_offset_x / 16.f, cyo = screen_cy + gs_offset_y / 16.f;
	int i;

	for (i = 0; i < 3; i++)
	{
		m[i].v[0] = focal_x * view->r[i][0] + cxo * view->r[i][2];
		m[i].v[1] = -focal * view->r[i][1] + cyo * view->r[i][2];
		m[i].v[2] = -z_k2 * view->r[i][2];
		m[i].v[3] = view->r[i][2];
	}
	m[3].v[0] = focal_x * view->t[0] + cxo * view->t[2];
	m[3].v[1] = -focal * view->t[1] + cyo * view->t[2];
	m[3].v[2] = z_k1 - z_k2 * view->t[2];
	m[3].v[3] = view->t[2];
}

/* the specular colours for this frame, on the schedule cached_pose has (a quarter of the vertices a frame), written to every slot of a vertex */
static void vu_spec_update(int frame)
{
	const int visor_first = CHIEF_VERTS - 69, full = !spec_primed;
	int i, j, k = spec_phase;

	for (i = 0; i < CHIEF_VERTS; i++)
	{
		if (i < visor_first && (k == 0 || full))
		{
			u64 sp = specular_rgbaq(armor_cube, chief_wn[frame][i].v, chief_wp[frame][i].v, chief_ls[frame][i].v);
#ifdef NO_SPECULAR_PASS
			sp = 0;
#endif
			chief_spec_rgbaq[i] = sp;
			for (j = chief_slot_first[i]; j < chief_slot_first[i + 1]; j++) chief_vu_spec[chief_slot_list[j]] = (u32)sp;
		}
		if (++k == SPEC_SPREAD) k = 0;
	}
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		if (k == 0 || full)
		{
			u64 sp = specular_rgbaq(i < RIFLE_GRIP_VERTS ? rifle_grip_cube : rifle_gun_cube, rifle_wn[frame][i].v, rifle_wp[frame][i].v, rifle_ls[frame][i].v);
#ifdef NO_SPECULAR_PASS
			sp = 0;
#endif
			rifle_spec_rgbaq[i] = sp;
			for (j = rifle_slot_first[i]; j < rifle_slot_first[i + 1]; j++) rifle_vu_spec[rifle_slot_list[j]] = (u32)sp;
		}
		if (++k == SPEC_SPREAD) k = 0;
	}
	spec_primed = 1;
	if (++spec_phase == SPEC_SPREAD) spec_phase = 0;
	SyncDCache(chief_vu_spec, chief_vu_spec + VU_PAD(CHIEF_SLOTS));
	SyncDCache(rifle_vu_spec, rifle_vu_spec + VU_PAD(RIFLE_SLOTS));
}

/* the start of a frame: the clear goes out, the VU1 stream is started once it is done (see above) */
static void chief_vu1_kick(GSGLOBAL *gs, int frame, const xform *view)
{
	qvec m[4];
	u64 *d = vu_dyn[vu_dyn_toggle ^= 1];

	vu_matrix(view, m);
	d[0] = 4 | ((u64)DMA_ID_CNT << 28);                                                           /* tag: the matrix follows ... */
	d[1] = (u64)VIF_STCYCL(1, 1) | ((u64)VIF_UNPACK(VIF_UNPACK_V4_32, 4, 0, 0, VU_M) << 32);       /* ... as an UNPACK */
	memcpy(&d[2], m, sizeof(m));
	d[10] = (u64)DMA_ID_NEXT << 28 | ((u64)((u32)vu_chief.chain[frame] & 0x0FFFFFFF) << 32);      /* then on to the frame's chain */
	d[11] = 0;
	SyncDCache(d, d + 12);
	gsKit_queue_exec(gs);                       /* the clear */
	vu_spec_update(frame);
	gsKit_finish();                             /* the clear is done: the VU1 draws may follow */
	GS_SETREG_CSR_FINISH(1);
	vu_wait_vif1();
	dma_channel_send_chain(DMA_CHANNEL_VIF1, d, 0, DMA_FLAG_TRANSFERTAG, 0);
}
#endif
#endif

/* the camera's axes for an eye and a target (z up) */
static void camera_look(const float *eye, const float *target)
{
	static const float up[3] = { 0.f, 0.f, 1.f };
	int i;

	for (i = 0; i < 3; i++) { cam_pos[i] = eye[i]; cam_fwd[i] = target[i] - eye[i]; }
	normalise(cam_fwd);
	cam_right[0] = cam_fwd[1] * up[2] - cam_fwd[2] * up[1];
	cam_right[1] = cam_fwd[2] * up[0] - cam_fwd[0] * up[2];
	cam_right[2] = cam_fwd[0] * up[1] - cam_fwd[1] * up[0];
	normalise(cam_right);
	cam_up[0] = cam_right[1] * cam_fwd[2] - cam_right[2] * cam_fwd[1];
	cam_up[1] = cam_right[2] * cam_fwd[0] - cam_right[0] * cam_fwd[2];
	cam_up[2] = cam_right[0] * cam_fwd[1] - cam_right[1] * cam_fwd[0];
}

static void render_frame(GSGLOBAL *gs, float seconds)
{
	float theta = 2.f * (float)M_PI * seconds / ORBIT_SECONDS;
	float cycles = seconds / CYCLE_SECONDS;
	int frame = (int)(fmodf(cycles, 1.f) * ANIM_FRAMES) % ANIM_FRAMES;
	float scroll = -ANIM_STRIDE * cycles;
	float c = cosf(theta), s = sinf(theta);
	xform orbit, view, world_to_view;
#ifdef NO_MAP
	xform bone, bone_world[NODE_COUNT], rifle_m, rifle_view, rifle_world;
#endif
#ifndef NO_VU0
	mat4 view4;
#ifdef NO_MAP
	mat4 bone_world4[NODE_COUNT], rifle_world4;
#endif
#endif
	int i, j;

#ifndef NO_MAP
	/* the Chief stands on the map and the camera circles him */
	orbit = chief_placement;
	{
		const float target[3] = { MAP_CHIEF_X, MAP_CHIEF_Y, MAP_CHIEF_Z + (MODEL_CENTER_Z - MODEL_FLOOR_Z) };
		const float eye[3] = { target[0] + MAP_CAMERA_DISTANCE * c, target[1] + MAP_CAMERA_DISTANCE * s, target[2] + MAP_CAMERA_HEIGHT };

		camera_look(eye, target);
	}
#else
	/* orbit about the model's centre; row-vector rotation [[c,s,0],[-s,c,0],[0,0,1]] */
	memset(&orbit, 0, sizeof(orbit));
	orbit.r[0][0] = c; orbit.r[0][1] = s; orbit.r[1][0] = -s; orbit.r[1][1] = c; orbit.r[2][2] = 1.f;
	{
		const float ctr[3] = { MODEL_CENTER_X, MODEL_CENTER_Y, MODEL_CENTER_Z };
		float rc[3];

		apply_vector(&orbit, ctr, rc);
		for (i = 0; i < 3; i++) orbit.t[i] = ctr[i] - rc[i];
	}
#endif
	/* the view: the camera's right, up and forward as the columns */
	for (i = 0; i < 3; i++)
	{
		view.r[i][0] = cam_right[i]; view.r[i][1] = cam_up[i]; view.r[i][2] = cam_fwd[i];
	}
	for (j = 0; j < 3; j++)
		view.t[j] = -(cam_pos[0] * view.r[0][j] + cam_pos[1] * view.r[1][j] + cam_pos[2] * view.r[2][j]);
	xform_then(&orbit, &view, &world_to_view);

#ifdef NO_MAP
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
#elif !defined(NO_VU0)
	mat4_from_xform(&view4, &view);
#endif

#ifndef NO_MAP
	gsKit_clear(gs, GS_SETREG_RGBAQ(0x70, 0x9c, 0xd0, 0x00, 0x00));    /* the sky */
#ifdef CHIEF_VU1
	chief_vu1_kick(gs, frame, &view);                                  /* the clear goes out, then VU1 draws the Chief while the EE builds the map */
	PROF_MARK(P_VU1);
#endif
	PROF_MARK(P_SETUP);
#ifndef NO_VU0
	draw_map(gs, &view, &view4);
#else
	draw_map(gs, &view, NULL);
#endif
	(void)scroll; (void)world_to_view;
	PROF_MARK(P_GROUND);
#else
	/* backdrop */
	gsKit_clear(gs, GS_SETREG_RGBAQ(0x20, 0x26, 0x2c, 0x00, 0x00));
	gsKit_set_test(gs, GS_ZTEST_OFF);
	PROF_MARK(P_SETUP);
	draw_ground(gs, &world_to_view, theta, scroll);
	gsKit_set_test(gs, GS_ZTEST_ON);
	PROF_MARK(P_GROUND);
#endif

#ifndef NO_MAP
#ifndef NO_VU0
	vu0_load_view(&view4);
	vu0_load_constants(vu_const);
#endif
	cached_pose(frame, &view);
	PROF_MARK(P_RIFLE);
#else
#ifndef NO_VU0
	skin_chief(bone_world4, &view4, CHIEF_VERTS - 69);
	PROF_MARK(P_SKIN);
	for (i = 0; i < RIFLE_VERTS; i++)
	{
		vec4 pw, nn, pp;
		svert *o = &rifle_v[i];

		vu0_transform(&rifle_world4, &rifle_p4[i], &rifle_n4[i], &pw, &nn, &pp);
		project(pp.v, 0, o);
		o->uv = uv_reg(rifle_uv[i * 2], rifle_uv[i * 2 + 1], (float)RIFLE_TEX_SIZE);
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
		o->uv = uv_reg(rifle_uv[i * 2], rifle_uv[i * 2 + 1], (float)RIFLE_TEX_SIZE);
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
		project(pp, DISPLAY_NUDGE, o);
		o->uv = uv_reg(display_uv[i * 2], display_uv[i * 2 + 1], (float)DISPLAY_TEX_SIZE);
		o->rgbaq = GS_SETREG_RGBAQ(128, 128, 128, 0x80, 0x00);
	}
	PROF_MARK(P_RIFLE);
#endif

#ifndef CHIEF_VU1                    /* (VU1 has drawn the armour and the rifle with their masks, before the map) */
	draw_textured(gs, &chief_tex, chief_v, NULL, NULL, chief_tri, CHIEF_ARMOR_FIRST_TRI, CHIEF_ARMOR_TRIS);
#endif
	draw_flat_gouraud(gs, chief_v, chief_tri, CHIEF_VISOR_FIRST_TRI, CHIEF_VISOR_TRIS);
	/* the armour's reflection and highlight: its triangles again, the mask texture added on */
#if !defined(NO_SPECULAR_PASS) && !defined(CHIEF_VU1)
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	draw_textured(gs, &mask_tex, chief_v, chief_spec_rgbaq, chief_mask_uv, chief_tri, CHIEF_ARMOR_FIRST_TRI, CHIEF_ARMOR_TRIS);
	gs->PrimAlphaEnable = GS_SETTING_OFF;
#endif
#ifndef CHIEF_VU1
	draw_textured(gs, &rifle_tex, rifle_v, NULL, NULL, rifle_tri, 0, RIFLE_TRIS);
#endif
	draw_textured(gs, &display_tex, display_v, NULL, NULL, display_tri, 0, DISPLAY_TRIS);
#if !defined(NO_SPECULAR_PASS) && !defined(CHIEF_VU1)
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
	draw_textured(gs, &rifle_mask_tex, rifle_v, rifle_spec_rgbaq, NULL, rifle_tri, 0, RIFLE_TRIS);
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

	gs = gsKit_init_global_custom(6 * 1024 * 1024, 16 * 1024);   /* a frame of the map's packets (four passes of a third of its triangles) takes about 2.5 MB */
	gs->Mode = GS_MODE_NTSC;        /* 60 Hz whatever the console's region: the scene is timed for it */
	gs->Height = 448;               /* gsKit sized the buffers for the console's own mode (PAL: 512 lines) before this */
	gs->Width = SCREEN_WIDTH;       /* and its magnification (MagH = DW / Width - 1) follows the width */
#if defined(FB16) || !defined(NO_MAP)
	gs->PSM = GS_PSM_CT16S;         /* 16-bit frame buffers: 1.2 MB less video memory, for the map's textures */
	gs->Dithering = GS_SETTING_ON;  /* hides the banding that 5 bits a channel would show */
#else
	gs->PSM = GS_PSM_CT32;
#endif
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
	/* the rifle's mask is the size of its base map (the second pass reuses its UV registers), the Chief's has its own UVs */
	texture_setup(gs, &chief_tex, CHIEF_TEX_SIZE, GS_PSM_T8, chief_tex_pixels, chief_tex_clut);
	texture_setup(gs, &rifle_tex, RIFLE_TEX_SIZE, GS_PSM_T8, rifle_tex_pixels, rifle_tex_clut);
	texture_setup(gs, &mask_tex, MASK_TEX_SIZE, GS_PSM_T4, mask_tex_pixels, mask_tex_clut);
	texture_setup(gs, &rifle_mask_tex, RIFLE_MASK_SIZE, GS_PSM_T4, rifle_mask_pixels, rifle_mask_clut);
	texture_setup(gs, &display_tex, DISPLAY_TEX_SIZE, GS_PSM_T8, display_tex_pixels, display_tex_clut);
#ifndef NO_MAP
	for (i = 0; i < MAP_TEXTURES; i++)
	{
		texture_setup_mips(gs, &map_tex[i], map_textures[i].width, map_textures[i].height, map_textures[i].levels, map_textures[i].pixels, map_textures[i].clut, 0);
		map_tex[i].alpha = map_textures[i].alpha;
	}
	for (i = 0; i < MAP_DETAILS; i++)         /* the grey textures share one palette: alpha = index */
		texture_setup_mips(gs, &map_detail_tex[i], map_details[i].width, map_details[i].height, map_details[i].levels, map_details[i].pixels, map_gray_clut, i ? map_detail_tex[0].t.VramClut : 0);
	for (i = 0; i < MAP_LIGHTMAPS; i++)
		texture_setup_clut(gs, &map_lm_tex[i], map_lightmaps[i].width, map_lightmaps[i].height, map_lightmaps[i].pixels, map_gray_clut, map_detail_tex[0].t.VramClut);
	for (i = 0; i < MAP_LIGHTMAPS; i++) map_lm_tex[i].t.Filter = GS_FILTER_LINEAR;
#endif

	printf("VRAM: %u of 4194304 bytes allocated (buffers %u, textures %u, free %u)\n", (unsigned)gs->CurrentPointer,
	       (unsigned)gs->TexturePointer, (unsigned)(gs->CurrentPointer - gs->TexturePointer), (unsigned)(4194304 - gs->CurrentPointer));
	if (vram_failed) { printf("VRAM: a texture did not fit; stopping\n"); return 1; }
	tables_setup();
#ifndef NO_VU0
	pack_vertices();
#endif
#ifdef NO_MAP
	{
		static const float base[3] = { 0.075f, 0.09f, 0.11f }, add[3] = { 0.10f, 0.14f, 0.17f };

		for (i = 0; i < 3; i++) ground_color[i] = powf(base[i] + add[i], GAMMA);
	}
#endif

	for (i = 0; i < CHIEF_VERTS; i++) chief_mask_uv[i] = uv_reg(chief_uv[i * 2], chief_uv[i * 2 + 1], (float)MASK_TEX_SIZE);
	/* the rifle's mask has its base map's size (RIFLE_MASK_SIZE == RIFLE_TEX_SIZE): the second pass shares its UVs */

	/* lights, and the camera: in front of the model (+x), a little above its middle, looking at the
	   centre (the map scene moves the camera every frame) */
	{
		float centre[3] = { MODEL_CENTER_X, MODEL_CENTER_Y, MODEL_CENTER_Z };
		float eye[3] = { centre[0] + CAMERA_DISTANCE, centre[1], centre[2] + CAMERA_HEIGHT };
		static const float key[3] = { 0.6f, -0.5f, 0.7f }, fill[3] = { 0.3f, 0.8f, 0.2f }, back[3] = { -0.8f, -0.1f, 0.3f };

		camera_look(eye, centre);
		memcpy(light_dir[0], key, sizeof(key)); memcpy(light_dir[1], fill, sizeof(fill)); memcpy(light_dir[2], back, sizeof(back));
		for (i = 0; i < 3; i++) normalise(light_dir[i]);
	}
	screen_cx = gs->Width * 0.5f;
	screen_cy = gs->Height * 0.5f;
#ifndef NO_MAP
	focal = (1.f / tanf(MAP_FOV_HALF_DEGREES * (float)M_PI / 180.f)) * gs->Height * 0.5f;
	z_near = MAP_NEAR;
	z_far = MAP_FAR;
#else
	focal = (1.f / tanf(FOV_HALF_DEGREES * (float)M_PI / 180.f)) * gs->Height * 0.5f;
	z_near = CAMERA_DISTANCE - MODEL_RADIUS * 1.5f;
	z_far = CAMERA_DISTANCE + MODEL_RADIUS * 2.5f;
#endif
	focal_x = focal * (float)gs->Width / 640.f;        /* the display is 640 wide whatever the buffer's width */
	z_k1 = 65535.f * z_far * z_near / (z_far - z_near);
	z_k2 = 65535.f * z_near / (z_far - z_near);
#ifndef NO_MAP
	clip_setup(gs);
	placement_setup();
	pose_cache_build();
#ifdef CHIEF_VU1
	chief_vu1_init();
#endif
	map_lod_setup();
#endif

	refresh = gs->Mode == GS_MODE_PAL ? 50.f : 60.f;
	gsKit_add_vsync_handler(vsync_handler);
#ifdef MIP_AUDIT                     /* the whole orbit in half-degree steps, then the report */
	{
		int f, t, d;

		for (t = 0; t < MAP_TEXTURES; t++) for (d = 0; d < 4; d++) audit_lod[d][t] = AUDIT_BIG;
		for (d = 0; d < MAP_DETAILS; d++) audit_detail_lod[d] = AUDIT_BIG;
		for (f = 0; f < 720; f++)
		{
			render_frame(gs, f * (ORBIT_SECONDS / 720.f));
			gsKit_queue_exec(gs);
			gsKit_sync_flip(gs);
		}
		for (t = 0; t < MAP_TEXTURES; t++)
			printf("MIPAUDIT tex %d drawn %u base %.2f d0 %.2f d1 %.2f bump %.2f\n", t, audit_drawn[t], audit_lod[0][t], audit_lod[1][t], audit_lod[2][t], audit_lod[3][t]);
		for (d = 0; d < MAP_DETAILS; d++) printf("MIPAUDIT detail %d %.2f\n", d, audit_detail_lod[d]);
		printf("MIPAUDIT done\n");
		for (;;) ;
	}
#endif

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
#elif defined(PROFILE)             /* the orbit steps by frame, not by the clock, so that a dropped frame does not move the camera and builds see the same views */
		{
			static int frame_no;

			render_frame(gs, frame_no++ / refresh);
		}
#else
		render_frame(gs, vsync_count / refresh);
#endif
		gsKit_queue_exec(gs);
		PROF_MARK(P_EXEC);
#ifdef PROFILE
		{
			u32 busy = cpu_ticks() - prof_frame_start;

			if (busy > prof_busy_max) prof_busy_max = busy;
		}
#endif
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
#ifndef NO_MAP
			printf("PROFILE   map: %u of %d chunks in view, %u vertices processed, %u triangles clipped a frame\n",
				prof_map_chunks / frames, MAP_CHUNKS, prof_map_vertices / frames, prof_map_clipped / frames);
			printf("PROFILE   clipped: %u by the near plane, %u by the guard band\n", prof_clip_near / frames, prof_clip_side / frames);
			prof_clip_near = prof_clip_side = 0;
			prof_map_clipped = prof_map_vertices = prof_map_chunks = 0;
#endif
			{
				float busy = 0.f;

				for (p = 0; p < P_COUNT; p++)
				{
					printf("PROFILE   %-11s %6.2f ms (at most %.2f)\n", prof_name[p], prof_sum[p] * ms_per_tick / frames, prof_max[p] * ms_per_tick);
					prof_max[p] = 0;
					if (p != P_FLIP) busy += prof_sum[p] * ms_per_tick / frames;
					prof_sum[p] = 0;
				}
				printf("PROFILE   EE busy %.2f ms of %.2f; the worst frame %.2f ms\n", busy, 1000.f / refresh, prof_busy_max * ms_per_tick);
				prof_busy_max = 0;
			}
			prof_tris = prof_tris_culled = 0;
			frames = 0;
		}
		prof_frame_start = prof_last = cpu_ticks();         /* after the report: printing it is not the frame's work */
#endif
	}
	return 0;
}
