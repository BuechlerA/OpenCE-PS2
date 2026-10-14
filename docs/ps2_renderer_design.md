# The PS2 renderer: design of the skeleton

Status: design. Nothing in this document is implemented. October 2026.

This is the design for `source/rasterizer/ps2/`, the game's own renderer on
the PlayStation 2, which the plan ([ps2_port_plan.md](ps2_port_plan.md),
"Renderer" and "Phase 3") calls for. It settles where the renderer plugs into
the game, the frame structure the port owns, the VU1 programs, the video
memory, and what to measure on a console before the design is committed to.
It ends with the files of the first milestone, what carries over from the
Master Chief sample, and the first three implementation steps.

Everything the sample measured was measured in PCSX2 only, never on hardware,
and PCSX2 does not time the GS. Each number below that comes from the sample is
marked **(PCSX2)**. Numbers about the game's own code (counts of functions,
sizes of structures, capacities) are read from the source.

What the sample is, and is not: `port/ps2/samples/masterchief` draws the Chief
on Blood Gulch at 60 fps in PCSX2
([ps2_blood_gulch_map.md](ps2_blood_gulch_map.md),
[ps2_vram_and_vu1.md](ps2_vram_and_vu1.md)). It is built on precomputed data:
the Chief's poses cached for 20 frames (3.3 MB), his VIF chains built once at
start-up for each frame of animation, a map baked by a Python exporter, every
texture resident, no clipping and no lighting on VU1, and the map transformed
on the EE through gsKit's per-frame packet heap (6 MB). None of that scales to
a campaign level in 32 MB; what does carry over is listed in "What carries over
from the sample".

## 1. Where the renderer plugs in

### The two options

**A. Replace `source/rasterizer/xbox` behind `rasterizer.h`.** The game's
render code (`render.c`, `structure_render.c`, `models.c`, the interface) calls
the `rasterizer_*` functions of `rasterizer.h`. Those live in
`source/rasterizer/rasterizer.c`, which is hardware-independent dispatch and
game-side logic (lightmap modes, debug options, frame statistics), and each one
forwards to an underscored backend function (`_rasterizer_window_begin`,
`_rasterizer_environment_lightmap_draw`, ...). The Xbox folder defines 128 of
those underscored functions and about 90 helpers the game-side files call
directly (`rasterizer_bitmap_new`, `rasterizer_vertex_buffer_new`,
`rasterizer_text_begin`, `rasterizer_text_draw_character`,
`rasterizer_set_texture_bitmap_data`, `rasterizer_set_framebuffer_blend_function`,
the `rasterizer_draw_*` family, the profile, water, active camouflage). The PS2
renderer defines the same names and the build leaves the Xbox folder out.

**B. Keep the Xbox rasterizer on a PS2 Direct3D.** `ps2_d3d_null.c` grows
drawing: the Xbox rasterizer's stream of render states, texture stage states,
vertex shader constants, NV2A pixel shader definitions
(`rasterizer_set_pixel_shader`, a register combiner program per draw) and
`DrawIndexedVertices` calls is translated to the GS.

**Recommendation: A.** The reasons, in order of weight:

1. The Xbox rasterizer's output is a program for the NV2A's register
   combiners, chosen per draw from the shader's flags (the lightmap pass alone
   has a dozen combiner setups in `rasterizer_xbox_environment.c`), on
   vertices that the NV2A's vertex shaders skin, light, fog and project. The GS
   has none of that: it blends, modulates by one texture and a vertex colour,
   tests depth and fogs. A Direct3D for the PS2 would be the translation layer
   the plan already rejected (`d3d8_gl.c` and `nv2a_vsh.c` cannot target the
   GS), with the translation done per draw at run time on the EE.
2. The Xbox rasterizer draws the BSP from index buffers the CPU builds every
   frame (`structure_render_build_dynamic_triangles`) and reads the vertex
   data in the Xbox's 40-byte format. The PS2's data is the converter's: VIF
   batches with 16-bit positions and 8-bit indices that the EE never touches.
   A Direct3D cannot be told to draw a cluster's batches; the `rasterizer.h`
   backend can, because it sees the material and the vertex buffer, not a
   triangle range.
3. The memory budget counts on it: "The Xbox rasterizer goes when the PS2
   renderer replaces it" (the plan's code line, 3.5 MB). The Xbox folder is
   32,000 lines; with option B it stays, plus the translator.
4. The sample has already written, in C and VU1 assembly, the pieces option A
   needs (the matrix, the packets, the VU1 chain, the fog, the LOD), and
   nothing option B needs.

Option B's one merit is that it draws something sooner (the first milestone
would be a translation of the few states menus use). That is a week, not a
reason.

### The mechanics

- `rasterizer.c` is not changed. It includes the Xbox headers
  (`rasterizer/xbox/rasterizer_xbox.h`, `_internal.h`, `_models.h`,
  `_dynavobgeom.h`) only for declarations, and `<xtl.h>` for types; it calls
  no Direct3D itself. The PS2 backend provides the same underscored functions;
  the Xbox headers keep declaring them.
- `port/ps2/port.json` gets a `game` exclude list: `source/rasterizer/xbox/*.c`
  and `source/cache/xbox_texture_cache.c`. `tools/ps2_build.py` compiles
  `source/rasterizer/ps2/*.c` with the game's flags (it is game code, in the
  game's tree, as the plan placed it) and adds a `dvp-as` rule for
  `port/ps2/vu1/*.vsm` (the sample's Makefile rule).
- The other users of Direct3D outside the Xbox folder are three:
  `xbox_texture_cache.c` (replaced, see section 4), `progress_bar.c` (the
  loading bar's two `IDirect3DTexture8` objects and render states: it keeps
  the null Direct3D's resource and state functions until the HUD milestone
  replaces it) and one `IDirect3DDevice8_PersistDisplay` call in
  `marketing_and_strategic_business_development.c` (stays a no-op).
- `ps2_d3d_null.c` keeps its resource, state and D3DX functions for those two
  files and loses its drawing side when nothing calls it. Its platform hooks
  (`halo_screen_width`, `halo_screen_commit`, `halo_shadow_map_scale`,
  `halo_screen_ui_offset`, `halo_screen_anti_alias`,
  `halo_vertex_shader_lighting`, the pointer and texel hooks) move to a
  `ps2_screen_hooks.c` with the renderer's answers (width 640, no anti-alias,
  no pointer).
- `tools/ps2/unimplemented_stubs.py` already writes a stub for every function
  the game calls and nothing defines, but its stubs stop the program. The
  renderer's unimplemented functions must be no-ops that count their calls, so
  the first file is a generated `rasterizer_ps2_stubs.c` (below), and each
  milestone deletes the stubs it implements.
- gsKit is not used by the game. Its queue and heap are the frame structure
  the port replaces (section 2), and its display setup sizes buffers for the
  console's own video mode before the program asks for one
  ([ps2_vram_and_vu1.md](ps2_vram_and_vu1.md), "Pitfalls"). The game links
  ps2sdk's `libdma`, `libgraph` and `libkernel`, and writes the GS's
  privileged registers (PMODE, SMODE2, DISPFB1/2, DISPLAY1/2, BGCOLOR) itself:
  about forty lines, with the values the sample measured. gsKit stays in the
  sample.

