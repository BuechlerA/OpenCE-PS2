#!/usr/bin/env python3
"""Checks that the PS2's tag relocation knows every pointer of a map
(port/ps2/game/tag_relocate.c; docs/ps2_port_plan.md, "Relocating the tag
data"):

    python tools/ps2/relocation_check.py (--iso IMAGE | --maps DIR) [--only a10 ...]

An Xbox map's tags are linked to 0x803A6000, and each structure BSP to its
own address in the Xbox's 22 MB tag cache, which the PS2 cannot give them:
the PS2 build reads them elsewhere and moves every pointer they hold. The
relocation finds the pointers where tag_relocate.c does: the tag header,
the tag index, the vertex and index buffer headers, each structure BSP's
header, and each block, data and tag reference of the tag schema.

This marks those, then reads every other aligned word of the tags and of
each BSP. A word whose value falls in the linked tags (or, in a BSP, in the
tags or that BSP) and that the relocation would leave is a pointer it may
miss, or a number that happens to look like one: they are listed by tag
group, block and offset in the block's element. Words in the models' and
BSPs' raw vertex and index data, and the buffer headers' unused Lock words,
are counted apart: they are not pointers. A tag reference's name pointer
is recognized wherever it is (the name pointer of the tag instance its
index names) and relocated as tag_relocate.c does. The exit status is 1
when a word that may be a missed pointer is found.
"""

import argparse
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from map_budget import (BLOCK, DATA, SCHEMA, STRUCT, XBOX_TAG_BASE, SCENARIO_STRUCTURE_BSP_REFERENCES,  # noqa: E402
                        TagData, add_map_arguments, maps, s32, u32)

REFERENCE = 3
args_verbose = False
BUFFER_HEADER = 12  # D3DVertexBuffer, D3DIndexBuffer: Common, Data, Lock
BSP_HEADER = 0x18   # struct cache_file_structure_bsp_header

# The pointers the tag schema does not list (it lists what the validator
# checks), by definition. Each is (kind, the address's offset[, the size of
# a block's elements, their own extras]): a POINTER, the address of a BLOCK
# (whose elements hold no pointer unless their extras are given), or of a
# DATA (whose bytes are not pointers). tag_relocate.c's table, the same.
POINTER, BLOCK_ADDRESS, DATA_ADDRESS = 'pointer', 'block', 'data'
EXTRA_POINTERS = {
    # the triangle buffer's base_address and hardware_format (its index data
    # and D3D header) and the vertex buffer's hardware_format (rasterizer.c)
    'model_geometry_part': ((POINTER, 0x4C), (POINTER, 0x50), (POINTER, 0x64)),
    # the vertices' and lightmap vertices' vertex buffers' hardware_format
    # (structure_bsp_definitions.h)
    'structure_material': ((POINTER, 0xC0), (POINTER, 0xD4)),
    # the blocks and data the schema leaves out: the child scenarios'
    # references (their names are tag references'), a data, the editor's
    # comments (each with its text), the decals
    # (Halo 1's documented layouts: a child scenario is a tag reference and
    # 16 bytes; a structure bsp reference is file offset, size, address, 4
    # bytes and a tag reference, whose name is at 0x14; a comment is a
    # point, 16 bytes and its text)
    'scenario': ((BLOCK_ADDRESS, 0x44, 0x20, ((POINTER, 0x04),)), (DATA_ADDRESS, 0x110),
                 (BLOCK_ADDRESS, 0x11C, 0x30, ((DATA_ADDRESS, 0x28),)), (BLOCK_ADDRESS, 0x3AC),
                 (BLOCK_ADDRESS, 0x5A8, 0x20, ((POINTER, 0x14),))),
    'structure_bsp': ((BLOCK_ADDRESS, 0x244),),  # markers
    'particle_system': ((BLOCK_ADDRESS, 0x54),),
    'game_globals': ((BLOCK_ADDRESS, 0x1A4),),
}
# ... and by group, for the groups the schema has no definition of, with
# the size of their root: a meter's (its stencil and source bitmaps'
# references, its encoded stencil data)
EXTRA_GROUP_POINTERS = {'metr': (0xAC, ((DATA_ADDRESS, 0xA4),))}

