# PlayStation 2 port: analysis and plan

Status: planning. Nothing in this document is implemented.

Target: a retail PS2 (32 MB). Development uses PCSX2 with the default 32 MB
of EE memory (not its 128 MB development-kit mode), so that the emulator
enforces the retail budget. See "Optimizing memory and game logic" for the
measured numbers.

## Summary

A PS2 port is possible in principle but is far harder than the Linux,
Windows and Android ports. Those ports run the Xbox game nearly as-is on
machines with more memory and faster CPUs: they emulate Direct3D 8, the NV2A
shaders and DirectSound, and reserve the Xbox memory window at
`0x80000000`. On the PS2, none of these approaches works:

- **Memory.** The Xbox game budgets about 51 MB for game state and caches
  (22 MB of tag data, 22 MB of textures, 4 MB of sounds, 3.3 MB of game
  state). The PS2 has 32 MB of main RAM in total, plus 4 MB of video RAM and
  2 MB of sound RAM.
- **Addresses.** Cache files are memory images linked to `0x803A6000`. On
  the PS2 that address is in kseg0, which the TLB cannot map for a user
  program, so every map's tag data must be relocated.
- **Graphics.** The Graphics Synthesizer has no pixel shaders, no DXT
  textures, no cube maps and no dot3 bump mapping. A D3D8/NV2A translation
  layer like `port/linux/src/d3d8_gl.c` cannot target it. The PS2 needs a
  native rasterizer backend.
- **CPU.** The EE (MIPS R5900, about 295 MHz, 8 KB data cache, no L2 cache,
  single-precision FPU without IEEE denormals, infinity or NaN) is several
  times slower than the Xbox's 733 MHz Pentium III on branchy C code. The
  game runs a 30 Hz simulation (AI, physics, scripts) that was tuned for the
  Xbox.

The plan therefore has three parts:

1. An **offline map converter** that runs on a PC, starts from the user's
   Xbox maps, and writes PS2 maps: relocated tag data, trimmed to a budget,
   with textures, sounds and geometry in PS2-native formats.
2. A **PS2 platform layer** (`port/ps2/`) and a **native rasterizer**
   (`source/rasterizer/ps2/`) that implement the interfaces the game already
   has (`rasterizer.h`, `platform_sound.h`, the XDK shims).
3. **Feasibility gates** before the expensive work. Phase 0 measures memory
   and CPU cost on the existing PC build. Phase 1 runs the simulation
   headless on real hardware. If either gate fails, the project stops or
   changes target (see "Open questions" about the 128 MB development-kit
   memory mode).

## What the repository is

These facts drive the plan:

| Fact | Where | Consequence for PS2 |
| --- | --- | --- |
| A decompilation of Xbox build 2342: 466 C files, about 490k lines, in 38 subsystems (AI 79k lines, rasterizer 46k, interface 42k, bitmaps 37k, game 32k, hs 28k). | `source/` | The game code is portable C. The x86 assembly is already replaced with C. |
| The game is compiled with MSVC semantics (`-fms-extensions`, `-fshort-wchar`, `-malign-double`, `-fcommon`, `-freg-struct-return`) for ILP32. | `tools/linux_build.py`, `port/linux/include/halo_linux_prefix.h` | The PS2 EE is little-endian with 32-bit pointers, like the Xbox. Struct layouts can be kept identical to the Xbox's. |
| An ILP32 port to a non-x86 CPU already exists (Android, `arm64_32` guest). It found and fixed calling-convention mismatches (`tools/android_abi_check.py`, `halo_android_variadic_prototypes.h`). | `port/android/` | Much of the work for "not x86" is already done. MIPS EABI passes floats in their own registers, like AArch64, so the same fixes apply. |
| The platform layer emulates the XDK: kernel, files, threads, memory, D3D8, DirectSound, XInput, Winsock. | `port/linux/src/` (about 30 files), `port/include/xdk/` | Most non-graphics files can be reused or used as a model (`xbox_kernel.c`, `xbox_files.c`, `xbox_xapi.c`). `d3d8_gl.c`, `nv2a_*.c` and `dsound_sdl.c` cannot be reused. |
| The rasterizer has a clean API (`rasterizer.h`, about 100 functions: environment lightmaps, models, decals, detail objects, HUD, screen effects, and so on). The Xbox implementation is in `source/rasterizer/xbox/` (about 40 files). Outside it, only three files use D3D directly. | `source/rasterizer/` | A PS2 backend can replace `source/rasterizer/xbox/` at the `rasterizer.h` level, without emulating D3D. |
| Sound goes through `platform_sound.h`, with `sound_dsound_xbox.c` as the Xbox implementation. | `source/sound/` | A `sound_ps2.c` that drives the SPU2 can replace it. |
| Fixed memory map: game state at `0x80061000` (0x345000 bytes), tag cache at `0x803A6000` (0x1600000 bytes), then the texture cache (22 MB, 16 KB pages) and the sound cache (4 MB). The port enlarged all of these for the desktop. | `source/cache/physical_memory_map.c`, `port/linux/include/halo_port_capacity.h` | The PS2 build needs its own capacity branch, at Xbox values or smaller. |
| A **tag schema** describes every pointer, count, index and file offset in every tag group, with compiler-checked offsets. The tag validator and the Custom Edition converter use it. | `port/linux/game/tag_schema*.c`, `tag_validate.c`, `custom_edition_cache.c`, `cache_file_formats.c` | This is the basis of the PS2 map converter: it can find every pointer that must be relocated and every resource that must be transcoded. |
| Offline tools already read maps (`port/tools/cache_file_report.c`, `tools/map_validate.c`) and extract maps from an Xbox disc image (`port/linux/src/xiso.c`). | `port/tools/`, `tools/` | The converter can be built from these parts. |
| Many features are new to the port: 128-player netcode, internet play with mbedtls, Opus voice chat, UPnP, Discord, updater, high-res HUD and text, frame interpolation, Custom Edition maps, anti-aliasing. | `port/linux/src/`, `port/linux/game/`, `port/third_party/` | None of these fits in 32 MB. The PS2 build leaves them out (see "Scope"). |

## Hardware comparison

