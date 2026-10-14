# PS2 tools

These tools support the PS2 port ([docs/ps2_port_plan.md](../../docs/ps2_port_plan.md)):

- The map tools measure the Xbox maps for the PS2's memory budget. They
  read the maps and write reports. They do not change or convert a map.
- The build tools compile the game for the PS2 and run PS2 programs in
  PCSX2.

The tools need Python 3.9 or later. The map tools also need a disc image of
Halo: Combat Evolved for the Xbox. Keep the image in `assets/` at the root
of the repository, which git ignores. Do not put it anywhere else in the
repository.

| Tool | Function |
| --- | --- |
| `map_budget.py` | The memory each map takes in the Xbox's 22 MB tag cache, by tag group and tag block: the shared tags, each structure BSP, and the peak. Also the textures and sounds that stay in the map file. |
| `converter_prototype.py` | Runs four passes of the PS2 map converter without writing anything: animations in the engine's keyframe format at four tolerances, no super-high LOD, collision BSPs with 16-bit indices, script nodes trimmed. Reports the bytes each saves. |
| `budget_projection.py` | Combines the results of the two tools above into each map's projected PS2 peak. |
| `export_tag_schema.py` | Makes `tag_schema.json` from the tag schema in `port/linux/game/tag_schema_*.c`. |
| `tag_schema.json` | The tag schema: each tag group and block definition, with the offsets and sizes of the Xbox ABI. Generated, committed. |
| `compile_check.py` | Compiles each of the game's C files for the PS2's EE with ps2dev's GCC, and reports which fail and why, grouped by error. Also reports the size of the code and data. |
| `pcsx2_run.py` | Boots a PS2 program in PCSX2 without its window, and prints what it writes to the console. Stops when a given text appears or after a timeout. |
| `convert_map.py` | The map converter's first pass: an Xbox map as the PS2 reads it (inflated, its tags' runtime values reset, its tags and BSPs moved to the PS2's tag cache). |
| `pcsx2_bench.py` | Runs the PS2 build's benchmark in PCSX2: the game loads a level from the main menu, plays it with a scripted player and logs how long its simulation ticks take. |
| `relocation_check.py` | Checks, on each map, that the PS2's tag relocation (`port/linux/game/tag_relocate.c`) knows every pointer in the tags and BSPs: it applies the same rules, then lists every other word that points into them. |
| `semantics_header.py` | Makes the PS2 build's `halo_msvc_semantics.h` (used by `ninja ps2` and `compile_check.py`). |
| `unimplemented_stubs.py` | Writes a stub for each function the PS2 build calls and nothing defines yet (used by `ninja ps2`). |

## Measure the maps

From the root of the repository:

```bash
python tools/ps2/map_budget.py --iso assets/halo.xiso.iso --json build/ps2_budget.json
python tools/ps2/converter_prototype.py --iso assets/halo.xiso.iso --json build/ps2_prototype.json
python tools/ps2/budget_projection.py build/ps2_budget.json build/ps2_prototype.json
```

- `--maps DIR` reads a folder of `.map` files instead of a disc image.
- `--only a10 d20` limits the tools to some maps.
- `converter_prototype.py` takes about three minutes for all 24 maps.
  Almost all of that is the animations.
- `budget_projection.py --tolerance "0.5 deg, 3 mm"` uses another of the
  animation tolerances.

To check the tag relocation on every map (about a minute; the exit status
is 1 when a word that may be a missed pointer is found):

```bash
python tools/ps2/relocation_check.py --iso assets/halo.xiso.iso
```

The JSON files contain sizes and counts only, no game data. Keep them in
`build/`, which git also ignores.

## Make the schema again

Do this when a `tag_schema_*.c` file or a tag structure changes:

```bash
python tools/ps2/export_tag_schema.py
```

The script compiles the schema with clang for the Xbox's ABI, with the
flags of the Linux build, and reads the tables from the LLVM IR. On Linux
it uses the system's 32-bit C headers. On other systems, give it a 32-bit
x86 C library's headers, for example musl's:

1. In a musl source tree, enter `make ARCH=i386 prefix=DIR install-headers`.
2. Enter `python tools/ps2/export_tag_schema.py --libc-include DIR/include`.

## Build the game for the PS2

`ninja ps2` builds `build/ps2/halo.elf` (`tools/ps2_build.py`). It needs the
ps2dev toolchain (below). `configure.py` finds it through `PS2DEV`, or
through `--ps2dev DIR`, and leaves the PS2 build out without it.

