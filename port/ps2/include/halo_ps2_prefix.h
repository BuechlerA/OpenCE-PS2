/*
HALO_PS2_PREFIX.H

Force-included ahead of every translation unit of the PS2 build (gcc
-include), as halo_linux_prefix.h is in the Linux build, whose MSVC and XDK
environment it reuses. The PS2's EE (n32 ABI) has the Xbox's 32-bit long and
pointers and its 8-byte alignment of double and long long, so the game's
structures keep their Xbox layout (docs/ps2_port_plan.md).

What differs from the Linux build is what GCC does not take from clang:
MSVC's __int64 and x86 calling conventions, and its leniency with inline
functions (below).
*/

#ifndef __HALO_PS2_PREFIX_H
#define __HALO_PS2_PREFIX_H

#if !defined(_EE) || !defined(__mips__)
#error the PS2 build targets the Emotion Engine (mips64r5900el-ps2-elf-gcc -D_EE)
#endif

#define HALO_PS2 1

/* GCC's <stddef.h> leaves wchar_t out once _WCHAR_T_DEFINED is defined,
which the Linux prefix does for the XDK headers: have it first (16-bit, by
-fshort-wchar) */
#include <stddef.h>

#include "../../linux/include/halo_linux_prefix.h"

/* ---------- MSVC types and calling conventions clang knows and GCC does not */

#define __int64 long long
#define __stdcall
#define __cdecl
#define __fastcall

/* __declspec, which GCC knows only for Windows targets: the forms the game
and the XDK declarations use */
#define __declspec(specifier) __halo_ps2_declspec_##specifier
#define __halo_ps2_declspec_align(bytes) __attribute__((aligned(bytes)))
#define __halo_ps2_declspec_selectany __attribute__((weak))
#define __halo_ps2_declspec_noreturn __attribute__((noreturn))
#define __halo_ps2_declspec_dllexport
#define __halo_ps2_declspec_dllimport

/* ---------- <limits.h>

newlib's <sys/types.h> includes <limits.h>, which glibc's does not under
__STRICT_ANSI__, and cseries.h declares LONG_MAX, LONG_MIN, CHAR_MAX and
CHAR_MIN itself, as enum constants of the same values. Include it once here
and give those names to cseries.h. */

#include <limits.h>
#undef LONG_MAX
#undef LONG_MIN
#undef CHAR_MAX
#undef CHAR_MIN

/* ---------- MSVC inline semantics

The Linux prefix makes __inline static, and its halo_msvc_semantics.h marks
every inline function name `#pragma weak` for those that also have an
ordinary prototype, whose definitions clang keeps external. GCC refuses both
a static definition after an extern prototype and a weak static function.
Here the inline functions are GNU89 inline instead (-std=gnu89): each unit
emits an external definition, which the header's `#pragma weak` makes weak,
so the linker keeps one, as MSVC keeps one COMDAT. The few functions the
source itself declares `static __inline` stay static, and
tools/ps2/compile_check.py leaves their names out of the pragmas. */

#undef __inline
#undef _inline
#undef __forceinline
#define __inline __inline__
#define _inline __inline__
#define __forceinline __inline__ __attribute__((always_inline))

#endif /* __HALO_PS2_PREFIX_H */
