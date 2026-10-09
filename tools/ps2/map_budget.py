#!/usr/bin/env python3
"""The memory a Halo Xbox map takes, by tag group and tag block, for the PS2
port's budget (docs/ps2_port_plan.md):

    python tools/ps2/map_budget.py (--iso IMAGE | --maps DIR) [--only a10 ...] [--json OUT]

Reads the maps straight from an Xbox disc image (XDVDFS, as
port/linux/src/xiso.c reads it) or from a folder, inflates them, and walks
every tag of the shared tag data and of each structure BSP with the tag
schema (export_tag_schema.py). Every byte is attributed to a tag group and
the block it is in.

How an Xbox map uses the 22 MB tag cache: the shared tags stay loaded for
the whole level, at 0x803A6000. They start with the tag index and the tag
names, then the vertex and index buffers of every model (the tag header's
vertex_buffers and index_buffers point at them), then the tags. Each
structure BSP loads when the level switches to it, at an address that ends
at the top of the tag cache. The peak is therefore the shared tags plus the
largest BSP.

Map files are untrusted input: every offset is bounds-checked, and nothing
is written but the report.
"""

import argparse
import bisect
import collections
import json
import struct
import sys
import zlib
from pathlib import Path

SECTOR = 2048
# where the game partition of a whole-disc image starts (xiso.c)
PARTITIONS = (0, 0x0FD90000, 0x02080000, 0x18300000)
MEDIA = b'MICROSOFT*XBOX*MEDIA'
XBOX_TAG_BASE = 0x803A6000
TAG_CACHE_SIZE = 0x1600000
HEADER_BYTES = 0x800
CACHE_VERSION_XBOX = 5
# offsetof(struct scenario, structure_bsp_references), asserted in
# source/scenario/scenario_definitions.h (the schema leaves it to the loader)
SCENARIO_STRUCTURE_BSP_REFERENCES = 0x5A4
SCHEMA = Path(__file__).with_name('tag_schema.json')

# tag_schema.h, enum tag_schema_field_type
BLOCK, DATA, FILE_DATA, STRUCT = 0, 1, 2, 9

M = 1048576


# ---------- disc images

class Xiso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        for part in PARTITIONS:
            self.f.seek(part + 0x10000)
            d = self.f.read(0x800)
            if d[:20] == MEDIA and d[0x7EC:0x800] == MEDIA:
                self.part = part
                self.root = struct.unpack_from('<II', d, 20)
                return
        raise SystemExit(f'{path}: not an Xbox disc image')

    def read(self, sector, size):
        self.f.seek(self.part + sector * SECTOR)
        return self.f.read(size)

    def listdir(self, sector, size):
        """a directory's entries, a binary tree of 4-byte-aligned records"""
        buf = self.read(sector, size)
        out = {}
        seen = set()
        pending = [0]
        while pending:
            off = pending.pop()
            if off in seen or off + 14 > len(buf):
                continue
            seen.add(off)
            left, right, start, length, attributes, name_length = struct.unpack_from('<HHIIBB', buf, off)
            if left == 0xFFFF:
                continue
            name = buf[off + 14:off + 14 + name_length].decode('latin-1')
            out[name.lower()] = (start, length, bool(attributes & 0x10), name)
            pending += [child * 4 for child in (left, right) if child]
        return out

    def maps(self):
        root = self.listdir(*self.root)
        if 'maps' not in root or not root['maps'][2]:
            raise SystemExit('no maps folder in the image')
        for key, (start, length, is_directory, name) in sorted(self.listdir(*root['maps'][:2]).items()):
            if not is_directory and key.endswith('.map'):
                yield name, (lambda s=start, n=length: self.read(s, n))


def folder_maps(path):
    for p in sorted(Path(path).glob('*.map')):
        yield p.name, p.read_bytes


def maps(args):
    """(name, bytes) of each map the arguments name, inflated"""
    source = Xiso(args.iso).maps() if args.iso else folder_maps(args.maps)
    for name, read in source:
        if args.only and Path(name).stem.lower() not in args.only:
            continue
        data, how = inflate_map(read())
        if data is None:
            print(f'{name}: skipped ({how})', file=sys.stderr)
            continue
        yield name, data


def add_map_arguments(parser):
    parser.add_argument('--iso', help='an Xbox disc image (.iso, .xiso)')
    parser.add_argument('--maps', help='a folder of .map files')
    parser.add_argument('--only', nargs='*', help='map names without .map, e.g. a10 d20')


# ---------- maps

def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def s32(b, o):
    return struct.unpack_from('<i', b, o)[0]


def inflate_map(raw):
    """the map as the Xbox copies it to its hard disk: the retail maps are
    the header, then the rest deflated with zlib"""
    if len(raw) < HEADER_BYTES:
        return None, 'too short'
    version, file_length = s32(raw, 4), s32(raw, 8)
    if version != CACHE_VERSION_XBOX:
        return None, f'cache version {version}'
    if len(raw) == file_length:
        return raw, 'uncompressed'
    try:
        body = zlib.decompressobj().decompress(raw[HEADER_BYTES:], file_length + 1)
    except zlib.error:
        return None, 'could not inflate'
    data = raw[:HEADER_BYTES] + body
    if len(data) != file_length:
        return None, 'inflated to the wrong length'
    return data, f'deflated, {len(raw):,} bytes on disc'


