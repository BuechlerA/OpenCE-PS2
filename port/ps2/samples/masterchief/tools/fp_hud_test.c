/* Host test of src/fp_hud.c: the quads the HUD makes for the player's weapon state, checked against what the tags say, and drawn into a picture (a software composite of the quads
   with the baked textures, the way the GS draws them: tint times texture, alpha blended; a meter's palette for its value) for looking at: pass an output prefix and it writes
   <prefix>_<scenario>.ppm.

     cc -O1 -I src -o /tmp/fp_hud_test tools/fp_hud_test.c src/fp_hud.c src/hud_assets.c -lm && /tmp/fp_hud_test [prefix]
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fp_hud.h"

#define W 640
#define H 448

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float picture[H][W][3];

/* the palette entry of an 8-bit texel in the GS's order (an entry i is kept at (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1)) */
static unsigned gs_entry(const unsigned *clut, int i) { return clut[(i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1)]; }

static void composite(const fph_quad *q)
{
	const hud_image *im = &hud_images[q->image];
	const hud_texture *t = &hud_textures[im->texture];
	unsigned palette[256];
	int x, y, x0 = (int)floorf(q->x0), y0 = (int)floorf(q->y0), x1 = (int)ceilf(q->x1), y1 = (int)ceilf(q->y1);
	float tr = ((q->color >> 16) & 255) / 255.f, tg = ((q->color >> 8) & 255) / 255.f, tb = (q->color & 255) / 255.f, ta = (q->color >> 24) / 255.f;

	if (q->kind == FPH_METER) fph_meter_palette(q, palette);
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
		{
			float u = (x + 0.5f - q->x0) / (q->x1 - q->x0) * (im->x1 - im->x0) + im->x0, v = (y + 0.5f - q->y0) / (q->y1 - q->y0) * (im->y1 - im->y0) + im->y0;
			int tx = (int)u, ty = (int)v, c;
			float r, g, b, a;

			if (x < 0 || y < 0 || x >= W || y >= H || tx < 0 || ty < 0 || tx >= t->width || ty >= t->height) continue;
			c = t->pixels[ty * t->width + tx];
			if (q->kind == FPH_METER)
			{
				unsigned e = palette[c];

				r = (e & 255) / 255.f; g = ((e >> 8) & 255) / 255.f; b = ((e >> 16) & 255) / 255.f; a = (e >> 24) / 255.f;
				picture[y][x][0] = picture[y][x][0] * (1.f - a) + r * a; picture[y][x][1] = picture[y][x][1] * (1.f - a) + g * a; picture[y][x][2] = picture[y][x][2] * (1.f - a) + b * a;
			}
			else
			{
				unsigned e = gs_entry(t->clut, c);

				r = (e & 255) / 255.f * tr; g = ((e >> 8) & 255) / 255.f * tg; b = ((e >> 16) & 255) / 255.f * tb; a = (((e >> 24) & 255) / 128.f) * ta;
				if (a > 1.f) a = 1.f;
				picture[y][x][0] = picture[y][x][0] * (1.f - a) + r * a; picture[y][x][1] = picture[y][x][1] * (1.f - a) + g * a; picture[y][x][2] = picture[y][x][2] * (1.f - a) + b * a;
			}
		}
}

static void write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	int x, y;

	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
		{
			int k;

			for (k = 0; k < 3; k++) { float v = picture[y][x][k]; fputc((int)(v < 0.f ? 0.f : v > 1.f ? 255.f : v * 255.f + 0.5f), f); }
		}
	fclose(f);
}

static fph_player rifle(int loaded, int total)
{
	fph_player p;

	memset(&p, 0, sizeof(p));
	p.weapon = 0; p.magazine_count = 1; p.zoom_level = -1; p.grenade_count = 2; p.shield = 1.f; p.health = 1.f;
	p.magazines[0].rounds_loaded = loaded; p.magazines[0].rounds_loaded_maximum = 60; p.magazines[0].rounds_remaining = total; p.magazines[0].can_fire = 1;
	return p;
}

static int count_images(const fph_quad *q, int n, int kind) { int i, c = 0; for (i = 0; i < n; i++) if (q[i].kind == kind) c++; return c; }

