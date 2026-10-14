# Master Chief on the PS2 (sample)

A stand-alone sample, not the game's PS2 build (`tools/ps2_build.py`, `port/ps2/src`). It needs the Xbox `bloodgulch.map` (see the main
README for the game data) and bakes the Chief and the assault rifle out of it; no game data is
committed here (`src/assets.c`, `src/assets.h` and the ELF are generated and ignored).

The scene (Chief running in place with the assault rifle on Blood Gulch's terrain, the camera orbiting
him) as a PS2 program: EE code skins, lights and projects every vertex; the GS draws Gouraud textured triangles
(8-bit palettised textures, 16-bit Z). Built with ps2dev and gsKit.

    python3 tools/export_assets.py     # run from here; reads HALO_MAP (default assets/maps/bloodgulch.map) -> src/assets.[ch]
    python3 tools/export_map.py        # the map's structure BSP -> src/map_assets.[ch]
    make                               # PS2DEV, PS2SDK set; writes masterchief.elf
    make NO_MAP=1                      # the Chief alone, on the old ground grid (needs no map_assets)
    make NO_CHIEF_VU1=1                # the Chief drawn by the EE, not VU1 (the map scene's default)
    PCSX2 -nogui -batch -elf masterchief.elf

`export_assets.py` needs Python 3 with numpy and Pillow. It bakes the Chief's colour change into his
base map, quantises the two base maps to 256 colours, and precomputes the 20 frames of skinning
matrices (with the left-hand adjustment) so the PS2 only blends two bones per vertex.

The armour's reflection and highlight are a second pass: each vertex gets the armour cube map's colour
(times 0.5 + fresnel) and the key light's n.h^18 on the EE, and the GS adds that over the lit armour
through the specular mask (multipurpose red) as a 4-bit grey texture (16 levels, error diffused: it
looks the same as 8-bit and takes half the video memory), so only the masked panels shine. The rifle
gets the same pass with its own specular mask and its two reflection cube maps (grip, gun). A mask
is the size of the base map it is drawn with, because the pass reuses the base map's UV registers
(texels), except the Chief's, which is smaller than his base map and has its own UV registers. The Chief's base
map is exported at 256x256, half its source: he is about 125 pixels tall in the map scene
(`BASE_TEX_SIZE=512 python3 tools/export_assets.py` keeps it full size, for 192 KB more); the rifle's base map and
both masks are 128x128 (box filtered; it covers about 500 pixels). The visor's gold reflection and highlight
are computed per vertex on the EE.

The rifle's ammo display (its own 32x32 map, the shader's blue self-illumination baked in; it covers about 3 pixels) is drawn
unlit, nudged toward the camera so the 16-bit Z buffer doesn't hide it in the receiver.

Left out: the rifle's HUD panels (compass, numbers, data: transparent shaders), mip maps, and the soft
shadow under the feet. Checked only in PCSX2. For a still frame, to compare builds, add
`EXTRA_CFLAGS="-DDEBUG_SECONDS=1.45f"` (and `-DNO_SPECULAR_PASS` to leave the armour pass out).

## Performance