class Region:
    """bytes loaded at a base address, with the bytes claimed so far"""

    def __init__(self, buf, base):
        self.buf, self.base = buf, base
        self.mask = bytearray(len(buf))

    def offset(self, address, size=0):
        o = address - self.base
        if o < 0 or size < 0 or o + size > len(self.buf):
            return None
        return o

    def claim(self, o, size):
        self.mask[o:o + size] = b'\x01' * size


class TagData:
    """a map's shared tags: the tag index and each tag's group and address"""

    def __init__(self, data):
        offset, size = s32(data, 0x10), s32(data, 0x14)
        self.region = Region(data[offset:offset + size], XBOX_TAG_BASE)
        t = self.region.buf
        (self.instances, self.scenario_index, _, self.count, self.vertex_buffer_count, self.vertex_buffers,
         self.index_buffer_count, self.index_buffers) = struct.unpack_from('<IiIiiIiI', t, 0)
        io = self.region.offset(self.instances, self.count * 0x20)
        if io is None:
            raise ValueError('bad tag index')
        self.tags = []
        for i in range(self.count):
            group, _, _, _, name, address = struct.unpack_from('<IIIiII', t, io + i * 0x20)
            self.tags.append((group.to_bytes(4, 'big').decode('latin-1'), name, address))

    def name(self, pointer):
        o = self.region.offset(pointer)
        if o is None:
            return ''
        end = self.region.buf.find(b'\0', o)
        return self.region.buf[o:end].decode('latin-1')

    def of_group(self, group):
        """(name, offset) of each tag of the group"""
        for g, name, address in self.tags:
            o = self.region.offset(address)
            if g == group and o is not None:
                yield self.name(name), o

    def structure_bsps(self, data):
        """(base address, bytes) of each structure BSP of the scenario"""
        index = self.scenario_index & 0xFFFF
        if not 0 <= index < self.count:
            return
        so = self.region.offset(self.tags[index][2])
        if so is None:
            return
        t = self.region.buf
        count = s32(t, so + SCENARIO_STRUCTURE_BSP_REFERENCES)
        bo = self.region.offset(u32(t, so + SCENARIO_STRUCTURE_BSP_REFERENCES + 4), max(count, 0) * 0x20)
        for b in range(count if bo is not None else 0):
            file_offset, file_size, base = struct.unpack_from('<iiI', t, bo + b * 0x20)
            if 0 <= file_offset and 0 <= file_size and file_offset + file_size <= len(data):
                yield base, data[file_offset:file_offset + file_size]


class Walker:
    """attributes a tag's bytes to its group and blocks, by the schema"""

    def __init__(self, schema):
        self.definitions = schema['definitions']
        self.groups = schema['groups']
        self.bytes = collections.Counter()       # (group, path): bytes in the tag cache
        self.file_bytes = collections.Counter()  # (group, path): bytes left in the map file
        self.errors = 0

    def group_definition(self, group):
        seen = set()
        while group in self.groups and group not in seen:
            seen.add(group)
            if self.groups[group]['definition']:
                return self.groups[group]['definition']
            parents = self.groups[group]['parents']
            group = parents[0] if parents else None
        return None

    def walk(self, region, group, key, o, path, depth=0):
        if depth > 24:
            self.errors += 1
            return
        for f in self.definitions[key]['fields']:
            for i in range(max(f['count'], 1)):
                fo = o + f['offset'] + i * f['size']
                if fo + 16 > len(region.buf):
                    self.errors += 1
                    continue
                name = f"{path}/{f['name']}"
                if f['type'] == BLOCK:
                    count, address = s32(region.buf, fo), u32(region.buf, fo + 4)
                    if count <= 0:
                        continue
                    element = self.definitions[f['definition']]['size']
                    eo = region.offset(address, count * element)
                    if eo is None:
                        self.errors += 1
                        continue
                    self.bytes[(group, name)] += count * element
                    region.claim(eo, count * element)
                    for e in range(count):
                        self.walk(region, group, f['definition'], eo + e * element, name, depth + 1)
                elif f['type'] in (DATA, FILE_DATA):
                    size = s32(region.buf, fo)
                    if size <= 0:
                        continue
                    if f['type'] == FILE_DATA:
                        self.file_bytes[(group, name)] += size
                        continue
                    address = u32(region.buf, fo + 12)
                    eo = region.offset(address, size) if address else None
                    if eo is None:
                        self.errors += 1
                        continue
                    self.bytes[(group, name)] += size
                    region.claim(eo, size)
                elif f['type'] == STRUCT:
                    self.walk(region, group, f['definition'], fo, path, depth + 1)

    def tag(self, region, group, address):
        key = self.group_definition(group)
        if not key:
            return False
        size = self.definitions[key]['size']
        o = region.offset(address, size)
        if o is None:
            self.errors += 1
            return False
        self.bytes[(group, '')] += size
        region.claim(o, size)
        self.walk(region, group, key, o, '')
        return True


