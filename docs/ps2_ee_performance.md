# PS2 performance: what the EE does slowly, and how to measure it

Measured in PCSX2 while taking the Master Chief sample
(`port/ps2/samples/masterchief`) from 10 to 60 fps in October 2026. The
numbers are PCSX2's cycle accounting, not hardware (see "Caveats"), but the
ratios held up: the frame time fell by what each change predicted. Everything
here applies to the game's port as much as to the sample.

## The cost of common operations, in EE cycles (PCSX2)

From `tools/ps2/eebench` (build it with the ps2dev toolchain, run it with
`tools/ps2/pcsx2_run.py bench.elf --until "BENCH DONE"`):

| Operation | Cycles |
| --- | --- |
| `powf` (newlib) | ~540 |
| float expression with a `double` literal in it (promote, add, truncate: all software) | ~850 |
| one `double` add (software) | ~320 |
| `sqrtf`, `1/x` (`div.s`), float to int | 16 each |
| `mul.s` + `add.s` | a few |
| x^18 by repeated squaring (5 multiplies) | 30 |
| 2048-entry table lookup with linear interpolation | 32 |
| 3x3 transform and translate (12 multiplies, 9 adds) | ~70 |
| normalise with `sqrt.s` and three `div.s` | ~85; with one reciprocal and three multiplies ~70 |
| call of a small non-inlined function | 13 |
| 28-byte struct copy | 31 |
| integer `%` by a non-constant | ~40 |

The rules that follow:

- **No `powf`, `expf` or `logf` in anything per vertex, per pixel or per
  object.** One call costs as much as transforming eight vertices. Tabulate
  the function (a table with linear interpolation is exact to 8 bits), or
  turn constant exponents into multiplies (x^18 = ((x^2)^2)^2)^2 * x^2). A
  power of a scaled input comes from the power of the input: (x/4)^g =
  4^-g * x^g.
- **No `double`, anywhere hot.** A `double` literal (`0.5`, a `#define`
  without an `f`) in a float expression costs more than `powf`. The sample's
  exporter writes its constants with `f`; the game's build should use
  `-fsingle-precision-constant` and `-Wdouble-promotion` (see the plan).
  Check an object with `objdump -d | grep -c __adddf3` (also `__muldf3`,
  `__extendsfdf2`, `__truncdfsf2`).
- **Divide once.** `1.f / sqrtf(l2)` then three multiplies, not three
  divides; one reciprocal of the cube map's major axis, not two divides.
- **Skip work the data allows.** 1954 of the Chief's 2192 vertices have one
  bone, and the skinning matrices are rotations (checked: |R R^T - I| under
  4e-6), so those vertices skip the second bone and the normalisation.
- **Mark small helpers `static inline`** when they are called per vertex; the
  call and the spills around it cost as much as the arithmetic.
- **VU0 macro mode for transforms** is worth about a third of the FPU's time
  on the same work (the sample's skinning went from 2.7 to about 1.5 ms for
  2192 vertices). Pack the vertices as 16-byte quads at start-up, keep a
  matrix in `vf` registers across the loop (nothing else touches them), and
  use `vrsqrt` with `vwaitq` for the normalisation. Inline assembly with
  ps2dev's GCC accepts the `$vfN`, `$ACC`, `$Q` syntax; a 20-line example is
  `vu0_skin` in the sample's `main.c`.

## Sending primitives to the GS

gsKit's `gsKit_prim_*` functions cost about 680 EE cycles per primitive and,
for a textured one, write TEX0 (with a CLUT reload), TEX1 and PRIM for every
primitive. At 5000 triangles a frame that was 6 ms of EE time and 5000 CLUT
reloads on the GS. Instead, per mesh:

1. One A+D packet with PRIM, TEX0 and TEX1.
2. One REGLIST packet: the GIF tag with NREG registers (RGBAQ, UV, XYZ2:
   3 for textured, RGBAQ and XYZ2 for flat), NLOOP = the vertex count, then
   64 bits per register per vertex, straight from the vertex array. When
   NLOOP * NREG is odd, the GIF skips the last 64 bits of the final quadword.

