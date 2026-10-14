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
    PCSX2 -nogui -batch -elf masterchief.elf

`export_assets.py` needs Python 3 with numpy and Pillow. It bakes the Chief's colour change into his
base map, quantises the two base maps to 256 colours, and precomputes the 20 frames of skinning
matrices (with the left-hand adjustment) so the PS2 only blends two bones per vertex.

The armour's reflection and highlight are a second pass: each vertex gets the armour cube map's colour
(times 0.5 + fresnel) and the key light's n.h^18 on the EE, and the GS adds that over the lit armour
through the specular mask (multipurpose red) as an 8-bit texture, so only the masked panels shine.
The rifle gets the same pass with its own specular mask (256x256, to spare video memory) and its two
reflection cube maps (grip, gun). The visor's gold reflection and highlight are computed per vertex
on the EE.

The rifle's ammo display (its own 256x256 map, the shader's blue self-illumination baked in) is drawn
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

## The map (Blood Gulch port: base maps, detail maps, lightmap pages, mip maps)

`export_map.py` reads the map's structure BSP (`bsp.py`; 5,503 triangles and 5,790 vertices in 79
materials: the terrain, the cliffs and the two round metal bases; scenery is not in the BSP) and bakes:

- positions, texture coordinates (they tile, over several units) and the triangles, wound
  counter-clockwise from outside and sorted into spans by base map, then lightmap page (36);
- the ten base maps the environment shaders use, as 8-bit palettised textures with mip levels (DXT
  decoded, cut to 256 pixels on a side unless 1,000 or more triangles use the map: the ground stays
  512x512), 1.1 MB;
- each shader's primary and micro detail maps as 8-bit grey textures, 128 pixels a side with mips, the last
  level their average (a detail map tiles visibly far away, so it fades out); 7 of them, 148 KB;
- the lightmap pages (RGB565, swizzled; their unused texels are a red and yellow checker, filled in from the
  used ones) as 8-bit grey textures of the brightest channel, 127 KB, and at each vertex the lightmap's
  hue (colour / brightest channel). The few vertices of the other shaders (lights, the teleporter shield)
  get a flat colour;
- the texture repeats a world unit of surface, per base map, for the mip level;
- the ground under the Chief: he stands on open ground in front of a base (`CHIEF_X`, `CHIEF_Y`).

On the EE (`draw_map`) a frame first tests each of the exporter's 93 chunks (a bounding sphere against the five
planes below), then takes the triangles of the chunks in view, span by span. A triangle's vertices are
transformed on VU0, projected and given their registers (one divide) the first time a visible triangle needs
them, once a frame. A triangle with a vertex behind the near plane or more than 1400 pixels off the screen
(the GS's coordinates are 2048 pixels either side of its origin, and gsKit puts that at the screen's corner:
more wraps) is clipped in view space before projection: the camera stands inside the map. Each span's
vertices are kept as 48-byte records (RGBAQ, the ST of the base map, the lightmap and each detail map, XYZ2),
and each pass is those records sent whole, as one REGLIST packet with six registers a vertex of which the
pass uses RGBAQ, XYZ2 and one ST; the others go to UV, which perspective-correct drawing ignores. The textures
are perspective-correct (Q = 1 / z) and may tile beyond the 1024 texels UV allows. The passes:

1. the base map times the lightmap's hue (the GS multiplies the texel by colour / 128);
2. the primary and micro detail maps: the destination times the texture's alpha / 128 (`ALPHA(1, 2, 0, 2, 0)`),
   the alpha taken from a palette whose entry i has alpha i, so a detail map's grey d multiplies by 2 d (the
   shader's "double biased multiply"); the detail's ST is the base map's times the shader's scale;
3. after all spans, the lightmap pages the same way (the brightness x 2, clamped to the page).

The GS picks a mip level from Q alone (log2 (1 / Q) + K), so K is worked out for each pass from the exporter's
texture repeats a world unit, the texture's size and its scale. The Z value is `k1 / z - k2`, so it is affine
in screen space as the GS interpolates it (the linear Z the Chief-only scene used bends across a large
triangle). `-DNO_DETAIL_PASS` and `-DNO_LIGHTMAP_PASS` leave a pass out, to compare.

- The frame buffers are 16 bits with dithering (`FB16`, the default with the map; `make NO_MAP=1` keeps
  32 bits): each pass quantises to 5 bits a channel again, and the dither keeps the result smooth.
  Video memory: 4.14 MB of 4.19 MB in use with the Chief's and the map's textures.
- `gs->Height` is set to 448 with the NTSC mode: gsKit sized the buffers for the console's own mode
  before that (512 lines on a PAL BIOS), which put the 32-bit build over 4 MB without an error.
- In PCSX2 (`-DPROFILE`): 60 fps through the whole 4-second orbit; the EE is busy 8.6 to 13.4 ms of the
  16.7 ms frame, by where the camera points (the map 5 to 8 ms, the Chief 4 ms). PCSX2 does not time the GS.
  Checked only in PCSX2. What got it there (from 24 ms; see also `docs/ps2_ee_performance.md`):
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

Not yet: the sky, the shader's bump and reflection, the lights' glow, scenery.