# Words that look like pointers and are not, by definition and offset, and why.
NOT_POINTERS = {
    ('hs_script', 0x24): 'the root expression: a datum index, whose salt has its top bit set',
    ('hs_global', 0x28): 'the initial expression: a datum index',
    ('animation', 0x6C): 'nodes_with_rotation_flags',
    ('hud_globals', 0xD0): 'a color',
    ('hud_globals', 0xD4): 'a color',
    ('model', 0x4): 'node_list_checksum',
    ('animation', 0x28): 'node_list_checksum',
}
# Blocks whose elements are packed numbers, read here 4 bytes at a time
PACKED = {'structure_node', 'structure_leaf', 'structure_portal_index', 'detail_object'}


class Space:
    """the linked ranges (the tags', a BSP's) a pointer may point into"""

    def __init__(self, ranges=()):
        self.ranges = list(ranges)

    def add(self, base, size, name):
        self.ranges.append((base, base + size, name))

    def find(self, value):
        for start, end, name in self.ranges:
            if start <= value < end:
                return name
        return None


class Region:
    """bytes linked at base, with the offsets the relocation rebases, the
    words that are not pointers, and what each block element is (for the
    report)"""

    def __init__(self, buf, base, name):
        self.buf, self.base, self.name = buf, base, name
        self.relocated = set()
        self.not_pointers = set()
        self.raw = []  # (start, end): vertex and index data
        self.elements = []  # (start, end, group, path, element size, definition)

    def offset(self, address, size=0):
        o = address - self.base
        return o if 0 <= o and o + size <= len(self.buf) else None

    def pointer(self, o, space):
        """marks the word at o, if it points into the space"""
        if 0 <= o and o + 4 <= len(self.buf) and space.find(u32(self.buf, o)):
            self.relocated.add(o)
            return True
        return False


class Walker:
    def __init__(self, schema, space):
        self.definitions = schema['definitions']
        self.groups = schema['groups']
        self.space = space

    def group_definition(self, group):
        seen = set()
        while group in self.groups and group not in seen:
            seen.add(group)
            if self.groups[group]['definition']:
                return self.groups[group]['definition']
            parents = self.groups[group]['parents']
            group = parents[0] if parents else None
        return None

    def extras(self, region, o, extras):
        for extra in extras:
            kind, offset = extra[0], extra[1]
            if not region.pointer(o + offset, self.space):
                continue
            if kind == DATA_ADDRESS:
                # struct tag_data: size, flags, file offset, address
                size = s32(region.buf, o + offset - 12)
                do = region.offset(u32(region.buf, o + offset), max(size, 0))
                if do is not None and size > 0:
                    region.raw.append((do, do + size))
            elif kind == BLOCK_ADDRESS and len(extra) > 2:
                # struct tag_block: count, address
                count, element = s32(region.buf, o + offset - 4), extra[2]
                eo = region.offset(u32(region.buf, o + offset), max(count, 0) * element)
                for e in range(max(count, 0) if eo is not None else 0):
                    self.extras(region, eo + e * element, extra[3])

    def walk(self, region, group, key, o, path, depth=0):
        if depth > 24:
            return
        self.extras(region, o, EXTRA_POINTERS.get(self.definitions[key]['name'], ()))
        for f in self.definitions[key]['fields']:
            for i in range(max(f['count'], 1)):
                fo = o + f['offset'] + i * f['size']
                if fo + 16 > len(region.buf):
                    continue
                name = f"{path}/{f['name']}"
                if f['type'] == BLOCK:
                    count = s32(region.buf, fo)
                    if count <= 0 or not region.pointer(fo + 4, self.space):
                        continue
                    element = self.definitions[f['definition']]['size']
                    eo = region.offset(u32(region.buf, fo + 4), count * element)
                    if eo is None:
                        continue
                    region.elements.append((eo, eo + count * element, group, name, element,
                                            self.definitions[f['definition']]['name']))
                    for e in range(count):
                        self.walk(region, group, f['definition'], eo + e * element, name, depth + 1)
                elif f['type'] == DATA:
                    if region.pointer(fo + 12, self.space):
                        # a data's bytes are not pointers (the script nodes'
                        # array header aside: tag_relocate.c)
                        size = s32(region.buf, fo)
                        do = region.offset(u32(region.buf, fo + 12), max(size, 0))
                        if do is not None and size > 0:
                            region.raw.append((do, do + size))
                elif f['type'] == REFERENCE:
                    region.pointer(fo + 4, self.space)  # the tag's name
                elif f['type'] == STRUCT:
                    self.walk(region, group, f['definition'], fo, path, depth + 1)

    def tag(self, region, group, address):
        key = self.group_definition(group)
        o = region.offset(address)
        if o is not None and group in EXTRA_GROUP_POINTERS:
            size, extras = EXTRA_GROUP_POINTERS[group]
            region.elements.append((o, o + size, group, '', size, group))
            self.extras(region, o, extras)
        if key and o is not None:
            size = self.definitions[key]['size']
            region.elements.append((o, o + size, group, '', size, self.definitions[key]['name']))
            self.walk(region, group, key, o, '')


