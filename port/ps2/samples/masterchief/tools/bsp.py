"""Read a map's structure BSP: its materials (shader, vertices, lightmap vertices, triangles).

Layout (Xbox cache file, build 01.01.14.2342; see source/structures/structure_bsp_definitions.h):
  scenario + 0x5A4        structure_bsp_references: file offset, size and load address of each BSP
  BSP + 0x0               pointer to the structure_bsp
  structure_bsp + 0xF8    surfaces: three 16-bit vertex indices each, local to the surface's material
  structure_bsp + 0x104   lightmaps (32 bytes each, materials block at +0x14)
  structure_material (256 bytes): shader tag reference at 0, first surface at 0x14, surface count at 0x18,
    vertex buffer count at 0xB4, lightmap vertex count at 0xC8, compressed vertex data at 0xEC
    (size, flags, file offset, address): per vertex 32 bytes (position, packed normal, binormal,
    tangent, UV), then per lightmap vertex 8 bytes (packed radiosity, lightmap U and V as 16 bits)
"""
import struct
import numpy as np
from xmap import load
from extract import B
from mesh import decode_vec
from bmp import bitmap_info
from dxt import decode_dxt1

SCENARIO_BSP_REFERENCES = 0x5A4
SB_SURFACES, SB_LIGHTMAPS = 0xF8, 0x104
LM_MATERIALS = 0x14
MAT_SIZE = 256

def _name(td, ptr):
    return td[ptr - B:td.index(b'\0', ptr - B)].decode('latin1') if ptr else None