### The entry points by milestone

The 128 underscored backend functions and the helpers, grouped by the
milestone that implements them. Each milestone's list is complete for what the
game calls on the way to its goal; everything else stays a counting stub.

**Milestone 1: menus and text** (the main menu, the console, the loading
screen; `ui.map` only, 640x448, no 3D)

| Group | Functions |
| --- | --- |
| Frame and window | `_rasterizer_initialize`, `_rasterizer_dispose`, `_rasterizer_reset_state`, `_rasterizer_frame_begin`, `_rasterizer_frame_end`, `_rasterizer_windows_begin`, `_rasterizer_window_begin`, `_rasterizer_window_end`, `_rasterizer_windows_end`, `_rasterizer_present`, `_rasterizer_set_vblank_callback`, `_rasterizer_window_get_fog`, `_rasterizer_window_set_fog`, `rasterizer_set_frustum_z`, `rasterizer_get_target`, `rasterizer_set_target` (the primary target only) |
| Screen geometry | `_rasterizer_psuedo_dynamic_screen_quad_draw`, `_rasterizer_dynamic_screen_geometry_draw`, `_rasterizer_dynamic_screen_geometry_add_multitexture_params_to_base`, `_rasterizer_hud_begin`, `_rasterizer_hud_end`, `_rasterizer_screen_flash`, `rasterizer_set_framebuffer_blend_function` |
| Text | `rasterizer_text_begin`, `rasterizer_text_end`, `rasterizer_text_draw_character` |
| Bitmaps | `rasterizer_bitmap_new`, `rasterizer_bitmap_changed`, `rasterizer_bitmap_delete`, `rasterizer_get_bitmap_default_hardware_format`, `rasterizer_set_texture_bitmap_data`, `rasterizer_set_texture_direct`, `rasterizer_set_texture_non_blocking`, `rasterizer_set_texture_direct_non_blocking`, and the texture cache (`texture_cache.h`: `texture_cache_new`, `_open`, `_close`, `_delete`, `_idle`, `_flush`, `_bitmap_load`, `_bitmap_delete`, `_steal_memory`, `_return_memory`, `_texture_cache_bitmap_get_hardware_format`, `texture_cache_debug_render`) |
| Dynamic buffers | `_rasterizer_dynamic_vertices_new`, `_lock`, `_unlock`, `_delete`, `_get_type`, `_rasterizer_dynamic_triangles_new`, `_lock`, `_unlock`, `_delete` (CPU memory from the rasterizer memory pool; `render_sprite.c` and the widgets lock screen vertices through them) |
| No-ops that must not stop | `_rasterizer_profile_enable`, `rasterizer_profile_*`, `rasterizer_spin_begin`, `rasterizer_spin_end`, `rasterizer_secondary_render_target_debug`, the debug drawing family |

