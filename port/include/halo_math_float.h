/*
HALO_MATH_FLOAT.H

Single-precision maths where the game's arguments are single precision
(included by the game's <math.h> after halo_math.h, on every port).

The game's reals are floats, but it calls <math.h>'s double functions on
them: `(real)cos(x*6.2831855f)`, `fabs(a->i)`, `floor(frame)`. MSVC's x87
code made the promotions free; the PS2's EE has no double-precision FPU, so
each costs a software conversion (about 320 cycles for a double add, more
for the round trip), and the double libm function runs in software too
(docs/ps2_ee_performance.md). The macros below send a call whose argument
is a float to the float function, and leave a double argument alone.

The exact functions (fabs, floor, ceil, sqrt, fmod) give the same bits in
either precision: floorf(x) == (float)floor((double)x), and sqrt rounded to
double then to float is the correctly rounded float square root (53 bits is
more than twice 24). Those go to the compiler's builtins (abs.s, sqrt.s on
the EE) and need nothing from a C library.

The transcendental ones (sin, cos, tan, atan2, pow and the rest) are
another matter. halo_math.h gives them halo_ names so that the native ports
agree bit for bit in a system link game (musl's code on every port). On the
native ports halo_sinf is still halo_sin's result rounded to float: musl's
own sinf computes in double, so it would save nothing, and nothing changes
in those builds. The PS2 is never in a system link game with them, so its
halo_sinf is newlib's sinf, which is single precision throughout.

fast_ftol and fmod(x, 1.0) have replacements here too (fast_ftol is in
cseries.h, both are everywhere in the game's per-tick code), and the EE's
FPU rounds toward zero, so they do not depend on the rounding mode.
*/

#ifndef __HALO_MATH_FLOAT_H
#define __HALO_MATH_FLOAT_H

/* Is the argument a float (not a double, not an integer)? Both branches of
__builtin_choose_expr are compiled, only the chosen one is evaluated, and
__typeof__ evaluates nothing. GCC and clang have both in every C mode. */
#define HALO_IS_FLOAT(x) __builtin_types_compatible_p(__typeof__(x), float)
#define HALO_FLOAT_OR_DOUBLE(x, float_call, double_call) \
	__builtin_choose_expr(HALO_IS_FLOAT(x), float_call, double_call)

/* ---------- the transcendental functions */

#ifdef HALO_PS2
/* newlib's, single precision throughout; its <math.h> hides them under
__STRICT_ANSI__, so they are declared here */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float x, float y);
#define halo_sinf(x) sinf(x)
#define halo_cosf(x) cosf(x)
#define halo_tanf(x) tanf(x)
#define halo_asinf(x) asinf(x)
#define halo_acosf(x) acosf(x)
#define halo_atanf(x) atanf(x)
#define halo_atan2f(y, x) atan2f(y, x)
#define halo_expf(x) expf(x)
#define halo_logf(x) logf(x)
#define halo_log2f(x) log2f(x)
#define halo_log10f(x) log10f(x)
#define halo_powf(x, y) powf(x, y)
#else
/* the native ports: the same results as before, bit for bit */
#define halo_sinf(x) ((float)halo_sin(x))
#define halo_cosf(x) ((float)halo_cos(x))
#define halo_tanf(x) ((float)halo_tan(x))
#define halo_asinf(x) ((float)halo_asin(x))
#define halo_acosf(x) ((float)halo_acos(x))
#define halo_atanf(x) ((float)halo_atan(x))
#define halo_atan2f(y, x) ((float)halo_atan2(y, x))
#define halo_expf(x) ((float)halo_exp(x))
#define halo_logf(x) ((float)halo_log(x))
#define halo_log2f(x) ((float)halo_log2(x))
#define halo_log10f(x) ((float)halo_log10(x))
#define halo_powf(x, y) ((float)halo_pow(x, y))
#endif

#undef sin
#undef cos
#undef tan
#undef asin
#undef acos
#undef atan
#undef atan2
#undef exp
#undef log
#undef log2
#undef log10
#undef pow