class MapFile:
    """a map's tags by group and name, and its bitmaps decoded"""
    def __init__(self, path):
        self.full, _off, _size, self.td, self.h = load(path)
        self.tags = {}
        for i in range(self.h[2]):
            o = (self.h[0] - B) + i * 0x20
            group = self.td[o:o + 4][::-1]
            name, addr = struct.unpack_from('<II', self.td, o + 0x10)
            self.tags[(group, _name(self.td, name))] = addr

    def ref(self, o):
        """the name a tag reference at td offset o points at"""
        _g, name, _n, _i = struct.unpack_from('<4sIII', self.td, o)
        return _name(self.td, name)

    def bitmaps(self, name):
        return bitmap_info(self.td, self.tags[(b'bitm', name)])

    def rgb(self, info):
        """one bitmap's first level as floats (h, w, 3): DXT1/3/5 (the alpha ignored) or R5G6B5"""
        w, h, fmt, po = info['w'], info['h'], info['fmt'], info['pixoff']
        if fmt in (14, 15, 16):
            assert not info['flags'] & 8, 'compressed bitmaps are not swizzled'
            size = ((w + 3) // 4) * ((h + 3) // 4) * (8 if fmt == 14 else 16)
            return decode_dxt1(self.full[po:po + size], w, h, dxt5=fmt != 14)[..., :3].astype(np.float64) / 255
        if fmt == 6:
            raw = np.frombuffer(self.full, dtype='<u2', count=w * h, offset=po).astype(np.uint32)
            if info['flags'] & 8: raw = unswizzle(raw, w, h)
            raw = raw.reshape(h, w)
            return np.stack([((raw >> 11) & 31) / 31, ((raw >> 5) & 63) / 63, (raw & 31) / 31], -1)
        if fmt in (10, 11):             # x8r8g8b8, a8r8g8b8 (the sky's is 11): 32 bits, alpha ignored here
            raw = np.frombuffer(self.full, dtype='<u4', count=w * h, offset=po)
            if info['flags'] & 8: raw = unswizzle(raw.astype(np.uint64), w, h).astype(np.uint32)
            raw = raw.reshape(h, w)
            return np.stack([((raw >> 16) & 255) / 255.0, ((raw >> 8) & 255) / 255.0, (raw & 255) / 255.0], -1)
        raise ValueError('bitmap format %d' % fmt)

    def alpha(self, info):
        """one bitmap's first level's alpha as floats (h, w): DXT3 (explicit 4 bits), DXT5, A8, A8R8G8B8 (11); 1 for the others"""
        w, h, fmt, po = info['w'], info['h'], info['fmt'], info['pixoff']
        if fmt == 15:
            n = ((w + 3) // 4) * ((h + 3) // 4)
            blocks = np.frombuffer(self.full, dtype=np.uint8, count=n * 16, offset=po).reshape(n, 16)[:, :8]
            nib = np.stack([blocks & 15, blocks >> 4], -1).reshape(n, 16).astype(np.float64) / 15.0   # low nibble first
            img = nib.reshape(h // 4, w // 4, 4, 4).transpose(0, 2, 1, 3).reshape(h, w)
            return img
        if fmt == 16:
            return decode_dxt1(self.full[po:po + ((w + 3) // 4) * ((h + 3) // 4) * 16], w, h, dxt5=True)[..., 3].astype(np.float64) / 255
        if fmt == 0:
            raw = np.frombuffer(self.full, dtype=np.uint8, count=w * h, offset=po).astype(np.uint32)
            if info['flags'] & 8: raw = unswizzle(raw, w, h)
            return raw.reshape(h, w) / 255.0
        if fmt == 11:
            raw = np.frombuffer(self.full, dtype='<u4', count=w * h, offset=po)
            if info['flags'] & 8: raw = unswizzle(raw.astype(np.uint64), w, h).astype(np.uint32)
            return (raw.reshape(h, w) >> 24).astype(np.float64) / 255
        return np.ones((h, w))

    def model_parts(self, name, geometry):
        """the parts of one geometry of a model: dict(shader=(group, name), pos (n, 3), normal (n, 3), uv (n, 2) in [-1, 1],
        tris (m, 3), node (n,)), the strips as triangle lists. Also the model's base map scale."""
        td = self.td
        mo = self.tags[(b'mode', name)] - B
        scale = struct.unpack_from('<2f', td, mo + 0x30)
        sc_, sa = struct.unpack_from('<II', td, mo + 0xac + 48); sa -= B
        shaders = []
        for i in range(sc_):
            g, nm, _n, _i = struct.unpack_from('<4sIII', td, sa + i * 0x20); shaders.append((g[::-1], _name(td, nm)))
        gc, ga = struct.unpack_from('<II', td, mo + 0xac + 36); ga -= B
        assert geometry < gc, (name, geometry, gc)
        pc, pa = struct.unpack_from('<II', td, ga + geometry * 0x30 + 0x24); pa -= B
        out = []
        for p in range(pc):
            po = pa + p * 0x68
            flags, shader = struct.unpack_from('<IH', td, po)
            ttype, tcount, tbase = struct.unpack_from('<HxxII', td, po + 0x44)
            vtype, vcount, voff, vbase, vhw = struct.unpack_from('<HxxIIII', td, po + 0x54)
            vdata = struct.unpack_from('<I', td, vhw - B + 4)[0] - B
            n = tcount + 2 if ttype == 1 else tcount * 3
            idx = np.array(struct.unpack_from('<%dH' % n, td, tbase - B), dtype=np.int64)
            raw = np.frombuffer(td, dtype=np.uint8, count=vcount * 32, offset=vdata).reshape(vcount, 32)
            pos = np.frombuffer(raw[:, :12].tobytes(), dtype='<f4').reshape(vcount, 3).astype(np.float64)
            nrm = np.array([decode_vec(int(u)) for u in np.frombuffer(raw[:, 12:16].tobytes(), dtype='<u4')])
            uv = np.frombuffer(raw[:, 24:28].tobytes(), dtype='<i2').reshape(vcount, 2) / 32767.0
            node = raw[:, 28].astype(np.int64) // 3
            tris = []
            if ttype == 1:
                for i in range(len(idx) - 2):
                    a, b, c = idx[i], idx[i + 1], idx[i + 2]
                    if a == b or b == c or a == c: continue
                    tris.append((a, c, b) if i & 1 else (a, b, c))
            else:
                tris = [tuple(idx[i * 3:i * 3 + 3]) for i in range(tcount)]
            out.append(dict(shader=shaders[shader] if shader < len(shaders) else None, pos=pos, normal=nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9),
                            uv=uv, tris=np.array(tris, dtype=np.int64).reshape(-1, 3), node=node, flags=flags, scale=scale))
        return out

    def model_lod_geometry(self, name, region=0, permutation=0, lod=0):
        """the geometry index of one LOD (0: the lowest detail with a geometry, going up) of a region's permutation"""
        td = self.td
        mo = self.tags[(b'mode', name)] - B
        rc, ra = struct.unpack_from('<II', td, mo + 0xac + 24); ra -= B
        pcnt, pa = struct.unpack_from('<II', td, ra + region * 0x4c + 0x40); pa -= B
        geoms = struct.unpack_from('<5h', td, pa + permutation * 0x58 + 0x44)
        valid = [g for g in geoms if g >= 0]
        return valid[min(lod, len(valid) - 1)] if valid else 0

def swizzle_masks(w, h):
    """rasterizer_swizzle.c's compute_swizzle_masks: x and y bits interleaved, a finished axis skipped"""
    size, bit, xm, ym = 1, 1, 0, 0
    while True:
        adv = 0
        if size < w: xm |= bit; bit <<= 1; adv = bit
        if size < h: ym |= bit; bit <<= 1; adv = bit
        size <<= 1
        if not adv: return xm, ym

def _spread(n, mask):
    out = np.zeros_like(n); i = 0
    for b in range(32):
        if mask >> b & 1:
            out |= ((n >> i) & 1) << b; i += 1
    return out

def unswizzle(raw, w, h):
    xm, ym = swizzle_masks(w, h)
    x = np.arange(w, dtype=np.uint32)[None, :]; y = np.arange(h, dtype=np.uint32)[:, None]
    return raw[(_spread(x, xm) | _spread(y, ym)).reshape(-1)]

def load_bsp(mf, index=0):
    """-> list of materials: dict(shader, group, lightmap, verts (n, 8 floats), normals (n, 3), tris (m, 3) local, lm (k, 4) u16)"""
    full, td, h = mf.full, mf.td, mf.h
    tag_index, scenario_tag = h[0], h[1]
    inst = (tag_index - B) + (scenario_tag & 0xFFFF) * 0x20
    scenario = struct.unpack_from('<I', td, inst + 0x14)[0] - B
    count, ref = struct.unpack_from('<iI', td, scenario + SCENARIO_BSP_REFERENCES)
    file_offset, file_size, base = struct.unpack_from('<iiI', td, ref - B + index * 0x20)
    buf = full[file_offset:file_offset + file_size]
    sb = struct.unpack_from('<I', buf, 0)[0] - base
    lightmap_group = _name(td, struct.unpack_from('<I', buf, sb + 4)[0])
    def block(o):
        c, a, _ = struct.unpack_from('<iII', buf, o)
        return c, a - base
    sc, so = block(sb + SB_SURFACES)
    surfaces = np.frombuffer(buf, dtype='<u2', count=sc * 3, offset=so).reshape(-1, 3)
    lc, lo = block(sb + SB_LIGHTMAPS)
    out = []
    for l in range(lc):
        bitmap_index = struct.unpack_from('<h', buf, lo + l * 32)[0]
        mc, ma = block(lo + l * 32 + LM_MATERIALS)
        for m in range(mc):
            mo = ma + m * MAT_SIZE
            group, name_ptr = struct.unpack_from('<4sI', buf, mo)
            first, tris = struct.unpack_from('<ii', buf, mo + 0x14)
            vcount = struct.unpack_from('<i', buf, mo + 0xB4)[0]
            lcount = struct.unpack_from('<i', buf, mo + 0xC8)[0]
            size, _flags, _fo, addr = struct.unpack_from('<iIII', buf, mo + 0xEC)
            if vcount == 0: continue
            assert size == vcount * 32 + lcount * 8, (size, vcount, lcount)
            d = addr - base
            v = np.frombuffer(buf, dtype='<f4', count=vcount * 8, offset=d).reshape(vcount, 8)
            raw = np.frombuffer(buf, dtype='<u4', count=vcount * 8, offset=d).reshape(vcount, 8)
            normals = np.array([decode_vec(int(u)) for u in raw[:, 3]])
            lm = np.frombuffer(buf, dtype='<u2', count=lcount * 4, offset=d + vcount * 32).reshape(lcount, 4) if lcount else None
            t = surfaces[first:first + tris].astype(np.int64)
            assert t.size == 0 or t.max() < vcount
            out.append(dict(shader=_name(td, name_ptr), group=group[::-1], lightmap=bitmap_index, lightmap_group=lightmap_group,
                            verts=v, normals=normals, tris=t, lm=lm))
    return out

def oriented(P, N, T):
    """triangles wound counter-clockwise seen from outside (against the vertex normals), as export_assets.py does"""
    T = np.array(T)
    fn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    flip = (fn * (N[T[:, 0]] + N[T[:, 1]] + N[T[:, 2]])).sum(1) < 0
    T[flip] = T[flip][:, [0, 2, 1]]
    return T, int(flip.sum())

def ground_height(P, T, x, y, below):
    """the highest upward-facing surface under (x, y) that is not above `below`"""
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    n = np.cross(b - a, c - a)
    up = n[:, 2] > 0.5 * np.linalg.norm(n, axis=1)
    best = None
    for i in np.nonzero(up)[0]:
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = a[i], b[i], c[i]
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(d) < 1e-12: continue
        w0 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
        w1 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
        w2 = 1 - w0 - w1
        if min(w0, w1, w2) < -1e-6: continue
        z = w0 * az + w1 * bz + w2 * cz
        if z <= below and (best is None or z > best): best = z
    return best

# the structure BSP's collision_bsp (source/physics/collision_bsp_definitions.h): the block of one collision_bsp is at
# structure_bsp + SB_SURFACES - 72 (the render surfaces' block, less the six blocks and a box between them: collision_bsp,
# nodes, world bounds, leaves and surface references); in it bsp3d nodes and planes, leaves, bsp2d references and nodes, then
# the surfaces, edges and vertices a character collides with
SB_COLLISION_BSP = SB_SURFACES - 72
SB_COLLISION_MATERIALS = SB_COLLISION_BSP - 12

def parse_collision_bsp(buf, base, cb):
    """one collision_bsp at offset cb of buf (a structure BSP's buffer loaded at `base`, or the tags' data at B) ->
    dict(vertices (n, 3), polygons [list of vertex indices, counter-clockwise from the plane's front], normals (m, 3), offsets (m), flags (m),
    material (m), and the tree: nodes (k, 3) int (plane, back child, front child), planes (p, 4), leaves [list of surface indices], where a
    child with the top bit set is a leaf (index in the low bits; all ones is none: solid)"""
    def block(o):
        c, a, _ = struct.unpack_from('<iII', buf, o)
        return c, a - base
    nodes_c, nodes_o = block(cb)
    planes_c, planes_o = block(cb + 12)
    leaves_c, leaves_o = block(cb + 24)
    refs_c, refs_o = block(cb + 36)
    n2_c, n2_o = block(cb + 48)
    sc, so = block(cb + 60)
    ec, eo = block(cb + 72)
    vc, vo = block(cb + 84)
    planes = np.frombuffer(buf, dtype='<f4', count=planes_c * 4, offset=planes_o).reshape(-1, 4) if planes_c else np.zeros((0, 4), np.float32)
    nodes = np.frombuffer(buf, dtype='<i4', count=nodes_c * 3, offset=nodes_o).reshape(-1, 3) if nodes_c else np.zeros((0, 3), np.int32)
    surfaces = np.frombuffer(buf, dtype=[('plane', '<i4'), ('first', '<i4'), ('flags', 'u1'), ('breakable', 'u1'), ('material', '<i2')], count=sc, offset=so)
    edges = np.frombuffer(buf, dtype='<i4', count=ec * 6, offset=eo).reshape(-1, 6)
    verts = np.frombuffer(buf, dtype='<f4', count=vc * 4, offset=vo).reshape(-1, 4)[:, :3].astype(np.float64)
    polygons, normals, offsets = [], np.zeros((sc, 3)), np.zeros(sc)
    for s in range(sc):
        e = first = int(surfaces['first'][s]); ring = []
        while True:
            rev = int(edges[e, 5] == s)                      # surface_indices[1]
            ring.append(int(edges[e, rev]))                  # vertex_indices[rev]
            e = int(edges[e, 2 + rev])                       # edge_indices[rev]
            if e == first or len(ring) > 16: break
        assert e == first, 'a surface\'s edges do not close'
        polygons.append(ring)
        d = int(surfaces['plane'][s]); p = planes[d & 0x7FFFFFFF].astype(np.float64)
        if d < 0: p = -p
        normals[s] = p[:3]; offsets[s] = p[3]
    # the leaves: each leaf's bsp2d references (a plane and the root of a 2D tree) and the surfaces at the bottom of those trees
    n2 = np.frombuffer(buf, dtype='<i4', count=n2_c * 5, offset=n2_o).reshape(-1, 5) if n2_c else np.zeros((0, 5), np.int32)   # plane (nx, ny, d as floats), two children
    leaf_rows = np.frombuffer(buf, dtype=[('flags', '<u2'), ('count', '<i2'), ('first', '<i4')], count=leaves_c, offset=leaves_o)
    refs = np.frombuffer(buf, dtype='<i4', count=refs_c * 2, offset=refs_o).reshape(-1, 2) if refs_c else np.zeros((0, 2), np.int32)
    def surfaces_under(child, out, depth=0):
        assert depth < 64
        if child & 0x80000000:
            out.add(child & 0x7FFFFFFF); return
        surfaces_under(int(n2[child, 3]) & 0xFFFFFFFF, out, depth + 1)
        surfaces_under(int(n2[child, 4]) & 0xFFFFFFFF, out, depth + 1)
    leaves = []
    for l in range(leaves_c):
        found = set()
        for r in range(int(leaf_rows['first'][l]), int(leaf_rows['first'][l]) + int(leaf_rows['count'][l])):
            surfaces_under(int(refs[r, 1]) & 0xFFFFFFFF, found)
        assert all(f < sc for f in found)
        leaves.append(sorted(found))
    return dict(vertices=verts, polygons=polygons, normals=normals, offsets=offsets, flags=surfaces['flags'].copy(), material=surfaces['material'].copy(),
                nodes=nodes, planes=planes, leaves=leaves)

def load_collision_bsp(mf, index=0):
    """the structure BSP's collision BSP (parse_collision_bsp's dict), plus materials [(shader, physics material)]"""
    full, td, h = mf.full, mf.td, mf.h
    inst = (h[0] - B) + (h[1] & 0xFFFF) * 0x20
    scenario = struct.unpack_from('<I', td, inst + 0x14)[0] - B
    _count, ref = struct.unpack_from('<iI', td, scenario + SCENARIO_BSP_REFERENCES)
    file_offset, file_size, base = struct.unpack_from('<iiI', td, ref - B + index * 0x20)
    buf = full[file_offset:file_offset + file_size]
    sb = struct.unpack_from('<I', buf, 0)[0] - base
    def block(o):
        c, a, _ = struct.unpack_from('<iII', buf, o)
        return c, a - base
    mc, ma = block(sb + SB_COLLISION_MATERIALS)
    count, cb = block(sb + SB_COLLISION_BSP)
    assert count == 1
    out = parse_collision_bsp(buf, base, cb)
    out['materials'] = [(_name(td, struct.unpack_from('<I', buf, ma + i * 20 + 4)[0]), struct.unpack_from('<h', buf, ma + i * 20 + 18)[0]) for i in range(mc)]
    return out

# a collision model ('coll' tag): its nodes block at 0x28C, each node 64 bytes with its bsps block (one collision_bsp of 96 bytes per permutation) at +52
COLL_NODES_AT, COLL_NODE_SIZE, COLL_NODE_BSPS_AT, COLLISION_BSP_SIZE = 0x28C, 64, 52, 96

def load_collision_model(mf, name, permutation=0):
    """-> [(node index, parse_collision_bsp's dict)] of the collision model `name`: each node's BSP for one permutation, in the model's own space
    (the nodes' own transforms are not applied: the scenery's nodes with surfaces are all the first, at the origin)"""
    td = mf.td
    a = mf.tags[(b'coll', name)] - B
    nc, na, _ = struct.unpack_from('<iII', td, a + COLL_NODES_AT)
    out = []
    for i in range(nc):
        e = na - B + i * COLL_NODE_SIZE
        bc, ba, _ = struct.unpack_from('<iII', td, e + COLL_NODE_BSPS_AT)
        if bc <= permutation: continue
        out.append((i, parse_collision_bsp(td, B, ba - B + permutation * COLLISION_BSP_SIZE)))
    return out
