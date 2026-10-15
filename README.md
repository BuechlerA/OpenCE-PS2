# Halo: Combat Evolved for the PlayStation 2 (OpenCE-PS2)

A port of the Halo: Combat Evolved decompilation (Xbox build 01.01.14.2342,
`cachebeta.exe`) to the PlayStation 2. It starts from
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE),
the port to Linux, Windows and Android, which started from
[bnunu/halo-1](https://github.com/bnunu/halo-1), a fork of
[punpckhdq/halo](https://github.com/punpckhdq/halo). This repository keeps
only what the PS2 port uses.

The target is a retail PS2 with 32 MB of memory. Development uses the PCSX2
emulator, with its memory kept at 32 MB.

## Status

Phase 1 of [the plan](docs/ps2_port_plan.md) (the simulation on the PS2's
CPU) is under way:

- All 466 C files of the game compile for the Emotion Engine with ps2dev's
  GCC and link (`ninja ps2`). The game's structures keep their Xbox layout.
- In PCSX2 the game starts, loads converted maps and plays the 13
  multiplayer levels and the campaign level `c20` with a scripted player.
  It draws nothing (`port/ps2/src/ps2_d3d_null.c`), has no sound and no
  gamepad yet.
- A first map converter (`tools/ps2/convert_map.py`) makes an Xbox map into
  what the PS2 reads. The converter's reductions, the PS2 renderer, the
  sound, the input and the streaming are not written (Phases 2 to 4).
- Not measured: the game's speed on a real PS2. In PCSX2 the simulation
  takes 4 to 15 ms a tick on the multiplayer levels, and 45 ms on `c20`.

The measurements behind the plan (memory per map, the cost of each saving,
the EE's slow operations) are in the plan and in [docs/research](docs/research).

## Layout

| Path | Contents |
| --- | --- |
| `source/` | The game: 466 C files of the decompilation, compiled as they are with few changes. Changes from the original are marked with a comment that starts `port:`. |
| `port/ps2/` | What the PS2 build adds: `src/` the platform layer (the Xbox kernel, files and DirectX replaced for the EE), `game/` the port's changes to the game, `include/` the MSVC environment and prefix header, `port.json` the build's inputs |
| `port/include/xdk/` | The Xbox SDK declarations the game uses, written without the SDK ([README](port/include/xdk/README.md)) |
| `port/third_party/` | zlib, musl's maths and the TOML parser |
| `tools/ps2_build.py`, `tools/ps2/` | The `ninja ps2` build, the map tools and the PCSX2 runners ([tools/ps2/README.md](tools/ps2/README.md)) |
| `docs/ps2_port_plan.md` | The plan, the memory budget and the phases |
| `docs/research/` | Measurements and pitfalls from the renderer prototype: the EE, the GS's video memory, VU1 and Blood Gulch's data |
| `assets/` | Game data for local runs (git ignores it) |

## Build

1. Install Python 3.9 or later, [ninja](https://ninja-build.org/) and the
   [ps2dev](https://github.com/ps2dev/ps2dev) toolchain. Set `PS2DEV`,
   `PS2SDK` and `PATH` as its README says.
2. In the root of the repository, enter `python configure.py` (or
   `python configure.py --ps2dev DIR`; add `--release` for a build that does
   not check assertions).
3. Enter `ninja ps2`. The result is `build/ps2/halo.elf`.

Run it in PCSX2 with `tools/ps2/pcsx2_run.py`, and convert maps first:
[tools/ps2/README.md](tools/ps2/README.md).

## Game data

This repository does not include the game data, and the PS2 port will not
either. Use your own Xbox disc image of the game. Keep it in `assets/` at the
root of the repository.

## Removed files

The repository was cleaned up for the PS2 port. The Linux, Windows and
Android ports, the Custom Edition and desktop-only code, the CI, the
optimization profiles, the Master Chief / Blood Gulch sample and the PC
tools are not in the tree. The commit before the cleanup is the tag
`pre-ps2-cleanup`: use `git show pre-ps2-cleanup:<path>` to read a file, or
`git worktree add ../old pre-ps2-cleanup` to build one of them. Comments in
`source/` and in `port/ps2` that name a file by a path that is gone mean that
tag. The upstream project is at
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE).

## License

[LICENSE.md](LICENSE.md). Authors: [AUTHORS.md](AUTHORS.md).
