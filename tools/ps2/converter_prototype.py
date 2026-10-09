#!/usr/bin/env python3
"""Prototypes of the PS2 map converter's passes, measured on the retail maps
(docs/ps2_port_plan.md, "Memory: tag data"):

    python tools/ps2/converter_prototype.py (--iso IMAGE | --maps DIR) [--only a10 ...] [--json OUT]

Nothing is converted or written but the report. For each map:

- animations: each uncompressed animation is decoded (for each frame and
  node, an 8-byte rotation, a 12-byte translation and a 4-byte scale, as
  the node's flags say; the layout is checked against frame_size) and
  re-encoded in the engine's own keyframe format (model_animations.c,
  struct compressed_animation_header): each node's default (frame 0), and
  for each animated channel the keyframes that linear interpolation
  (normalized for rotations, as the decoder does) needs to stay within a
  tolerance, measured after the 6-byte quantization; the last frame is
  always a keyframe, as the decoder asserts. Rotations are 6 bytes,
  translations and scales floats, keyframe frame indices words.
- super-high LOD: the vertices and indices of model geometry that only slot
  4 of a permutation's geometry_indices (the most detailed) uses, which the
  converter would drop, pointing slot 4 at slot 3's geometry. First-person
  models are counted apart.
- collision: every collision BSP (each structure BSP's and each collision
  model's) with 16-bit indices, block by block. A field fits when its
  largest index is below 0x8000 where the engine keeps a flag in the sign
  bit (plane designators, bsp3d children, bsp2d children and roots) and
  below 0xFFFF otherwise; a block with a field that does not fit keeps its
  32-bit layout. Planes and vertices keep their floats.
- scripts: the scenario's script nodes, a data array allocated at its
  maximum (19001 nodes) whatever the scripts use.
"""

import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from map_budget import M, SCHEMA, TagData, add_map_arguments, maps, s32  # noqa: E402

QUATERNION_MAXIMUM = 32767  # COMPRESSED_QUATERNION_COMPONENT_MAXIMUM

# struct animation_graph and struct animation (model_animation_definitions.h),
# as compiled for the Xbox ABI
GRAPH_ANIMATIONS = 116
ANIMATION_SIZE = 180
A_FRAME_COUNT, A_FRAME_SIZE, A_NODE_COUNT, A_FLAGS = 34, 36, 44, 58
A_TRANSLATION_FLAGS, A_ROTATION_FLAGS, A_SCALE_FLAGS, A_DATA = 92, 108, 124, 160
ANIMATION_COMPRESSED_BIT = 1
COMPRESSED_HEADER = 0x2C  # offsetof(struct compressed_animation_header, rotation_node_headers)

# struct model (model_definitions.h), struct model_geometry_part (rasterizer.c)
MODEL_REGIONS, MODEL_GEOMETRIES = 196, 208
REGION_SIZE, REGION_PERMUTATIONS = 76, 64
PERMUTATION_SIZE, PERMUTATION_GEOMETRY_INDICES = 88, 64
GEOMETRY_SIZE, GEOMETRY_PARTS = 48, 36
PART_SIZE, PART_TRIANGLE_COUNT, PART_VERTEX_COUNT = 104, 0x44 + 4, 0x54 + 4
MODEL_VERTEX_SIZE = 32  # struct model_vertex_compressed
SUPER_HIGH = 4

# struct collision_bsp (collision_bsp_definitions.h): block, its offset, its
# element's size, the compact size, and the index fields (offset, flagged)
COLLISION_BSP_SIZE = 96
COLLISION_BLOCKS = {
    'bsp3d nodes': (0, 12, 6, [(0, True), (4, True), (8, True)]),
    'planes': (12, 16, 16, []),
    'leaves': (24, 8, 6, [(4, False)]),
    'bsp2d references': (36, 8, 4, [(0, True), (4, True)]),
    'bsp2d nodes': (48, 20, 16, [(12, True), (16, True)]),
    'surfaces': (60, 12, 8, [(0, True), (4, False)]),
    'edges': (72, 24, 12, [(o, False) for o in range(0, 24, 4)]),
    'vertices': (84, 16, 16, []),
}
COLLISION_MODEL_NODES, COLLISION_NODE_SIZE, COLLISION_NODE_BSPS = 652, 64, 52
STRUCTURE_BSP_COLLISION = 176

