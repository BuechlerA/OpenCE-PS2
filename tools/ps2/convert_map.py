#!/usr/bin/env python3
"""The PS2 map converter's first pass: an Xbox map as the PS2 reads it.

    python tools/ps2/convert_map.py MAP.map... [--out DIR]

The Xbox's maps are a 0x800-byte header and a zlib stream; the Xbox
decompresses each to a cache file on its hard disk before it plays it
(source/cache/cache_files_decompress_windows.c). A retail PS2 has no disk, so
the converter does that once, on the PC: its output is the map as the Xbox's
cache file holds it, a header and the inflated data, which the PS2 build
reads in place (source/cache/cache_files_windows.c, "PS2").

The header says how long the whole map is (its size, at offset 8), so a
converted map is told from one that is not by its file's length.

It also moves the map's tags, and each structure BSP, to where the PS2 reads
them (PS2_TAG_CACHE_ADDRESS), as the PS2 build does for a map linked elsewhere
(port/ps2/game/tag_relocate.c, which finds the pointers as
relocation_check.py does, the rules this uses): every pointer into the tags
gets the difference between the addresses, and every pointer a BSP holds into
itself the difference between where it was linked and where it is read, a page
after the tags. The PS2 then does none of it, and the tag schema it needs for
it stays out of the build.

Its other work is what the game does to the tags as it loads a map on the
other builds (port/ps2/game/tag_validate.c, which the PS2 build leaves out:
its claims are 1.2 MB for each 9 MB of tag cache): every runtime value the
tag schema names (a bitmap's address in the texture cache, a sound's played
permutations) is set to the value it starts with. The Xbox's maps hold the
values their last session left. The converted map says so in its header
(PS2_MARK), which the PS2 build asks for. Nothing else in the tags changes:
the retail maps need no correction.

The rest of the converter (docs/ps2_port_plan.md, Phase 2) comes after it.
"""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from map_budget import (BLOCK, SCHEMA, STRUCT, SCENARIO_STRUCTURE_BSP_REFERENCES, XBOX_TAG_BASE,  # noqa: E402
                        TagData, s32, u32)
from relocation_check import discover  # noqa: E402

HEADER_SIZE = 0x800
HEADER_SIGNATURE = b"daeh"  # 'head', little-endian
FOOTER_SIGNATURE = b"toof"  # 'foot'
SIZE_OFFSET = 8
NAME_OFFSET = 0x20
# four bytes of the header's unused part: a map the converter has made
PS2_MARK_OFFSET = 0x68
PS2_MARK = b"ps2\x02"
# and, after it, where the tags are linked: the PS2's tag cache, which is at the
# same address in every build (HALO_PORT_TAG_CACHE_ADDRESS, halo_port_capacity.h)
PS2_TAG_CACHE_ADDRESS = 0x00EB0000
PS2_TAG_CACHE_SIZE = 0x01130000
RESET = 8  # tag_schema.h, _tag_schema_reset


class MapError(Exception):
    pass


