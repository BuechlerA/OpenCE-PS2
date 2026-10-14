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
| `assets/` | Game data for local runs (gitignored) |

## Building and testing

- `python configure.py` writes `build.ninja`; `ninja ps2` builds the game for
  the PS2 with the ps2dev toolchain (`--ps2dev`, or `PS2DEV`).
- Run PS2 programs in PCSX2 without its window with `tools/ps2/pcsx2_run.py`;
  keep the emulator's memory at 32 MB so it enforces the retail budget.
- The Master Chief sample is built apart from `ninja`: in
  `port/ps2/samples/masterchief` run `python tools/export_assets.py`, then
  `make` ([README](port/ps2/samples/masterchief/README.md)). It needs
  `assets/maps/bloodgulch.map` and the ps2dev toolchain with gsKit.

## Rules

- **Game data stays out of the repository.** Keep the maps and disc image in
  `assets/`. The sample's generated `src/assets.c`, `src/assets.h` and the ELF
  hold the game's models and textures: they are gitignored; never commit
  them, nor screenshots, GIFs or videos made from them.
- Change `source/` as little as possible and mark each change from the
  original with a comment that starts `port:`.
- The Xbox SDK's headers are never read or copied (`port/include/xdk` was
  written without them).
- Commit subjects are plain sentences saying what changed; the body says why
  and what was checked. Say when something was checked only in PCSX2 and not
  on hardware.