def buffer_headers(region, address, count, space):
    """the D3D headers' Data (relocated) and Lock (unused) words"""
    o = region.offset(address, max(count, 0) * BUFFER_HEADER)
    for i in range(max(count, 0) if o is not None else 0):
        region.pointer(o + i * BUFFER_HEADER + 4, space)
        region.not_pointers.add(o + i * BUFFER_HEADER)      # Common, which registering sets
        region.not_pointers.add(o + i * BUFFER_HEADER + 8)  # Lock


def is_group_tag(word):
    """four printable characters, as a group tag is ('scnr', 'bitm')"""
    return all(32 <= b < 127 for b in word.to_bytes(4, 'big'))


def tag_reference_names(region, tags, space):
    """every tag reference's name pointer (struct tag_reference: group tag,
    name, name length, tag index): a word in a tag's root or block element
    (never in raw data) that points into the tag names, after a group tag and
    before a name length under 256. A reference to a tag the map does not
    have (a child scenario's) has its name and NONE."""
    names = [n for _, n, _ in tags.tags if n]
    low, high = min(names), max(names) + 256
    t = region.buf
    words = set()
    for start, end, *_ in region.elements:
        words.update(range(start + (-start) % 4, end - 3, 4))
    for o in sorted(words):
        if o < 4 or o + 8 > len(t) or o in region.relocated:
            continue
        value = u32(t, o)
        if low <= value < high and is_group_tag(u32(t, o - 4)) and u32(t, o + 4) < 256:
            region.relocated.add(o)


def discover(data, schema):
    """the pointers of a map: (tags, the shared tags' region, their space, the
    walker, [(region, space)] of the shared tags and then each structure BSP)"""
    tags = TagData(data)
    t = tags.region.buf
    space = Space()
    space.add(XBOX_TAG_BASE, len(t), 'tags')
    bsps = list(tags.structure_bsps(data))
    for i, (base, buf) in enumerate(bsps):
        space.add(base, len(buf), f'bsp {i}')

    shared = Region(t, XBOX_TAG_BASE, 'tags')
    tags_space = Space([(XBOX_TAG_BASE, XBOX_TAG_BASE + len(t), 'tags')])
    space = tags_space
    if tags.vertex_buffers and tags.index_buffers:
        # the models' vertex and index data, after their buffer headers, up to the first tag
        first = min(a for _, _, a in tags.tags if shared.offset(a) is not None)
        shared.raw.append((shared.offset(tags.vertex_buffers) + tags.vertex_buffer_count * BUFFER_HEADER,
                           shared.offset(tags.index_buffers)))
        shared.raw.append((shared.offset(tags.index_buffers) + tags.index_buffer_count * BUFFER_HEADER,
                           shared.offset(first)))
    # the bytes between the last name and the buffer headers are padding
    names_end = max(shared.offset(n) + t[shared.offset(n):].index(b'\0') for _, n, _ in tags.tags if n)
    first_buffer = min(x for x in (shared.offset(tags.vertex_buffers), shared.offset(tags.index_buffers)) if x)
    shared.raw.append((names_end, first_buffer))
    for o in (0x00, 0x14, 0x1C):  # tag instances, vertex buffers, index buffers
        shared.pointer(o, space)
    io = shared.offset(tags.instances)
    for i in range(tags.count):
        shared.pointer(io + i * 0x20 + 0x10, space)  # name
        shared.pointer(io + i * 0x20 + 0x14, space)  # the tag's root
    buffer_headers(shared, tags.vertex_buffers, tags.vertex_buffer_count, space)
    buffer_headers(shared, tags.index_buffers, tags.index_buffer_count, space)
    walker = Walker(schema, space)
    for group, _, address in tags.tags:
        walker.tag(shared, group, address)
    tag_reference_names(shared, tags, space)
    # (the scenario's structure bsp references' addresses, where each BSP
    # loads, the PS2's loader sets as it places each BSP after the tags)

    regions = [(shared, tags_space)]
    for i, (base, buf) in enumerate(bsps):
        # a BSP points into the tags and itself: the BSPs overlap each other
        bsp_space = Space([*tags_space.ranges, (base, base + len(buf), f'bsp {i}')])
        bsp = Region(buf, base, f'bsp {i}')
        # its vertex data, after its buffer headers, up to its root
        bsp.raw.append((u32(buf, 0x10) - base + s32(buf, 0x0C) * BUFFER_HEADER, u32(buf, 0) - base))
        for o in (0x00, 0x08, 0x10):  # base address, vertex buffers, lightmap vertex buffers
            bsp.pointer(o, bsp_space)
        buffer_headers(bsp, u32(buf, 0x08), s32(buf, 0x04), bsp_space)
        buffer_headers(bsp, u32(buf, 0x10), s32(buf, 0x0C), bsp_space)
        Walker(schema, bsp_space).tag(bsp, 'sbsp', u32(buf, 0))
        tag_reference_names(bsp, tags, bsp_space)
        regions.append((bsp, bsp_space))
    return tags, shared, tags_space, walker, regions


