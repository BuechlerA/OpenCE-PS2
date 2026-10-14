# Blood Gulch on the PS2: what the map's data is, how it is drawn, and what it cost

Findings from putting Blood Gulch's level (the structure BSP, its shaders, its
lightmaps, the scenery and the sky) on the PS2 in the Master Chief sample
(`port/ps2/samples/masterchief`), October 2026. The Chief runs in place on the
terrain, the camera orbits him, and the scene holds 60 fps in PCSX2. Everything
here was checked in PCSX2 only, not on hardware, and PCSX2 does not time the GS.

How to build and run it is in the
[sample's README](../port/ps2/samples/masterchief/README.md); the EE costs
behind the speed-ups are in [ps2_ee_performance.md](ps2_ee_performance.md). This
document is about the data and the design.

## Result

| | |
| --- | --- |
| Scene | The Chief (2,192-vertex armour, the assault rifle) on Blood Gulch's terrain, a camera circling him every 4 seconds; sky, scenery, shadows from the lightmap, detail, bump and reflection |
| Geometry in the frame | 10,765 triangles and 11,498 vertices of map (5,503 and 5,790 of them the BSP, 5,202 triangles of scenery, 60 of sky), 26 base textures, 53 spans, 152 chunks |
| Frame rate | 60 fps (one NTSC vsync) through the whole orbit |
| EE time (PCSX2) | 8.9 to 13.3 ms of the 16.7 ms frame, by where the camera points; the worst single frame 15.2 ms (since then, with the Chief on VU1 and the fog: 7.8 to 12.4 ms, 10.1 on average, the worst frame 14.5; see [ps2_vram_and_vu1.md](ps2_vram_and_vu1.md)) |
| Video memory | 3.39 of 4.19 MB, about 790 KB spare (since then 2.43 MB, 1.76 MB spare: [ps2_vram_and_vu1.md](ps2_vram_and_vu1.md)) |
| Left out | The vehicles, the sky's clouds, the lights' glow, shadows of objects (see "Not done"); the fog came later |

## What the map is made of

All of it was read from the retail Xbox `bloodgulch.map` with the sample's tools
(`bsp.py`, `export_map.py`). The tag offsets are in those files' comments.

### The structure BSP

- One BSP of 1.3 MB: 5,503 triangles, 5,790 vertices, 79 materials in 16
  lightmap groups (15 with a lightmap page, one, of 44 materials, without),
  30 clusters. The terrain is one bowl of 2,425 triangles of ground and 1,342 of
  cliff; the two round bases (metal, up to 8 units high) are in the BSP; the
  buildings on top of them and the rocks and plants are not (see "Scenery").
- Coordinates are world units (1 = 10 feet); the Chief model is in the same
  units (0.7 tall), so nothing is scaled. The map spans x 6 to 132, y -190 to
  -45, z 0 to 26.
- 17 shaders: 10 `senv` (shader_environment: 5,371 triangles) and 7 `sotr`
  (transparent: lights and the teleporter's shield, 132 triangles). Each `senv`
  has a base map, usually a primary and a micro detail map, sometimes a bump map
  and a reflection cube map.
- A vertex is 32 bytes (position, three packed 11-11-10 vectors, texture
  coordinates), and a lightmap vertex 8 bytes (packed radiosity, lightmap U and V
  as signed 16 bits in [-1, 1]); a material's triangles index its own vertices.
  The surfaces' winding is the opposite of "counter-clockwise from outside"; the
  exporter turns each triangle against the vertex normals as the Chief's does.

### Bitmaps

Formats (the enum in `source/bitmaps/bitmaps.c`; 4, 5, 7, 12 and 13 are unused):
0 A8, 1 Y8, 2 AY8, 3 A8Y8, 6 R5G6B5, 8 A1R5G5B5, 9 A4R4G4B4, 10 X8R8G8B8, 11
A8R8G8B8 (the sky's gradient), 14 DXT1, 15 DXT3, 16 DXT5, 17 P8 bump.

- The compressed formats are not swizzled; the uncompressed ones (the lightmaps,
  the sky) are, in the Xbox's order: x and y bits interleaved from the low bit,
  an axis that has run out skipped (`rasterizer_swizzle.c`, `compute_swizzle_masks`).
- A **lightmap page** is an RGB565 picture of 64x32 to 256x256 pixels holding
  charts; the texels no chart uses are a pure red and yellow checker
  ((31, 0, 0) and (31, 63, 0) in 5-6-5). A bilinear sample at a chart's edge
  reads them, so they must be filled from the used texels before the page is
  used as a texture. The 15 pages are 126,016 pixels in all, with a mean colour
  of about (0.85, 0.62, 0.36): the sun is warm.
- A **bump map** (format 17) is not a height map: each byte indexes the engine's
  table of 256 normals, `global_vector_palette` in `source/bitmaps/bitmaps.c`
  (0xFFRRGGBB, the vector as RGB x 2 - 1).
- A DXT3 or DXT5 base map of an `senv` shader carries something other than a
  cut-out in its alpha (the metals); a `soso` (shader_model) one is the foliage's
  cut-out.

### The scenery

The scenario's `scenery` block has 50 instances of 15 models (a position and
three Euler angles each): 6 kinds of rock, shrubs, ferns, broadleaf plants, trees,
the teleporter and flag bases, beacons, floor arrows. Eight vehicles (two
warthogs, four ghosts, two scorpions) are in the `vehicles` block. The models
are small (20 to 800 triangles each, one geometry for most). A model's vertices
are in model space, texture coordinates are 16-bit and multiplied by the model's
own base-map scale (0x30 in the model: 1 for most, 256 by 119 for the desert
trees) and by the shader's; without the scale the trees' repeats a unit come out
near zero, with it they are 0.4 to 2.3, which is sensible.

### The sky

The scenario's sky tag points at a model of five geometries: a half-dome (60
triangles, 37 vertices, 27,700 units across), two horizon quads, a ring (the
glare, 86 triangles) and clouds (33 triangles, a cut-out). The dome's shader is
a transparent "chicago" shader with three maps (stars, their twinkle, and a 256x256
gradient, `sky clear blue`, A8R8G8B8).

## How the scene is built

### The exporter (`tools/export_map.py`)

Reads the map, writes `src/map_assets.[ch]` (gitignored: they are the game's data).

- **Vertices and triangles** of the BSP, the scenery and the sky merged into one
  array, wound counter-clockwise from outside (the sky's from inside), sorted
  into **spans** (a base map, then a lightmap page) and, within a span, into
  **batches** (one per chunk).
- **Textures:** DXT decoded and quantised to 256 colours (8 bits, median cut, the
  palette in the GS's CSM1 order), with mip levels quantised against the first
  level's palette. Sizes: the ground 256x256 (512 at first), the cliff 256x512,
  other BSP maps 256 at most, scenery 64 (128 for the biggest rock). A cut-out
  (foliage) takes 255 colours and index 255 for what is transparent, whose palette
  entry has alpha 0. The detail map's average is folded into a scenery base map
  (a detail map multiplies by twice its grey).
- **Lighting:** the lightmap's *hue* (colour divided by its brightest channel)
  is the vertex colour; the brightest channel is the lightmap page's texture
  (an 8-bit grey, 64 pixels wide at least for the GS). Scenery takes its colour
  from the lightmap at the nearest vertex of the map and a sun term on its normal.
- **Detail maps:** 8-bit greys at 128 pixels with mips; the last mip level is
  the map's average, so far away a detail map fades to nothing instead of tiling
  visibly (a 60-times-repeated 128-pixel map made a visible crosshatch).
- **Bump:** the table of normals against a light from the upper left in the
  texture's space, 0.5 (n . L) / L.z stretched by 2.4 about 1, as a grey with mips.
- **Reflection:** the shader's cube map as 6 faces of 16x16 RGB bytes, and a
  signed-byte normal for each vertex.
- **Mip level of detail:** the GS picks a level from Q alone (log2 (1 / Q) + K),
  so K is worked out for each pass from the exporter's measure of the texture
  repeats a world unit of surface (the median over a map's triangles, by area)
  and the texture's size and scale.
- **Chunks:** a 12-unit grid over the terrain's triangle centres, each chunk with
  a bounding sphere (93 on the BSP); each piece of scenery is a chunk of its own
  with a distance beyond which it is not drawn (30 radii + 20 units, between 35
  and 120); the sky's chunk is always in view.
- **A plane for each triangle** (normal and offset), so a back-facing triangle
  can be dropped by the EE before its vertices are touched.
- The ground under the Chief by a ray down through the triangles.

### The renderer (`draw_map` in `src/main.c`)

- **A frame:** chunks in view (sphere against the five clip planes, and the
  distance limit) -> their triangles span by span -> a triangle that faces away
  (plane test in the world) is dropped; the others' vertices are transformed and
  projected on VU0 the first time they are needed (a stamp per vertex) -> a
  triangle that is wholly outside a plane is dropped, one that crosses the near
  plane or the guard band is clipped in view space, the rest goes on ->
  the records are kept per span -> the passes.
- **Projection:** Q = 1 / z per vertex; S = u Q, T = v Q for each texture, so every
  texture is perspective-correct and may tile beyond the 1024 texels UV allows.
  Z is `k1 / z - k2` (affine in screen space, as the GS interpolates it); linear
  Z, which the Chief-only scene had, bends across a large triangle.
- **Passes:** the base map times the lightmap's hue; the detail maps and the bump
  map multiplied in (the destination times the texture's alpha / 128, the alpha
  from a palette whose entry i has alpha i, so a grey d multiplies by 2 d); the
  lightmap's brightness the same way; then the reflection added as a flat colour.
  A cut-out is an alpha test (alpha >= 0x40) on the base pass.
- **The sky** is the first span, its vertices transformed by the view without its
  translation, so it surrounds the camera at any distance.
- **The Chief** (map mode) has his 20 animation frames skinned and lit once at
  start-up (3.3 MB), and a frame only projects them; the camera-dependent
  specular colour is worked out for a quarter of the vertices a frame.

## The budgets

### Video memory

The numbers below are as of e90fbcec (3.39 MB). Where the memory went afterwards (the Chief's small textures, the mip
audit, 512-wide buffers: 2.43 MB) and why the Chief's specular maps were never the big spender is in
[ps2_vram_and_vu1.md](ps2_vram_and_vu1.md).

| | KB |
| --- | --- |
| Two 640x448 16-bit frame buffers and a 16-bit Z buffer | 1,680 |
| The map's 26 base textures with their mips (pixel data) | 968 |
| 10 detail and emboss maps with mips | 212 |
| 15 lightmap pages | 127 |
| The Chief's five textures (256x256: two 8-bit base maps, two 4-bit masks, the display), all the palettes and the allocator's alignment (the rest) | about 320 |
| Allocated in all (`VRAM: 3385088 bytes` at start-up) | 3,306 of 4,096 KB, about 790 spare |

Findings:

- With 32-bit frame buffers the Chief alone is 3.79 MB. The map needs 16-bit
  buffers (1.2 MB less), which the GS dithers.
- gsKit sizes its buffers for the console's own mode (PAL: 512 lines) before the
  program forces NTSC, so the sample's buffers were 640x512 and the 32-bit build
  overflowed 4 MB with only a console message ("Not enough VRAM"): the failed
  allocation is address 0, the frame buffer, and the texture still draws from
  it. Set `gs->Height` with `gs->Mode`; the sample now stops when a texture
  does not fit.
- The Chief's textures were 960 KB at 512x512: at 256 (he is about 250
  pixels tall in this scene) with 4-bit specular masks they are 320 KB and look
  the same. The masks must be the size of the base maps: the second pass
  reuses the first's UV registers, which are in texels (the rifle's 256 mask
  under 512 UVs tiled twice before).
- To fit the map: the ground's base map from 512 to 256 (colour from one
  texture across 145 units of terrain is low-frequency; the detail map supplies
  the grain), scenery at 64 pixels, details at 128.

### EE time (PCSX2), and what moved it

| Step | EE ms (worst view) | What was done |
| --- | --- | --- |
| Chief alone, perf branch | 13.6 | (from the earlier work) |
| Phase 0: map as flat colour, 5.5 K triangles | 21.6 | the map 7.9 ms, FPU transforms |
| Phase 1: textured, vertex lightmap | 22.5 | |
| Phase 3: detail, lightmap, mips | 24.2 | the map 10.6 ms |
| Chief's poses cached, chunks, lazy VU0 vertices | 15 | Chief 12 -> 4 ms; map vertices a third of the work |
| `-O3 -ffast-math -funroll-loops` | 13.4 | 1.7 ms in the heaviest view |
| Scenery added (+2.7 K triangles at first) | 17.7 | frames dropped |
| Back-face planes, VU0 projection, no beacons, distance limits | 12.5 | |
| Bump and reflection, then compact passes and a plane mask for the clipper | 13.3 (worst frame 15.2) | |

What mattered, in order of what it saved: caching what does not change (the
Chief's poses); not touching what is not seen (chunks, back-facing planes,
distance limits, lazy vertices); one divide a vertex; the compiler flags; sending
each pass only its registers. VU0 helped a whole vertex (200 to 70 instructions)
but little for the transform alone. The longer list is in
[ps2_ee_performance.md](ps2_ee_performance.md).

## Pitfalls met (each cost real time)

- **The guard band.** GS coordinates are 12.4 fixed point in 16 bits around an
  origin gsKit puts at the screen's corner, so a vertex further than 2048 pixels
  wraps. A 1,700-pixel margin put the right edge (640 + 1,700) past it: huge
  triangles and stretched textures, with the vertex data itself correct. The
  margin is 1,400. When the picture is wrong and the vertices look right, check the
  packed XYZ2.
- **A clipped vertex lies on the guard band's edge**, give or take rounding: a
  projection that leaves out-of-band vertices unfilled leaves garbage records for
  half the clipped triangles. Fill them (clamped).
- **`mips` is a predefined macro** on the EE's GCC.
- **A matrix in VU0's vf24-vf27 read back as garbage** (the sky's transform);
  unexplained, so the sky's 37 vertices use the FPU and VU0 keeps vf1-vf23.
- **NOP in a REGLIST** and **UV as a dummy** both draw correctly in PCSX2; the sample
  uses UV (ignored when drawing with ST).
- **A GIF REGLIST packet must be exactly its length:** over-allocating a packet's
  quadwords makes the DMA read the rest as the next GIF tag.
- **The profiler's own `printf`** every 60 frames cost a vsync and showed up as
  the next frame's busy time, and per-vertex `cpu_ticks()` cost more than the
  work timed. The profile reports the worst frame; a window averaging 14 ms
  dropped frames.
- **numpy on this Mac** prints spurious "divide by zero in matmul" warnings
  (Accelerate); `einsum` avoids them.
- **The exporter fails silently if it asserts** when its output is hidden: a
  stale `map_assets.c` was built and looked like a different bug.
  (`CHIEF_BELOW` defaults to 6 so the ground under a hill is found.)

## Where this differs from the plan

[ps2_port_plan.md](ps2_port_plan.md) sketched the environment as diffuse x
lightmap in three channel-masked passes, with detail maps as an extra pass and
bump and reflection left out. What the sample does instead, and why:

- The lightmap is a **luminance page plus per-vertex hue**, one pass instead of
  three (the sun's tint varies slowly; the shadows' sharp edges are in the
  brightness).
- **Bump is emulated** as a fixed-light relief texture, not dropped: it is one
  more multiply pass.
- **Reflection is per vertex on the EE** and added in one flat pass, as the plan
  suggested for models.
- Textures are **resident**, not streamed: the map's whole set fits once the ground
  is cut and scenery is small.

## Not done, and open questions

- **Vehicles** (8 of them: many large textures; no video memory for them).
- **The sky's clouds** (33 triangles, an alpha mask times a colour map: about 20
  KB of video memory and an alpha-blended span) and the ring and horizon quads.
