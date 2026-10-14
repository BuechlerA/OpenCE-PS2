# Master Chief on the PS2 (sample)

A stand-alone sample, not the game's PS2 build (`tools/ps2_build.py`, `port/ps2/src`). It needs the Xbox `bloodgulch.map` (see the main
README for the game data) and bakes the Chief and the assault rifle out of it; no game data is
committed here (`src/assets.c`, `src/assets.h` and the ELF are generated and ignored).

The rotation scene (Chief running with the assault rifle, camera orbiting a ground grid) as a PS2
program: EE code skins, lights and projects every vertex; the GS draws Gouraud textured triangles
(8-bit palettised textures, 16-bit Z). Built with ps2dev and gsKit.

    python3 tools/export_assets.py     # run from here; reads HALO_MAP (default assets/maps/bloodgulch.map) -> src/assets.[ch]
    make                               # PS2DEV, PS2SDK set; writes masterchief.elf
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
