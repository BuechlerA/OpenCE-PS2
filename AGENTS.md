# Notes for agents

This fork ports the Halo: Combat Evolved decompilation (Xbox build
01.01.14.2342, `cachebeta.exe`) to the PlayStation 2. It starts from
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE),
the port to Linux, Windows and Android, whose own code is no longer in the
tree. Start with [README.md](README.md) and
[docs/ps2_port_plan.md](docs/ps2_port_plan.md).

The branch the PS2 work lands on is `main` of this fork (`BuechlerA/OpenCE-PS2`,
the `ps2` remote). Upstream's own `main` is the local `upstream-main`, tracking
`origin/main`. Pull requests go to the fork's `main`.

## Layout

| Path | Contents |
| --- | --- |
| `source/` | The game: 466 C files of the decompilation, compiled as they are with few changes |
| `port/ps2/src/` | The platform layer: the Xbox kernel, files and XAPI (`xbox_*.c`, `msvc_*.c`, `posix_files.c`, `port_config.c`) and the PS2's own files (`ps2_*.c`: memory, files, input, vblank, atomics, the null Direct3D, the null platform, the benchmark) |
| `port/ps2/game/` | The port's changes to the game that are not in `source/` (co-op, netcode, menus, tag fixes...), all compiled with the game's flags |
| `port/ps2/include/` | The MSVC environment (`halo_ps2_prefix.h`, `halo_linux_prefix.h`, the CRT header shims) and the memory capacities (`halo_port_capacity.h`, `halo_port_limits.h`) |
| `port/ps2/port.json` | The build's inputs: the game's files, defines and include directories |
| `port/include/xdk/` | The Xbox SDK declarations, written without the SDK |
| `port/third_party/` | zlib, musl's maths, tomlc17 |
| `tools/ps2_build.py`, `tools/ps2/` | The `ninja ps2` build, the map tools and converter, and the PCSX2 runners ([tools/ps2/README.md](tools/ps2/README.md)) |
| `docs/ps2_port_plan.md` | The plan, the memory budget and the phases |
| `docs/ps2_kernel_plan.md` | The PS2 boot and runtime layer (milestones K0 to K7): how the PS2's architecture replaces the Xbox kernel's design |
| `docs/ps2_renderer_design.md` | The design of the game's PS2 renderer (`source/rasterizer/ps2/`, not written yet): where it plugs in behind `rasterizer.h`, the frame's DMA chains, the VU1 programs, the texture cache, the hardware gate, the first milestone's files and steps |
| `docs/ps2_campaign_roadmap.html` | The campaign's roadmap as a page |
| `docs/research/` | Measured findings from the renderer prototype: `ee_performance.md` (what the EE does slowly, read before writing per-vertex or per-tick code), `blood_gulch_map.md` (Blood Gulch's data and how it was drawn on the GS), `vram_and_vu1.md` (video memory, GS fog, mip audit, the Chief on VU1, VIF/VU1/PCSX2 pitfalls: read before touching textures, frame buffers, GIF paths or VU1), `first_person_controller.md` (the sample's first-person controller: what was ported from which file of `source/`, the walking collision) |
| `assets/` | Game data for local runs (gitignored) |

### What was removed

The Linux, Windows and Android ports, Custom Edition, the tag validator and
schema C tables, the CI, the PGO profiles, the PC tools and the Master Chief /
Blood Gulch sample are not in the tree. They are in the tag `pre-ps2-cleanup`
(`git show pre-ps2-cleanup:<path>`, or `git worktree add ../old pre-ps2-cleanup`).
Comments that name a file by a `port/linux/...`, `port/android/...` or
`port/windows/...` path, or by a deleted file, mean that tag. The sample, and
the Linux build with its sanitizer and flush-to-zero audits, can be rebuilt
from it when needed.

## Building and testing

- `python configure.py` writes `build.ninja`; `ninja ps2` builds
  `build/ps2/halo.elf` with the ps2dev toolchain (`--ps2dev`, or `PS2DEV` and
  `PS2SDK`). A build takes about 10 s on a laptop with `-j` at its default.
- `python tools/ps2/compile_check.py` compiles each game file and groups the
  errors.
- The game's units are compiled with `-fsingle-precision-constant` (the EE has
  no double FPU; `port/include/halo_math_float.h`) and `-Wdouble-promotion`,
  which `-w` hides: `HALO_PS2_WARNINGS=1 python configure.py` shows each float
  that still becomes a double.
- Run PS2 programs in PCSX2 without its window with `tools/ps2/pcsx2_run.py`;
  keep the emulator's memory at 32 MB so it enforces the retail budget.
  `tools/ps2/pcsx2_bench.py` runs the tick benchmark.
- A check that nothing moved after a build-system change: `size build/ps2/halo.elf`
  (text, data, bss) and the set of objects under `build/ps2/obj`.
- Programs that draw need PCSX2's window (`PCSX2 -nogui -batch -elf
  /absolute/path/x.elf`; a relative path boots to nothing). Capture that window
  only (`screencapture -l <window id>`), never the whole screen, and stop only
  the process you started.

## Rules

- **Game data stays out of the repository.** Keep the maps and disc image in
  `assets/`. Never commit models, textures, converted maps, ELFs that embed
  them, or screenshots, GIFs or videos made from the game.
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