- **The lights' glow and objects' shadows.** (Fog is done: [ps2_vram_and_vu1.md](ps2_vram_and_vu1.md).)
- **The lightmap's overbright (x 2) is an assumption.** The Xbox's pixel
  shaders were not read; shadowed ground comes out dark. `LM_SCALE` scales the
  pages if it is wrong.
- **The bump relief is fixed in the texture's space** and so does not move with
  the light as it would on the Xbox.
- **The GS's load is unmeasured.** PCSX2 does not time it; each pixel of the map
  takes up to five blended passes, and on a console the packets (about 2 MB a
  frame in the heaviest view) and the fill rate need checking.
- **Hardware.** Everything here is PCSX2's cycle accounting; it undercounts
  memory traffic, and the EE's headroom (worst frame 15.2 of 16.7 ms) is smaller
  than it looks.
- **Scenery's lighting is a guess:** the lightmap at the nearest vertex, which
  can put a bright rock in a shadow's edge. Euler angles are applied as
  yaw, pitch, roll about z, y, x; a leaning tree's sign may be wrong.
- **The first spot for the Chief** (in front of a base, 46.9 -89.0) shows the
  bases; the default (68.5 -97.5) shows the scenery.

## Reproduce

```bash
# in port/ps2/samples/masterchief, with PS2DEV and PS2SDK set (see the README)
HALO_MAP=/path/to/bloodgulch.map python tools/export_assets.py   # the Chief
HALO_MAP=/path/to/bloodgulch.map python tools/export_map.py      # the map
make
PCSX2 -nogui -batch -elf masterchief.elf
# a still: make EXTRA_CFLAGS="-DDEBUG_SECONDS=2.0f"; the frame budget:
# make EXTRA_CFLAGS=-DPROFILE; tools/ps2/pcsx2_run.py masterchief.elf
```

`NO_DETAIL_PASS`, `NO_LIGHTMAP_PASS`, `NO_BUMP_PASS`, `NO_REFLECTION_PASS` leave a
pass out; `NO_MAP=1` builds the Chief on the old grid; `NO_VU0` keeps the
transforms on the FPU. Compare stills pixel by pixel after any change to the
drawing (crop the window's title bar; the sample's changes moved nothing by more
than 8 of 255 except where a feature was added on purpose).
