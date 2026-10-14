# The sample's video memory, the GS's fog, and the Chief on VU1

What it took to take the Master Chief sample's Blood Gulch scene from 3.39 to 2.43 MB of the GS's 4 MB
of video memory, to give it the map's fog, and to move the Chief's two textured passes off the EE onto
VU1. October 2026. Everything here was measured in PCSX2 only, not on hardware; PCSX2 does not time the
GS, so the EE numbers are the EE's own cycle counter.

The sample is `port/ps2/samples/masterchief`; its README says how to build and run it. The earlier
documents this builds on: [ps2_blood_gulch_map.md](ps2_blood_gulch_map.md) (the data and the design) and
[ps2_ee_performance.md](ps2_ee_performance.md) (what the EE does slowly, how to profile).

## Result

| | Before (e90fbcec) | After |
| --- | --- | --- |
| Video memory used | 3,385,088 of 4,194,304 bytes | **2,434,816** (free: 809,216 -> **1,759,488**, 928 KB gained) |
| Frame buffers | two 640x448 16-bit + 16-bit Z: 1,720,320 | two 512x448 + Z: 1,376,256 |
| Textures | 1,664,768 | 1,058,560 |
| EE busy, mean over the 4 s orbit | 11.40 ms (worst frame 15.50) | **9.42 ms** (worst frame 13.81), with fog |
| Fog | none | GS fog from the sky tag, +0.5 ms |
| The Chief's textured passes | on the EE (3.5 ms) | VU1; their specular colours on VU0 (0.8 ms left, the cube lookup and the scatter) |
| Picture | | the same, but fog; and 512 wide is a little soft across (below) |

How each step moved video memory:

| Step | Used | Saved |
| --- | --- | --- |
| e90fbcec | 3,385,088 | |
| Chief set: ammo display 32x32, rifle and its mask 128x128, Chief's mask 128x128 | 3,222,272 | 162,816 |
| Mip levels the orbit never reads, left out (`tools/mip_floor.json`) | 2,742,016 | 480,256 |
| 512-wide frame buffers (the audit then keeps a few more levels: +36,864) | 2,434,816 | 307,200 |

"EE busy" is the frame's phases without the vsync wait (`-DPROFILE`), over two orbits stepped by frame
number so that every build sees the same views (see the pitfalls: stepping by the clock moved the
camera whenever a frame dropped).

## Where the video memory goes (measured)

At e90fbcec: buffers 1,720,320 (41%); the map's 26 base textures 991 KB with their mips (24%; the nine
256x256 maps and the 256x512 cliff are 87% of that, and the top mip level is 75% of each chain); the
Chief's five textures 262 KB (6%: base 64, rifle 64, ammo display 64, two 4-bit masks 32 each); the
10 detail and bump maps 218 KB; the 15 lightmap pages 131 KB; palettes and allocator alignment about
63 KB. The sum of the pixel data, reconciled with gsKit's allocations: within 2%.

The Chief's textures were never the big spender, which is where the first idea (shrink the Chief's
specular maps and move the specular to the GS) went wrong: the masks are 4-bit and 32 KB each, and the GS
has no shaders to compute a specular term with. What the GS does is the mask's modulation and the
additive blend; the cube map's colour and the highlight stay a per-vertex job (now the one big EE cost
the Chief has left).

## 1. Texture sizes, from what the camera sees

The camera orbits the Chief at 2.2 units with a 27.5 degree half field of view (640x448). Projecting every
triangle through that camera over a whole orbit gives the texture side each mesh wants (pixels a UV
unit, by area, median / 90th percentile):

| | Screen | Wants | Had | Now |
| --- | --- | --- | --- | --- |
| Chief's armour | 5,200 px^2, **about 125 px tall** (the "250 px" in an earlier commit message was a Retina screenshot at 2x) | 118 / 173 | 256 | 256 |
| Rifle | 510 px^2 | 37 / 52 (worst case 80) | 256 | 128 |
| Ammo display | about 3 px^2 | 5 to 8 | 256 (64 KB) | 32 (1 KB) |

The rifle's mask goes with its base map (the second pass shares the UV registers, so its size must be
the rifle's: `main.c` has an `#error` for it). The Chief's mask is smooth, so it is 128 against his
256 base map and has UV registers of its own (`chief_mask_uv`). The cut-down maps are box filtered from the
512 sources (the old 256 base maps were point sampled and are left as they were). The Chief's base map
at 128 would be right for the median pixel and soft for 40% of his area; not done.