| | Xbox | PS2 | Gap |
| --- | --- | --- | --- |
| CPU | Pentium III, 733 MHz, 128 KB L2 cache | R5900 (EE), about 295 MHz, 16 KB I-cache, 8 KB D-cache, 16 KB scratchpad, no L2 cache | Estimate 3x to 5x slower on game logic. VU0 can do vector math. |
| Floating point | x87/SSE, IEEE | Single precision only, no denormals, infinity or NaN (results saturate). `double` is emulated in software. | 144 uses of `double` in the game (36 in `hs.c`). |
| Main memory | 64 MB unified | 32 MB EE (the kernel keeps the first 1 MB) + 2 MB IOP | About half. |
| Video | NV2A: vertex and pixel shaders, DXT, cube maps, 64 MB shared | GS: 4 MB eDRAM, fixed-function blending, palettized textures, huge fill rate. VU1 for vertex work. | A rewrite of the renderer. |
| Sound | DirectSound, 64 hardware 3D voices, Xbox ADPCM | SPU2: 48 voices, 2 MB sound RAM, VAG ADPCM | Different format, fewer voices, less RAM. |
| Storage | DVD + 8 GB hard disk (maps decompressed to the disk cache) | DVD (about 3 to 5 MB/s, slow seeks). Optional HDD on early models. 8 MB memory card. | Streaming must be planned around DVD seeks. Saves must be small. |
| Unaligned memory access | Allowed | Address error exception | Must be audited. |

## The hard problems and how to solve them

### 1. Memory budget (the main risk)

Target budget for 31 MB of usable EE RAM. The first three lines are
measured (see "Optimizing memory and game logic"); the others are targets
that Phase 0 must confirm:

| Item | Xbox | PS2 target | How |
| --- | --- | --- | --- |
| Code and constant data | 3.6 MB linked (measured: 3.44 MB of code and constants, 0.18 MB of data, at `-Os` with `--gc-sections`); 2.8 MB on x86 | 3.5 MB | `--gc-sections` drops the copies of the headers' inline functions each unit emits and what the PS2 never calls: it took the linked code from 6.42 MB to 3.44 MB. The Xbox rasterizer goes when the PS2 renderer replaces it. |
| Static (`.bss`) data | 3.1 MB (measured) | 0.5 MB | Remove profiling and debug arrays, fix an oversized bit vector, size the netcode for 16 players. |
| Game state | 3.2 MB (measured) | 2.5 MB | Xbox capacities, not the port's (`halo_port_capacity.h` has a 17 MB game state), then cut by measured peak use. |
| Tag data (shared tags + the current BSP) | 21.3 to 22.0 MB on campaign levels (measured) | 16 MB (projected: 13.7 to 14.4 MB) | Converter: geometry in PS2 formats, no tangent space, keyframed animations, no super-high LOD, trimmed scripts; 16-bit collision. |
| Texture cache | 22 MB (DXT) | 4.8 MB | Half size; bump maps (13% of the textures) not loaded. |
| Sound cache | 4 MB | 0.5 MB EE + 2 MB SPU2 + IOP stream buffers | Music and long dialogue are streamed from the disc on the IOP. |
| Render packets | (in D3D) | 1.5 MB | Double-buffered VIF/GIF packet chains. |
| Heap, stacks, ps2sdk | | 1.5 MB | |

The tag data line decides feasibility. Only one BSP is in memory at a time
(`cache_files.c` loads each BSP into the tag cache when the level switches
to it), so the number that matters is the shared tags plus the largest
BSP.

The measured MIPS code first looked like 5 MB (the objects, before the
link), which would have left 14.5 MB for the tag data. Linked with
`--gc-sections` it is 3.6 MB with its data, as the plan first assumed, so
the tag data keeps its 16 MB: the campaign levels project to 13.7 to 14.4
MB with all the converter and game-code changes, a margin of 1.6 MB or
more.

### 2. Relocating the tag data

The Xbox tag data is linked to `0x803A6000`, and each structure BSP to its
own address at the top of the Xbox's 22 MB tag cache. Those addresses are
the EE kernel's, which a PS2 program cannot use, and the PS2's tag cache is
smaller. The game also expected the game state at a fixed address.

Done (`port/linux/game/tag_relocate.c`, called by `cache_files.c` as the
tags and each BSP load):

- The PS2 reads the tags into its tag cache and moves every pointer they
  hold: each word that points into the linked tags gets the difference
  between where they are and where they were linked. Each BSP is read
  after the tags (a page in) and moved the same way, with its pointers into
  the tags. Where the tag cache is the Xbox's (every other port), nothing
  is done.
- The pointers are found where `tools/ps2/relocation_check.py` finds them:
  the tag header and index, the buffer headers, each structure BSP's
  header, every block, data and tag reference of the tag schema, a table
  of the pointers the schema does not list (the models' and BSP materials'
  buffer pointers, blocks the validator does not check: the scenario's
  child scenarios, comments, decals and BSP references, the BSPs' markers
  and a few more), and the names of the tag references the schema does not
  list.
- The check reads every other word of every map's tags and BSPs: on all 24
  maps of the retail disc, nothing else points into them but known
  numbers (datum indices, flags, colors, checksums), raw vertex and index
  data, and a few bytes no pointer reaches at the end of some BSPs. It
  relocates 6,465 (ui) to 55,862 (c10) pointers a map.
- The C relocation, run in PCSX2 on the tags and BSPs of ui, bloodgulch and
  a10 (14 buffers), gives the same bytes as the check's rules, word for
  word (compared by hash).
- The game state and the caches are wherever the PS2's heap puts them
  (`port/ps2/src/ps2_memory.c`), the same each run of a build, which saved
  games need. `physical_memory_map.c` asks without the Xbox's addresses on
  the PS2.

The map converter will do the same relocation once, on the PC, to the
PS2's tag cache address; the PS2 then reads converted maps without it.

### 3. Compiler and ABI

Use ps2dev's GCC (`mips64r5900el-ps2-elf`) with ps2sdk. Clang has no
supported R5900 target (the R5900 has no double-precision FPU and has CPU
errata that GCC works around).

