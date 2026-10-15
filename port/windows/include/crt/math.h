/*
MATH.H

The C runtime's <math.h> for the game, with sin, pow and the rest the same
on every port (port/include/halo_math.h).
*/

#ifndef __HALO_WINDOWS_MATH_H
#define __HALO_WINDOWS_MATH_H

#include_next <math.h>

#include "../../../include/halo_math.h"
/* and their single-precision forms where the argument is a float */
#include "../../../include/halo_math_float.h"

#endif