**Milestone 2: the BSP with lightmaps** (a multiplayer map from the
converter's batches: terrain, sky, fog as a vertex coefficient; no models)

| Group | Functions |
| --- | --- |
| Hardware geometry | `rasterizer_vertex_buffer_new`, `rasterizer_vertex_buffer_delete`, `rasterizer_triangle_buffer_new`, `rasterizer_triangle_buffer_delete` (register the converter's batch streams in place) |
| Environment passes | `_rasterizer_environment_lightmaps_begin`, `_rasterizer_environment_lightmap_begin`, `_rasterizer_environment_lightmap_draw`, `_rasterizer_environment_lightmap_end`, `_rasterizer_environment_lightmaps_end`, `_rasterizer_environment_diffuse_textures_begin`, `_rasterizer_environment_diffuse_texture_draw`, `_rasterizer_environment_diffuse_textures_end` |
| Passes that draw nothing yet, in order | specular lights and lightmaps, reflection lightmap masks, mirrors, reflections (`_rasterizer_environment_reflections_begin`/`_draw`/`_end` is the additive flat pass once the converter writes normals), transparent geometry, fog (`_rasterizer_environment_fog_begin`/`_draw`/`_end` becomes the exact fog pass in the fog milestone), fog screen, shadows, decals, detail objects, diffuse lights |
| Sky | the sky is drawn as a model (`render_sky.c` calls `rasterizer_models_begin(TRUE)` and the model path): milestone 2 draws it through the model path's one-node case, or leaves the clear colour; it comes free with milestone 3 |

**Milestone 3: models** (objects, the first-person weapon, vertex lighting,
change colour)

| Group | Functions |
| --- | --- |
| Models | `_rasterizer_models_begin`, `_rasterizer_models_end`, `_rasterizer_model_begin`, `_rasterizer_model_end`, `_rasterizer_model_draw`, `rasterizer_set_model_skinning`, `rasterizer_model_part_skinning`, `rasterizer_set_model_lighting`, `rasterizer_set_model_lighting_point_light`, `rasterizer_set_model_lighting_distant_light`, `rasterizer_model_ambient_reflection_tint`, `rasterizer_model_draw_environment_shader` (a model with a `shader_environment`: the environment program on a model's batches) |
| Later | `_rasterizer_model_transparent_geometry_submit` and `rasterizer_transparent_geometry_group_draw` (transparents milestone), `_rasterizer_dynamic_unlit_geometry_draw` and `_rasterizer_dynamic_lit_geometry_draw` (particles, effects), `rasterizer_environment_shadow_*` (shadows), `rasterizer_water_*`, `rasterizer_active_camouflage_*`, `_rasterizer_screen_effect`, `rasterizer_sun_glow_draw`, `_rasterizer_widget_*`, `_rasterizer_decals_*`, `_rasterizer_detail_objects_*`, `_rasterizer_hud_motion_sensor_blip_*` |

Two game-side files change later, each with a `port:` comment: on the PS2
`structure_render.c` need not build the dynamic triangle lists (the backend
ignores them and draws whole batches; the build costs CPU and 16-bit indices
for every visible surface each frame), and `rasterizer_lights.c` can drop the
256 KB of lens flare occlusion results (the plan's ray cast). Neither is needed
for milestone 2 to draw.

## 2. The frame structure

### The frame rate

The game simulates at 30 Hz and the plan targets 30 frames a second. The
renderer's budget is therefore one frame of 33.3 ms, in which the EE's tick
(15 ms on the bench maps, more on the campaign) and the EE's render pass share
the EE, while VU1 and the GS run beside it on the frame before. The sample's
60 fps is not the target; it is headroom.

### What the EE builds

During the game's render pass (between `_rasterizer_frame_begin` and
`_rasterizer_present`) the EE builds one **VIF1 DMA chain** for the frame and
one **GIF DMA chain** of texture uploads. It does not transform a vertex and it
does not copy geometry: the chain's REF tags point into the converter's data in
the tag cache (vertex batches, index lists) and into the texture cache's
main-memory pages (texture data in GS layout), and its CNT tags carry the small
packets the EE writes itself (matrices, per-material GS state, screen-space
sprites for text and the HUD). Both chains are double-buffered: frame N+1 is
built while frame N's chains are in flight, and nothing frame N references is
overwritten until its FINISH has been seen.

```
EE:   | tick N+1 | render pass N+1: build chains N+1 | wait FINISH N | kick N+1 | ...
DMA:  |  uploads N (GIF, path 3)  |  chain N (VIF1: path 2 clear, path 1 batches, path 2 HUD) |
GS:   |                           |  clear N  |  BSP N  |  models N  |  HUD N  | FINISH N
```

The VIF1 chain is a sequence of these elements, in this order:

1. **`FLUSHA`** (a VIF code): the first draw waits for the frame's texture
   uploads on path 3 to have ended. So a texture uploaded this frame is in
   video memory before anything draws with it, without the EE waiting.
2. **The clear**, by `DIRECT` (path 2): a GIF packet that sets FRAME, ZBUF,
   SCISSOR, XYOFFSET, TEST (Z always) and draws one sprite over the screen
   with the window's clear colour and the far depth, then TEST back to normal.
   The sample's finding holds: nothing may draw before the clear, and the
   clear must be the first thing the GS does for the frame.
3. **The windows** (one for a single player, up to four split screen): per
   window a SCISSOR and XYOFFSET packet (DIRECT), the view constants for VU1
   (an UNPACK into VU1's constant area: the world-to-screen matrix, the fog
   constants, the guard band), then the environment's batches (`MSCAL`
   kicks: path 1), then the models' (bones and lights by UNPACK, batches by
   `MSCAL`), then the transparents, then the window's 2D (HUD, text) by
   `DIRECT`.
4. **The frame's end**: a `FLUSH` (path 1 and path 2 quiet), a `DIRECT`
   packet with a FINISH, and an END tag.

Path 1 (VU1's XGKICK), path 2 (DIRECT from the VIF1 chain) and path 3 (the GIF
channel) meet at the GIF. Their ordering is the rule the sample learnt and
this design keeps: within the VIF1 chain, path 1 and path 2 are ordered by the
VIF (a `FLUSH` before a `DIRECT` that must follow VU1's draws, as the frame's
FINISH does); path 3 is ordered against the chain only by `FLUSHA` at its
start, so all of a frame's uploads go before any of its draws. A texture first
needed in the middle of frame N's render pass is still uploaded at the start of
frame N's DMA, because the chain is kicked only at `_rasterizer_present`, when
the whole frame is known.

The EE waits for frame N's FINISH before kicking frame N+1's uploads, so an
upload never overwrites a video memory slot a frame in flight still reads, and
the chain buffers of frame N are free again. The wait is at present time, after
the next frame's tick and render pass: the GS and VU1 have the whole previous
frame to finish, which is where the frame of latency comes from (the GS runs a
frame behind the EE, as in the sample). The vertical blank flips DISPFB to the
buffer frame N drew once its FINISH has been seen (`ps2_vblank.c` already
counts presented frames at the blank; it gains the flip).

Dynamic geometry the game writes each frame (particles, decals, screen
vertices, debug lines) is locked and unlocked through the `rasterizer.h`
dynamic buffer functions. Those buffers are CPU memory in the rasterizer memory
pool; the EE copies what a draw uses into the frame's CNT arena at draw time
(the pool is reused next frame, the arena is not until FINISH). For
milestone 1 all dynamic geometry is 2D and goes down path 2 as REGLIST sprites
and triangles the EE packs (the sample's `send_map_pass` shape: an A+D packet
with PRIM, TEX0, TEX1, CLAMP, ALPHA, TEST, then one REGLIST). Particles in 3D
get a small VU1 program in the effects milestone.

### The chain builder

`rasterizer_ps2_chain.c` owns two chain buffers and two CNT arenas and offers:

- `chain_ref(address, qwords, vif0, vif1)`: a REF tag (the sample's `vu_tag`),
  with the rule that a tag's two VIF codes go before its data, so an UNPACK or
  an MPG is the second code and the first a NOP.
- `chain_cnt(qwords)`: returns a pointer into the arena for a CNT packet the
  EE fills (the matrices, a material's state).
- `chain_direct(qwords)`: a CNT packet whose data is a GIF packet for path 2.
- `chain_mscal(address)`, `chain_flush()`, `chain_flusha()`, `chain_end()`.
- `chain_kick()` and `chain_wait_finish()`: `dma_channel_send_chain` on VIF1
  with `DMA_FLAG_TRANSFERTAG`, after `SyncDCache` of the arena, and the
  FINISH flag in CSR (clear it, wait, as the sample does around VU1's kick).
- The upload chain on the GIF channel: `upload_add(texture, levels)` appends
  a BITBLTBUF/TRXPOS/TRXREG/TRXDIR A+D packet and an IMAGE tag whose data is a
  REF into the texture's main-memory page; `upload_kick()`.

Every address a REF tag carries must be 16-byte aligned and in main memory
(not the scratchpad), and the VIF reads 8-bit UNPACK data by words, so the
converter pads index lists to a quadword with zeros.

### The main-memory budget

The sample's 6 MB packet heap and 3.3 MB pose cache do not exist here; they
were the EE transforming vertices and caching the results. The frame structure
needs:

| Item | Size | Why |
| --- | --- | --- |
| Two chain buffers (tags) | 2 x 128 KB | A batch is four tags (header, vertices, indices, MSCAL): 64 bytes. 1,000 batches a frame is 64 KB; the rest is per-material state and window packets. |
| Two CNT arenas | 2 x 384 KB | Per model: bones (up to 44 nodes x 3 quadwords), lights, matrix; per material: 5 registers a pass; the 2D REGLIST packets (text, HUD, menus: `RASTERIZER_MAXIMUM_DYNAMIC_SCREEN_VERTICES` is 16,384 vertices at 24 bytes = 384 KB at the limit, a few KB in play); particles when they come. Sized by a high-water mark the renderer logs. |
| Two upload chains | 2 x 8 KB | 8 quadwords per upload, 64 uploads a frame. |
| VU1 programs | 16 KB | Resident in main memory, loaded by MPG when the program changes. |
| Texture records | 64 KB | A `struct ps2_texture` per bitmap the map has (TEX0 fields, slot, levels). |
| **Total** | **about 1.1 MB** | The plan's "render packets" line is 1.5 MB. |

Where it sits in the 32 MB, with the Xbox rasterizer gone:

| | MB |
| --- | --- |
| EE kernel | 1.0 |
| Code and data (3.6 linked today with `--gc-sections`, less the Xbox rasterizer: measured in step 1) | about 3.4 |
| Game state | 3.3 |
| Tag cache (`HALO_PORT_TAG_CACHE_SIZE`, 17.2 today; 16 planned) | 16 to 17.2 |
| Texture cache, main memory pages (section 4) | 2 to 4 |
| Sound cache, stacks, heap | 2 |
| The frame structure (above) | 1.1 |
| **Total** | **28.8 to 32.0** |

The texture cache's main-memory size is the variable: 2 MB fits today's tag
cache, 4 MB needs the converter's tag reductions (Phase 2) to have landed. The
tag cache and the texture pages are the two things that must shrink together.

## 3. The VU1 program family

Two programs, both in `port/ps2/vu1/`, both driven by the same chain builder
and the same double-buffered input (VIF `BASE` and `OFFSET`, `TOPS` addressing,
as `chief_vu1.vsm` is). The EE never reads VU1's output; the GS does, by
XGKICK. Each program's memory map is a header generated from one table
(`tools/ps2/vu1_layout.py` writes `rasterizer_ps2_vu1_layout.h` and a `.inc`
for `dvp-as`), so the EE's UNPACK addresses and the program's agree by
construction; the sample kept the two by hand and spent a day on a mismatch.

VU1 runs at the EE's clock (294 MHz) and has 16 KB of data memory (1,024
quadwords). Both programs' maps fit that:

| Region | Environment program | Model program |
| --- | --- | --- |
| Constants (matrix, fog, guard band, screen offset, pass table) | 16 qw | 16 qw |
| Bones (44 nodes x 3 qw) and lights (ambient, 2 distant, 2 point) | | 132 + 8 qw |
| Current material's GS state (5 registers x 4 passes, as A+D packets) | 24 qw | 24 qw |
| Input, double-buffered (header 4, up to 40 vertices x 3 qw, indices for 48 triangles 9 qw) | 2 x 133 qw | 2 x 133 qw |
| Vertex cache (transformed, 40 vertices x 5 qw) | 200 qw | 160 qw |
| Output, double-buffered (144 listed vertices x 1.5 qw + tags) | 2 x 224 qw | 2 x 224 qw |
| **Total** | **954 qw** | **1,054 qw: the model program's output buffers are 190 qw (128 listed vertices, 40 triangles a batch)** |

The batch sizes (about 40 vertices and 48 triangles) are what the memory
allows with REGLIST output; the converter cuts each material's triangles into
batches of that size in strip order, so a vertex is shared by about two
triangles of its batch and transformed once. If the output has to be PACKED
(below), the triangle count halves.

### The environment program (`vu1_environment.vsm`)

Per batch, after `xtop`:

1. **Transform** each of the batch's unique vertices once into the vertex
   cache: position (16-bit, scaled by the batch header's scale and offset)
   times the world-to-screen matrix (the sample's `vu_matrix`: X/W and Y/W
   are pixels with the GS's origin added, Z/W the 16-bit depth, W the view
   depth), one divide (`div Q, VF00w, VFw`), `ftoi4` for XY, `ftoi0` for Z,
   the fog coefficient from W (`F = 255 (1 - density clamp((W - start) /
   (end - start)))`, four instructions with the constants prepared by the EE),
   S and T for each texture the material's passes read (base UV times Q, the
   lightmap UV times Q, the base UV times each detail scale times Q), the
   colour word copied through, and the clip flags.
2. **Clip flags.** `clipw.xyz` against the clip-space position tests x, y and
   z against plus and minus w; the projection is set so that plus and minus w
   are the guard band (the screen plus a margin; a vertex further than 2,048
   pixels from the GS's origin wraps, so the margin is under 1,400) and z the
   near and far planes. The flags are kept per vertex in the cache (one
   word). The band is a clipping parameter only: batches are culled against
   the screen's own frustum, not the band. The sample's lesson is that culling
   with the band's planes drew everything within it (its 1,400-pixel band
   processed 47 times the screen's area and is 64 now); the margin here is
   whatever keeps the slow path rare, which the gate's VU1 test sets, starting
   from 64.
3. **Per triangle** (the index list, 8-bit): AND of the three vertices' flags
   non-zero: skip (all outside one plane). OR zero: the fast path. Otherwise
   the slow path: the triangle is clipped against the planes it crosses in
   clip space (Sutherland-Hodgman, into a scratch polygon of at most nine
   vertices; the sample's EE clipper is the reference, including the rule
   that a near-plane cut needs every plane) and the pieces are projected like
   cached vertices. The sample clipped about 8% of a view's triangles at its
   1,400-pixel band and about 150 a frame at 64 **(PCSX2, the sample's
   counts)**.
4. **Back faces**: the sign of the screen-space area (the `msub` and the MAC
   flag's x-sign bit, 0x0080, as `chief_vu1.vsm` tests it; the GS has no
   culling). Two-sided materials (foliage) skip the test.
5. **Output, per pass in the material's pass mask**: for each visible
   triangle, the pass's three registers of each of its three vertices, read
   from the cache and stored into the output buffer; the pass's GIF tag's
   NLOOP patched with the count at the end (an ADC bit cannot drop a triangle
   from a list), the pass's state packet (from the material slot) kicked, then
   the draw kicked. Two output buffers alternate between passes: XGKICK stalls
   until the previous kick has been read, so the buffer a pass writes is
   always free.

The registers each pass needs, and the GS state the EE writes into the
material slot for it: this is how `shader_environment` is expressed on the GS.

| Pass | Registers per vertex | GS state (A+D) | Blend | What it draws |
| --- | --- | --- | --- | --- |
| 1 base | ST (base), RGBAQ (the lightmap's hue at the vertex, alpha 0x80, Q), XYZF2 | TEX0 base map (T8 or T4 CLUT), TEX1 with the pass's LOD K, CLAMP repeat, ALPHA off, TEST Z on and written (alpha test at 0x40 when the shader is alpha-tested), FOG on | none | the diffuse texture times the lightmap's colour, fogged toward a FOGCOL scaled by the haze gain |
| 2 lightmap | ST (lightmap), RGBAQ (white, Q), XYZF2 | TEX0 lightmap page (T8 grey, CLUT alpha = index), CLAMP clamp, TEST Z equal-or-greater and not written, FOG off | `(Cd - 0) * As + 0`: the destination times the lightmap's brightness; 0x80 is 1.0, so 2x overbright | the shadows and the sun |
| 3 detail (0, 1 or 2 maps, and the bump relief) | ST (base times the detail scale), RGBAQ, XYZF2 | TEX0 detail map (T8 grey with the same CLUT), repeat | the same multiply; a grey d multiplies by 2d | the grain |
| 4 reflection | RGBAQ (the cube map's colour at the vertex, scaled by F), XYZF2 | no texture | `(Cs - 0) * FIX + Cd`, FIX 0x80 | the metal's reflection, flat per vertex |

This is the sample's multipass, which **(PCSX2)** matched the look at 60 fps,
with one change of role: on the Xbox the lightmap pass comes first and the
diffuse texture pass multiplies it; on the GS the texture cannot multiply the
destination (the blend's multiplier is an alpha, not a colour), so the base
map goes first with the lightmap's hue as the vertex colour and the lightmap's
brightness multiplies afterwards. `_rasterizer_environment_lightmap_draw`
therefore draws passes 1 and 4 and `_rasterizer_environment_diffuse_texture_draw`
passes 2 and 3; the light-layer decals the game draws between the two are
multiplied by the lightmap instead of by the texture, a difference the decals
milestone can revisit. The exact alternative, three channel-masked lightmap
passes (FBMSK one channel at a time, each with that channel as an alpha
texture), is the plan's; it costs two more passes of fill and is the fallback
if the hue approximation shows on the campaign's coloured lightmaps.

The pass mask is per batch, not only per material: the EE drops the detail and
bump passes for a batch beyond a distance (a detail map's last mip level is
its average, which multiplies by about 1), and the reflection pass where the
material has none. That is the one lever on fill the EE has per batch, and it
is set by the distance test the chain builder already does for culling. The
sample does this per chunk since 9c641fc3 (45% fewer detail and bump vertices
sent, the far cliff within 1.2/255 **(PCSX2)**); the converter writes the
depth per material at which its detail passes reach their last level.

Per vertex, the converter writes (24 bytes, from the Xbox's 40):

| Field | Format | UNPACK |
| --- | --- | --- |
| position | 3 x int16, scaled per batch (the header has the scale and the offset) + 1 pad | V4_16 (8 bytes) |
| base UV, lightmap UV | 4 x int16, 8.8 fixed for the base UV (a world unit of terrain repeats a texture up to a few tens of times), the Xbox's signed 16 for the lightmap's | V4_16 (8 bytes) |
| colour | RGBA bytes: the lightmap's hue at the vertex's lightmap texel (the sample's exporter), alpha 0x80 | V4_8 (4 bytes) |
| normal and flags | 3 x int8 for the reflection pass, 1 byte of flags | V4_8 (4 bytes) |

and per batch a header quadword (vertex count, triangle count, the pass mask
VU1 may draw, the scale) and the index list (3 bytes a triangle, zero padded).
The bounding sphere and the material's index live in an EE-side table beside
the stream, since VU1 does not need them. The colour word passes through VU1
untouched (a float register holds any bit pattern), which is why REGLIST's
RGBAQ, a 32-bit colour beside a 32-bit Q, costs no integer packing: the
converter packs the colour, VU1 stores it beside Q.

**REGLIST or PACKED.** REGLIST output is 64 bits a register (24 bytes a
textured vertex); PACKED is a quadword a register (48 bytes). The sample's VU1
wrote PACKED, its EE path REGLIST. REGLIST halves the GIF's bytes and the
VU1's stores per pass, and is the target: a vertex's three registers are a
quadword and a half, so the output loop is unrolled by two vertices and stores
with 64-bit lane masks (`sq.xy`, `sq.zw`). PACKED is the fallback the sample
proved; the hardware gate (section 5) measures whether the GIF's path 1 rate
makes the difference matter. In either mode PRIM goes through A+D, not the
tag's PRE bit, which PCSX2 ignores.

### The model program (`vu1_model.vsm`)

The same skeleton with skinning and lighting before the transform:

1. **Skin.** Position by node 0's matrix, and by node 1's when the weight is
   not 1, blended by the weight; the normal by node 0's rotation (the game's
   node matrices are rotations and translations, so the normal keeps its
   length: the sample checked |R R^T - I| under 4e-6 on the Chief's). The
   matrices are in VU memory (44 x 3 quadwords, UNPACKed per model from
   `rasterizer_model_begin_parameters.skinning`), composed with the window's
   view by VU1 itself or by the EE on VU0 (the EE's choice; 44 x 12 multiplies
   is nothing on VU0 macro mode, and it keeps the program simpler).
2. **Light.** Ambient plus up to two distant lights (`MAXIMUM_RENDERED_DISTANT_LIGHTS`)
   and two point lights (`MAXIMUM_RENDERED_POINT_LIGHTS`), as
   `render_lighting` gives them: diffuse `max(n . l, 0)` scaled by the
   light's colour, the point lights by distance (the Xbox vertex shader's
   model; the game's `rasterizer_set_model_lighting` fills the constants). The
   result is the vertex's RGBAQ for pass 1, clamped to 0x80.
3. **Transform, clip, fog, cull, output** as the environment program.

Passes for `shader_model`: pass 1 the base map times the light (alpha test for
cut-outs); pass 2 the multipurpose map's channel as an alpha mask times the
change colour, added (the sample's specular mask pass: a 4-bit mask texture
whose CLUT alpha is the index, flat RGBAQ per vertex); pass 3 the reflection
(the cube map as a sphere map, VU1 computing ST from the reflected view
vector, added through the same mask); pass 4 self-illumination (the mask's
other channel times the illumination colour, added). Milestone 3 does passes 1
and 2; the Chief's reflection is the sample's and comes next.

Per vertex, the converter writes 16 bytes (from the Xbox's 32):

| Field | Format | UNPACK |
| --- | --- | --- |
| position, node indices | 3 x int16 scaled per part, node0 and node1 as two bytes in the fourth lane | V4_16 (8 bytes) |
| normal, weight | 3 x int8, weight as a byte | V4_8 (4 bytes) |
| UV | 2 x int16, the Xbox's encoding times the model's base map scale | V2_16 (4 bytes) |

Models are indexed triangle strips in the tags; the converter writes each
part's strips as a triangle list in 8-bit indices per batch (3 bytes a
triangle is cheaper than strip restarts in VU1).

What the EE sends per model draw: the bones (3 quadwords a node, by
`node_matrix_count`), the lights (8 quadwords), the view constants when the
window changed, the change colours (1 quadword), and then the part's batches
by REF. A model with one node (scenery, the sky) sends one matrix.

### What the EE keeps doing

The EE's render pass per batch: the bounding sphere against the window's five
planes and the distance limits, the pass mask, the material's state when it
changes (TEX0 from the texture cache's slot, TEX1's K from the material's LOD
constant plus the window's focal length), four tags. No vertex is read. The
sample's chunk culling and lazy vertices were the EE doing the GS's and VU1's
work; here they are gone, and the EE's per-frame render cost should be the
chain walk over visible clusters, materials and batches: cache misses, which
PCSX2 undercounts, so it is a gate measurement.

## 4. Video memory and the texture cache

### The frame buffer

| Choice | Buffers | Left for textures | For |
| --- | --- | --- | --- |
| 640x448, CT16 dithered, two buffers, Z16 | 1,720,320 | 2,473,984 | The menus, text and HUD are authored at 640 and drawn by the game in 640-pixel coordinates; the sample found 512 "a little soft across", most visible at 2x zoom **(PCSX2 captures)**; fill is 25% more a pass |
| 512x448, the same | 1,376,256 | 2,818,048 | 344 KB more texture memory, 20% less fill a pass; the display magnifies by 1.25 horizontally (MagH), so the field of view is unchanged and the projection has two focal lengths |

**Recommendation: 640x448 for milestones 1 to 3, with the width a constant the
projection, the scissor and the display read from one place**, so 512 is a
rebuild. The menus and text are the first milestone and their sharpness is the
whole picture at that point; the texture memory is not short until models and
the campaign's larger texture sets arrive; and the fill difference is one of
the gate's numbers (section 5). If the gate shows the GS short of fill at five
passes, 512 is the first cut and it is cheap to take. The Z buffer is 16 bits
with the sample's hyperbolic depth (`Z = k1 / z - k2`, which interpolates
exactly on the GS and gives most of the precision to the near range); the
campaign's longest views and the first-person weapon's own near and far planes
(`first_person_weapon_near_clip_distance`, drawn with its own depth constants
and a cleared depth range) are the check, and Z24 (+573 KB at 640) is the
fallback.

Interlaced NTSC at 448 lines, as the sample; PAL at 512 lines later (the
height is the same constant). The HUD and menus, authored for 480 lines, are
scaled vertically by 448/480 in the 2D path's transform (the plan's note).

### The two levels of the texture cache

The game's texture cache (`texture_cache.h`) is kept as an interface and
reimplemented in `rasterizer_ps2_texture_cache.c`, which replaces
`xbox_texture_cache.c` and keeps its page machinery (16 KB pages, LRU, loaded
from the map file through `cache_files`, `texture_cache_bitmap_load`,
`_texture_cache_bitmap_get_hardware_format(bitmap, block, load)`) with the
`D3DBaseTexture` header replaced by a `struct ps2_texture`. The converter
writes every bitmap's texels in GS layout (4-bit or 8-bit CLUT with the
palette in CSM1 order, CT16 for the few true-colour ones, mip levels at the
GS's row widths), so a page's data is uploaded by a REF tag as it is.

1. **Main memory** (the pages, 2 to 4 MB): the textures of the current BSP's
   visible and predicted clusters and the loaded objects, as today, loaded by
   the game's own predicted-resource logic.
2. **Video memory** (about 2.4 MB at 640): the resident set and the slots.

The resident set, uploaded when a map loads and never evicted while it is the
map:

- The hardware character cache (the 128x128 text atlas the game draws glyphs
  into; it changes at run time, so it is re-uploaded when `rasterizer_bitmap_changed`
  says so) and the HUD's bitmaps the game draws every frame.
- The current BSP's lightmap pages (Blood Gulch's 15 pages are 131 KB as
  8-bit greys **(PCSX2)**; a campaign BSP's are a few hundred KB at the
  converter's half size) and the shared detail and bump-relief maps (218 KB
  on Blood Gulch).
- The grey CLUT (alpha = index) that lightmaps and detail maps share, kept in
  a CSA slot so that switching to them reloads no CLUT.

The slots: the rest, allocated to the base maps of environment materials and
to models' textures as draws ask for them, in size classes (the sizes the
converter writes: 256x256 8-bit with mips is 85 KB; 128, 64, 32), evicted
least-recently-drawn. A draw that asks for a texture not in a slot gets the
texture queued for upload at the start of the next frame's DMA and draws this
frame with the default texture (the game's own behaviour on the Xbox when the
cache misses: `rasterizer_set_texture_non_blocking` returns FALSE and the
caller draws without it or with the default). The uploads per frame are capped
(128 KB to begin with; the gate measures path 3's rate); past the cap the
remaining textures wait a frame. Because the chain is kicked at present time,
the whole frame's misses are known before its uploads go out, so a texture
first seen anywhere in the frame is in video memory for the next.

The resident set plus the slots must fit 2.4 MB with the campaign's materials:
the converter's report per map (the plan's `cache_file_report` for the PS2)
prints the resident set's size and the number of 256-class slots that remain,
and refuses a map whose resident set alone is over 1.2 MB.

### Mip levels without an audit

The GS picks the level from Q alone: `L = log2(1/Q) + K`, K in TEX1 in 1/16.
The sample's K per pass (`lod_k`: `log2(density * size * scale / focal) +
bias`, where density is the texture's repeats per world unit of surface,
measured per material by the exporter) carries over: the converter writes
`log2(density)` per material pass, the renderer adds `log2(size * scale)` and
subtracts `log2(focal)` for the window. The audit table (`mip_floor.json`),
which said which low levels one camera path never read, is replaced by a
per-texture floor computed each frame from geometry the renderer already has:

- Each batch has a bounding sphere and the chain builder computes its nearest
  distance to the eye (`|center - eye| - radius`) for culling.
- The texture's minimum level this frame is `floor(log2(d_min * density * size
  / focal))` over the visible batches that use it, clamped at 0.
- A texture is uploaded from that level up (TEX0 points at it as level 0, K
  shifted by 16 x level, TEX1's MXL counts the levels left). If next frame's
  floor is lower (the camera came nearer), the texture is re-uploaded with the
  lower floor and a bigger slot, with one level of hysteresis so a camera
  pacing a boundary does not thrash.

This is the audit's saving (433 to 469 KB on Blood Gulch, bit-identical
pictures **(PCSX2)**) without the table, and it is why the slot allocator has
size classes. Milestone 2 ships without the floor (every texture whole, from
the converter's half size) and adds it when the resident set and the slots
first fail to fit, which the converter's report says in advance.

### Palettes

A TEX0 write with CLD set reloads the CLUT from video memory, and the four
passes alternate between a base map's 256-entry CLUT and the grey one.
Per batch that is four reloads; a thousand batches a frame is four thousand 1
KB reloads, a GS cost PCSX2 does not model (the sample saw none of it). The
grey CLUT in a CSA slot (4-bit CLUTs have sixteen slots; an 8-bit grey needs
its own buffer load) is the first mitigation; 4-bit lightmaps and detail maps
(16 levels, dithered, which the sample did not try) the second; the hardware
gate measures whether either is needed.

## 5. The first hardware gate

PCSX2 charges the EE by instruction count and the GS nothing at all, so the
numbers that decide whether five passes a pixel fit in 33 ms exist only on a
console. The gate is a stand-alone program (`tools/ps2/gsbench`, built like
`eebench`, run from a memory card or ps2link on a PS2 that boots homebrew),
not the game, so it runs before milestone 2's data exists. It times with the
EE's cycle counter between a FINISH written before and after the work, and
prints per test:

1. **Fill, per full-screen pass, per mode**, at 640x448 and 512x448, 16-bit
   with dithering: (a) textured, Z tested and written, no blend (pass 1);
   (b) textured, Z equal, `(Cd - 0) * As + 0` (passes 2 and 3); (c) flat,
   additive (pass 4); (d) the exact fog pass (black with alpha F through the
   fog unit, `Cd * As + Cs`); each as 8 full-screen sprites, and as 8,000
   triangles of 200 pixels covering the screen once (the GS's cost per
   triangle is not its cost per pixel). The decision number: a frame of the
   environment at overdraw 2 is about 8 passes of the screen (1, 2, 3 at
   overdraw, 4 on the metal); the fog pass is one more. With 33 ms, the GS
   has to do those in under 12 ms to leave room for models, transparents and
   the HUD. If a blended textured pass costs over 1.2 ms at 640, 512 and
   four passes are the design; if over 2 ms, the lightmap goes back into the
   vertex colour (the plan's fallback) and the environment is two passes.
2. **CLUT reloads**: test (a) with a TEX0 write (CLD 1) before each of 4,000
   small draws against the same with CLD 0 and against a 4-bit texture with
   CSA; the difference per switch, in microseconds, decides the palette
   question of section 4.
3. **Path 3 uploads**: MB per ms of IMAGE transfers of 8-bit textures, with
   path 1 idle and with path 1 drawing test (b); the per-frame upload cap
   follows, and whether uploads can go during the frame instead of before it.
4. **VU1 throughput**: the environment program (its first version, without
   clipping) on a synthetic stream of 1,000 batches of 40 vertices and 48
   triangles, drawing to a zero scissor: cycles per vertex transformed and
   per listed vertex emitted, REGLIST against PACKED. The design assumes
   under 30 cycles a vertex and under 10 a listed vertex a pass; at 10,000
   unique vertices and 45,000 listed vertices over 4 passes that is about 6
   ms of VU1 a frame, which fits; at double, the pass count per batch has to
   fall with distance more aggressively.
5. **The EE's chain walk**: the same 1,000 batches' tags built from a table
   laid out as the converter will lay it out (sphere test, pass mask, four
   tags each), in milliseconds, against PCSX2's number for the same code: the
   ratio is the correction to apply to every EE estimate the sample made.
6. **The picture**: 16-bit dithered output through four multiply passes on a
   CRT or TV (banding in the shadows, flicker of the interlaced 448 lines),
   and 16-bit depth across a long view with the hyperbolic mapping, as
   stills photographed from the screen; the sample's captures are PCSX2's
   rendering, not the GS's.

The gate passes when 1, 4 and 5 leave the environment under 12 ms of GS and 8
ms of VU1 at 640 (or at 512, with that recorded as the design's width) and the
EE's chain walk under 3 ms for 1,000 batches. Then milestone 2 is built on the
five-pass design; otherwise sections 3 and 4 are revised before it, not after.

Until a console is available the gate program is still written and run in
PCSX2 for its VU1 and EE numbers (which PCSX2 reports, low) and for
correctness; its GS numbers read zero there and are not to be quoted.

## 6. The files of the first milestone

Under `source/rasterizer/ps2/` (game code, the game's flags and prefix):

| File | Holds |
| --- | --- |
| `rasterizer_ps2.c` | `_rasterizer_initialize`, `_dispose`, `_reset_state`, `_frame_begin`, `_frame_end`, `_windows_begin`, `_window_begin`, `_window_end`, `_windows_end`, `_present`, `_set_vblank_callback`, `_window_get_fog`, `_set_fog`, `rasterizer_set_frustum_z`, the world-to-screen matrix per window (the sample's `vu_matrix`, with `k1`, `k2` from the window's near and far), the clear, the FINISH wait, the flip |
| `rasterizer_ps2_chain.c`, `.h` | The double-buffered VIF1 chain and CNT arena, the upload chain on the GIF channel, the kick and the FINISH wait, the high-water marks |
| `rasterizer_ps2_gs.h` | Register packing for PRIM, TEX0, TEX1, CLAMP, ALPHA, TEST, FRAME, ZBUF, SCISSOR, XYOFFSET, FOGCOL, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, RGBAQ, ST, UV, XYZF2; GIF tags (A+D, REGLIST, PACKED, IMAGE); the VIF codes (NOP, STCYCL, OFFSET 0x02, BASE 0x03, FLUSH, FLUSHA, FLUSHE, MSCAL, DIRECT, MPG, UNPACK) and DMA tags (CNT, NEXT, REF, END). The sample's macros, with the corrected opcodes, and nothing from gsKit |
| `rasterizer_ps2_display.c` | The video mode (NTSC 448 interlaced; PAL later), the two frame buffers and the Z buffer's addresses, the privileged registers, DISPFB flip at the vertical blank with `ps2_vblank.c` |
| `rasterizer_ps2_screen.c` | `_rasterizer_psuedo_dynamic_screen_quad_draw`, `_rasterizer_dynamic_screen_geometry_draw`, `_add_multitexture_params_to_base`, `_rasterizer_hud_begin`, `_hud_end`, `_rasterizer_screen_flash`, `rasterizer_set_framebuffer_blend_function`: 2D vertices to REGLIST sprites and triangles by DIRECT, the multi-map quads as further blended passes, the 480-to-448 scale |
| `rasterizer_ps2_text.c` | `rasterizer_text_begin`, `_end`, `_draw_character`: a character is a SPRITE of two vertices from the resident atlas, with the text's shadow as the game asks |
| `rasterizer_ps2_bitmaps.c` | `struct ps2_texture` behind `bitmap_data.hardware_format`; `rasterizer_bitmap_new`, `_changed`, `_delete`, `rasterizer_get_bitmap_default_hardware_format`, `rasterizer_set_texture_bitmap_data`, `rasterizer_set_texture_direct` and the non-blocking forms; the format table (the converter's GS formats; until the converter writes them, `rasterizer_ps2_texels.c` decodes the Xbox's DXT1, DXT3, DXT5 and swizzled 8-bit and 16-bit formats into CT16 and T8 on the EE at load time, which is slow and is left behind with the converter's second pass) |
| `rasterizer_ps2_texture_cache.c` | `texture_cache.h`, replacing `xbox_texture_cache.c`: the pages, the video memory slots and the resident set, the upload queue, the floor (later) |
| `rasterizer_ps2_dynamic.c` | The dynamic vertex and triangle buffers in the rasterizer memory pool |
| `rasterizer_ps2_stubs.c` | Generated by `tools/ps2/rasterizer_stubs.py` from the Xbox folder's definitions: every backend function and helper not yet implemented as a counting no-op (`_rasterizer_initialize` returns TRUE, the buffer registrations return TRUE, the lock functions return the pool's memory); the counts print at map unload so a milestone sees what the game asked for |
| `rasterizer_ps2_internal.h` | What the files share: the frame state, the window state, the texture record, the arena |

Milestone 2 adds `rasterizer_ps2_environment.c`, `rasterizer_ps2_geometry.c`
(the buffer registrations), `rasterizer_ps2_vu1.c` (program upload by MPG in
256-instruction pieces, the BASE and OFFSET set-up, the material slot, the
batch tags) and `port/ps2/vu1/vu1_environment.vsm`; milestone 3 adds
`rasterizer_ps2_models.c` and `port/ps2/vu1/vu1_model.vsm`.

Under `port/ps2/`:

| File | Holds |
| --- | --- |
| `port.json` | A `game` exclude list (the Xbox folder, `xbox_texture_cache.c`) |
| `src/ps2_screen_hooks.c` | The platform hooks now in `ps2_d3d_null.c`, with the renderer's answers |
| `src/ps2_d3d_null.c` | Shrinks to resources, states and D3DX for `progress_bar.c`; its drawing side goes |
| `src/ps2_vblank.c` | Gains the flip of DISPFB when the frame's FINISH has been seen |
| `vu1/` | The microprograms and the generated `.inc` of their memory maps |

Under `tools/`:

| File | Holds |
| --- | --- |
| `tools/ps2_build.py` | Compiles `source/rasterizer/ps2`, the `dvp-as` rule, the stub generator and the layout generator as build steps |
| `tools/ps2/rasterizer_stubs.py` | Reads the definitions in `source/rasterizer/xbox/*.c` and writes `rasterizer_ps2_stubs.c` minus the names `source/rasterizer/ps2/*.c` define |
| `tools/ps2/vu1_layout.py` | One table of each program's memory map, written as a C header and a `dvp-as` include |
| `tools/ps2/gsbench/` | The hardware gate's program |
| `tools/ps2/convert_map.py` | The second pass, when Phase 2 starts: bitmaps to GS layout and palettes (from `export_map.py`'s quantiser), the BSP's materials to batches (from `bsp.py` and `export_map.py`'s chunking and winding), the models' parts to batches, the per-material LOD constants and hue colours |

## 7. What carries over from the sample

**Verbatim or nearly** (a copy with the names changed):

- The VIF code and DMA tag macros and the two-codes-before-the-data rule
  (`vu_tag`), the program upload in 256-instruction MPG pieces, the BASE and
  OFFSET double buffer, the per-batch chain shape (header, data UNPACKs,
  MSCAL) in `chief_vu1_init` and `vu_chain_object`: into
  `rasterizer_ps2_vu1.c` and `rasterizer_ps2_chain.c`.
- The world-to-screen matrix (`vu_matrix`), the focal length from the field
  of view, the GS's origin offset, the 16-bit depth constants `k1` and `k2`,
  the guard band (64 pixels in the sample now) and `clip_setup`'s planes:
  into `rasterizer_ps2.c`.
- `chief_vu1.vsm`'s skeleton: `xtop`, the matrix in VF01 to VF04, the
  transform and divide, `ftoi4` and `ftoi0`, the back-face test by the MAC
  flag's x-sign bit (0x0080, with `fmand` after the `msub` has set it), the
  NLOOP patch with EOP, the paired XGKICKs, the `[E]` ending: the base of
  both programs.
- The frame ordering: the clear first and its FINISH waited for, VU1's draws
  next, path 2 draws last, with a FLUSH before the frame's FINISH.
- The REGLIST packet with an A+D state packet before it and UV as the dummy
  register, from `send_map_pass`: the 2D path.
- The LOD constant (`lod_k`, `LOD_BIAS` 0.3) and the fog coefficient
  (`fog_factor`, `fog_bits`, `FOG_HAZE_GAIN` 0.5, the fog in the base pass
  and the exact pass as `fog_pass`).
- The texture set-up's rules (TBW, the mip levels' addresses, 4-bit greys,
  the CLUT in CSM1 order, a failed allocation is address 0 and must stop the
  program) from `texture_setup_mips`, without gsKit's allocator.
- The profiler's pattern (`prof_mark`, 60-frame windows, the worst frame,
  the timer reset after the report) as the renderer's `debug.frame_profile`.
- The pitfalls in the two documents, as comments at the lines they apply to.

**Adapted** (the idea stays, the code is rewritten):

- The EE clipper (`clip_triangle`, the outcodes, "a near-plane cut needs
  every plane", the clamped fill of a clipped vertex): VU1's slow path is
  written from it; the EE copy serves decals and particles later.
- The chunk culling and the distance limits: per batch, from the converter's
  spheres, in the chain builder.
- The exporter's knowledge (`bsp.py`, `export_map.py`): the BSP's layout and
  offsets, the winding against the vertex normals, the lightmap page's filled
  checker, the detail map's average in the last mip, the hue and brightness
  split, the median texture density per material: into the converter's second
  pass.
- The Chief's two textured passes: the model program's pass 1 and 2.

**Left behind**:

- gsKit: its queue, heap, `gsKit_prim_*`, display set-up and allocator.
- The pose cache (3.3 MB) and the per-frame-of-animation VIF chains built at
  start-up: models are skinned on VU1 from the game's node matrices each
  frame.
- The EE's map pipeline (`draw_map`, `map_vertex`, the VU0 macro-mode
  projection and the lazy vertex stamps, the 48-byte vertex records): VU1
  does it. VU0 macro mode stays in the plan for the game's matrix maths
  (Phase 5), not for vertices.
- The mip audit and `mip_floor.json`: the distance floor of section 4.
- The sample's resident everything: the two-level cache.
- `DEBUG_SECONDS`: the game has a debug camera and saved films for still
  comparisons.

## 8. Open questions

- **The hue approximation of the lightmap** on the campaign's coloured
  lightmaps (the Library's blue, the Silent Cartographer's interiors): compare
  the Linux build's frame with the PS2's on the first campaign BSP the
  converter produces; the three-pass exact form is the fallback and costs two
  passes of fill.
- **Bump maps** are not drawn (the plan); the sample's fixed-light relief
  texture is a converter option per shader, not a renderer feature.
- **The Z buffer's 16 bits** on the campaign's long views, and the
  first-person weapon's own depth range: a gate observation; Z24 is the
  fallback and costs 573 KB at 640.
- **Split screen** multiplies the passes by the windows but divides the
  pixels; the per-window chain is designed in but not measured.
- **The GIF's path 3 during the frame**: if the gate shows uploads can
  interleave with path 1 without stalling it, textures first seen in a frame
  can be uploaded in that frame, with the `FLUSHA` moved to the first draw
  that needs them; the design assumes they cannot.

## 9. The first three implementation steps

1. **The link without the Xbox rasterizer.** Add the `game` exclude list to
   `port/ps2/port.json`, the `source/rasterizer/ps2` sources and the
   `rasterizer_stubs.py` step to `tools/ps2_build.py`, and
   `ps2_screen_hooks.c`. `ninja ps2` links `halo.elf`; in PCSX2 the game boots
   to the main menu (drawing nothing) and `pcsx2_bench.py --map c20` still
   plays its 300 ticks, with the stubs' call counts printed at the end. Record
   the ELF's code and `.bss` sizes before and after (the Xbox rasterizer's
   share of the 3.6 MB linked, which section 2's budget guesses at a few
   hundred KB).
2. **A frame.** `rasterizer_ps2.c`, `rasterizer_ps2_chain.c`,
   `rasterizer_ps2_gs.h`, `rasterizer_ps2_display.c` and the flip in
   `ps2_vblank.c`: the video mode, two 640x448 CT16 buffers and a Z16 buffer,
   the double-buffered chain with the clear by DIRECT and the FINISH, the
   present's wait and kick, a frame profile printed every 60 frames. Check in
   PCSX2 with a window capture that the clear colour, cycled by the frame
   index, changes at the NTSC rate and that the frame counter the game reads
   (`rasterizer_globals.frame_index`) advances once a frame; log the arena's
   high-water mark (expected: the clear alone).
3. **The main menu.** `rasterizer_ps2_bitmaps.c` with the EE-side texel
   decoder, `rasterizer_ps2_texture_cache.c` in its resident-only form
   (everything the menu asks for is uploaded once and kept; eviction comes
   with milestone 2), `rasterizer_ps2_text.c`, `rasterizer_ps2_screen.c` and
   `rasterizer_ps2_dynamic.c`. Check: a PCSX2 capture of the main menu beside
   the Linux build's at 640x480 (the PS2's is 448 lines tall: compare after
   scaling), the console's text readable, the EE's render pass under 2 ms in
   the frame profile, and the video memory line (`VRAM: ...`, as the sample
   prints it) under 1 MB for the menu's bitmaps.

After these, milestone 2 waits on two things that are not renderer code: the
converter's second pass (Phase 2: batches and GS-layout bitmaps for one
multiplayer map) and the hardware gate's numbers. The gate program is step 4
and can be written beside step 3; the converter's pass can start from
`export_map.py` at once.
