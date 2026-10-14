# Notes for agents

This fork ports the Halo: Combat Evolved decompilation (Xbox build
01.01.14.2342, `cachebeta.exe`) to the PlayStation 2. It starts from
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE),
the port to Linux, Windows and Android. Start with [README.md](README.md) (the
PS2 status comes first) and [docs/ps2_port_plan.md](docs/ps2_port_plan.md).

The branch the PS2 work lands on is `main` of this fork (`BuechlerA/OpenCE-PS2`,
the `ps2` remote); it was called `ps2` until October 2026. Upstream's own `main`
is the local `upstream-main`, tracking `origin/main`. Pull requests go to the
fork's `main`.

## Layout

| Path | Contents |
| --- | --- |
| `source/` | The game: 466 C files of the decompilation, compiled as they are with few changes |
| `port/linux/` | The platform layer and game-side code the Linux, Windows and Android ports share; the PS2 build takes its game files from here (`port/linux/port.json`) |
| `port/ps2/` | What the PS2 build adds: its platform layer (`src/`), prefix header (`include/`) and inputs (`port.json`) |
| `port/ps2/samples/masterchief/` | A stand-alone PS2 sample, not the game's build: it draws the Master Chief running with the assault rifle on Blood Gulch's terrain (EE, VU0, VU1 and GS, gsKit), from assets its exporters bake out of an Xbox map |
| `tools/ps2/`, `tools/ps2_build.py` | The PS2 tools (map measuring and converting, PCSX2 runners) and the `ninja ps2` build ([tools/ps2/README.md](tools/ps2/README.md)) |
| `docs/ps2_port_plan.md` | The plan, the memory budget and the phases |
| `docs/ps2_ee_performance.md` | What the EE does slowly (measured), how to send primitives to the GS cheaply, how to profile in PCSX2 and its quirks: read before writing per-vertex or per-tick code |
| `docs/ps2_blood_gulch_map.md` | What Blood Gulch's data is (BSP, shaders, bitmap formats, scenery, sky), how the sample draws it on the GS, the video memory and EE budgets, the pitfalls met, and what is not done |
| `docs/ps2_vram_and_vu1.md` | Where the sample's video memory goes (measured), the savings and what each cost visually, the mip audit, 512-wide buffers, the GS's fog, the Chief on VU1, what was tried and rejected, and the pitfalls (VIF codes, VU1 flags, PCSX2 capture): read before touching textures, the frame buffers, the GIF paths or VU1 |
| `assets/` | Game data for local runs (gitignored) |

## Building and testing

- `python configure.py` writes `build.ninja`; `ninja ps2` builds the game for
  the PS2 with the ps2dev toolchain (`--ps2dev`, or `PS2DEV`).
- Run PS2 programs in PCSX2 without its window with `tools/ps2/pcsx2_run.py`;
  keep the emulator's memory at 32 MB so it enforces the retail budget.
- The Master Chief sample is built apart from `ninja`: in
  `port/ps2/samples/masterchief` run `python tools/export_assets.py` and
  `python tools/export_map.py`, then `make`
  ([README](port/ps2/samples/masterchief/README.md)). It needs
  `assets/maps/bloodgulch.map` and the ps2dev toolchain with gsKit. The map
  scene draws the Chief's textured passes on VU1: `src/chief_vu1.vsm` is
  assembled with `dvp-as` (in the toolchain's `dvp/bin`, which the shell does
  not put on PATH); `make NO_CHIEF_VU1=1` is the EE's path.

### Checking the Master Chief sample

It draws, so it needs PCSX2's window (`tools/ps2/pcsx2_run.py` is for programs
that only print):

- `PCSX2 -nogui -batch -elf /absolute/path/masterchief.elf` (a relative path
  makes PCSX2 boot to nothing and the window stays black); then capture that
  window only (`screencapture -l <window id>`, the id from
  `CGWindowListCopyWindowInfo`, the window about 640 points wide), never the
  whole screen, and stop only the process you started. A capture can come out
  black: check its brightness and retry. Captures are 2x on a Retina screen
  (the Chief is about 125 px tall in the frame, 250 in a capture).
