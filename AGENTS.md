# Notes for agents

This fork ports the Halo: Combat Evolved decompilation (Xbox build
01.01.14.2342, `cachebeta.exe`) to the PlayStation 2. It starts from
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE),
the port to Linux, Windows and Android. Start with [README.md](README.md) (the
PS2 status comes first) and [docs/ps2_port_plan.md](docs/ps2_port_plan.md).

## Layout

| Path | Contents |
| --- | --- |
| `source/` | The game: 466 C files of the decompilation, compiled as they are with few changes |
| `port/linux/` | The platform layer and game-side code the Linux, Windows and Android ports share; the PS2 build takes its game files from here (`port/linux/port.json`) |
| `port/ps2/` | What the PS2 build adds: its platform layer (`src/`), prefix header (`include/`) and inputs (`port.json`) |
| `port/ps2/samples/masterchief/` | A stand-alone PS2 sample, not the game's build: it draws the Master Chief running with the assault rifle (EE and GS, gsKit), from assets its exporter bakes out of an Xbox map |
| `tools/ps2/`, `tools/ps2_build.py` | The PS2 tools (map measuring and converting, PCSX2 runners) and the `ninja ps2` build ([tools/ps2/README.md](tools/ps2/README.md)) |
| `docs/ps2_port_plan.md` | The plan, the memory budget and the phases |
| `docs/ps2_ee_performance.md` | What the EE does slowly (measured), how to send primitives to the GS cheaply, how to profile in PCSX2 and its quirks: read before writing per-vertex or per-tick code |
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
  `assets/maps/bloodgulch.map` and the ps2dev toolchain with gsKit.

### Checking the Master Chief sample

It draws, so it needs PCSX2's window (`tools/ps2/pcsx2_run.py` is for programs
that only print):

- `PCSX2 -nogui -batch -elf masterchief.elf`; then capture that window only
  (`screencapture -l <window id>`, the id from `CGWindowListCopyWindowInfo`),
  never the whole screen, and stop only the process you started.
- It is time driven, so for a still frame add `EXTRA_CFLAGS="-DDEBUG_SECONDS=1.45f"`
  to `make`; add `-DNO_SPECULAR_PASS` to compare a shading pass with and
  without it (build both ELFs, capture both). Compare stills pixel by pixel
  (crop the window's title bar first): a change to the shading should move
  nothing by more than a few levels.
- `make EXTRA_CFLAGS=-DPROFILE` prints the frame's phases in milliseconds and
  the frame rate; `tools/ps2/pcsx2_run.py` shows them. The target is one
  vsync (60 fps, NTSC). `-DNO_VU0` keeps the transforms on the FPU.
- Video memory is full. The map scene (the default build) has two 640x448
  16-bit buffers with dithering, a 16-bit Z buffer, the Chief's five textures
  and the map's: about 3.98 of 4.19 MB, and the sample prints what it allocated at
  start-up (`VRAM: ...`; gsKit prints "Not enough VRAM" when an allocation
  fails, and goes on). Work out what a new texture takes before adding it.
- gsKit sizes its buffers from the console's own video mode (PAL: 512 lines)
  before the sample forces NTSC: set `gs->Height` with `gs->Mode`.
- GS coordinates are 12.4 fixed point in 16 bits around an origin gsKit puts
  at the screen's corner: a vertex further than 2048 pixels from it wraps. The
  map clips at 1400 pixels outside the screen.

## Rules

- **Game data stays out of the repository.** Keep the maps and disc image in
  `assets/`. The sample's generated `src/assets.[ch]`, `src/map_assets.[ch]` and the ELF
  hold the game's models and textures: they are gitignored; never commit
  them, nor screenshots, GIFs or videos made from them.
- Change `source/` as little as possible and mark each change from the
  original with a comment that starts `port:`.
- The Xbox SDK's headers are never read or copied (`port/include/xdk` was
  written without them).
- Branches off `ps2` are made with `git switch -c <name> --no-track ps2/ps2`:
  a branch that tracks `ps2/ps2` makes a plain `git push` land on `ps2`
  itself. Push with an explicit refspec (`git push ps2 <name>:refs/heads/<name>`)
  and never force-push `ps2`.
- Commit subjects are plain sentences saying what changed; the body says why
  and what was checked. Say when something was checked only in PCSX2 and not
  on hardware.