def analyze(name, data, schema):
    tags = TagData(data)
    region = tags.region
    walker = Walker(schema)
    region.claim(0, 0x28)
    region.claim(region.offset(tags.instances), tags.count * 0x20)
    addresses = sorted(a for _, _, a in tags.tags if region.offset(a) is not None)
    first = region.offset(addresses[0]) if addresses else len(region.buf)

    def span_end(address):
        j = bisect.bisect_right(addresses, address)
        return region.offset(addresses[j]) if j < len(addresses) else len(region.buf)

    span = collections.Counter()
    unknown = set()
    for group, _, address in tags.tags:
        o = region.offset(address)
        if o is None:
            continue
        # every byte from a tag's root to the next tag's (the cache tool lays a
        # tag's blocks out after its root), schema or not
        span[group] += span_end(address) - o
        if not walker.tag(region, group, address):
            unknown.add(group)
    unclaimed = collections.Counter({'(before the first tag)': region.mask[:first].count(0)})
    for group, _, address in tags.tags:
        o = region.offset(address)
        if o is not None:
            unclaimed[group] += region.mask[o:span_end(address)].count(0)

    layout = {}
    names = [region.offset(n) for _, n, _ in tags.tags if region.offset(n) is not None]
    vo, io = region.offset(tags.vertex_buffers), region.offset(tags.index_buffers)
    if names and vo is not None and io is not None and min(names) < vo < io < first:
        layout = dict(index=min(names), names=vo - min(names), model_vertices=io - vo, model_indices=first - io)

    bsps = []
    for base, buf in tags.structure_bsps(data):
        bsp = Region(buf, base)
        bw = Walker(schema)
        bsp.claim(0, 0x18)  # struct cache_file_structure_bsp_header
        bw.tag(bsp, 'sbsp', u32(buf, 0))
        bsps.append(dict(size=len(buf), base=base, claimed=sum(bsp.mask), errors=bw.errors,
                         blocks=[[path, n] for (_, path), n in bw.bytes.most_common()]))
    return dict(
        map=name, tag_data_size=len(region.buf), tag_count=tags.count, file_length=len(data),
        claimed=sum(region.mask), errors=walker.errors, groups_without_schema=sorted(unknown),
        groups_by_span=span.most_common(), unclaimed=unclaimed.most_common(), before_first_tag=layout,
        blocks=[[group, path, n] for (group, path), n in walker.bytes.most_common()],
        file_data=[[group, path, n] for (group, path), n in walker.file_bytes.most_common()],
        bsps=bsps)


def peak(r):
    return r['tag_data_size'] + max((b['size'] for b in r['bsps']), default=0)


def mb(n):
    return f'{n / M:6.2f} MB'


def report(r):
    largest = max((b['size'] for b in r['bsps']), default=0)
    print(f"== {r['map']}")
    print(f"  shared tags {mb(r['tag_data_size'])} ({r['tag_count']} tags), largest BSP {mb(largest)}, "
          f"peak {mb(peak(r))} of {mb(TAG_CACHE_SIZE)}")
    print(f"  the schema claims {100 * r['claimed'] / max(r['tag_data_size'], 1):.1f}% of the shared tags; "
          f"{r['errors']} unreadable fields; groups without a schema: {' '.join(r['groups_without_schema']) or '-'}")
    if r['before_first_tag']:
        lay = r['before_first_tag']
        print(f"  before the first tag: tag index {mb(lay['index'])}, names {mb(lay['names'])}, "
              f"model vertices {mb(lay['model_vertices'])}, model indices {mb(lay['model_indices'])}")
    print('  groups (shared tags, from each root to the next):')
    for group, n in r['groups_by_span'][:12]:
        print(f'    {mb(n)}  {group}')
    print('  largest blocks (shared tags):')
    for group, path, n in r['blocks'][:15]:
        print(f'    {mb(n)}  {group}{path or " (root)"}')
    for i, b in enumerate(r['bsps']):
        print(f"  bsp {i}: {mb(b['size'])} at {b['base']:#x}, the schema claims "
              f"{100 * b['claimed'] / max(b['size'], 1):.1f}%")
        for path, n in b['blocks'][:8]:
            print(f'      {mb(n)}  {path or "(root)"}')
    files = collections.Counter()
    for group, _, n in r['file_data']:
        files[group] += n
    print('  left in the map file (streamed): ' + ', '.join(f'{g} {mb(n).strip()}' for g, n in files.most_common()))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    add_map_arguments(parser)
    parser.add_argument('--schema', type=Path, default=SCHEMA)
    parser.add_argument('--json', type=Path, help='also write the results as JSON')
    args = parser.parse_args()
    if not (args.iso or args.maps):
        parser.error('give --iso or --maps')
    schema = json.loads(args.schema.read_text())
    results = []
    for name, data in maps(args):
        r = analyze(name, data, schema)
        report(r)
        results.append(r)
        sys.stdout.flush()
    if args.json:
        args.json.write_text(json.dumps(results, indent=1))


if __name__ == '__main__':
    main()