The scene runs at 60 fps in PCSX2 (NTSC, one vsync per frame; the EE's work takes about 14 of the
16.7 ms). `make EXTRA_CFLAGS=-DPROFILE` prints, every 60 frames, how long each phase of the frame
took (the EE's cycle counter, scaled by the vsyncs the frames spanned) and how many triangles the
GS got; `tools/ps2/pcsx2_run.py masterchief.elf` shows the output. The GS is never the limit: it
finishes each frame's list long before the EE finishes the next one.

What the EE does slowly, and what the per-vertex path does instead:

- `powf` costs about 540 cycles, and the first version called it 32,000 times a frame (the gamma
  encoding of every light sum and highlight). The gamma curve is a 2048-entry table with linear
  interpolation (exact to the 8 bits the GS gets), n.h^18 and n.h^60 are repeated squarings, and a
  quarter-tone's encoding is 0.25^(1/2.2) times the full tone's.
- Double arithmetic is software (the EE's FPU is single precision): the exporter writes the model's
  constants with an `f` suffix.
- The transforms (two bones, the blend, the normal, its normalisation, the view) run on VU0 in macro
  mode, from vertices packed as 16-byte quads at start-up; `-DNO_VU0` keeps them on the FPU, to compare.
  Vertices weighted to one bone (most of them) skip the second bone and the normalisation.
- The triangles go to the GS as one REGLIST packet per mesh (three 64-bit registers per vertex, PRIM
  and the texture's TEX0 and TEX1 set once by A+D) instead of one gsKit call per triangle, which
  wrote TEX0 (with a CLUT reload), TEX1 and PRIM for each. The PRIM is not carried in the tag:
  PCSX2 ignores PRE on a REGLIST tag.

The findings behind the map (its data, the design, the budgets, the pitfalls and what is not done) are in
[docs/ps2_blood_gulch_map.md](../../../../docs/ps2_blood_gulch_map.md).

## The map (Blood Gulch port: base maps, detail maps, lightmap pages, mip maps)

`export_map.py` reads the map's structure BSP (`bsp.py`; 5,503 triangles and 5,790 vertices in 79
materials: the terrain, the cliffs and the two round metal bases) and bakes:

- positions, texture coordinates (they tile, over several units) and the triangles, wound
  counter-clockwise from outside and sorted into spans by base map, then lightmap page;
- the base maps the environment shaders use, as 8-bit palettised textures with mip levels (DXT
  decoded, cut to 256 pixels on a side unless 1,000 or more triangles use the map; the ground is
  256x256, the cliff 128x256 (`CLIFF_SIZE=source` keeps the map's 256x512));
- each shader's primary and micro detail maps as 8-bit grey textures, 128 pixels a side with mips, the last
  level their average (a detail map tiles visibly far away, so it fades out); 7 of them, 148 KB;
- the lightmap pages (RGB565, swizzled; their unused texels are a red and yellow checker, filled in from the
  used ones) as 8-bit grey textures of the brightest channel, 127 KB, and at each vertex the lightmap's
  hue (colour / brightest channel). The few vertices of the other shaders (lights, the teleporter shield)
  get a flat colour;
- **the scenery**: the rocks, shrubs, ferns, plants, trees, teleporter and flag bases of the scenario's scenery
  block (50 instances of 15 models, the vehicles and the game's markers left out), at their lowest level of detail,
  placed in the world and lit from the lightmap at the nearest vertex of the map (times a sun term on the
  normal), so they are more map triangles: 5,200 of them. Their base maps are cut to 64 pixels (128 for the
  biggest rock), the average of the detail map folded into the colour; foliage is cut out by a palette entry of
  alpha 0 and an alpha test, drawn from both sides; each piece is a chunk of its own, drawn only up to a distance
  that follows its size;
- **the sky**: the sky model's dome (60 triangles) with its gradient as a 64x64 texture, drawn first, its vertices
  transformed by the view without its translation, so that it surrounds the camera at any distance;
- **bump**: each environment shader's bump map (8 bits indexing the engine's table of normals, read from
  `source/bitmaps/bitmaps.c`) as a grey "emboss" texture, 128 pixels a side with mips: the normal against a light
  from the upper left in the texture's own space (`EMBOSS_GAIN`), 3 maps for the cliffs and the metal. The GS has no
  per-pixel lighting, so the relief is fixed in the texture's space, not in the world's;
- **reflection**: for the shaders with a reflection cube map (the metal: 3 cube maps, 16x16 a face) a normal for each
  vertex, and the strength head-on and at a graze;
- the texture repeats a world unit of surface, per base map, for the mip level;
- a plane for each triangle, so that the EE can drop the ones facing away before transforming their vertices;
- the ground under the Chief: he stands on open ground among the shrubs and boulders of the valley's west side
  (`CHIEF_X`, `CHIEF_Y`, `CHIEF_BELOW` in the environment move him; the first choice was in front of a base:
  46.9, -89.0, 1).

On the EE (`draw_map`) a frame first tests each of the exporter's chunks (a bounding sphere against the five
planes below, and the distance a piece of scenery is drawn to), then takes the triangles of the chunks in view, span by
span. A triangle facing away from the camera (a plane test in the world) is dropped without its vertices being
touched. The others' vertices are transformed and projected on VU0 (one divide, then the screen position, the depth
and the four texture-coordinate pairs in one block) the first time a visible triangle needs them, once a frame. A
triangle with a vertex behind the near plane or more than 64 pixels off the screen (the GS's coordinates are 2048
pixels either side of its origin, and gsKit puts that at the screen's corner: more wraps) is clipped in view space
before projection: the camera stands inside the map. Each span's vertices are kept as 64-byte records (the ST of the
base map, the lightmap, each detail map and the bump map, the reflection's colour, RGBAQ, XYZ2, in four quadwords). The textures are perspective-correct (Q = 1 / z) and may tile
beyond the 1024 texels UV allows. The passes:

1. the base map times the lightmap's hue (the GS multiplies the texel by colour / 128);
2. the primary and micro detail maps: the destination times the texture's alpha / 128 (`ALPHA(1, 2, 0, 2, 0)`),
   the alpha taken from a palette whose entry i has alpha i, so a detail map's grey d multiplies by 2 d (the
   shader's "double biased multiply"); the detail's ST is the base map's times the shader's scale;
3. after all spans, the lightmap pages the same way (the brightness x 2, clamped to the page);
4. the bump maps multiply like the detail maps (the emboss grey x 2, the bump map's scale on the base map's ST),
   after them, so they act on the picture before the lightmap;
5. last, what the shiny surfaces reflect, added as a flat colour a vertex: the cube map in the direction the view
   mirrors about the vertex's normal, weighted by 1 - n.v squared between the head-on and the graze strength.
   The colour follows the camera, so a vertex has it worked out again every 4 frames (a degree and a half a frame).

Each pass sends only what it reads of the 64-byte record (its ST, RGBAQ and XYZ2: 24 bytes a vertex), so a pass is a
quarter of the record's bytes on the GIF. `-DNO_DETAIL_PASS`, `-DNO_LIGHTMAP_PASS`, `-DNO_BUMP_PASS` and
`-DNO_REFLECTION_PASS` leave a pass out, to compare.

The GS picks a mip level from Q alone (log2 (1 / Q) + K), so K is worked out for each pass from the exporter's
texture repeats a world unit, the texture's size and its scale. The Z value is `k1 / z - k2`, so it is affine
in screen space as the GS interpolates it (the linear Z the Chief-only scene used bends across a large
triangle).

- The frame buffers are 16 bits with dithering (`FB16`, the default with the map; `make NO_MAP=1` keeps
  32 bits): each pass quantises to 5 bits a channel again, and the dither keeps the result smooth.
  Video memory: 2.30 MB of 4.19 MB in use (1.89 MB spare) with the Chief's and the map's textures (the program
  prints the budget at start-up, `VRAM: ...`, and stops if a texture did not fit: gsKit's failed allocation is
  address 0, the frame buffer, and the texture would draw from it). How it got there from 3.39 MB: the Chief's
  small textures, the mip levels the orbit never reads (`tools/mip_floor.json`), 512-wide buffers; see
  [docs/ps2_vram_and_vu1.md](../../../../docs/ps2_vram_and_vu1.md).
- `gs->Height` is set to 448 with the NTSC mode: gsKit sized the buffers for the console's own mode
  before that (512 lines on a PAL BIOS), which put the 32-bit build over 4 MB without an error.
- In PCSX2 (`-DPROFILE`, which steps the orbit by frame so that builds see the same views): 60 fps through the whole
  4-second orbit; the EE is busy 7.1 to 11.7 ms of the 16.7 ms frame by where the camera points (9.4 ms on average,
  its worst frame 13.8 ms) with the Chief on VU1, his specular colours on VU0 and the fog on (the EE's path: 9.6 to
  14.2, mean 11.9). PCSX2 does
  not time the GS. Checked only in PCSX2. What got it there (from 24 ms; see also `docs/ps2_ee_performance.md`):
  - The Chief's 20 animation frames are skinned and lit once at the start (his pose only changes 30 times a
    second and only the camera moves: `cached_pose`); a frame projects the cached positions, and gives each
    vertex its view-dependent specular colour once in 3 frames (the visor's, 69 vertices, every frame).
  - The map's chunks, lazy vertices and VU0 transform above, one divide for a vertex, and the records
    built once for all the passes.
  - `-O3 -ffast-math -funroll-loops` (the Makefile's): 1.7 ms in the heaviest view; stills match the `-O2`
    build to within 0.02 of 255 on average.
- The lightmap's brightness is the shader's overbright (x 2) as far as can be told without the Xbox's
  pixel shaders; shadowed ground comes out dark. `LM_SCALE` in the exporter's environment scales the
  pages' brightness (default 1).

## Fog, 512-wide buffers, the mip audit and VU1

All in [docs/ps2_vram_and_vu1.md](../../../../docs/ps2_vram_and_vu1.md), with the measurements:

- **Fog** from the sky tag's atmospheric fog, by the GS's own fog unit: a coefficient per vertex (the top byte of XYZF2),
  blended toward FOGCOL in the base pass (+0.5 ms; `FOG_HAZE_GAIN` scales the haze down for the lightmap's multiply).
  `-DFOG_EXACT_PASS` does it as the Xbox does, in one more pass (+2.1 ms); `-DNO_FOG` leaves it out.
- **512x448 frame buffers** (`SCREEN_WIDTH`; the display stretches them to the same picture, so there are two focal
  lengths): 344 KB less video memory, a little soft across. `-DSCREEN_WIDTH=640` restores the old picture.
- **The mip audit** (`-DMIP_AUDIT`, `tools/mip_audit.py`, `tools/mip_floor.json`): the mip levels the camera's orbit
  never reads are left out of video memory, 433 KB, and the picture is identical. It describes the orbit's one path;
  `MIP_FLOOR=0 python3 tools/export_map.py` keeps every level.
- **The Chief on VU1** (`src/chief_vu1.vsm`, the map scene's default; needs `dvp-as` from the ps2dev toolchain on PATH):
  his armour and rifle, each two textured passes, are transformed, culled and drawn by a VU1 microprogram from a
  prebuilt VIF1 chain per animation frame; the EE keeps the visor and the ammo display, and the cube-map lookups of the
  specular colours, whose other work (the mirror direction, n.h^18, the gamma curves) runs on VU0 in macro mode,
  three vertices at a time (`tools/gen_spec_vu0.py` writes `src/spec_vu0.h`; `-DNO_VU0_SPEC` keeps the FPU's). The
  picture equals the EE path's to 0.007/255; the EE saves 1.8 ms a frame with VU1 and 0.7 with VU0.

Not yet: the lights' glow, the sky's clouds, the vehicles.