- ABI (measured with ps2dev v2.0.0, GCC 15.2): the EE's default ABI is
  n32, with 32-bit `long` and pointers, 8-byte alignment of `double` and
  `long long` (as `-malign-double` gives on x86), hardware single-precision
  floats and software doubles. With `-fshort-wchar`, `wchar_t` is 16 bits.
  So the game's structures keep their Xbox layout without `-mlong32` or
  other options, and every layout assert in the game holds.
- Flags: the Linux build's, less those of clang and x86:
  `-fms-extensions -fshort-wchar -fcommon -fno-strict-aliasing -fwrapv
  -fno-delete-null-pointer-checks -freg-struct-return -ffp-contract=off
  -std=gnu89 -fpermissive` (`-fpermissive` makes GCC 14 and later treat
  implicit declarations and pointer mismatches as warnings, as clang's
  `-Wno-error=...` does), with `-D_EE -G0`.
- `port/ps2/include/halo_ps2_prefix.h` adds what GCC does not take from
  clang: `__int64`, the x86 calling conventions, `__declspec`, and MSVC's
  COMDAT inline functions. Those become GNU89 inlines that each unit emits
  and the generated `#pragma weak` makes weak.
- Struct layout: x86 MSVC aligns 8-byte members to 8 (`-malign-double`).
  Write a layout manifest from the x86 build (`sizeof` and `offsetof` of
  every tag, game state and saved-game structure, using the tag schema and
  `tools/pdb200_types.py`), and add static asserts that the PS2 build has
  the same layout.
- Stepping stone: build the Linux port with GCC (`--linux-cc gcc`) first.
  This separates compiler problems from platform problems.
- Prototypes: the files the Android build gives the variadic prototypes
  (`tools/android_abi_check.py` finds them) get them on the PS2 too,
  because n32 also passes floats in their own registers. Run the check on
  the PS2's code as well.
- Unaligned access: x86 and AArch64 allowed it, so nothing has checked it
  yet. Run the Linux build with `-fsanitize=alignment` through the menus
  and every level, and fix each report (network bitstreams, packed
  structures, byte-pointer casts).
- Floating point: run the Linux build with flush-to-zero and
  denormals-are-zero enabled, and with traps for infinity and NaN, to find
  code that depends on IEEE behavior. Change hot `double` code to `float`
  where the result does not change (check the netcode's determinism needs
  first).

### 4. Renderer

Write `source/rasterizer/ps2/` to the `rasterizer.h` interface. The EE
builds packet chains, VU1 microprograms transform, clip, light and fog the
vertices, and the GS draws them. The converter prepares data offline, so
the run-time work is small.

| Halo feature | PS2 method |
| --- | --- |
| Environment: diffuse x lightmap (`shader_environment`) | Pass 1: diffuse texture. Pass 2: lightmap as a multiply of the destination: the GS cannot multiply by destination color, but `(Cd - 0) * As + 0` multiplies by source alpha. Do it three times with `FBMSK` channel masks (one pass per color channel) for colored lightmaps. Alpha `0x80` is 1.0, so 2x overbright works. The GS fill rate is large enough for the extra passes. Fallback: a luminance lightmap plus vertex color. |
| Detail maps | An extra blended pass, or combine offline into the diffuse texture where the scale permits. |
| Bump maps, specular cube maps (environment and models) | No bump mapping. Convert cube maps offline to sphere maps and draw an additive pass with VU1-generated texture coordinates, at low resolution, or leave them out per shader. |
| Models (`shader_model`): vertex lighting, change color, self-illumination, multipurpose map | Vertex lighting on VU1 (the Xbox also lit models in its vertex shader). Change color and self-illumination as extra passes masked by alpha. |
| Transparent shaders (`generic`, `chicago`): up to 4 stages with combiner math | Map each combination of stages to one or more GS blend passes. The converter lists the combinations that the maps really use, and simplifies the rare ones. |
| Fog (atmospheric and planar) | GS per-vertex fog, computed on VU1. Planar fog is approximated per vertex. |
| Active camouflage, water, glass refraction, screen effects | The GS can read its own frame buffer as a texture, so full-screen and refraction effects are cheap. |
| Shadows | Render to texture in GS memory, as on the Xbox, at a lower resolution. |
| Lens flare visibility (Xbox occlusion queries) | A ray cast against the collision BSP, which the game already has. |
| Textures | The converter changes DXT and 32-bit textures to 4-bit and 8-bit CLUT textures in GS layout, at reduced size. The run-time texture cache uploads them to GS memory through GIF path 3 as the frame needs them. GS memory: two 16-bit 640x448 frame buffers and a 24-bit depth buffer use about 1.7 MB, which leaves about 2.3 MB for textures. |
| Resolution | 640x448 NTSC (512 lines PAL), with 480p as an option. The HUD and menus are 640x480, so they are scaled vertically. |
| Geometry | The converter writes BSP clusters and model parts as VIF packets with compressed vertices (16-bit fixed point), so that the EE only links them into the packet chain. |

### 5. Sound

- The converter transcodes Xbox ADPCM to VAG ADPCM, and marks music and
  long dialogue for streaming.
- `sound_ps2.c` implements `platform_sound.h`. An IOP module (custom IRX,
  or audsrv with extensions) plays the voices and streams from the disc.
  The EE calculates volume, pan and pitch for 3D sounds.
- With 48 voices, the sound manager needs a voice priority limit. Leave out
  reverb at first (the SPU2 has a reverb unit that could replace it later).

### 6. Disc and streaming

- The converted maps are on the disc uncompressed. The Xbox copied
  compressed maps to its hard disk; the PS2 cannot do this.
- The tag data loads completely when a level starts (about 15 MB at 3 to
  5 MB/s is 3 to 5 seconds).
- Textures, geometry and sounds stream through the IOP. The converter
  places each BSP's resources together on the disc to reduce seeks, and
  `predicted_resources.c` gives the prefetch list.
- Saves go on the memory card: compress the game state with the game's own
  zlib (`source/memory/zlib`). It is mostly empty pools and compresses
  well.

### 7. Other platform pieces