Allocate both with `gsKit_heap_alloc(gs, qwords, qwords * 16, GIF_AD)`,
which reserves one more quadword for the tag and keeps gsKit's queue
bookkeeping right (gsKit's own list functions do the same). The sample's
submission went from 5.9 to 1.6 ms, and the GS gets 72 bytes per textured
triangle instead of 128.

**PCSX2 ignores PRE on a REGLIST tag:** a PRIM carried in the tag is not
applied, and the GS keeps the last PRIM written (in the sample, the ground's
lines, so the meshes drew as wireframe). Write PRIM through A+D. Hardware
honours PRE, but the A+D write costs nothing to keep.

Culling before packing (`(b - a) x (c - a)` in screen space, about half the
triangles) is cheap and halves everything after it.

## Drawing a map: what the Blood Gulch scene taught

Measured in PCSX2 taking the Chief standing on the Blood Gulch terrain
(`port/ps2/samples/masterchief`, 5,500 triangles, up to four passes each)
from 24 ms of EE time to 8.6 to 13.4 ms. The order they paid off in:

- **Do work once that does not change.** The Chief's pose changes 30 times a
  second and only the camera moves, so his skinned world positions, normals
  and diffuse light are cached for all 20 frames at start-up (3.3 MB), and a
  frame only projects them: 12 ms became 4. What does depend on the camera
  (his specular colour) is worked out for a third of the vertices a frame.
- **Cull before transforming.** A grid of 93 chunks with bounding spheres,
  tested against the five clip planes, lets a frame skip 20 to 70% of the map;
  its vertices are then transformed and projected only when a visible
  triangle first needs them (a stamp per vertex), which halved the vertex
  work again.
- **One divide a vertex.** q = 1 / z gives the screen position, the depth
  (k1 q - k2) and every texture coordinate (u q); the position tests (near
  plane, guard band) are comparisons on the screen position, not plane
  distances; the registers' floats are stored straight into the record.
- **Build the packet data once, send it as often as the passes need.** A
  vertex record of every register any pass uses (48 bytes) is built once per
  frame; each pass is that record sent whole, as a REGLIST packet in which the
  registers it does not use are directed at UV (ignored when drawing with ST).
  Copy with 128-bit loads and stores (`u128`): the EE's `memcpy` moves
  less.
- **Compiler flags matter more than expected.** `-O3 -ffast-math
  -funroll-loops` took 1.7 ms off the heaviest view (the EE's FPU has no IEEE
  behaviour to give up). Compare stills afterwards.
- **VU0 macro mode** helped less on the map (a vertex is one point times a
  matrix, a small part of its cost) than on the skinning: the FPU build was
  within 10%.
- A triangle that must be clipped costs about 2,000 cycles even with a lean
  vertex path; keep the guard band wide (the GS allows 2048 pixels from its
  origin, gsKit puts that at the screen's corner: 1400 outside the screen
  fits) so only triangles that cross the near plane and the very large ones
  are clipped (about 8% of a view's triangles).
- `mips` is a predefined macro on the EE's GCC: a variable of that name does
  not compile.
- A wrong guard band does not fail loudly. At 1700 pixels the right edge
  (640 + 1700) passes 2048 and the packed coordinates wrap: huge triangles
  and stretched textures, with the vertex data itself correct. Print a few
  vertices' view position, screen position and packed XYZ2 before suspecting
  the packets.

## How to measure

- **Phases, not guesses.** Read the EE's cycle counter (`cpu_ticks()` from
  `<timer.h>`) at the end of each phase of the frame, accumulate, and print
  averages every 60 frames together with the vsyncs the frames spanned. The
  vsync count gives the real frame time and, divided into the ticks,
  calibrates the counter. The sample's `-DPROFILE` build is the pattern.
- **Ablate to break a phase down.** A build that cycles a `static int`
  through "stop after stage k" every 60 frames gives, in one run, what each
  stage of a per-vertex pipeline costs. The sample's lighting was spread
  over its stages rather than in one hot spot: the trims that got the last
  milliseconds (one light sum instead of two, no clamps where the range is
  known, the quarter-tone from the full tone) came from this.
- **Count the slow calls in the object**, not the source:
  `objdump -d x.elf`, then tally `jal` targets per function. Inlining and
  constant promotion put `powf` and `__adddf3` where the source does not
  show them.
- **A micro-benchmark needs a `volatile` sink.** Writes to a static array
  that is never read are deleted, and the loop with them, giving zero
  cycles. Time long loops against `GetTimerSystemTime()` (bus clock, half
  the EE's) rather than trusting a single short `cpu_ticks()` interval.
- **Check the output did not change**: build the old and new program with
  `-DDEBUG_SECONDS=1.45f`, capture each PCSX2 window with
  `screencapture -l <window id>`, crop the title bar, and compare pixel by
  pixel. The sample's optimisation moved no pixel by more than 31 of 255 and
  0.2% of them by more than 8 (edges on the rifle, whose matrix is composed
  in a different order).

## Caveats

- PCSX2 charges cycles per recompiled block, roughly by instruction count,
  and models neither the EE's caches nor its pipeline. Branchy code and
  memory traffic cost less there than on hardware; floating-point latency
  is also hidden. Treat PCSX2 times as the lower bound, and verify the
  ratios on a console at each gate.
- PCSX2 does not time the GS at all: the sample's "wait for the GS" phase
  reads zero. On hardware the per-primitive CLUT reloads that gsKit's
  single-primitive functions cause would have cost GS time as well.
- The GS runs a frame behind the EE (gsKit sends the list, then the EE
  starts the next frame), so EE time and GS time overlap; the frame takes
  the longer of the two.
- gsKit picks PAL (50 Hz) when the BIOS is European. The sample sets
  `gs->Mode = GS_MODE_NTSC` so a vsync is 16.7 ms whatever the BIOS.