def check(name, data, schema):
    tags, shared, tags_space, walker, regions = discover(data, schema)
    t = tags.region.buf

    missed = collections.Counter()
    unreachable = collections.Counter()
    raw = 0
    unknown = []  # the spans of the tags whose group has no schema
    for group, _, address in tags.tags:
        if not walker.group_definition(group) and shared.offset(address) is not None:
            later = [a for _, _, a in tags.tags if a > address and shared.offset(a) is not None]
            unknown.append((shared.offset(address), shared.offset(min(later)) if later else len(t), group))
    examples = {}
    for region, region_space in regions:
        starts = sorted(region.elements)
        for o in range(0, len(region.buf) - 3, 4):
            if o in region.relocated or o in region.not_pointers:
                continue
            value = u32(region.buf, o)
            target = region_space.find(value)
            if not target:
                continue
            if any(start <= o < end for start, end in region.raw):
                raw += 1
                continue
            where = None
            if region is shared:
                for start, end, group in unknown:
                    if start <= o < end:
                        where = f'{group} (no schema) +0x{o - start:x}'
            skip = False
            for start, end, group, path, element, definition in starts:
                if start <= o < end and (definition in PACKED or
                                         (definition, (o - start) % element) in NOT_POINTERS):
                    skip = True
            if skip:
                continue
            for start, end, group, path, element, definition in starts:
                if start <= o < end:
                    where = f'{definition} +0x{(o - start) % element:x}   ({group}{path or " (root)"}, -> {target})'
            if where is None:
                unreachable[region.name] += 1
                if args_verbose:
                    print(f'  unreachable: {region.name} 0x{region.base + o:08x} = 0x{value:08x}')
                continue
            missed[where] += 1
            examples.setdefault(where, f'0x{region.base + o:08x} = 0x{value:08x}')
    relocated = sum(len(r.relocated) for r, _ in regions)
    print(f'== {name}: {relocated} pointers relocated; {sum(missed.values())} other words point into the linked tags '
          f'or BSPs ({raw} more in raw data, {sum(unreachable.values())} in bytes no pointer reaches)')
    for where, n in missed.most_common(40):
        print(f'  {n:6}  {where}   (e.g. {examples[where]})')
    return sum(missed.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    add_map_arguments(parser)
    parser.add_argument('--schema', type=Path, default=SCHEMA)
    parser.add_argument('--verbose', action='store_true', help='also list the words in bytes no pointer reaches')
    args = parser.parse_args()
    global args_verbose
    args_verbose = args.verbose
    if not (args.iso or args.maps):
        parser.error('give --iso or --maps')
    schema = json.loads(args.schema.read_text())
    total = 0
    for name, data in maps(args):
        total += check(name, data, schema)
        sys.stdout.flush()
    sys.exit(1 if total else 0)


if __name__ == '__main__':
    main()