static void scene(const char *prefix, const char *name, fph_state *st, const fph_player *p, int ticks)
{
	static fph_quad quads[FPH_MAX_QUADS];
	fph_window w = { 48, 34, 592, 414 };
	int i, x, y, n;
	char path[256];

	for (i = 0; i < ticks; i++) fph_tick(st, p);
	n = fph_render(st, p, &w, quads, FPH_MAX_QUADS);
	printf("%s: %d quads (%d meters)\n", name, n, count_images(quads, n, FPH_METER));
	if (!prefix) return;
	for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float g = 0.10f + 0.30f * (float)y / H; picture[y][x][0] = g * 0.9f; picture[y][x][1] = g * 0.85f; picture[y][x][2] = g * 0.6f; }
	for (i = 0; i < n; i++) composite(&quads[i]);
	sprintf(path, "%s_%s.ppm", prefix, name);
	write_ppm(path);
}

int main(int argc, char **argv)
{
	const char *prefix = argc > 1 ? argv[1] : NULL;
	static fph_quad quads[FPH_MAX_QUADS];
	fph_window w = { 48, 34, 592, 414 };
	fph_state st;
	fph_player p;
	int n, i;

	/* the full rifle: the crosshair, the total ammunition's panel, number and icon, the loaded meter, the grenades, the shield and the health, the motion sensor */
	fph_init(&st); p = rifle(60, 180);
	fph_tick(&st, &p);
	n = fph_render(&st, &p, &w, quads, FPH_MAX_QUADS);
	CHECK(n > 10 && n < FPH_MAX_QUADS, "%d quads for the full rifle", n);
	CHECK(count_images(quads, n, FPH_METER) == 3, "the rifle's hud has %d meters (the loaded rounds, the shield, the health)", count_images(quads, n, FPH_METER));
	for (i = 0; i < n; i++) CHECK(quads[i].x1 > quads[i].x0 && quads[i].y1 > quads[i].y0, "quad %d is empty", i);
	{
		/* the crosshair is first, centred on the window's middle (320, 224) */
		float cx = (quads[0].x0 + quads[0].x1) * 0.5f, cy = (quads[0].y0 + quads[0].y1) * 0.5f;

		CHECK(fabsf(cx - 320.f) < 1.f && fabsf(cy - 224.f) < 1.f, "the crosshair is at (%.1f, %.1f)", cx, cy);
	}
	/* a reload: the ammunition meter is full again and the numbers change; more shots: the meter's threshold falls with the rounds (4 * rounds + 2) */
	for (i = 0; i < n; i++) if (quads[i].kind == FPH_METER && quads[i].threshold == 242) break;
	CHECK(i < n, "no meter at the threshold 4 * 60 + 2 for 60 rounds");
	p = rifle(37, 180);
	fph_tick(&st, &p);
	n = fph_render(&st, &p, &w, quads, FPH_MAX_QUADS);
	for (i = 0; i < n; i++) if (quads[i].kind == FPH_METER && quads[i].threshold == 4 * 37 + 2) break;
	CHECK(i < n, "no meter at the threshold 4 * 37 + 2 for 37 rounds");

	scene(prefix, "rifle_full", (fph_init(&st), &st), &(fph_player){ 0 }, 0);       /* (a player with no weapon: the unit hud only) */
	fph_init(&st); p = rifle(60, 180); scene(prefix, "rifle", &st, &p, 1);
	fph_init(&st); p = rifle(29, 100); scene(prefix, "rifle_29", &st, &p, 1);
	fph_init(&st); p = rifle(8, 100); scene(prefix, "rifle_low", &st, &p, 45);        /* under a sixth of the magazine: flashing */
	fph_init(&st); p = rifle(0, 0); p.magazines[0].can_fire = 1; scene(prefix, "rifle_empty", &st, &p, 30);
	fph_init(&st); p = rifle(30, 100); p.magazines[0].reloading = 1; scene(prefix, "rifle_reload", &st, &p, 20);
	fph_init(&st); p = rifle(12, 48); p.weapon = 1; p.magazines[0].rounds_loaded_maximum = 12; scene(prefix, "pistol", &st, &p, 1);
	fph_init(&st); p = rifle(12, 48); p.weapon = 1; p.magazines[0].rounds_loaded_maximum = 12; p.zoom_level = 0; scene(prefix, "pistol_zoom", &st, &p, 1);
	printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
	return failures != 0;
}
