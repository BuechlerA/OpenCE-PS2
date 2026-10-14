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
