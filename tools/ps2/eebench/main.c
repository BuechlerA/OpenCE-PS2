/*
 * Prices the floating-point operations the game and the samples use per vertex or per object, in
 * EE cycles. Each loop runs for a quarter of a second against the bus-clock timer (147.456 MHz, half
 * the EE's). The output array is volatile so the compiler cannot delete a loop whose result is never
 * read. See docs/ps2_ee_performance.md for the numbers and what they mean.
 *
 *     make                                  # PS2DEV, PS2SDK set; writes bench.elf
 *     python tools/ps2/pcsx2_run.py tools/ps2/eebench/bench.elf --until "BENCH DONE"
 */
#include <stdio.h>
#include <math.h>
#include <kernel.h>
#include <timer.h>

#define N 20000
static float in[N];
static volatile float out[N];
static double din[N];
static float lut[2049];
static volatile float m[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.1f, 0.2f, 0.3f };
typedef struct { float x, y; int z; unsigned char rgb[3]; float u, v; } svert;
static svert sv[64];

__attribute__((noinline)) static float ext_fn(float x) { return x * 2.f; }

static float lut_pow(float x) /* x in [0, 4): 2048 entries, linear interpolation */
{
	float f = x * 512.f; int i = (int)f; float t = f - i;
	return lut[i] + (lut[i + 1] - lut[i]) * t;
}

static void pow18(int i) { float a = in[i], b = a * a, c = b * b, d = c * c, e = d * d; out[i] = e * b; }

#define TIME(label, ...) do { u64 t0 = GetTimerSystemTime(), t1; u32 reps = 0; int i; \
	do { for (i = 0; i < N; i++) { __VA_ARGS__; } reps++; t1 = GetTimerSystemTime(); } while (t1 - t0 < 147456 * 250); \
	printf("BENCH %-30s %7.1f EE cycles each\n", label, 2.0 * (t1 - t0) / ((double)reps * N)); } while (0)

int main(void)
{
	int i;

	for (i = 0; i < N; i++) { in[i] = 0.05f + 2.0f * (i % 97) / 97.f; din[i] = in[i]; }
	for (i = 0; i <= 2048; i++) lut[i] = powf(i / 512.f, 1.f / 2.2f);
	TIME("empty loop (load + store)", out[i] = in[i]);
	TIME("powf(x, 1/2.2)", out[i] = powf(in[i], 1.f / 2.2f));
	TIME("powf(x, 18)", out[i] = powf(in[i], 18.f));
	TIME("x^18 by squaring", pow18(i));
	TIME("sqrtf", out[i] = sqrtf(in[i]));
	TIME("1/x (div.s)", out[i] = 1.f / in[i]);
	TIME("lut_pow (2048, lerp)", out[i] = lut_pow(in[i]));
	TIME("double add (soft float)", out[i] = (float)(din[i] + 0.3));
	TIME("float * double literal", out[i] = (float)((double)in[i] * 0.5f + (double)in[(i + 1) % N]));
	TIME("mul.s + add.s (and a % N)", out[i] = in[i] * 1.5f + in[(i + 3) % N]);
	TIME("apply_point (12 mul, 9 add, % N)", { const float *q = &in[i % (N - 3)]; float x = q[0]*m[0]+q[1]*m[1]+q[2]*m[2]+m[9], y = q[0]*m[3]+q[1]*m[4]+q[2]*m[5]+m[10], z = q[0]*m[6]+q[1]*m[7]+q[2]*m[8]+m[11]; out[i] = x + y + z; });
	TIME("normalise (sqrt, 3 div, % N)", { const float *q = &in[i % (N - 3)]; float l = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]); if (l > 1e-9f) { out[i] = q[0] / l + q[1] / l + q[2] / l; } });
	TIME("normalise (1/sqrt, 3 mul, % N)", { const float *q = &in[i % (N - 3)]; float l = 1.f / sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]); out[i] = (q[0] + q[1] + q[2]) * l; });
	TIME("float -> int (trunc)", { int k = (int)(in[i] * 100.f); out[i] = (float)k; });
	TIME("call of a non-inline fn", out[i] = ext_fn(in[i]));
	TIME("28-byte struct copy (% 64)", { sv[i % 64] = sv[(i + 1) % 64]; out[i] = sv[i % 64].x; });
	printf("BENCH DONE\n");
	SleepThread();
	return 0;
}