#define sin(x) HALO_FLOAT_OR_DOUBLE(x, halo_sinf(x), halo_sin(x))
#define cos(x) HALO_FLOAT_OR_DOUBLE(x, halo_cosf(x), halo_cos(x))
#define tan(x) HALO_FLOAT_OR_DOUBLE(x, halo_tanf(x), halo_tan(x))
#define asin(x) HALO_FLOAT_OR_DOUBLE(x, halo_asinf(x), halo_asin(x))
#define acos(x) HALO_FLOAT_OR_DOUBLE(x, halo_acosf(x), halo_acos(x))
#define atan(x) HALO_FLOAT_OR_DOUBLE(x, halo_atanf(x), halo_atan(x))
#define exp(x) HALO_FLOAT_OR_DOUBLE(x, halo_expf(x), halo_exp(x))
#define log(x) HALO_FLOAT_OR_DOUBLE(x, halo_logf(x), halo_log(x))
#define log2(x) HALO_FLOAT_OR_DOUBLE(x, halo_log2f(x), halo_log2(x))
#define log10(x) HALO_FLOAT_OR_DOUBLE(x, halo_log10f(x), halo_log10(x))

/* two arguments: the float function when both are floats, or when the
other is an integer (pow(0.75f, ticks): an integer converts to float
exactly), never when either is a double */
#define HALO_IS_DOUBLE(x) __builtin_types_compatible_p(__typeof__(x), double)
#define HALO_FLOAT_PAIR(x, y) \
	((HALO_IS_FLOAT(x) || HALO_IS_FLOAT(y)) && !HALO_IS_DOUBLE(x) && !HALO_IS_DOUBLE(y))
#define atan2(y, x) __builtin_choose_expr(HALO_FLOAT_PAIR(y, x), halo_atan2f(y, x), halo_atan2(y, x))
#define pow(x, y) __builtin_choose_expr(HALO_FLOAT_PAIR(x, y), halo_powf(x, y), halo_pow(x, y))

/* ---------- the exact functions: the builtins, the same bits in either precision */

#define fabs(x) HALO_FLOAT_OR_DOUBLE(x, __builtin_fabsf(x), __builtin_fabs(x))
#define floor(x) HALO_FLOAT_OR_DOUBLE(x, __builtin_floorf(x), __builtin_floor(x))
#define ceil(x) HALO_FLOAT_OR_DOUBLE(x, __builtin_ceilf(x), __builtin_ceil(x))
#define sqrt(x) HALO_FLOAT_OR_DOUBLE(x, __builtin_sqrtf(x), __builtin_sqrt(x))
#define fmod(x, y) __builtin_choose_expr(HALO_FLOAT_PAIR(x, y), __builtin_fmodf(x, y), __builtin_fmod(x, y))

/* ---------- replacements for the hottest sites */

/* fmod(x, 1.0) for a float: x less its integer part, keeping x's sign, the
same bits as fmod. The conversion to long truncates whatever the FPU's
rounding mode, and the difference is exact. Past 2^23 a float is an
integer, so the fraction is 0 (fmod gives a zero of x's sign). */
static __inline__ float halo_fmod1f(
	float x)
{
	return __builtin_fabsf(x) < 8388608.0f ? x - (float)(long)x : 0.0f*x;
}

/* (long)rint(x), to the nearest and to the even on a tie, as cseries.h's
fast_ftol has it (FISTP under the default control word): from the truncated
conversion and the exact remainder, so it holds under the EE's round-toward-
zero FPU too (a float add of 2^23 would not). Past 2^23 the value is an
integer already; (long) of it is the game's own overflow, as before. */
static __inline__ long halo_rint_to_long(
	float x)
{
#ifndef HALO_PS2
	/* one instruction on x86 and ARM, which round to the nearest */
	return __builtin_lrintf(x);
#else
	long truncated;
	float remainder;

	if (!(__builtin_fabsf(x) < 8388608.0f))
		return (long)x;
	truncated = (long)x;
	remainder = x - (float)truncated;
	if (remainder > 0.5f || (remainder == 0.5f && (truncated & 1)))
		truncated++;
	else if (remainder < -0.5f || (remainder == -0.5f && (truncated & 1)))
		truncated--;
	return truncated;
#endif
}

#endif /* __HALO_MATH_FLOAT_H */