Caveat: these sizes are for this camera. A first-person weapon is large on screen and wants its own sizes.

## 2. The mip audit

The GS picks a mip level from the depth of the pixel alone: `level = log2 (z) + K / 16`. A texture whose
lowest level over the orbit is N never reads the N levels above it, so they can be left out of video
memory and the picture does not change. The audit build (`make EXTRA_CFLAGS=-DMIP_AUDIT`) sweeps the
orbit in half-degree steps and reports, per texture and pass, the lowest level any drawn vertex asks
for; `tools/mip_audit.py` turns that into `tools/mip_floor.json` and `export_map.py` leaves the levels out.

- **Bit-identical:** stills at three angles are equal to the full-chain build to 0.000/255.
- **A fixed point:** the audit run on the trimmed build finds nothing more to drop and no pass asks for a
  level below the one that is left.
- It saves 469 KB at 640 wide and 433 KB at 512 (the guard band reaches different vertices). The ground,
  the cliff and every texture near the camera keep their chains; what loses levels are the metal of the
  two bases and a few scenery maps, which the orbit never gets near.
- The audit counts every vertex that reaches the GS, including those in the guard band (1,400 pixels when the audit was
  run, 64 now: the table is conservative for the narrower band, and the levels it keeps are a superset), so it
  errs toward keeping a level.
- **Sample-specific.** It describes one camera path. A free camera or a Chief somewhere else needs the audit
  run again or `MIP_FLOOR=0` (every level). The real game streams its textures and will not have a table.
- Changing the exporter's sizes or the screen width changes K: regenerate the table from scratch (delete
  the file, export, audit, tool, export).

Detail maps at 64x64 instead of 128 were tried and rejected: the ground loses its grain (mean difference
7/255 over the frame, the picture visibly smoother). `DETAIL_SIZE=64 python3 tools/export_map.py` does it,
for 98 KB. Not tried: 4-bit detail maps and lightmaps (about 110 and 65 KB), which gsKit and the exporter
would need small changes for.

## 3. 512-wide frame buffers

gsKit sets the display's horizontal magnification from the buffer's width (`MagH = DW / Width - 1`, 4 for
512 on NTSC), so the same picture is 640 pixels wide on the TV and each buffer pixel is 1.25 times as wide
as it is high. The projection therefore has two focal lengths (`focal` up, `focal_x = focal * 512 / 640`
across): the field of view and the aspect are unchanged (stills side by side: same composition, same
proportions), and the cost is a slight horizontal softening, most visible on the Chief at 2x zoom. A frame
buffer is 573,440 bytes at 640x448 and 458,752 at 512; two of them and the Z buffer save 344,064 bytes.

`SCREEN_WIDTH` (default 512 for the map, 640 for `NO_MAP`) sets it; `make EXTRA_CFLAGS=-DSCREEN_WIDTH=640`
restores the old picture (and the 2,778,880 bytes). Texture detail (the LOD's K) still uses the vertical
focal length, so textures are as sharp as before and alias a little across.

Half-height buffers (224 lines, field style) would save 860 KB more and halve the vertical resolution;
not done.

## 4. Fog from the GS

Blood Gulch's sky tag has atmospheric fog (colour (1.0, 0.90, 0.77), maximum density 0.4, from 3 to 100
units): the Xbox computes it per vertex from the view depth (`rasterizer_xbox.c`, the fog constants:
`density * clamp ((z - start) / (opaque - start))`) and blends toward the colour in every pass. The GS does
exactly that in hardware: a coefficient F per vertex (the top byte of XYZF2, bits 56 to 63, free because
the depth is 16 bits), interpolated, and `F * colour + (1 - F) * FOGCOL` for a pass with the fog flag on.
`export_map.py` reads the sky tag; every map pass reads its vertices as XYZF2 (24-bit Z + F) and ignores F
unless the pass turns the fog on.

The catch is the multipass design: fog must come after what multiplies the picture (the detail maps, the
lightmap, whose brightness runs up to 2x). Three ways, measured on the same orbit:

