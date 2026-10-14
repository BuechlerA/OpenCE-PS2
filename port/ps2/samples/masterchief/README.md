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

## The map (phase 0 of the Blood Gulch port: geometry only)

`export_map.py` reads the map's structure BSP (`bsp.py`; 5,503 triangles and 5,790 vertices in 79
materials: the terrain, the cliffs and the two round metal bases) and bakes positions, a colour per
vertex (a tint for the kind of shader, lit by the packed normal: no textures yet) and the triangles,
wound counter-clockwise from outside. It also finds the ground under where the Chief stands (open
ground in front of a base; `CHIEF_X`, `CHIEF_Y` in the environment move him). The base buildings and
other scenery are not in the BSP.

On the EE (`draw_map`), every vertex is transformed (FPU, not VU0 yet) and a triangle with a vertex behind
the near plane or more than 1000 pixels off the screen is clipped in view space before projection: the
camera stands inside the map, and the GS's coordinates reach only about 1700 pixels around the screen. The
Z value is `k1 / z - k2`, so it is affine in screen space as the GS interpolates it (the linear Z the
Chief-only scene used bends across a large triangle).

- The frame buffers are 16 bits with dithering (`FB16`, the default with the map; `make NO_MAP=1` keeps
  32 bits). Video memory in use after the Chief's textures: 2.64 MB of 4 MB, so 1.5 MB is left for the
  map's textures. With 32-bit buffers it is 3.79 MB.
- `gs->Height` is set to 448 with the NTSC mode: gsKit sized the buffers for the console's own mode
  before that (512 lines on a PAL BIOS), which put the 32-bit build over 4 MB without an error.
- In PCSX2: 30 fps, the EE at about 21.6 of 33.3 ms: the Chief 13.6 ms, the map 7.9 ms (`-DPROFILE`).
  Checked only in PCSX2.
