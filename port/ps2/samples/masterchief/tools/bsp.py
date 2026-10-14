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

SCENARIO_BSP_REFERENCES = 0x5A4
SB_SURFACES, SB_LIGHTMAPS = 0xF8, 0x104
LM_MATERIALS = 0x14
MAT_SIZE = 256

def _name(td, ptr):
    return td[ptr - B:td.index(b'\0', ptr - B)].decode('latin1') if ptr else None

def load_bsp(path, index=0):
    """-> list of materials: dict(shader, group, lightmap, verts (n, 8 floats), normals (n, 3), tris (m, 3) local, lm (k, 4) u16)"""
    full, _off, _size, td, h = load(path)
    tag_index, scenario_tag, tag_count = h[0], h[1], h[2]
    inst = (tag_index - B) + (scenario_tag & 0xFFFF) * 0x20
    scenario = struct.unpack_from('<I', td, inst + 0x14)[0] - B
    count, ref = struct.unpack_from('<iI', td, scenario + SCENARIO_BSP_REFERENCES)
    file_offset, file_size, base = struct.unpack_from('<iiI', td, ref - B + index * 0x20)
    buf = full[file_offset:file_offset + file_size]
    sb = struct.unpack_from('<I', buf, 0)[0] - base
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
            out.append(dict(shader=_name(td, name_ptr), group=group[::-1], lightmap=bitmap_index,
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
