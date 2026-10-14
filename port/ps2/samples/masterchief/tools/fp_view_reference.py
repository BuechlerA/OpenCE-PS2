#!/usr/bin/env python3
"""A reference for src/fp_view.c: a first-person animation's frame played straight from the map's tags (numpy, no exporter bytes), and where a few of the
arms' and the gun's vertices land in the camera's space, for tools/fp_view_test.c to match.

    python3 tools/fp_view_reference.py "assault rifle" 7 40      # weapon, animation type (weapons.h: _first_person_weapon_animation_*), frame
"""
import os, struct, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bsp import MapFile
from skel import quat_rows, mat4
import export_fp_models as X

B = 0x803a6000
mf = MapFile(X.MAP); td = mf.td
name, atype, frame = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
tag = 'weapons\\%s\\fp\\fp' % name
g = mf.tags[(b'antr', tag)] - B
nc, na, _ = struct.unpack_from('<iII', td, g + 0x68); na -= B
ac, aa, _ = struct.unpack_from('<iII', td, g + 0x74); aa -= B
fc, fa, _ = struct.unpack_from('<iII', td, g + 0x48); fa -= B
cnt, addr, _ = struct.unpack_from('<iII', td, fa + 0x10); addr -= B
index = struct.unpack_from('<h', td, addr + atype * 2)[0]
names = [td[na + i * 0x40:na + i * 0x40 + 32].split(b'\0')[0].decode() for i in range(nc)]
parents = [struct.unpack_from('<h', td, na + i * 0x40 + 0x24)[0] for i in range(nc)]
o = aa + index * 0xB4
fsize, = struct.unpack_from('<h', td, o + 0x24)
tfl = struct.unpack_from('<2I', td, o + 0x5C); rfl = struct.unpack_from('<2I', td, o + 0x6C); sfl = struct.unpack_from('<2I', td, o + 0x7C)
_ds, _f, _fo, daddr, _d = struct.unpack_from('<IIIII', td, o + 0x8C)
_s, _f, _fo, faddr, _d = struct.unpack_from('<IIIII', td, o + 0xA0)
p = faddr - B + frame * fsize; dp = daddr - B
L = []
for n in range(nc):
    bit = lambda f: (f[n >> 5] >> (n & 31)) & 1
    if bit(rfl): q = struct.unpack_from('<4h', td, p); p += 8
    else: q = struct.unpack_from('<4h', td, dp); dp += 8
    if bit(tfl): t = struct.unpack_from('<3f', td, p); p += 12
    else: t = struct.unpack_from('<3f', td, dp); dp += 12
    if bit(sfl): s = struct.unpack_from('<f', td, p)[0]; p += 4
    else: s = struct.unpack_from('<f', td, dp)[0]; dp += 4
    q = np.array(q, float) / 32767.0; q /= np.linalg.norm(q)
    M = mat4(quat_rows(q) * s, t)
    L.append(M)
W = []
for n in range(nc):
    W.append(L[n] @ (W[parents[n]] if n > 0 else np.eye(4)))
tex = X.Textures()
for model_name in (X.HANDS, tag):
    m = X.load_model(mf, model_name, tex, tex)
    for vi in (0, 100, 500, 1000, len(m['P']) - 1):
        if vi >= len(m['P']): continue
        gn = [names.index(n['name']) if n['name'] in names else -1 for n in m['nodes']]
        def S(i): return m['inverse_bind'][i] @ W[gn[i]]
        n0, n1 = m['ND'][vi]; w = m['WG'][vi]
        v = np.append(m['P'][vi], 1.0)
        pos = (v @ S(n0))[:3] * w + (v @ S(n1))[:3] * (1 - w)
        print('%s %d %.5f %.5f %.5f' % ('hands' if model_name == X.HANDS else 'gun', vi, *pos))