| Fog | EE busy (mean) | Worst frame | Picture |
| --- | --- | --- | --- |
| none | 11.40 | 15.50 | |
| **in the base pass** (the default) | 11.89 (+0.49) | 16.28 | the haze is multiplied by the lightmap: too bright in sun, darker in shade; `FOG_HAZE_GAIN` 0.5 scales FOGCOL to the average and lands within 3/255 of the exact one on average |
| one more pass over the map's triangles, last (`-DFOG_EXACT_PASS`) | 13.50 (+2.1) | 18.68 | exact: dest * F + (1 - F) * FOGCOL, as the Xbox; it drops frames |

The exact pass works as a black vertex with alpha F through the fog unit (which gives (1 - F) * FOGCOL) and
the blend `Cd * As + Cs`. The reflection, added after the base pass, is scaled by F at its vertices in every mode.
`-DNO_FOG` leaves the fog out.

A cheaper exact version was tried and does not work in PCSX2: the lightmap pass already multiplies the
frame buffer by an alpha, so with vertex alpha F and the fog unit it could carry the fog too (`Cd * (L * F)
+ Cs`, one pass fewer). In PCSX2 the destination came out at exactly half with a non-black FOGCOL and
right with a black one, at its default blending accuracy; a copy of the configuration with the accuracy
raised gave the same picture (it may not have been read). Whether the hardware or the emulator is at fault
is not known; the sample does not depend on that blend.

## 5. The Chief on VU1

`make` (the map scene) draws the Chief's armour and rifle, each its base map and then its specular mask
added over it, with a VU1 microprogram (`src/chief_vu1.vsm`); `make NO_CHIEF_VU1=1` is the EE path.
What the EE did for them (projecting 3,000 vertices, culling, building four REGLIST packets) took 3.5 ms;
now it works out the specular colours of a quarter of the vertices (1.5 ms: the bulk of it) and hands VU1
a matrix.

**Data.** His animation is cached (every vertex's world position and diffuse colour for each of the 20
frames), so everything VU1 reads is static except the view matrix and those specular colours. The
triangles are listed out three vertices each, a frame's whole VIF1 DMA chain (batches of 12 triangles: an
UNPACK each of header, positions, UVs, and the two passes' colours as 8-bit data, then MSCAL) is built
once at start-up for each of the 20 frames (about 400 KB), and a frame sends a small packet with its
matrix that continues into its chain. The VU1 transforms with one matrix (view and projection, world to
pixel with the GS's origin and the 16-bit depth, one divide a vertex), culls the back faces by the sign of
the screen-space area, writes the visible triangles of both passes one after another, patches the GIF
tags' NLOOP (ADC bits cannot drop a triangle from a list without upsetting the GS's vertex count) and
XGKICKs once a pass. Memory map, registers and timing are in the file's header. VU1 does no clipping: the
Chief is always in front of the camera and on the screen here.

**Order.** VU1 draws through GIF path 1, past gsKit's queue (path 3), and the clear and the Z test have to
come out right. A frame: the queue's clear goes first (`gsKit_queue_exec`), the EE updates the specular
colours while it draws, waits for its FINISH, clears the flag and starts the VU1 chain, which ends with a
FINISH of its own; the EE builds the map meanwhile; the map's queue goes last (`gsKit_queue_exec` waits for
the previous FINISH, which is now VU1's), and the Z buffer sorts the two: nothing the map draws is in front of
the Chief and what it draws behind him fails the depth test. The visor and the ammo display (a few dozen
vertices) stay on the EE, drawn with the map.

**Checked:** with culling off the picture equals the EE path's to 0.007/255 (back faces are hidden anyway),
with culling on to 0.005 to 0.007/255 at three angles; the output buffers hold 12 to 15 vertices of a batch
of 36 (a third faces the camera); 60 fps in the profile. The EE's fallback and the other variants
(`NO_MAP`, 640 wide, no fog, the exact fog pass) still build and boot.

**Left on the EE, and the next steps:** the specular colours were 1.5 ms of it (a normalise, a cube-map lookup and
n.h^18 for a quarter of the vertices a frame): they are on VU0 now, next section; the map's 8 ms (culling, VU0 vertices, 64-byte records, four passes) is the
real cost and would want VU1 with a vertex cache and clipping; the double-buffered input leaves room for
more triangles a batch.

## 6. The specular colours on VU0