# (name, rotation in degrees, translation in world units (3.048 m), scale)
TOLERANCES = (
    ('exact', 0.0, 0.0, 0.0),
    ('0.1 deg, 1 mm', 0.1, 0.0003, 0.0005),
    ('0.25 deg, 2 mm', 0.25, 0.0006, 0.001),
    ('0.5 deg, 3 mm', 0.5, 0.001, 0.002),
)


# ---------- animations

def six_byte(q):
    """the quaternion as quaternion_compress_6byte keeps it: 12 bits a component"""
    out = []
    for c in q:
        v = int(c * QUATERNION_MAXIMUM)
        out.append((v & 0xFFF0 if v >= 0 else -((-v) & 0xFFF0)) / QUATERNION_MAXIMUM)
    return out


def normalize(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return [c / n for c in q]


def nlerp(a, b, t):
    if sum(x * y for x, y in zip(a, b)) < 0:
        b = [-x for x in b]
    return normalize([x + (y - x) * t for x, y in zip(a, b)])


def angle(a, b):
    return math.degrees(2 * math.acos(min(1.0, abs(sum(x * y for x, y in zip(a, b))))))


def lerp(a, b, t):
    return [x + (y - x) * t for x, y in zip(a, b)]


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def keyframe_count(values, interpolate, error, tolerance):
    """the keyframes after frame 0 (the default) that keep interpolation
    within tolerance, the last frame always one; greedy, each segment as
    long as it can be"""
    n = len(values)
    if all(error(values[0], v) <= tolerance for v in values):
        return 0
    keys = 0
    start = 0
    while start < n - 1:
        end = start + 1
        while end + 1 < n and all(
                error(interpolate(values[start], values[end + 1], (f - start) / (end + 1 - start)), values[f])
                <= tolerance for f in range(start + 1, end + 1)):
            end += 1
        keys += 1
        start = end
    return keys


def animation_sizes(t, o):
    """(the uncompressed data's bytes, {tolerance: keyframe format's bytes}),
    or None when the frame layout is not what the flags say"""
    frames, frame_size = struct.unpack_from('<hh', t, o + A_FRAME_COUNT)
    nodes = struct.unpack_from('<h', t, o + A_NODE_COUNT)[0]
    flags = {k: struct.unpack_from('<II', t, o + off)
             for k, off in (('r', A_ROTATION_FLAGS), ('t', A_TRANSLATION_FLAGS), ('s', A_SCALE_FLAGS))}
    size, _, _, address = struct.unpack_from('<iiiI', t, o + A_DATA)

    def has(channel, node):
        return (flags[channel][node >> 5] >> (node & 31)) & 1

    expected = sum(8 * has('r', n) + 12 * has('t', n) + 4 * has('s', n) for n in range(nodes))
    if expected == frame_size == size == 0:
        # a pose with no animated channel (stand idles, death poses): it stays
        return 0, {name: 0 for name, *_ in TOLERANCES}
    do = address - 0x803A6000
    if expected != frame_size or frames * frame_size > size or do < 0 or do + size > len(t):
        return None
    channels = {k: collections.defaultdict(list) for k in 'rts'}
    for f in range(frames):
        p = do + f * frame_size
        for n in range(nodes):
            if has('r', n):
                channels['r'][n].append(normalize(six_byte([c / QUATERNION_MAXIMUM for c in struct.unpack_from('<4h', t, p)])))
                p += 8
            if has('t', n):
                channels['t'][n].append(list(struct.unpack_from('<3f', t, p)))
                p += 12
            if has('s', n):
                channels['s'][n].append([struct.unpack_from('<f', t, p)[0]])
                p += 4
    out = {}
    for name, rotation, translation, scale in TOLERANCES:
        total = COMPRESSED_HEADER + nodes * (6 + 12 + 4)  # header, every node's defaults
        for channel, interpolate, error, tolerance, key_size in (
                ('r', nlerp, angle, rotation, 6), ('t', lerp, distance, translation, 12), ('s', lerp, distance, scale, 4)):
            total += 4 * len(channels[channel])  # node headers
            total += (2 + key_size) * sum(keyframe_count(v, interpolate, error, tolerance)
                                          for v in channels[channel].values())
        out[name] = (total + 3) & ~3
    return size, out


def animations(tags):
    t = tags.region.buf
    result = dict(compressed=0, compressed_bytes=0, uncompressed=0, uncompressed_bytes=0, layout_mismatches=0,
                  keyframes={name: 0 for name, *_ in TOLERANCES})
    for _, o in tags.of_group('antr'):
        count, address = struct.unpack_from('<iI', t, o + GRAPH_ANIMATIONS)
        for a in range(max(count, 0)):
            ao = tags.region.offset(address + a * ANIMATION_SIZE, ANIMATION_SIZE)
            if ao is None:
                continue
            if struct.unpack_from('<H', t, ao + A_FLAGS)[0] & ANIMATION_COMPRESSED_BIT:
                result['compressed'] += 1
                result['compressed_bytes'] += s32(t, ao + A_DATA)
                continue
            sizes = animation_sizes(t, ao)
            if sizes is None:
                result['layout_mismatches'] += 1
                continue
            result['uncompressed'] += 1
            result['uncompressed_bytes'] += sizes[0]
            for name, n in sizes[1].items():
                result['keyframes'][name] += n
    return result


# ---------- super-high LOD

def super_high_lod(tags):
    t = tags.region.buf
    removable = [0, 0]  # vertex bytes, index bytes
    first_person = 0
    order = collections.Counter()
    for name, o in tags.of_group('mode'):
        is_first_person = '\\fp\\' in name or name.endswith('\\fp')
        count, address = struct.unpack_from('<iI', t, o + MODEL_GEOMETRIES)
        geometry_bytes = []
        for g in range(max(count, 0)):
            parts, pa = struct.unpack_from('<iI', t, address - 0x803A6000 + g * GEOMETRY_SIZE + GEOMETRY_PARTS)
            n = [0, 0]
            for p in range(max(parts, 0)):
                po = pa - 0x803A6000 + p * PART_SIZE
                n[0] += s32(t, po + PART_VERTEX_COUNT) * MODEL_VERTEX_SIZE
                n[1] += s32(t, po + PART_TRIANGLE_COUNT) * 2
            geometry_bytes.append(n)
        slots = collections.defaultdict(set)
        regions, ra = struct.unpack_from('<iI', t, o + MODEL_REGIONS)
        for r in range(max(regions, 0)):
            perms, pa = struct.unpack_from('<iI', t, ra - 0x803A6000 + r * REGION_SIZE + REGION_PERMUTATIONS)
            for p in range(max(perms, 0)):
                indices = struct.unpack_from('<5h', t, pa - 0x803A6000 + p * PERMUTATION_SIZE + PERMUTATION_GEOMETRY_INDICES)
                for slot, g in enumerate(indices):
                    if 0 <= g < count:
                        slots[g].add(slot)
                low, high = indices[0], indices[SUPER_HIGH]
                if 0 <= low < count and 0 <= high < count and low != high:
                    order['super-high larger' if sum(geometry_bytes[high]) > sum(geometry_bytes[low])
                          else 'super-low larger'] += 1
        only = [geometry_bytes[g] for g, s in slots.items() if s == {SUPER_HIGH}]
        if is_first_person:
            first_person += sum(sum(b) for b in only)
        else:
            removable[0] += sum(b[0] for b in only)
            removable[1] += sum(b[1] for b in only)
    return dict(removable_vertex_bytes=removable[0], removable_index_bytes=removable[1],
                first_person_bytes=first_person, order=dict(order))


# ---------- collision

def collision_bsp(buf, base, o, totals, misfits):
    for name, (bo, size, compact, fields) in COLLISION_BLOCKS.items():
        count, address = struct.unpack_from('<iI', buf, o + bo)
        eo = address - base
        if count <= 0 or eo < 0 or eo + count * size > len(buf):
            continue
        fits = all(
            all((v & 0x7FFFFFFF) < (0x8000 if flagged else 0xFFFF)
                for v in (s32(buf, eo + e * size + fo) for e in range(count)) if v != -1)
            for fo, flagged in fields)
        totals[name][0] += count * size
        totals[name][1] += count * (compact if fits else size)
        if not fits:
            misfits[name] += 1


def collision(tags, data):
    t = tags.region.buf
    misfits = collections.Counter()
    models = collections.defaultdict(lambda: [0, 0])
    for _, o in tags.of_group('coll'):
        nodes, na = struct.unpack_from('<iI', t, o + COLLISION_MODEL_NODES)
        for n in range(max(nodes, 0)):
            bsps, ba = struct.unpack_from('<iI', t, na - 0x803A6000 + n * COLLISION_NODE_SIZE + COLLISION_NODE_BSPS)
            for b in range(max(bsps, 0)):
                collision_bsp(t, 0x803A6000, ba - 0x803A6000 + b * COLLISION_BSP_SIZE, models, misfits)
    structure = []
    for base, buf in tags.structure_bsps(data):
        totals = collections.defaultdict(lambda: [0, 0])
        root = struct.unpack_from('<I', buf, 0)[0] - base
        count, address = struct.unpack_from('<iI', buf, root + STRUCTURE_BSP_COLLISION)
        for c in range(max(count, 0)):
            collision_bsp(buf, base, address - base + c * COLLISION_BSP_SIZE, totals, misfits)
        structure.append(dict(size=len(buf), blocks={k: v for k, v in totals.items()}))
    return dict(models={k: v for k, v in models.items()}, structure_bsps=structure,
                blocks_that_do_not_fit=dict(misfits))


# ---------- scripts

def scripts(tags, schema):
    t = tags.region.buf
    definition = schema['definitions'][schema['groups']['scnr']['definition']]
    field = next(f for f in definition['fields'] if f['name'] == 'hs_syntax_data')
    for _, o in tags.of_group('scnr'):
        size, _, _, address = struct.unpack_from('<iiiI', t, o + field['offset'])
        h = tags.region.offset(address, 52)
        if h is None:
            return None
        # struct data_array (data.h): maximum_count and size at 32, its
        # signature at 40, then first_free_absolute_index, count, actual_count
        maximum, element = struct.unpack_from('<hh', t, h + 32)
        _, count, in_use = struct.unpack_from('<hhh', t, h + 44)
        return dict(bytes=size, maximum=maximum, element_size=element, count=count, in_use=in_use)
    return None


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
        tags = TagData(data)
        r = dict(map=name, animations=animations(tags), super_high_lod=super_high_lod(tags),
                 collision=collision(tags, data), scripts=scripts(tags, schema))
        a, lod, s = r['animations'], r['super_high_lod'], r['scripts']
        print(f"== {name}")
        print(f"  animations: {a['uncompressed']} uncompressed ({a['uncompressed_bytes'] / M:.2f} MB), "
              f"{a['compressed']} compressed, {a['layout_mismatches']} layout mismatches; as keyframes:")
        for tolerance, n in a['keyframes'].items():
            saved = 1 - n / a['uncompressed_bytes'] if a['uncompressed_bytes'] else 0
            print(f"    {tolerance:16} {n / M:5.2f} MB ({100 * saved:.0f}% saved)")
        print(f"  super-high-only geometry: {lod['removable_vertex_bytes'] / M:.2f} MB of vertices, "
              f"{lod['removable_index_bytes'] / M:.2f} MB of indices, first-person "
              f"{lod['first_person_bytes'] / M:.2f} MB; LOD order: {lod['order']}")
        c = r['collision']
        models = [sum(v[i] for v in c['models'].values()) for i in (0, 1)]
        print(f"  collision models {models[0] / M:.2f} -> {models[1] / M:.2f} MB; structure BSPs: " + ', '.join(
            f"{sum(v[0] for v in b['blocks'].values()) / M:.2f} -> {sum(v[1] for v in b['blocks'].values()) / M:.2f}"
            for b in c['structure_bsps']) + f"; not 16-bit: {c['blocks_that_do_not_fit'] or 'none'}")
        if s:
            print(f"  script nodes: {s['count']} made ({s['in_use']} in use) of {s['maximum']}, "
                  f"{s['bytes'] / 1024:.0f} KB allocated")
        results.append(r)
        sys.stdout.flush()
    if args.json:
        args.json.write_text(json.dumps(results, indent=1))


if __name__ == '__main__':
    main()