class Resetter:
    """sets the runtime values of a tag's bytes, by the schema, as
    relocation_check.py's walker finds the tag's blocks"""

    def __init__(self, schema):
        self.definitions = schema["definitions"]
        self.groups = schema["groups"]
        self.count = 0

    def group_definition(self, group):
        seen = set()
        while group in self.groups and group not in seen:
            seen.add(group)
            if self.groups[group]["definition"]:
                return self.groups[group]["definition"]
            parents = self.groups[group]["parents"]
            group = parents[0] if parents else None
        return None

    def walk(self, data, base, start, size, key, o, depth=0):
        """tag bytes data[start:start + size], linked at base; the structure
        of definition key at offset o of them"""
        if depth > 24:
            raise MapError("damaged: blocks inside blocks past 24 deep")
        for f in self.definitions[key]["fields"]:
            for i in range(max(f["count"], 1)):
                fo = o + f["offset"] + i * f["size"]
                if f["type"] == RESET:
                    if fo + f["size"] > size:
                        raise MapError("damaged: a runtime value past its tag data")
                    value = f.get("value", 0) if f["size"] <= 4 else 0
                    data[start + fo:start + fo + f["size"]] = (value & ((1 << (8 * f["size"])) - 1)).to_bytes(
                        f["size"], "little") if f["size"] <= 4 else bytes(f["size"])
                    self.count += 1
                elif f["type"] == BLOCK:
                    if fo + 8 > size:
                        continue
                    count, address = s32(data, start + fo), u32(data, start + fo + 4)
                    if count <= 0:
                        continue
                    element = self.definitions[f["definition"]]["size"]
                    eo = address - base
                    if eo < 0 or eo + count * element > size:
                        raise MapError("damaged: a block outside its tag data")
                    for e in range(count):
                        self.walk(data, base, start, size, f["definition"], eo + e * element, depth + 1)
                elif f["type"] == STRUCT:
                    self.walk(data, base, start, size, f["definition"], fo, depth + 1)

    def tag(self, data, base, start, size, group, address):
        key = self.group_definition(group)
        o = address - base
        if key and 0 <= o and o + self.definitions[key]["size"] <= size:
            self.walk(data, base, start, size, key, o)


def reset_runtime_values(data: bytearray, schema) -> int:
    """sets the runtime values in the tags and the structure bsps of a map
    (inflated) to what they start with; returns how many"""
    tags = TagData(bytes(data))
    resetter = Resetter(schema)
    start, size = s32(data, 0x10), s32(data, 0x14)
    for group, _, address in tags.tags:
        resetter.tag(data, tags.region.base, start, size, group, address)
    index = tags.scenario_index & 0xFFFF
    scenario = tags.region.offset(tags.tags[index][2]) if 0 <= index < tags.count else None
    if scenario is not None:
        references = s32(data, start + scenario + SCENARIO_STRUCTURE_BSP_REFERENCES)
        block = tags.region.offset(u32(data, start + scenario + SCENARIO_STRUCTURE_BSP_REFERENCES + 4),
                                   max(references, 0) * 0x20)
        for b in range(references if block is not None else 0):
            file_offset, file_size, base = struct.unpack_from("<iiI", data, start + block + b * 0x20)
            if 0 <= file_offset and 0 <= file_size and file_offset + file_size <= len(data) and file_size >= 4:
                resetter.tag(data, base, file_offset, file_size, "sbsp", u32(data, file_offset))
    return resetter.count