| XDK area | PS2 |
| --- | --- |
| Threads, events, critical sections (`xbox_kernel.c`) | ps2sdk kernel threads and semaphores. The EE kernel does not preempt threads of the same priority, so the 4 game threads (`cache_files_windows.c`, `cache_files_decompress_windows.c`, `input_xbox.c`, `thread_win32.c`) need explicit yields or priority changes. |
| Files (`xbox_files.c`) | `cdrom0:` (or `host:` and `mass:` during development) through ps2sdk's file I/O. |
| Input (`xinput_sdl.c`) | libpad and the multitap for 4-player split screen. The DualShock 2 buttons map as the README's DualSense table does (white = L1, black = R1). |
| Timers | EE timers or the cycle counter (`profile.c` already uses `QueryPerformanceCounter`). |
| Bink video | Not available, as on the other ports. |

## Optimizing memory and game logic

### How the numbers were measured

The static data and game state numbers come from the source code. Every
game file was compiled for i686 with the Linux build's MSVC ABI
flags, musl's i386 headers, and the Xbox values of
`halo_port_capacity.h`:

- **Game state:** the constant arguments of every `game_state_malloc`,
  `game_state_data_new` and `game_state_memory_pool_new` call, read from the
  LLVM IR. The total, 3,190,700 bytes, agrees with the Xbox's known
  3,165,260 bytes. The difference is the port's 128-player array.
- **Code and static data:** `llvm-size` and `llvm-nm` of the 466 game
  objects at `-Os`.

The tag data, texture and disc numbers come from the retail maps (see
"Memory: tag data").

### Memory: static data (about 2.6 MB to recover)

The game objects have 3.12 MB of `.bss`. Most of it is not needed on a
retail PS2:

| Symbol | Size | Action |
| --- | --- | --- |
| `profile_globals` (`cseries/profile.c`) | 1,081 KB | Leave out profiling in the PS2 build. |
| `render.environment_surface_flags` (`render/render.h`) | 512 KB | Done (090c8377): it was used as a bit vector (`BIT_VECTOR_SET_FLAG`) but declared with one `long` for each surface, not one bit. It is now 16 KB on every platform, and the `render` global went from 643,732 to 135,828 bytes. |
| `ai_debug` (`ai/ai_debug.h`) | 534 KB | Leave out AI debugging. |
| `update_client_globals`, `update_server_globals` (`game/player_queues_new.c`) | 656 KB | Size them for the PS2's player count. |
| `late_joiner_players` (`networking/network_server_manager.c`) | 512 KB | Netcode for 128 machines. Size it for 16, or leave it out. |
| `local_lens_flare_occlusion_test_results2` (`rasterizer/rasterizer_lights.c`) | 256 KB | One byte for each of 65,536 markers and 4 windows. The PS2 renderer uses a ray cast for visibility. Store results only for the lens flares that are visible. |
| libtiff tables (`TIFFFax1D*`) | 115 KB | libtiff is for tools. Leave it out. |
| `hs_expression_refusal.stack`, `render_debug_globals`, `tag_empty_data_bytes` | 170 KB | Debug and console code. Make it smaller or leave it out. |

### Memory: game state (about 0.7 MB to recover)

The largest pools at Xbox capacities:

| Pool | Size | Notes |
| --- | --- | --- |
| objects (memory pool) | 1,024 KB | Measure the peak use on each level, then make it smaller. |
| actor | 468 KB | 256 actors of 1,828 bytes. Few levels have 256 actors at once. |
| prop | 240 KB | 768 props of 312 bytes. |
| decal vertices (GPU part) | 160 KB | PS2 decals can use 16-bit positions. |
| hs thread | 137 KB | 256 script threads of 536 bytes. Size it to the largest scenario's script count. |
| particle, decals, lights | 111 to 115 KB each | |

A change to a capacity changes the layout of saved games. Make these cuts
before the first PS2 release, then keep them fixed.

The port uses the measured peaks from each level, plus a margin, as the
PS2's capacities. Add a debug-build check that logs each pool's peak use
(`game_state.c` already records every allocation).

### Memory: tag data (measured on the retail maps)

Measured on all 24 maps of a retail NTSC disc image (Halo: Combat Evolved,
USA, Rev 2). The tag schema, exported from the i686 build, was used to walk
every tag of the shared tag data and of each structure BSP.

