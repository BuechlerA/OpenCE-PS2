# The sample's video memory, the GS's fog, and the Chief on VU1

> **The sample this describes is not in the tree.** The Master Chief / Blood Gulch sample (`port/ps2/samples/masterchief`) was removed from `main` when the repository was cleaned up for the game's port. Its code, tools and README are in the tag `pre-ps2-cleanup` (`git worktree add ../sample pre-ps2-cleanup`). What follows is research: measurements and pitfalls that apply to the game's renderer.

What it took to take the Master Chief sample's Blood Gulch scene from 3.39 to 2.43 MB of the GS's 4 MB
of video memory, to give it the map's fog, and to move the Chief's two textured passes off the EE onto
VU1. October 2026. Everything here was measured in PCSX2 only, not on hardware; PCSX2 does not time the
GS, so the EE numbers are the EE's own cycle counter.

The sample is `port/ps2/samples/masterchief`; its README says how to build and run it. The earlier
documents this builds on: [blood_gulch_map.md](blood_gulch_map.md) (the data and the design) and
[ee_performance.md](ee_performance.md) (what the EE does slowly, how to profile).

## Result

| | Before (e90fbcec) | After |
| --- | --- | --- |
| Video memory used | 3,385,088 of 4,194,304 bytes | **2,434,816** (free: 809,216 -> **1,759,488**, 928 KB gained) |
| Frame buffers | two 640x448 16-bit + 16-bit Z: 1,720,320 | two 512x448 + Z: 1,376,256 |
| Textures | 1,664,768 | 1,058,560 |
| EE busy, mean over the 4 s orbit | 11.40 ms (worst frame 15.50) | **10.09 ms** (worst frame 14.48), with fog |
| Fog | none | GS fog from the sky tag, +0.5 ms |
| The Chief's textured passes | on the EE (3.5 ms) | VU1 (1.5 ms left: the specular colours) |
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
- The audit counts every vertex that reaches the GS, including those in the 1,400-pixel guard band, so it
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

**Left on the EE, and the next steps:** the specular colours (a normalise, a cube-map lookup and n.h^18 for a
quarter of the vertices a frame: 1.5 ms) could go to VU0 in macro mode (`vrsqrt`, the products) or to VU1
with a sphere-map lookup; the map's 8 ms (culling, VU0 vertices, 64-byte records, four passes) is the
real cost and would want VU1 with a vertex cache and clipping; the double-buffered input leaves room for
more triangles a batch.

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
```

Switches: `NO_FOG`, `FOG_EXACT_PASS`, `FOG_HAZE_GAIN=0.5f`, `SCREEN_WIDTH`, `NO_CHIEF_VU1` (make variable),
`MIP_FLOOR=0` (exporter), `DETAIL_SIZE`, `RIFLE_TEX_SIZE`, `MASK_TEX_SIZE`, `DISPLAY_TEX_SIZE`,
`BASE_TEX_SIZE` (exporters). Compare stills pixel by pixel after cropping the window's content area: a
change that is meant to move nothing should move less than 0.01/255 on average.
