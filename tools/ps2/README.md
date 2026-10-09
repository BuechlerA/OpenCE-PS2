# PS2 map tools

These tools measure the Xbox maps for the PS2 port's memory budget. Refer
to [docs/ps2_port_plan.md](../../docs/ps2_port_plan.md), "Optimizing memory
and game logic". They read the maps and write reports. They do not change
or convert a map.

You need Python 3.9 or later and a disc image of Halo: Combat Evolved for
the Xbox. Keep the image in `assets/` at the root of the repository, which
git ignores. Do not put it anywhere else in the repository.

| Tool | Function |
| --- | --- |
| `map_budget.py` | The memory each map takes in the Xbox's 22 MB tag cache, by tag group and tag block: the shared tags, each structure BSP, and the peak. Also the textures and sounds that stay in the map file. |
| `converter_prototype.py` | Runs four passes of the PS2 map converter without writing anything: animations in the engine's keyframe format at four tolerances, no super-high LOD, collision BSPs with 16-bit indices, script nodes trimmed. Reports the bytes each saves. |
| `budget_projection.py` | Combines the results of the two tools above into each map's projected PS2 peak. |
| `export_tag_schema.py` | Makes `tag_schema.json` from the tag schema in `port/linux/game/tag_schema_*.c`. |
| `tag_schema.json` | The tag schema: each tag group and block definition, with the offsets and sizes of the Xbox ABI. Generated, committed. |

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