- It is time driven, so for a still frame add `EXTRA_CFLAGS="-DDEBUG_SECONDS=2.6f"`
  to `make` (the angles 0.6, 2.6 and 3.4 s show him well; 1.45 has a bush in
  front of him and 3.0 is black); add `-DNO_SPECULAR_PASS` to compare a shading
  pass with and without it (build both ELFs, capture both). Compare stills
  pixel by pixel (crop the window's content area first): a change that is
  meant to move nothing (a trimmed mip level, the VU1 path against the EE's)
  moves less than 0.01/255 on average; shading changes move a few levels.
- `make EXTRA_CFLAGS=-DPROFILE` prints the frame's phases in milliseconds and
  the frame rate; `tools/ps2/pcsx2_run.py` shows them. The target is one
  vsync (60 fps, NTSC). `-DNO_VU0` keeps the transforms on the FPU. The orbit
  steps by frame in this build, so builds see the same views; compare the
  same one-second windows (the cost runs from 8 to 14 ms over an orbit), and
  the EE's busy time is the phases without `sync_flip`.
- Video memory: the map scene (the default build) has two 512x448 16-bit
  buffers with dithering (displayed at 640 wide: two focal lengths), a 16-bit
  Z buffer, the Chief's five textures (base map 256, rifle 128, masks 128 and
  4-bit, ammo display 32) and the map's, with the mip levels the camera's orbit
  never reads left out (`tools/mip_floor.json`), the cliff's base map at
  128x256: 2.30 of 4.19 MB, 1.89 MB spare. The sample prints what it allocated at start-up (`VRAM: ...`) and
  stops if a texture did not fit (gsKit's failed allocation is address 0, the
  frame buffer, and the texture would draw from it). Work out what a new
  texture takes before adding it; masks and other greys go in 4-bit. The
  frame buffers are 41% of the memory before and the map's base maps most of
  the rest: the Chief's textures were never the big spender
  ([docs/ps2_vram_and_vu1.md](docs/ps2_vram_and_vu1.md)). After a change to a
  texture's size or the screen width, regenerate `mip_floor.json` from scratch
  (the audit, `tools/mip_audit.py`), since the levels asked for move with both.
- The GS has no shaders: it blends, modulates by a texture and a vertex colour,
  tests depth and fogs (a coefficient per vertex, blended toward FOGCOL in a
  pass). Fog has to come after the passes that multiply, so the sample fogs
  the base pass and scales FOGCOL (`FOG_HAZE_GAIN`); an exact pass costs
  2 ms of the EE.
- VU1 draws through GIF path 1, gsKit's queue is path 3: order a frame so that
  the clear goes first and its FINISH is waited for, the VU1 stream (which ends
  with a FINISH) next, and gsKit's map queue last (it waits for that FINISH).
  The VIF1 chain's tags carry two VIF codes before their data, so an UNPACK
  goes in the second; 8-bit UNPACK data must be zero-padded to a quadword.
  VU1's data memory is readable from the EE (0x1100C000) when it has stopped:
  dump it to debug the microprogram.
- VU0 in macro mode (COP2) stalls on every dependent instruction: a loop of
  one vertex is as slow as the FPU, so work three or four vertices at once,
  instruction by instruction (the specular code is generated:
  `tools/gen_spec_vu0.py` -> `src/spec_vu0.h`; `-DSPEC_CHECK` compares it
  with the FPU's). The registers vf1 to vf31 are free between calls but the
  map code reloads its own at the start of `draw_map`.
- gsKit sizes its buffers from the console's own video mode (PAL: 512 lines)
  before the sample forces NTSC: set `gs->Height` with `gs->Mode`.
- GS coordinates are 12.4 fixed point in 16 bits around an origin gsKit puts
  at the screen's corner: a vertex further than 2048 pixels from it wraps. The
  map clips at 64 pixels outside the screen: that margin also decides which chunks and triangles are processed and sent to the
  GS, so a wide one costs EE time (1,400 pixels: 3 ms) and GS fill
  ([docs/ps2_vram_and_vu1.md](docs/ps2_vram_and_vu1.md)); never go over 1,400.

## Rules

- **Game data stays out of the repository.** Keep the maps and disc image in
  `assets/`. The sample's generated `src/assets.[ch]`, `src/map_assets.[ch]` and the ELF
  hold the game's models and textures: they are gitignored; never commit
  them, nor screenshots, GIFs or videos made from them.
- Change `source/` as little as possible and mark each change from the
  original with a comment that starts `port:`.
- The Xbox SDK's headers are never read or copied (`port/include/xdk` was
  written without them).
- Branches off the fork's `main` are made with `git switch -c <name> --no-track ps2/main`:
  a branch that tracks `ps2/main` makes a plain `git push` land on `main`
  itself. Push with an explicit refspec (`git push ps2 <name>:refs/heads/<name>`)
  and never force-push `main`. Open pull requests against `main`.
- Commit subjects are plain sentences saying what changed; the body says why
  and what was checked. Say when something was checked only in PCSX2 and not
  on hardware.