```bash
python configure.py
ninja ps2
python tools/ps2/pcsx2_run.py build/ps2/halo.elf --until unimplemented: --timeout 60
```

Every function the game calls that the PS2's platform layer has not got
is a stub: it prints `unimplemented: NAME` and stops. `build/ps2/unimplemented.txt`
lists the stubs, none now: `port/ps2/src/ps2_null_platform.c` answers for
the services the PS2 does not have (sound, the network, voice chat, internet
play, the PC version's menus, the high-res HUD and text). A missing data
symbol (a table, a global) stops the build instead, because a stub cannot
stand in for data. So does an object that defines a name ps2sdk's libraries
define (the linker would quietly take ours for the libraries' own calls
too), apart from the few the game defines on purpose.

`port/ps2/port.json` lists the Linux platform layer's files that the PS2
build compiles as they are. `port/ps2/src` holds the PS2's own files.

The game reads its files from `host:`, the ELF's folder `build/ps2`:
`config.toml` (the settings, written with the defaults when it is
missing), the game's log `debug.txt`, and the saves under `z/`. The maps
go in `build/ps2/maps`, converted (`convert_map.py`): the PS2 reads a map
there as it is and refuses a map that is not converted.

```bash
python tools/ps2/convert_map.py assets/maps/ui.map assets/maps/bloodgulch.map --out build/ps2/maps
```

`ui.map` is the main menu's: every run needs it. The tool inflates the Xbox's
compressed map, sets the runtime values of its tags to what they start with,
and moves its tags and structure BSPs to where the PS2 reads them (one
address in every build, `HALO_PORT_TAG_CACHE_ADDRESS`); it takes about a
second for a multiplayer map. A converted map is 33 to 142 MB.

## Measure the game's ticks

With the maps converted (above), the benchmark loads a multiplayer level from
the main menu, plays it with a scripted player and logs how long the
simulation's ticks take (`debug.bench_map` and `debug.bench_ticks`,
`port/ps2/src/ps2_bench.c`):

```bash
python tools/ps2/pcsx2_bench.py --map bloodgulch --ticks 600
```

The log has a line every 150 ticks (the average and the longest) and the
totals, with the ticks over the 15 ms of Gate 1. In PCSX2 the time is
emulated cycles, which leave out the caches' and the memory's waits: compare
builds with it, and measure on a PS2 for the gate. Of the campaign's levels only `c20` fits the PS2's tag cache
(`docs/ps2_port_plan.md`, Phase 1).

An assertion prints the call sites of the stack (the game's stack walk
scans for them) as addresses in `debug.txt`; name them with ps2dev's
`mips64r5900el-ps2-elf-addr2line -f -e build/ps2/halo.elf ADDRESS...`.
Some of the addresses are stale calls that the stack still holds.

## Check the compile for the PS2

You need the ps2dev toolchain (<https://github.com/ps2dev/ps2dev>). Its
prebuilt releases include macOS (Apple Silicon and Intel), Linux and
Windows. Set `PS2DEV`, `PS2SDK` and `PATH` as its README says.

```bash
python tools/ps2/compile_check.py
```

- `--optimize "-O2"` uses other optimization options. The default is
  `-Os`, because memory is the PS2's limit.
- `--only source/ai` compiles only some files.
- The objects and the report (`compile_check.json`) go to
  `build/ps2/check`, apart from the objects of `ninja ps2`.

The PS2 build uses the Linux build's MSVC environment under
`port/ps2/include/halo_ps2_prefix.h`. It adds what GCC does not take from
clang: `__int64`, the x86 calling conventions, `__declspec`, and MSVC's
inline functions as weak GNU89 inlines.

## Run a PS2 program in PCSX2

You need PCSX2, configured with a BIOS from your own console: start PCSX2
once and choose the BIOS.

```bash
python tools/ps2/pcsx2_run.py program.elf --until "text to wait for" --timeout 30
```

- The tool copies your PCSX2 configuration to a temporary folder and turns
  on console logging and the host filesystem there. Your own settings do
  not change.
- The program's folder is PCSX2's `host:` device: the program reads the
  files beside it as `host:name`.
- The exit status is 0 when the text appears, 1 at the timeout, 2 when
  PCSX2 stops or does not start, and 3 when the program ends (a halt in the
  game ends it: its reason is in `debug.txt` beside it).
- `--pcsx2 PATH` (or the `PCSX2` environment variable) selects the PCSX2
  executable. The default is `/Applications/PCSX2.app` on macOS, and
  `pcsx2-qt` on the PATH elsewhere.