What the VU1 Chief left on the EE was the specular colour of a quarter of his and the rifle's vertices each frame
(1.5 ms, 580 EE cycles a vertex). Measured by stage: the vector maths (two normalises, the mirror direction, n.h^18)
197 cycles, the cube-map lookup 116, the gamma colour assembly 213. Now (`-DNO_VU0_SPEC` keeps the FPU's):

| Stage | Cycles a vertex | |
| --- | --- | --- |
| A, VU0: v, n.v, mirror direction, half vector, n.h^18 | 58 (was 197) | |
| cube lookup, EE | 117 | 73 KB of cube: it does not fit VU0's 4 KB |
| C, VU0: the colour from cube, fres, spec, the lit sum, the gamma curves | 49 (was 213) | |
| packing the colour and writing it to each of the vertex's slots, EE | 75 | scattered stores into a 22 KB array |

EE busy over the orbit: 10.09 -> **9.42 ms** (worst frame 14.48 -> 13.81); the pictures are identical (0.001/255);
the colour bytes agree with the FPU function to a mean of 0.05 levels, worst 1 of 128 (`-DSPEC_CHECK` prints it).

- **Macro mode needs three vertices at once.** VU0 in macro mode stalls the EE on every dependent instruction, so
  one vertex alone (45 operations and two reciprocal square roots in a chain) costs as much as the FPU did. The
  code works on three, instruction by instruction, with the divide unit's waits filled with the next lane's
  products; `tools/gen_spec_vu0.py` writes it (`src/spec_vu0.h`) with the three lanes' registers spelt out, and the
  constants in vf25 to vf28. The batches are 48 vertices: the A stage, the cube lookups and the C stage each run
  over a batch, so that their arrays stay in the 8 KB data cache.
- **The gamma curve has no table on VU0:** x^(1/2.2) is a cubic in sqrt (x), 0.21 of the 128 levels at worst over
  [0.03, 3] (the sample's arguments are 0.035 to 2.4); `vsqrt` is 7 cycles against `vrsqrt`'s 13. The static half,
  the curve of a quarter of the lit sum (which is exactly 0.25^(1/2.2) times the curve of the sum), is a per-vertex
  array made with the poses (`chief_ge`, `rifle_ge`: 950 KB), so each vertex takes three square roots, not six.
- **What is left, and why it stays:** the cube lookup (117 cycles) and the write of each colour to its slots (75).
  Both were probed before building anything, and neither pays enough:
  - *The cube lookup is arithmetic, not cache misses:* with every texel read forced onto one hot cache line it
    still costs 110 cycles (a divide, two float to integer conversions, the face choice's branches). Choosing the
    face on VU0 branch-free (`|r|`, a max, three selection masks, the divide, the texel offset) would leave the
    EE three byte loads (about 35 cycles) and add roughly 45 to 55 cycles of VU0 time that the EE waits for:
    about 0.07 ms, under 1% of the frame, for a longer generator. Not done.
  - *The scatter cannot become an index on VU1:* VU1 could read pass 2's colour by vertex from a small per-batch
    table, but a batch of 12 triangles (36 vertices) touches vertices from all over the mesh: the median batch
    spans 22 vertex numbers, the tenth worst 870, the worst 1,857 (the armour; the rifle's worst is 332), however
    the triangles are ordered. A table of the whole mesh does not fit VU1's 16 KB beside the rest.
  - *Doing the same scatter in a cache-friendly order does not help either:* refreshing the vertices in the order
    of their first slot, with their slot lists flattened in that order, gave the same pictures and 9.42 ->
    9.38 ms: the 52 cycles are the roughly 1,900 stores a frame (write-allocating into a 22 KB array) and a
    vertex's later slots are spread through the mesh whatever the order. Reverted.
  The next real saving for the Chief is not here: his EE share is now about 0.8 ms of the 9.4.

## 7. The guard band, and detail passes that have faded out

Measured in PCSX2 on the same profile as the rest of this file (EE busy, mean over the orbit); the GS numbers come
from the triangles a frame sends, since PCSX2 does not time the GS.

**The guard band was the biggest cost left.** `CLIP_MARGIN` (how far off the screen a triangle may reach before it is clipped)
was 1,400 pixels, and the chunk-visibility test uses the same planes, so the frame processed and sent everything within 1,400
pixels of the screen: 47 times the screen's area (3,312 by 3,248 pixels for a 512 by 448 screen).

| `CLIP_MARGIN` | EE busy | Chunks in view | Vertices processed | Triangles sent |
| --- | --- | --- | --- | --- |
| 1,400 (before) | 10.04 ms | 52 | 2,073 | 2,969 |
| 256 | 8.15 | 34 | 1,135 | 2,188 |
| 128 | 7.46 | 28 | 913 | 1,895 |
| **64 (now)** | **7.01** | 26 | 778 | 1,701 |

The triangles the clipper cuts did not change (about 154 a frame, 1.0 ms: skipping the clip gives 6.02 ms). The picture: stills at
0.6, 2.6 and 3.4 s differ from the 1,400 build by 0.13 to 0.28/255 on average, one step of the 16-bit buffer's dither on 0.3 to 0.7%
of the ground's pixels (the clipped triangles interpolate a little differently); the cliffs, sky and Chief are identical, and the
same build captured twice differs by exactly 0. On the GS, modelling the post-cull triangles (mean 5,269 px^2 against a median of 24)
gives about 47 Mpixel a frame rasterised at 1,400 pixels (all passes) against 1.4 Mpixel that land on the screen, and about 2 to 3 at 64
to 128. Whether the GS rejects off-screen pixels cheaply was not measured (no hardware): if it does not, this is the difference
between 60 fps on a console and not.

**Detail and bump passes where the map is flat.** A detail map's last mip level is its average, so where a pixel's level
(log2 z + K / 16) reaches it the pass only multiplies by a constant (the grey / 128: 0.93 to 1.81 for these maps). `draw_map` now
lays each span's triangles out nearest chunk first, leaves a detail or bump pass out for the chunks whose nearest depth is past
where it flattens (`flat_z`, from the same K the TEX1 setting uses, plus 0.15 of a level) and multiplies their vertex colours by the
constant instead (`flat_scale`, `scale_colour`: red and blue in one multiply, which needs the channels at 128 or under, so a span
with a brighter vertex, as scenery has, keeps its passes). Each pass draws a prefix of the span.

| | |
| --- | --- |
| Detail and bump vertices sent | 45% fewer (3,356 of 6,071 a frame) |
| Their pixels (`-DPROFILE_AREA`) | **17.9% fewer** (865 of 1,053 Kpx a frame); the per-pixel model said 28%, a chunk's bounding sphere is coarser |
| EE busy | 7.01 -> 7.20 ms (+0.19: sorting the chunks and scaling the colours; the pass packets are 0.13 ms cheaper) |
| Picture | 2.6 and 3.4 s: 0.012 and 0.000/255 from step 1; 0.6 s (the far cliff wall): 1.2/255, a 1% brightening (+1.0, +3.2, +1.8 of 255 in R, G, B) from not truncating in the 16-bit buffer once per pass; no seams |

With the VU0 specular colours of section 6 (measured before it, these steps are 10.02 -> 7.20 ms; it alone takes the Chief's EE share from 1.5 to
0.85 ms), the whole frame is **6.54 ms** of EE mean over the orbit, the worst window 8.50, 60 fps.

Not worth it, measured at 64 pixels (EE ms each pass costs: detail 0.31, lightmap 0.11, bump 0.05, reflection 0.01, fog 0.27, VU0
0.9 ms saved): the passes are cheap for the EE; what they cost is the GS's fill.

## 8. The cliff's base map at 128x256

The cliff (1,342 triangles) had the map's own 256x512 base map, 175 KB with its chain. It is 128x256 now (`CLIFF_SIZE`, in the
exporter: `source` restores the original), which takes the video memory from 2,434,816 to **2,303,744** bytes (free 1,759,488 ->
**1,890,560**, 131 KB). Compared with the 256x512 build at three camera angles: close up the pictures are nearly the same (mean difference
1.8 and 2.8/255, the cliff's fine detail energy 99 to 100% of the original: the grain comes from the detail passes, not the base
map); the base map's colour is a little softer, and the far cliff wall differs by 1.2/255. 256x256 (98 KB) was tried and rejected: only
the vertical axis is halved, so the level of detail the GS picks (from the geometric mean of the sides) lands half a level sharper
than the 256x512 map would, and the far wall comes out blotchier (6.7/255) than the smaller 128x256 map, which halves both axes and keeps
the texel to pixel ratio.

## Pitfalls (each cost real time)

- **PCSX2 needs an absolute path for `-elf`** (it logs "Failed to read ELF" for a relative one, boots to
  nothing and the window is black). `tools/ps2/pcsx2_run.py` resolves the path; direct runs must.
- **A DMA tag's two VIF codes are sent before its data**, so an UNPACK or MPG goes in the *second* and a
  NOP in the first; with the UNPACK first it eats the NOP as its first data word and everything after is
  misaligned (VIF1 wedges with its interrupt-stall bit set).
- **VIF opcodes:** BASE is 0x03 and OFFSET 0x02 (not 0x42 and 0x43); with the wrong ones nothing is set,
  the double buffer sits at 0 and its first UNPACK overwrites the matrix and constants at 0 to 7.
- **8-bit UNPACK data is read as words:** the last quadword of a batch with a number of vertices that is not
  a multiple of 4 has padding words, and the VIF reads them as commands; they must be 0 (a copied colour
  has bit 31 set: an interrupt, and a stall).
- **MAC flags on this VU1:** the sign of x is bit 7 (0x0080), the sign flags being bits 7 to 4; a guess of
  0x0800 culled everything. `fmand` has to wait for the `msuby` that sets the flag.
- **VU1 data memory is readable from the EE** (0x1100C000) once VU1 stops: dumping the matrix, the constants
  and the output buffers found the BASE and the NLOOP bugs at once.
- **`ibeq`/`ibne` have a delay slot**, and nothing between a flag-setting op and `fmand` may set flags.
- **The orbit stepped by the clock** (`vsync_count`) moves when a frame is late, so profile windows stop
  lining up; `-DPROFILE` now steps it by frame number.
- **Profile windows are not comparable across camera positions:** the same build costs 9.6 to 14.2 ms
  over the orbit. Compare the same windows.
- **gsKit's `gsKit_queue_exec` waits for the previous FINISH, clears the flag and starts the DMA, returning at
  once;** that is what lets the VU1 stream sit between the clear and the map.
- **A Retina capture is 2x:** the "250 px tall" Chief was 125. The captures are of the window only
  (`screencapture -l`), the margins vary with focus (find the content area by its size), and a capture can
  come out black: check and retry.
- **`gsKit_clear` clears the Z buffer too** (sprites at Z 0, tests off), so nothing can draw before it.
- gsKit's `GS_CSR_FINISH` macro is not parenthesised: `!GS_CSR_FINISH` is `(!x) & 1`.
- Pillow 13 removes `Image.fromarray(..., mode)` (2026-10-15): the exporter no longer passes a mode.

## Reproduce

```bash
# from port/ps2/samples/masterchief, with PS2DEV, PS2SDK and PATH set (dvp-as is in $PS2DEV/dvp/bin)
python3 tools/export_assets.py && python3 tools/export_map.py     # HALO_MAP=... if assets/ is not here
make                                                              # the VU1 Chief; NO_CHIEF_VU1=1 for the EE path
# a still, to compare builds (the camera angles 0.6, 2.6, 3.4 s show him well)
make EXTRA_CFLAGS="-DDEBUG_SECONDS=2.6f"
# the profile, over a fixed orbit, and the video memory line
make EXTRA_CFLAGS=-DPROFILE && python3 ../../../../tools/ps2/pcsx2_run.py masterchief.elf
# the mip audit (full chains first, then the table, then the trimmed build)
rm tools/mip_floor.json && python3 tools/export_map.py
make EXTRA_CFLAGS=-DMIP_AUDIT
python3 ../../../../tools/ps2/pcsx2_run.py masterchief.elf --until "MIPAUDIT done" > audit.log
python3 tools/mip_audit.py audit.log && python3 tools/export_map.py
# the specular code on VU0 (src/spec_vu0.h is checked in); make EXTRA_CFLAGS=-DSPEC_CHECK compares it with the FPU's
python3 tools/gen_spec_vu0.py > src/spec_vu0.h
```

Switches: `NO_VU0_SPEC`, `SPEC_CHECK`, `NO_FOG`, `FOG_EXACT_PASS`, `FOG_HAZE_GAIN=0.5f`, `SCREEN_WIDTH`, `NO_CHIEF_VU1` (make variable),
`CLIP_MARGIN` (default 64), `PROFILE_AREA` (with `PROFILE`: the pixels the detail passes draw), `MIP_FLOOR=0` (exporter), `CLIFF_SIZE`, `DETAIL_SIZE`, `RIFLE_TEX_SIZE`, `MASK_TEX_SIZE`, `DISPLAY_TEX_SIZE`,
`BASE_TEX_SIZE` (exporters). Compare stills pixel by pixel after cropping the window's content area: a
change that is meant to move nothing should move less than 0.01/255 on average.