**How the Xbox lays out a map's memory.** The shared tags stay resident for
the whole level. Each structure BSP loads when the level switches to it, at
an address that ends at the top of the 22 MB tag cache. The resident peak is
therefore the shared tags plus the largest BSP. In the shared tags, the tag
index and names come first, then the vertex and index buffers of every model
(the tag header's `vertex_buffers` and `index_buffers` point at them; the
parts' `compressed_vertices` blocks are empty), then the tags. With that
region counted, the schema accounts for practically every byte: 99.8% or
more of each BSP, and all of the shared tags except a few unreadable
fields.

**Every campaign level fills the Xbox budget.** Each one peaks at 21.3 to
22.0 MB of 22 MB (d20: 21.97 MB). The multiplayer maps peak at 7.1 to 8.9
MB. a10 as an example:

| a10 | Size |
| --- | --- |
| Shared tags | 13.50 MB |
| (of which model vertices, 32 bytes each) | 3.91 MB |
| (of which model indices) | 0.42 MB |
| (of which animation frame data, `antr`) | 3.63 MB |
| (of which sound headers, `snd!`) | 1.34 MB |
| (of which collision models, `coll`) | 0.66 MB |
| (of which scripts, `scnr/hs_syntax_data`) | 0.36 MB |
| Largest BSP | 8.17 MB |
| (of which render vertices, `compressed_vertex_data`, 40 bytes each) | about 45% |
| (of which collision: edges, vertices, BSP nodes, surfaces) | about 30% |

**What the changes save.** Every number in this table is measured
(with the tools in `tools/ps2`, described in
[tools/ps2/README.md](../tools/ps2/README.md)), except the four game-code formats in
the last rows, whose savings follow from their layouts:

| Change | Needs | Measured size | Saving on a campaign level |
| --- | --- | --- | --- |
| Model vertices from 32 to 16 bytes (16-bit position, packed normal, 16-bit UV, nodes and weight; no binormal or tangent, because the GS has no bump mapping) | The PS2 renderer | 3.8 to 4.9 MB | 1.9 to 2.4 MB |
| BSP vertices from 40 to 20 bytes (16-bit position, UV and lightmap UV, packed normal, incident radiosity) | The PS2 renderer | 2.5 to 3.4 MB for the largest BSP | 0.5 to 1.9 MB |
| Animations in the engine's own keyframe format | The converter | 2.4 to 4.7 MB uncompressed | 1.0 to 1.9 MB at 0.25 degrees / 2 mm |
| No geometry that only the super-high LOD uses | The converter | 0.7 to 1.8 MB | 0.4 to 1.0 MB after the vertex change |
| Script nodes trimmed to those the scripts made | The converter | 371 KB allocated, 89 to 366 KB made | 0.01 to 0.26 MB (0.36 MB on each multiplayer map, which has no scripts) |
| Collision BSPs with 16-bit indices | Game code: `physics/collision_bsp*.c` | up to 3.3 MB for a BSP, 0.45 to 1.07 MB of collision models | 27 to 38% of each BSP's collision; 0.16 to 0.37 MB of collision models |
| Sound permutations without their name, subtitle and unused fields (124 to 44 bytes), mouth data at 4 bits | Game code: the sound code | 0.25 to 0.85 MB | 0.14 to 0.69 MB |
| Script nodes from 20 to 12 bytes | Game code: `hs` | 89 to 366 KB | 0.04 to 0.14 MB |
| Font glyphs at 4 bits | Game code: text drawing | 114 KB | 0.06 MB |

Projected peaks (shared tags plus the BSP that is largest after the
changes), with the animations at 0.25 degrees / 2 mm:

| Map | Xbox | Converter and renderer only | With the game-code changes |
| --- | --- | --- | --- |
| a10 | 21.67 MB | 16.18 MB | 14.11 MB |
| a30 | 21.48 MB | 15.95 MB | 14.08 MB |
| a50 | 21.77 MB | 16.14 MB | 14.04 MB |
| b30 | 21.29 MB | 15.50 MB | 13.74 MB |
| b40 | 21.78 MB | 16.25 MB | 14.42 MB |
| c10 | 21.55 MB | 15.56 MB | 13.88 MB |
| c20 | 17.16 MB | 12.75 MB | 11.34 MB |
| c40 | 21.80 MB | 15.98 MB | 14.32 MB |
| d20 | 21.97 MB | 16.04 MB | 14.39 MB |
| d40 | 21.86 MB | 15.90 MB | 14.12 MB |
| Multiplayer maps | 7.1 to 8.9 MB | 4.5 to 5.9 MB | 4.2 to 5.4 MB |

- **With the game-code changes,** every campaign level comes to 13.7 to
  14.4 MB. That is inside the 14.5 MB target ("Memory budget": the measured
  MIPS code took 1.5 MB of the first 16 MB), with little margin.
- **With the converter and the renderer alone,** they come to 15.5 to 16.3
  MB, over the target. The game-code changes are needed.
- **Collision is the most valuable code change.** It saves 0.9 to 1.3 MB a
  level.
- **Looser tolerances help a little.** At 0.5 degrees / 3 mm, the campaign
  comes to 13.4 to 14.1 MB.
- **Tag names.** Dropping three quarters of them (keeping those that the
  HUD and the fonts look up by name) saves about 0.1 MB more.

**The converter prototype** (`tools/ps2/converter_prototype.py`) runs four
converter passes on every map without writing anything:

- **Animations.** Each uncompressed animation is decoded (per frame and
  node: an 8-byte rotation, a 12-byte translation and a 4-byte scale, as
  each node's flags say; the layout matched `frame_size` in every animation
  of every map; some, 13 on a10, are poses without frame data, which stay as they are).
  It is then re-encoded in the engine's keyframe format
  (`struct compressed_animation_header`, `model_animations.c`):
  - Each node and channel keeps its default (frame 0).
  - Keyframes are kept only where linear interpolation (normalized for
    rotations, as the decoder does) would leave the tolerance, measured
    after the 6-byte quantization.
  - The last frame is always a keyframe, as the decoder asserts.

  The game already decodes this format, so the converter needs no new
  runtime code.

  | Tolerance | Saving (campaign levels) |
  | --- | --- |
  | Exact (no error beyond the 6-byte rotations) | -1 to 4% (translations stay 12-byte floats in this format) |
  | 0.1 degrees / 1 mm | 26 to 32% |
  | 0.25 degrees / 2 mm | 35 to 41% |
  | 0.5 degrees / 3 mm | 43 to 48% |

  Storing translation keyframes as 16-bit values (a small change to
  `animation_get_keyframe_translation`) saves only 1% more, because
  rotations are most of the keyframes. It is not worth a format change.
  Before choosing a tolerance, look at the hands, weapons and faces in
  cinematics at 0.25 and 0.5 degrees. Errors add up along a skeleton's
  chain of nodes.

  CPU cost: a compressed animation looks up each node's keyframe with a
  binary search each time it is sampled. On the EE, keep each animation
  channel's last keyframe index and search forward from it, because
  animations mostly play forward.
- **Super-high LOD.** Slot 4 of a permutation's `geometry_indices` is the
  most detailed geometry: it is larger than slot 0 in 41 to 75 of the
  permutations of each level, and smaller in 7 to 29 (models whose LODs are
  not ordered by size). Geometry that only slot 4 uses comes to 0.68 to
  1.77 MB of vertices and indices for each level. First-person models have
  a single geometry, so this change never touches them.
- **Collision.** Every collision BSP is measured block by block:
  - A field fits 16 bits when its largest index is below 0x8000 where the
    engine keeps a flag in the sign bit (plane designators, BSP children,
    BSP2D roots), and below 0xFFFF otherwise.
  - Edges go from 24 to 12 bytes, BSP3D nodes from 12 to 6, BSP2D
    references from 8 to 4, surfaces from 12 to 8, BSP2D nodes from 20 to
    16, leaves from 8 to 6.
  - Planes and vertices keep their floats, because collision needs the
    precision.
  - Every block fits, except the BSP3D nodes of the largest BSPs (more than
    32,768 nodes or leaves) on 8 of the 10 campaign levels. Those keep 32
    bits. A 24-bit layout for them would save up to about 0.2 MB more there.

  Smaller collision data also means fewer cache misses in the EE's 8 KB
  data cache, where collision tests spend their time.
- **Scripts.** Every map allocates the scenario's script nodes at the
  maximum, 19,001 nodes of 20 bytes (371 KB). The campaign levels make
  4,557 to 18,735 of them, and the multiplayer maps none. The game uses
  the array in place, and only the developer console adds nodes, so the
  converter can cut the array to the nodes made without any code change.

Keep the triangle strips indexed in the tag data. Pre-built VIF packets
repeat strip vertices, so they can be larger than indexed data. The renderer
builds packets from indexed data into the per-frame packet buffer, using
VIF unpack (an inexpensive DMA job), not the CPU.

### Memory: textures (measured)

The textures are not resident: they are in the map file and stream into the
texture cache as they are drawn. Each campaign map has 35 to 49 MB of them
(a10: 48.7 MB):

| a10 textures by usage | Size | PS2 |
| --- | --- | --- |
| Default (diffuse and others) | 29.2 MB | Half size: 7.3 MB |
| Lightmaps | 10.6 MB | Needed for the environment. Half size, 8-bit palette. |
| Height maps (bump) | 6.4 MB | Not loaded: the GS has no bump mapping |
| Cube maps | 1.3 MB | Small sphere maps |
| Alpha-blend, detail | 1.0 MB | Kept |

A 4-bit palette texture is the same size as DXT1, and an 8-bit one is the
same size as DXT3/5, so palette conversion alone does not save memory. The
savings come from leaving out the bump maps and halving the size of each
texture (dropping the top mipmap level; the display is 640x448). With
those changes, the Xbox's 22 MB texture cache becomes about 22 x 0.87 / 4,
or 4.8 MB, which is the budget. Measure the Xbox cache's real peak use as
well: `xbox_texture_cache.c` reports when it overflows.

### Memory: the disc

The 24 maps are 2.77 GB after inflation. 1.50 GB of that is sound samples.
The PS2's ADPCM is also 4-bit, so the sounds stay about the same size. A
converted disc fits on a single-layer DVD (4.7 GB).

### Game logic: floating point (the largest CPU risk that is easy to fix)

The EE has no double-precision FPU. Each `double` operation is a software
call that takes tens to hundreds of cycles. The game uses `double` in more
places than its 144 explicit uses:

- **Implicit promotion.** In C, `float_value * 0.5` is computed in `double`,
  because `0.5` is a `double` constant. MSVC's x87 code made this free.
  Counting double-precision instructions in the x86 objects shows them
  spread throughout the game: `rasterizer_xbox_environment.c` (96),
  `objects.c` (55), `actor_looking.c` (51), `units.c` (41), `physics.c`
  (30), `actor_moving.c` (25), `real_math.c` (25).
  - Fix: build with GCC's `-fsingle-precision-constant`, and use
    `-Wdouble-promotion` to find what remains.
- **Double-precision libm.** The game calls `fmod` 38 times, `cos` 36, `sin`
  32, `pow` 30, `floor` 39 and `fabs` 263. Many are in code that runs each
  frame or each tick: `periodic_functions.c` (shader and object functions),
  and `model_animations.c` (`fmod(frame, 1.0)` for each animation and each
  object).
  - Fix: a header that maps these to `sinf`, `cosf`, `floorf` and so on.
    Replace `fmod(x, 1.0)` with `x - floorf(x)`. Replace `pow` with
    constant exponents (0.5, 0.25, 2, 4 in `periodic_functions.c`) with
    `sqrtf` and multiplication.
- **Conversions.** `__builtin_rint((double)...)` in `bitmaps_inlines.h` and
  `decals.c` (color packing) should become `rintf` or a float-to-int
  conversion.
- **IEEE behavior.** The EE has no denormals, infinities or NaN. Find
  dependencies with the FTZ, DAZ and trap audit on Linux (Phase 0).

### Game logic: AI

The AI already rations its expensive work. `actor_get_timeslice`
(`ai/actors.c`) gives the "timeslice" to one actor each tick, and only that
actor does its expensive decisions (flee and fight positions, and so on).
But every active actor runs the rest of `actor_update` each tick: input,
perception, situation, emotions, decisions, actions, movement, looking and
combat. Perception does visibility tests against every prop, and those ray
casts go through the collision BSP.

Levers, in order of risk:

1. Give the timeslice less often, or to fewer actors (the threshold of 15
   ticks is a constant).
2. Stagger perception by distance. Near actors and props keep each tick.
   Far ones test visibility every 2 to 4 ticks, at a different phase for
   each actor.
3. Run the movement and look updates of actors that no player can see at
   15 Hz.
4. Lower the per-level actor limits in the converted scenarios, where an
   encounter's size allows it.

Each step changes behavior slightly. Compare saved films (or recorded
input) of the PC and PS2 builds for each level.

### Game logic: work that moves off the CPU

| Work | Xbox | PS2 |
| --- | --- | --- |
| Visible BSP triangle lists | The CPU builds index buffers each frame (`structure_render_build_dynamic_triangles`) | Draw whole precompiled cluster/material batches. The GS's fill rate makes overdraw inexpensive. |
| Skinning, model lighting, fog | Vertex shaders | VU1 microprograms |
| Matrix and vector math (`matrix_math.c`, `real_math.h`, animation blending) | x87/SSE | VU0 macro mode (COP2), with inline assembly behind the same C functions |
| Particle and contrail updates | CPU, each frame | A VU0 loop over the particle arrays; lower particle limits |
| Sound obstruction ray casts | Each tick (the port's change) | Round-robin, a few sounds each tick |
| Frame interpolation (`render_interpolation.c`) | The port's | Not used. 30 frames each second. |

### Game logic: the cache

The EE has an 8 KB data cache and no L2 cache, so each access to memory not
in the cache costs about 40 cycles or more. The game follows pointers from
object data into tag definitions (`tag_get`) many times each tick.

- Profile on hardware (or in PCSX2 with EE cache emulation on, see below),
  and find the hot structures.
- Copy the per-tick working sets of the hottest loops (the actor iterator,
  collision BSP traversal) into the 16 KB scratchpad.
- Compile hot files with `-O2` and the rest with `-Os`. With a 16 KB
  instruction cache, smaller code is often faster.

### PCSX2 for development

- Keep PCSX2's EE memory at 32 MB, so that a build that does not fit fails
  in the emulator.
- PCSX2 does not emulate the EE's caches or exact cycle timing by default.
  Cache-heavy code runs faster there than on hardware, so PCSX2 profiles
  are optimistic.
  - Profile with PCSX2's EE cache emulation on (it is slow).
  - Check timing on a real console at each gate. A PS2 that boots homebrew
    (for example with FreeMcBoot) and ps2link over the network adapter is
    enough.
- Add a debug overlay that shows free EE memory, the peak use of each pool
  and the tick time divided by subsystem.

### Next steps

1. Choose the animation tolerance by looking at cinematics at 0.25 and
   0.5 degrees.
2. Add a debug log of each game state pool's peak use, and the texture and
   sound caches' peak use, to the Linux build. Play every level.
3. Add the float fixes (`-fsingle-precision-constant`, float libm) to the
   Linux build first. They also make x86 slightly faster, and saved films
   show whether game behavior changes.

Done: `render.environment_surface_flags` is a 16 KB bit vector, not 512 KB
(090c8377). It compiles in all 505 game and port files. Play a level on
Linux or Windows to confirm that the level geometry still draws.

## Scope

The PS2 build includes:

- The campaign and split-screen multiplayer, from the user's Xbox maps.
- Xbox capacities, 30 frames each second without interpolation, and the
  Xbox HUD and fonts.

The PS2 build leaves out, at least at first:

- Internet play, voice chat, UPnP, Discord, the updater, mbedtls, Opus,
  miniupnpc.
- System link. A later option is the network adapter with ps2sdk's lwIP.
  It cannot play with PC machines, because the netcode needs the same
  capacities on every machine (`halo_port_capacity.h`).
- Custom Edition maps (made for PC memory budgets).
- The high-res HUD and text, anti-aliasing, and the enlarged pools.

Legal: as now, the project distributes no game data. The converter runs on
the user's PC from the user's own disc image and writes the PS2 disc image.

## Build integration

- `tools/ps2_build.py` with `generate_ps2_build` and
  `ps2_configure_inputs`, called from `configure.py` like the other
  platforms. A `port/ps2/port.json` lists the sources and excludes the
  editor, tools and netcode extras.
- Targets: `ninja ps2` makes `build/ps2/halo.elf`. `ninja ps2_converter`
  makes the host tool. `ninja ps2_iso` makes a disc image from converted
  maps.
- New folders: `port/ps2/src` (platform layer), `port/ps2/vu1` (VU1
  microcode), `port/ps2/irx` (IOP modules), `port/ps2/include`,
  `port/ps2/README.md`. The renderer goes in `source/rasterizer/ps2`, and
  the converter in `port/tools/ps2_map_convert.c` (reusing
  `cache_file_formats.c` and the tag schema).
- CI: the `ps2dev/ps2dev` Docker image builds the ELF. Run tests in PCSX2
  (boot to the main menu, load a level). Measure timing only on real
  hardware.

## Phases and gates

Effort estimates are rough, for one experienced developer working full time.

### Phase 0: measure on the PC (3 to 4 weeks)

No PS2 code. Instrument the Linux build:

- For each map: the tag data size, divided into geometry, collision,
  scripts, bitmap and sound headers, and other data.
- The highest use of the texture cache, the sound cache and every game
  state pool, with Xbox capacities.
- The CPU time of each 30 Hz tick, divided into AI, physics, objects,
  scripts and effects, through the menus and every campaign level.
- Do the GCC build, the struct layout manifest, the alignment audit
  (`-fsanitize=alignment`) and the floating-point audit (FTZ, DAZ and
  traps).

**Gate 0:** a believable path to about 15 MB of tag data for each campaign
level, and tick costs that are acceptable when multiplied by 4 to 5. If
not, go to "Open questions".

Status: the memory half is met on paper. The retail maps project to 13.7 to
14.4 MB ("Memory: tag data"). The tick costs are not measured yet. The
multiplayer maps need half the budget, so a multiplayer level is the
natural first target for Phase 1 and the first renderer milestones.

### Phase 1: the simulation on hardware (2 to 3 months)

Done so far:

- The ps2dev toolchain (v2.0.0) runs on the development Mac, and PCSX2
  runs programs from it (`tools/ps2/pcsx2_run.py`).
- All 466 game files compile for the EE (`tools/ps2/compile_check.py`).
  The objects have 4.49 MB of code at `-Os` (5.90 MB at `-O2`) and 3.39 MB
  of `.bss`.
- `ninja ps2` links `build/ps2/halo.elf` (`tools/ps2_build.py`), with the
  game, the port's game units, eight files of the Linux platform layer
  (`port/ps2/port.json`), musl's maths, zlib and `port/ps2/src`. 176
  functions of the platform layer are still stubs that name themselves and
  stop (`tools/ps2/unimplemented_stubs.py`). In PCSX2 the game creates its
  Direct3D device and stops at `XPhysicalAlloc` (`physical_memory_allocate`,
  the memory map, next).
- Direct3D: `port/ps2/src/ps2_d3d_null.c` draws nothing, for the Xbox
  rasterizer to run on until the PS2's renderer replaces it. Resources keep
  their Xbox headers and memory (as `d3d8_resources.c`), states are kept,
  visibility tests see nothing, and the vertical blank callback runs at the
  PS2's vertical blank (`ps2_vblank.c`).
- The kernel: `port/linux/src/xbox_kernel.c` builds as it is. ps2sdk's
  pthreads, condition variables with timeouts, recursive mutexes, clocks
  and sleeps behave as it needs (measured in PCSX2).
- Threads: the EE's kernel does not share time between threads of one
  priority, and the Xbox's did. Every game thread runs at ps2sdk's pthread
  priority (92, main's moved down to it before `main`), and a thread above
  them rotates that priority's queue at each vertical blank.
- Names: ps2sdk's libraries define some names the port also defines
  (newlib's `_open`, `_close` and the rest; the EE kernel's `CreateThread`
  and `ResumeThread`, which the Win32 ones replaced, stopping the program
  before `main`). The PS2 build keeps the libraries' (`msvc_crt.c`) or
  renames the port's (`halo_ps2_prefix.h`), and
  `tools/ps2/unimplemented_stubs.py` now refuses a build that redefines a
  library's name.
- The ELF loads 25.3 MB: 6.4 MB of code and constants, 0.4 MB of data and
  18.5 MB of `.bss`, of which 13.5 MB is the port's game units for features
  the PS2 does not have (Custom Edition maps 6.4 MB, the tag validator
  2.9 MB, the 128-player netcode 3.7 MB). They go next, with stubs in their
  place, before the game's memory map (`physical_memory_map.c`) can fit.
- The memory map: the PS2's capacities are the Xbox's
  (`halo_port_capacity.h`, `halo_port_limits.h`: 16 players on 4 machines)
  and the Custom Edition maps' limits are 1 map, which took the ELF's
  `.bss` from 18.5 MB to 5.6 MB. At start the game gets its game state
  (3.3 MB), a 9 MB tag cache (the largest multiplayer map's tags and BSP:
  the campaign's wait for the converter), a 2 MB texture cache and a 0.5 MB
  sound cache, with about 5 MB of the 32 MB left. The map's tags and BSPs
  are relocated to the tag cache as they load (above, "Relocating the tag
  data"). In PCSX2 the game now stops at `config_string`, the port's
  settings.
- newlib names its system calls `_open`, `_close`, `_read`, `_write`,
  `_lseek`, `_stat`, `_fstat` and `_mkdir`, which `msvc_crt.c` also defined
  for MSVC; on the PS2 newlib's stay (`_close` calling `close` called
  itself).
- Settings and files: `port_config.c` reads and writes `config.toml` as
  on Android (stdio, no SDL or environment), and the file layer
  (`xbox_files.c`, `posix_files.c`) runs on newlib and ps2sdk's devices,
  with `pread`/`pwrite` and the atomic operations GCC calls for the EE
  (which has no LL/SC) in `port/ps2/src`. The data root is the ELF's
  folder. PCSX2's `host:` device differs from POSIX in three ways the
  layer covers: a missing file fails with EPERM or EIO, not ENOENT;
  removing one makes a directory of its name; and there is no
  `ftruncate` (a file is made longer by writing its last byte). In
  PCSX2 the game writes its settings, log and saves, makes the Xbox's six
  map cache files, finds the maps, allocates its game state and stops in
  the rasterizer's start: the heap's last 5 MB do not hold the debug
  drawing's buffers (2 MB, not needed on the PS2). 140 stubs are left.

Still to do:

- The heap: the debug drawing's buffers and other allocations the PS2
  does not need, until the game's start fits.
- Maps without the cache files: the Xbox decompresses each map from the
  DVD to its hard disk (`cache_files_windows.c`, six files of 730 MB in
  all). A retail PS2 has no disk, so the PS2 reads the converter's
  maps, already decompressed, straight from the disc. In PCSX2 for now
  the cache files are on `host:`.
- A first converter that only relocates the tag data. Test it on Linux
  first.
- A null rasterizer and a null sound backend. Load a campaign level,
  replay recorded input (or a saved film), and log the tick times.

**Gate 1:** on a real PS2, a typical campaign tick takes 15 ms or less,
with a clear plan for the worst cases (large AI fights, many effects).

### Phase 2: the map converter (2 to 3 months, partly in parallel with Phase 3)

Textures to CLUT in GS layout, sounds to VAG, geometry to VIF packets,
tag data reduction, disc layout. A report of each map's budget, like
`cache_file_report`.

### Phase 3: the renderer (6 to 12 months)

In this order, each one a milestone: menus and text, BSP with lightmaps,
models, first-person weapons and the HUD, transparent shaders and effects,
fog, decals, detail objects, water, glass, active camouflage, shadows,
screen effects, split screen.

### Phase 4: sound, input, saves and streaming (2 to 3 months)

The IOP sound module and streaming, libpad and multitap, memory card saves,
texture and geometry streaming from DVD with prefetch.

### Phase 5: performance (ongoing, 3 or more months)

VU0 macro mode for `matrix_math.c` and `real_math.h`, scratchpad buffers
for hot loops, data cache layout of hot structures, packet building on the
scratchpad, the order of AI updates, and per-level caps on particles and
effects.

### Phase 6: finish (2 to 3 months)

NTSC and PAL, 480p, disc image creation, a README for users and builders,
and optionally system link.

Total: about 1.5 to 2.5 developer-years after Gate 1. Most of the
uncertainty is in Phase 3 and Phase 5.

## Risks

| Risk | Likelihood | Effect | Reduction |
| --- | --- | --- | --- |
| Tag data does not fit in about 15 MB | Medium | Fatal for 32 MB hardware | Measure in Phase 0. Consider the 128 MB target. |
| The simulation is too slow on the EE | Medium to high | 15 to 20 frames each second, or slow motion | Gate 1 on hardware. VU0 math, data cache work, lower AI update rates for distant actors. |
| Hidden dependence on unaligned access or IEEE floating point | High that some exist, low that they cannot be fixed | Crashes, physics differences | Sanitizer and FTZ audits on Linux first. |
| Shader combinations that the GS cannot approximate | Medium | Visual differences | The converter lists the combinations. Accept simplifications. |
| DVD seeks during play | Medium | Stalls, textures that appear late | Disc layout by BSP, prefetch from `predicted_resources.c`, larger IOP buffers. |
| Scripts or AI that depend on removed tag data | Low to medium | Level errors | The tag validator runs on every converted map. Play every level. |

## Open questions

1. **Target hardware.** Decided: a retail PS2 (32 MB), developed on PCSX2
   at 32 MB.
2. **HDD.** May the port require the HDD of early models (as Final Fantasy
   XI did)? It makes streaming much easier, but most consoles do not have
   one.
3. **Scope.** Is the campaign the first goal, or split-screen multiplayer?
   Multiplayer maps need 5 to 7 MB of tag data against 15 MB for the
   campaign (measured), so they are a faster first target.
4. **Frame rate.** Is 30 frames each second required, or is a lower frame
   rate acceptable at first?