def relocate(data: bytearray, schema) -> int:
    """moves the tags and the structure bsps of an inflated map to the PS2's
    tag cache; returns how many pointers moved"""
    tags, shared, _tags_space, _walker, regions = discover(bytes(data), schema)
    tags_start, tags_size = s32(data, 0x10), s32(data, 0x14)
    tags_delta = PS2_TAG_CACHE_ADDRESS - XBOX_TAG_BASE
    bsp_base = PS2_TAG_CACHE_ADDRESS + ((tags_size + 0xFFF) & ~0xFFF)
    if bsp_base + 0 > PS2_TAG_CACHE_ADDRESS + PS2_TAG_CACHE_SIZE:
        raise MapError("the tags do not fit the PS2's tag cache")
    moved = 0
    # each bsp: its file offset and link address, from the scenario
    index = tags.scenario_index & 0xFFFF
    scenario = tags.region.offset(tags.tags[index][2])
    references = s32(data, tags_start + scenario + SCENARIO_STRUCTURE_BSP_REFERENCES)
    block = tags.region.offset(u32(data, tags_start + scenario + SCENARIO_STRUCTURE_BSP_REFERENCES + 4),
                               max(references, 0) * 0x20)
    bsps = []
    for b in range(references if block is not None else 0):
        file_offset, file_size, base = struct.unpack_from("<iiI", data, tags_start + block + b * 0x20)
        if 0 <= file_offset and 0 <= file_size and file_offset + file_size <= len(data):
            bsps.append((tags_start + block + b * 0x20, file_offset, file_size, base))
    # the shared tags: each pointer marked is into the tags
    for o in shared.relocated:
        value = u32(data, tags_start + o)
        if not XBOX_TAG_BASE <= value < XBOX_TAG_BASE + tags_size:
            raise MapError("damaged: a pointer outside the tags")
        data[tags_start + o:tags_start + o + 4] = ((value + tags_delta) & 0xFFFFFFFF).to_bytes(4, "little")
        moved += 1
    if len(bsps) != len(regions) - 1:
        raise MapError("damaged: the structure bsps differ")
    for (entry, file_offset, file_size, base), (region, _space) in zip(bsps, regions[1:]):
        if bsp_base + file_size > PS2_TAG_CACHE_ADDRESS + PS2_TAG_CACHE_SIZE:
            raise MapError(f"a structure bsp of {file_size} bytes does not fit the PS2's tag cache")
        for o in region.relocated:
            value = u32(data, file_offset + o)
            if XBOX_TAG_BASE <= value < XBOX_TAG_BASE + tags_size:
                value += tags_delta
            elif base <= value < base + file_size:
                value = value - base + bsp_base
            else:
                raise MapError("damaged: a bsp pointer outside the tags and itself")
            data[file_offset + o:file_offset + o + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")
            moved += 1
        # where it is read, which the reference says (the PS2 reads each at
        # the same place, after the tags: one is in memory at a time)
        data[entry + 8:entry + 12] = bsp_base.to_bytes(4, "little")
    return moved


def convert(data: bytes, schema=None) -> bytes:
    """the map as the PS2 reads it, from the map as the DVD holds it"""
    if len(data) < HEADER_SIZE or data[:4] != HEADER_SIGNATURE or data[HEADER_SIZE - 4:HEADER_SIZE] != FOOTER_SIGNATURE:
        raise MapError("not a map: no header")
    if data[PS2_MARK_OFFSET:PS2_MARK_OFFSET + 4] == PS2_MARK:
        return data
    size = struct.unpack_from("<I", data, SIZE_OFFSET)[0]
    stream = zlib.decompressobj()
    try:
        body = stream.decompress(data[HEADER_SIZE:])
    except zlib.error as error:
        raise MapError(f"damaged: {error}") from error
    if not stream.eof:
        raise MapError("damaged: the data ends early")
    if HEADER_SIZE + len(body) != size:
        raise MapError(f"damaged: {HEADER_SIZE + len(body)} bytes, the header says {size}")
    converted = bytearray(data[:HEADER_SIZE] + body)
    schema = schema or json.loads(SCHEMA.read_text())
    reset_runtime_values(converted, schema)
    relocate(converted, schema)
    converted[PS2_MARK_OFFSET:PS2_MARK_OFFSET + 4] = PS2_MARK
    converted[PS2_MARK_OFFSET + 4:PS2_MARK_OFFSET + 8] = PS2_TAG_CACHE_ADDRESS.to_bytes(4, "little")
    return bytes(converted)


def map_name(data: bytes) -> str:
    return data[NAME_OFFSET:NAME_OFFSET + 0x20].split(b"\0")[0].decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("maps", nargs="*", type=Path)
    parser.add_argument("--out", type=Path, default=Path("build/ps2/maps"), help="default build/ps2/maps")
    args = parser.parse_args()
    if not args.maps:
        parser.error("no maps")
    args.out.mkdir(parents=True, exist_ok=True)
    schema = json.loads(SCHEMA.read_text())
    failed = False
    for path in args.maps:
        try:
            converted = convert(path.read_bytes(), schema)
        except MapError as error:
            print(f"{path}: {error}", file=sys.stderr)
            failed = True
            continue
        target = args.out / (map_name(converted) + ".map")
        if target.resolve() == path.resolve():
            print(f"{path}: already converted")
            continue
        target.write_bytes(converted)
        print(f"{target}: {len(converted)} bytes")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
