#!/usr/bin/env python3
"""Bake the first-person weapons' models, textures and animations out of a Halo Xbox map: src/fpm_assets.c + src/fpm_assets.h.

For each hand weapon of tools/export_weapons.py (HALO_MAP, default assets/maps/bloodgulch.map) and the arms the player holds it with
(game globals' first-person interface: characters\\cyborg\\fp\\fp):

  - the first-person models' triangles (the soso shaders' parts; the transparent chicago parts, a weapon's own display (the assault
    rifle's ammunition digits, compass, icons and glow), are the "display" below), their vertices (position and
    normal in the bind pose, texture coordinates, the two nodes and the weight that skin them), each node's inverse bind matrix, and which
    node of the animation graph each model node is (by name: model_build_remapping_table_for_animation_graph)
  - the weapon's first-person animation graph: its nodes' parents, and every animation the graph's table of animations by kind names,
    as the tag holds it (the flags of the nodes with a rotation, translation or scale, the default data, the frames' data)
  - each part's base map, as an 8-bit palettised 256x256 texture in the GS's palette order, gamma encoded as the Chief's; the arms' armour
    with the change colour baked in (the multipurpose map's blue) as the Chief's armour has it

  - the display parts: each chicago part's geometry (bind pose, UV with the map's scale and offset, node), the shader's numeric counter
    (counter limit, the bitmap's frame count: source/rasterizer/xbox/rasterizer_xbox_transparent_geometry.c) and its map: the map of the
    soso display (the same bitmap) is the texture the program already holds; the frames of the numeric maps (the digits 0-9 and the
    compass's 8 headings, 64x64 DXT1 each) are cut to 32x32 and packed into one 4-bit atlas of 256x128 (8 a row)

source/interface/first_person_weapons.c draws a weapon's model and the arms' with the same node matrices from the graph (remapped).
"""
import os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bsp import MapFile
from mesh import decode_vec
from skel import quat_rows, mat4

MAP = os.environ.get('HALO_MAP', os.path.join(HERE, '..', '..', '..', '..', '..', 'assets', 'maps', 'bloodgulch.map'))
OUT = os.path.join(HERE, '..', 'src')
B = 0x803a6000
TEX = int(os.environ.get('FP_TEXTURE_SIZE', 256))
CHANGE = np.array([0.341176, 0.403922, 0.145098])           # the biped's primary change colour, as export_assets.py
WEAPONS = ['assault rifle', 'pistol', 'sniper rifle', 'shotgun', 'rocket launcher', 'plasma rifle', 'plasma pistol', 'needler', 'flamethrower']
HANDS = 'characters\\cyborg\\fp\\fp'
BASE_AT, MULTI_AT = 0xA4, 0xBC                              # a shader_model's base map and multipurpose map references
ANIMATION_TYPES = 28


def cstr_array(name, data, ctype='unsigned char', align=16, const=True):
    out = ['%s%s %s[%d] __attribute__((aligned(%d))) = {' % ('const ' if const else '', ctype, name, max(len(data), 1), align)]
    line = []
    for v in data:
        line.append(repr(float(v)) + 'f' if ctype == 'float' else str(int(v)))
        if len(line) == 24: out.append(','.join(line) + ','); line = []
    if line: out.append(','.join(line))
    if not len(data): out.append('0')
    out.append('};')
    return '\n'.join(out) + '\n'


def gs_palette(pal):
    out = np.zeros(256, dtype='<u4')
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)         # a 256-entry CLUT in 8x2 blocks (bits 3 and 4 swap)
        r, g, b = (int(x) for x in pal[i])
        out[j] = r | (g << 8) | (b << 16) | (0x80 << 24)
    return out


def quantise(rgb01):
    im = Image.fromarray((np.clip(rgb01, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8))      # gamma encoded: the program lights in that space
    q = im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:768], dtype=np.uint8).reshape(-1, 3)
    if len(pal) < 256: pal = np.vstack([pal, np.zeros((256 - len(pal), 3), np.uint8)])
    return np.array(q, dtype=np.uint8), pal


def resample(img, n):
    """a (h, w, 3) picture at n x n: the mean of the texels each covers (box filter) when it is larger, else nearest"""
    h, w = img.shape[:2]
    if h >= n and w >= n:
        kh, kw = h // n, w // n
        return img[:n * kh, :n * kw].reshape(n, kh, n, kw, -1).mean(axis=(1, 3))
    ys = (np.arange(n) * h // n); xs = (np.arange(n) * w // n)
    return img[ys][:, xs]


def model_nodes(td, mo):
    c, a, _ = struct.unpack_from('<iII', td, mo + 0xac + 12)
    nodes = []
    for i in range(c):
        o = a - B + i * 0x9c
        nodes.append(dict(name=td[o:o + 32].split(b'\0')[0].decode(), parent=struct.unpack_from('<h', td, o + 0x24)[0],
                          t=struct.unpack_from('<3f', td, o + 0x28), q=struct.unpack_from('<4f', td, o + 0x34)))
    return nodes


def bind_world(nodes):
    W = []
    for n in nodes:
        local = mat4(quat_rows(n['q']), n['t'])
        W.append(local @ (W[n['parent']] if n['parent'] >= 0 else np.eye(4)))
    return W


def load_model(mf, name, textures, tex_of):
    """-> dict for one first-person model (all its parts with a shader_model and a base map, geometry 0)"""
    td = mf.td
    mo = mf.tags[(b'mode', name)] - B
    scale = struct.unpack_from('<2f', td, mo + 0x30)
    sc_, sa = struct.unpack_from('<II', td, mo + 0xac + 48); sa -= B
    shaders = []
    for i in range(sc_):
        g, nm, _n, _i = struct.unpack_from('<4sIII', td, sa + i * 0x20)
        shaders.append((g[::-1], td[nm - B:td.index(b'\0', nm - B)].decode()))
    gc, ga = struct.unpack_from('<II', td, mo + 0xac + 36); ga -= B
    assert gc >= 1
    pc, pa = struct.unpack_from('<II', td, ga + 0x24); pa -= B
    nodes = model_nodes(td, mo)
    nn = len(nodes)
    P, N, UV, ND, WG, T, parts = [], [], [], [], [], [], []
    base = 0
    for p in range(pc):
        po = pa + p * 0x68
        flags, shader = struct.unpack_from('<IH', td, po)
        group, sname = shaders[shader] if shader < len(shaders) else (b'', '')
        if group != b'soso': continue
        so = mf.tags[(group, sname)] - B
        base_map = mf.ref(so + BASE_AT)
        if not base_map: continue
        ttype, tcount, tbase = struct.unpack_from('<HxxII', td, po + 0x44)
        vtype, vcount, voff, vbase, vhw = struct.unpack_from('<HxxIIII', td, po + 0x54)
        vdata = struct.unpack_from('<I', td, vhw - B + 4)[0] - B
        n = tcount + 2 if ttype == 1 else tcount * 3
        idx = np.array(struct.unpack_from('<%dH' % n, td, tbase - B), dtype=np.int64)
        raw = np.frombuffer(td, dtype=np.uint8, count=vcount * 32, offset=vdata).reshape(vcount, 32)
        pos = np.frombuffer(raw[:, :12].tobytes(), dtype='<f4').reshape(vcount, 3).astype(np.float64)
        nrm = np.array([decode_vec(int(u)) for u in np.frombuffer(raw[:, 12:16].tobytes(), dtype='<u4')])
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        uv = np.frombuffer(raw[:, 24:28].tobytes(), dtype='<i2').reshape(vcount, 2) / 32767.0 * np.array(scale)
        n0 = raw[:, 28].astype(np.int64) // 3; n1 = raw[:, 29].astype(np.int64) // 3
        w = np.frombuffer(raw[:, 30:32].tobytes(), dtype='<i2') / 32767.0
        bad = n1 >= nn; n1[bad] = n0[bad]; w[bad] = 1.0
        n0[n0 >= nn] = 0
        tris = []
        if ttype == 1:
            for i in range(len(idx) - 2):
                a, b, c = idx[i], idx[i + 1], idx[i + 2]
                if a == b or b == c or a == c: continue
                tris.append((a, c, b) if i & 1 else (a, b, c))
        else:
            tris = [tuple(idx[i * 3:i * 3 + 3]) for i in range(tcount)]
        tris = np.array(tris, dtype=np.int64).reshape(-1, 3)
        # wound counter-clockwise seen from outside, against the vertex normals (as the rest of the sample's meshes)
        fn = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
        vn = nrm[tris[:, 0]] + nrm[tris[:, 1]] + nrm[tris[:, 2]]
        flip = (fn * vn).sum(1) < 0; tris[flip] = tris[flip][:, [0, 2, 1]]
        key = (base_map, sname)
        parts.append(dict(first=sum(len(t) for t in T), count=len(tris), texture=tex_of(mf, sname, base_map, mf.ref(so + MULTI_AT), name), name=sname))
        P.append(pos); N.append(nrm); UV.append(uv); ND.append(np.stack([n0, n1], 1)); WG.append(w); T.append(tris + base)
        base += vcount
    assert parts, 'no part with a shader_model in ' + name
    bw = bind_world(nodes)
    inv = np.stack([np.linalg.inv(m) for m in bw])
    return dict(name=name, P=np.concatenate(P), N=np.concatenate(N), UV=np.concatenate(UV), ND=np.concatenate(ND), WG=np.concatenate(WG), T=np.concatenate(T),
                parts=parts, nodes=nodes, inverse_bind=inv)


class Textures:
    """the 256x256 base maps of the parts, each once: (map, whether the primary change colour is baked in)"""
    def __init__(self): self.list, self.key = [], {}

    def __call__(self, mf, shader, base_map, multi_map, model_name):
        change = 'armor' in shader and 'hands' in model_name or ('armor hands' in shader)
        key = (base_map, change)
        if key not in self.key:
            rgb = mf.rgb(mf.bitmaps(base_map)[0])
            if change and multi_map:
                mp = mf.rgb(mf.bitmaps(multi_map)[0])
                if mp.shape[:2] != rgb.shape[:2]: mp = resample(mp, rgb.shape[0]) if mp.shape[0] > rgb.shape[0] else np.array(Image.fromarray((mp * 255).astype(np.uint8)).resize(rgb.shape[:2][::-1])) / 255.0
                mask = mp[..., 2:3]
                rgb = rgb * (1 - mask) + rgb * CHANGE * 1.55 * mask      # the primary change colour, as export_assets.py
            idx, pal = quantise(resample(rgb, TEX))
            self.key[key] = len(self.list)
            self.list.append(dict(name=base_map + (' (armour)' if change else ''), pixels=idx, clut=gs_palette(pal)))
        return self.key[key]


ATLAS_W, ATLAS_H, FRAME = 256, 128, 32                      # the numeric maps' frames, 32x32 each, 8 a row
SCHI_FLAG_NUMERIC = 0x80                                     # shader_transparent_flag_numeric_bit


class Atlas:
    """the frames of the numeric chicago maps (the digits, the compass), each bitmap's frames in order, one 4-bit texture"""
    def __init__(self): self.frames, self.first = [], {}

    def add(self, mf, bitmap):
        if bitmap not in self.first:
            self.first[bitmap] = len(self.frames)
            for info in mf.bitmaps(bitmap):
                self.frames.append(resample(mf.rgb(info), FRAME))
            assert len(self.frames) <= (ATLAS_W // FRAME) * (ATLAS_H // FRAME), 'the atlas is full'
        return self.first[bitmap]

    def pack(self):
        sheet = np.zeros((ATLAS_H, ATLAS_W, 3))
        for i, f in enumerate(self.frames):
            x, y = (i % (ATLAS_W // FRAME)) * FRAME, (i // (ATLAS_W // FRAME)) * FRAME
            sheet[y:y + FRAME, x:x + FRAME] = f
        # additive light on black: the colours the way the program's other textures hold them (gamma encoded), 16 of them
        im = Image.fromarray((np.clip(sheet, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8))
        q = im.quantize(16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pal = np.array(q.getpalette()[:48], dtype=np.uint8).reshape(-1, 3)
        if len(pal) < 16: pal = np.vstack([pal, np.zeros((16 - len(pal), 3), np.uint8)])
        idx = np.array(q, dtype=np.uint8)
        black = int(np.argmin(pal.astype(int).sum(1)))
        assert pal[black].sum() < 24, 'the atlas has no black'
        clut = np.zeros(16, dtype='<u4')
        for i in range(16):
            r, g, b = (int(x) for x in pal[i]); clut[i] = r | (g << 8) | (b << 16) | (0x80 << 24)
        return idx[:, 0::2] | (idx[:, 1::2] << 4), clut


def load_display(mf, name, textures, atlas):
    """the chicago parts of a first-person model -> list of dict(kind, P, UV, ND, T, texture, atlas_first, frames, limit, place)"""
    td = mf.td
    mo = mf.tags[(b'mode', name)] - B
    scale = struct.unpack_from('<2f', td, mo + 0x30)
    sc_, sa = struct.unpack_from('<II', td, mo + 0xac + 48); sa -= B
    shaders = []
    for i in range(sc_):
        g, nm, _n, _i = struct.unpack_from('<4sIII', td, sa + i * 0x20)
        shaders.append((g[::-1], td[nm - B:td.index(b'\0', nm - B)].decode()))
    gc, ga = struct.unpack_from('<II', td, mo + 0xac + 36); ga -= B
    pc, pa = struct.unpack_from('<II', td, ga + 0x24); pa -= B
    nodes = model_nodes(td, mo); nn = len(nodes)
    out = []
    for p in range(pc):
        po = pa + p * 0x68
        flags, shader = struct.unpack_from('<IH', td, po)
        group, sname = shaders[shader] if shader < len(shaders) else (b'', '')
        if group != b'schi': continue
        so = mf.tags[(group, sname)] - B
        limit, sflags, _ftype = struct.unpack_from('<BBh', td, so + 0x28)
        blend, = struct.unpack_from('<h', td, so + 0x2C)
        extra_flags, = struct.unpack_from('<H', td, so + 0x60)
        mc, ma, _ = struct.unpack_from('<iII', td, so + 0x54); ma -= B
        if not mc: continue
        bitmap = mf.ref(ma + 0x6C)
        u_scale, v_scale, u_offset, v_offset = struct.unpack_from('<4f', td, ma + 0x54)
        if blend != 3 or extra_flags & 2:             # (the sniper rifle's scope subscreen is alpha blended: not done)
            print('  %s: a chicago shader that is not added (blend %d, extra flags %x): left out' % (sname, blend, extra_flags)); continue
        ttype, tcount, tbase = struct.unpack_from('<HxxII', td, po + 0x44)
        vtype, vcount, voff, vbase, vhw = struct.unpack_from('<HxxIIII', td, po + 0x54)
        vdata = struct.unpack_from('<I', td, vhw - B + 4)[0] - B
        n = tcount + 2 if ttype == 1 else tcount * 3
        idx = np.array(struct.unpack_from('<%dH' % n, td, tbase - B), dtype=np.int64)
        raw = np.frombuffer(td, dtype=np.uint8, count=vcount * 32, offset=vdata).reshape(vcount, 32)
        pos = np.frombuffer(raw[:, :12].tobytes(), dtype='<f4').reshape(vcount, 3).astype(np.float64)
        uv = np.frombuffer(raw[:, 24:28].tobytes(), dtype='<i2').reshape(vcount, 2) / 32767.0 * np.array(scale)
        uv = uv * np.array([u_scale, v_scale]) + np.array([u_offset, v_offset])
        node = raw[:, 28].astype(np.int64) // 3
        node[node >= nn] = 0
        tris = []
        if ttype == 1:
            for i in range(len(idx) - 2):
                a, b, c = idx[i], idx[i + 1], idx[i + 2]
                if a == b or b == c or a == c: continue
                tris.append((a, c, b) if i & 1 else (a, b, c))
        else:
            tris = [tuple(idx[i * 3:i * 3 + 3]) for i in range(tcount)]
        tris = np.array(tris, dtype=np.int64).reshape(-1, 3)
        frames = len(mf.bitmaps(bitmap))
        numeric = bool(sflags & SCHI_FLAG_NUMERIC) and frames > 1
        part = dict(name=sname, P=pos, UV=uv, ND=node, T=tris, limit=limit if numeric else 0, frames=frames if numeric else 1, bitmap=bitmap, place=0)
        if numeric:
            part['kind'] = 2 if frames == 8 else 1                  # the compass has 8 frames (the counter reads function D), the digits 10 (A)
            part['atlas_first'] = atlas.add(mf, bitmap); part['texture'] = -1
        else:
            part['kind'] = 0; part['atlas_first'] = 0
            part['texture'] = textures(mf, 'display', bitmap, None, name)
        out.append(part)
    # a digit's place by where it reads: a number's digits are the parts of one shader, the one that reads first (smaller along the map's u) is the highest
    digits = [p for p in out if p['kind'] == 1]
    if len(digits) > 1:
        def u_axis(p):
            A = np.c_[p['P'], np.ones(len(p['P']))]
            return np.linalg.lstsq(A, p['UV'][:, 0], rcond=None)[0][:3]
        ref = u_axis(digits[0]); ref = ref / np.linalg.norm(ref)
        digits.sort(key=lambda p: float(p['P'].mean(0) @ ref))      # along the map's u: the first read is the highest place
        for i, p in enumerate(digits): p['place'] = len(digits) - 1 - i
    return out


def load_graph(mf, tag, weapon_types):
    td = mf.td
    g = mf.tags[(b'antr', tag)] - B
    nc, na, _ = struct.unpack_from('<iII', td, g + 0x68); na -= B
    ac, aa, _ = struct.unpack_from('<iII', td, g + 0x74); aa -= B
    fc, fa, _ = struct.unpack_from('<iII', td, g + 0x48); fa -= B
    assert fc == 1
    names, parents = [], []
    for i in range(nc):
        o = na + i * 0x40
        names.append(td[o:o + 32].split(b'\0')[0].decode()); parents.append(struct.unpack_from('<h', td, o + 0x24)[0])
    cnt, addr, _ = struct.unpack_from('<iII', td, fa + 0x10); addr -= B
    table = [struct.unpack_from('<h', td, addr + i * 2)[0] for i in range(min(cnt, ANIMATION_TYPES))]
    table += [-1] * (ANIMATION_TYPES - len(table))
    used = sorted(set(i for i in table if i >= 0))
    anims = []
    for i in used:
        o = aa + i * 0xB4
        name = td[o:o + 32].split(b'\0')[0].decode()
        typ, frames, fsize, finfo_type = struct.unpack_from('<4h', td, o + 0x20)
        node_count, loop = struct.unpack_from('<2h', td, o + 0x2C)
        key, second_key = struct.unpack_from('<2h', td, o + 0x34)
        flags, = struct.unpack_from('<H', td, o + 0x3A)
        assert not flags & 1, 'a compressed first-person animation: %s' % name
        tflags = struct.unpack_from('<2I', td, o + 0x5C); rflags = struct.unpack_from('<2I', td, o + 0x6C); sflags = struct.unpack_from('<2I', td, o + 0x7C)
        dsize, _f, _fo, daddr, _d = struct.unpack_from('<IIIII', td, o + 0x8C)
        size, _f, _fo, addr2, _d = struct.unpack_from('<IIIII', td, o + 0xA0)
        assert size == frames * fsize, (name, size, frames, fsize)
        anims.append(dict(index=i, name=name, type=typ, frames=frames, frame_size=fsize, node_count=node_count, loop=loop, key=key, second_key=second_key,
                          rflags=rflags, tflags=tflags, sflags=sflags, defaults=td[daddr - B:daddr - B + dsize], data=td[addr2 - B:addr2 - B + size]))
    return dict(tag=tag, names=names, parents=parents, table=table, used=used, animations=anims)


def main():
    mf = MapFile(MAP)
    textures = Textures()
    atlas = Atlas()
    weapons, graphs, displays = [], [], []
    hands = load_model(mf, HANDS, textures, textures)
    print('hands: %d vertices, %d triangles, %d nodes, parts %s' % (len(hands['P']), len(hands['T']), len(hands['nodes']), [(p['name'], p['count']) for p in hands['parts']]))
    for name in WEAPONS:
        tag = 'weapons\\%s\\fp\\fp' % name
        try:
            gun = load_model(mf, tag, textures, textures)
        except AssertionError as e:
            print('%-16s left out: %s' % (name, e)); weapons.append(None); graphs.append(None); displays.append([]); continue
        graph = load_graph(mf, tag, None)
        weapons.append(gun); graphs.append(graph)
        displays.append(load_display(mf, tag, textures, atlas))
        print('%-16s %d vertices, %d triangles, %d model nodes, graph of %d nodes and %d animations; parts %s' %
              (name, len(gun['P']), len(gun['T']), len(gun['nodes']), len(graph['names']), len(graph['animations']), [(p['name'], p['count']) for p in gun['parts']]))
    print('%d textures (%d KB), the display atlas of %d frames (%d KB)' % (len(textures.list), len(textures.list) * (TEX * TEX + 1024) // 1024, len(atlas.frames), ATLAS_W * ATLAS_H // 2 // 1024))

    scenario = [a for (g, n), a in mf.tags.items() if g == b'scnr'][0] - B
    local_north, = struct.unpack_from('<f', mf.td, scenario + 0x4C)        # scenario: three references, the sky block, type, flags, the scenario references, then local_north
    assert -7 < local_north < 7, local_north

    os.makedirs(OUT, exist_ok=True)
    c = open(os.path.join(OUT, 'fpm_assets.c'), 'w')
    c.write('/* generated by tools/export_fp_models.py from the map\'s first-person models and animation graphs; do not edit */\n#include "fpm_assets.h"\n\n')
    for i, t in enumerate(textures.list):
        c.write('/* %s */\n' % t['name'].replace('\\', '/'))
        c.write(cstr_array('fpm_texture%d_pixels' % i, t['pixels'].reshape(-1), 'unsigned char', 64))
        c.write(cstr_array('fpm_texture%d_clut' % i, t['clut'], 'unsigned int', 64))
    atlas_pixels, atlas_clut = atlas.pack()
    c.write('/* the numeric maps of the weapons\' displays: %d frames of %dx%d, 4-bit, %dx%d */\n' % (len(atlas.frames), FRAME, FRAME, ATLAS_W, ATLAS_H))
    c.write(cstr_array('fpm_atlas_pixels', atlas_pixels.reshape(-1), 'unsigned char', 64))
    c.write(cstr_array('fpm_atlas_clut', atlas_clut, 'unsigned int', 64))
    c.write('const fpm_texture fpm_textures[FPM_TEXTURES] = {\n')
    for i in range(len(textures.list)): c.write('\t{ fpm_texture%d_pixels, fpm_texture%d_clut },\n' % (i, i))
    c.write('};\n\n')

    def write_model(sym, m, graph_names):
        c.write(cstr_array(sym + '_position', m['P'].reshape(-1), 'float'))
        c.write(cstr_array(sym + '_normal', np.round(m['N'] * 127).astype(np.int8).reshape(-1), 'signed char'))
        c.write(cstr_array(sym + '_uv', m['UV'].reshape(-1), 'float'))
        c.write(cstr_array(sym + '_node', m['ND'].reshape(-1), 'unsigned char'))
        c.write(cstr_array(sym + '_weight', np.round(m['WG'] * 255).astype(np.uint8), 'unsigned char'))
        c.write(cstr_array(sym + '_triangle', m['T'].astype(np.uint16).reshape(-1), 'unsigned short'))
        inv = np.concatenate([m['inverse_bind'][:, :3, :3].reshape(len(m['nodes']), 9), m['inverse_bind'][:, 3, :3]], 1)
        c.write(cstr_array(sym + '_inverse_bind', inv.reshape(-1), 'float'))
        remap = [graph_names.index(n['name']) if n['name'] in graph_names else -1 for n in m['nodes']]
        c.write(cstr_array(sym + '_graph_node', remap, 'short'))
        c.write('const fpm_model %s = { %d, %d, %d, %d, %s_position, %s_normal, %s_uv, %s_node, %s_weight, %s_triangle, { %s }, %s_inverse_bind, %s_graph_node };\n\n' % (
            sym, len(m['P']), len(m['T']), len(m['parts']), len(m['nodes']), sym, sym, sym, sym, sym, sym,
            ', '.join('{ %d, %d, %d }' % (p['first'], p['count'], p['texture']) for p in m['parts']), sym, sym))

    # the arms are remapped against each weapon's graph (the nodes are the same names): the remap table of the arms, per weapon
    first_graph = next(g for g in graphs if g)
    write_model('fpm_hands', hands, first_graph['names'])
    for w_i, (name, gun, graph) in enumerate(zip(WEAPONS, weapons, graphs)):
        if gun is None: continue
        sym = 'fpm_w%d' % w_i
        write_model(sym + '_gun', gun, graph['names'])
        hands_remap = [graph['names'].index(n['name']) if n['name'] in graph['names'] else -1 for n in hands['nodes']]
        c.write(cstr_array(sym + '_hands_graph_node', hands_remap, 'short'))
        disp = displays[w_i]
        for d_i, d in enumerate(disp):
            ds = '%s_d%d' % (sym, d_i)
            remap = [graph['names'].index(gun['nodes'][n]['name']) if gun['nodes'][n]['name'] in graph['names'] else -1 for n in range(len(gun['nodes']))]
            c.write(cstr_array(ds + '_position', d['P'].reshape(-1), 'float'))
            c.write(cstr_array(ds + '_uv', d['UV'].reshape(-1), 'float'))
            c.write(cstr_array(ds + '_triangle', d['T'].astype(np.uint16).reshape(-1), 'unsigned short'))
        if disp:
            c.write('static const fpm_display_part %s_display_parts[%d] = {\n' % (sym, len(disp)))
            for d_i, d in enumerate(disp):
                ds = '%s_d%d' % (sym, d_i)
                assert len(set(d['ND'].tolist())) == 1, 'a display part on more than one node'
                c.write('\t{ %d, %d, %d, %d, %d, %d, %d, %d, %s_position, %s_uv, %s_triangle, %d },     /* %s */\n' % (
                    d['kind'], len(d['P']), len(d['T']), d['texture'], d['atlas_first'], d['frames'], d['limit'], d['place'], ds, ds, ds, int(d['ND'][0]), d['name'].replace('\\', '/')))
            c.write('};\n')
        c.write(cstr_array(sym + '_parent', graph['parents'], 'short'))
        for a_i, a in enumerate(graph['animations']):
            c.write(cstr_array('%s_a%d_defaults' % (sym, a_i), np.frombuffer(a['defaults'], dtype=np.uint8), 'unsigned char'))
            c.write(cstr_array('%s_a%d_data' % (sym, a_i), np.frombuffer(a['data'], dtype=np.uint8), 'unsigned char'))
        c.write('static const fpm_animation %s_animations[%d] = {\n' % (sym, max(len(graph['animations']), 1)))
        for a_i, a in enumerate(graph['animations']):
            c.write('\t{ /* %s */ %d, %d, %d, %d, %d, %d, %d, { %d, %d }, { %d, %d }, { %d, %d }, %d, %s_a%d_defaults, %s_a%d_data },\n' % (
                a['name'], a['type'], a['frames'], a['frame_size'], a['node_count'], a['loop'], a['key'], a['second_key'],
                a['rflags'][0], a['rflags'][1], a['tflags'][0], a['tflags'][1], a['sflags'][0], a['sflags'][1], len(a['defaults']), sym, a_i, sym, a_i))
        c.write('};\n')
        slot = [graph['used'].index(i) if i >= 0 else -1 for i in graph['table']]
        c.write('const fpm_graph %s_graph = { %d, %s_parent, %d, %s_animations, { %s } };\n\n' % (sym, len(graph['names']), sym, len(graph['animations']), sym, ', '.join(str(s) for s in slot)))
    c.write('const fpm_weapon fpm_weapons[FPW_COUNT] = {\n')
    for w_i, gun in enumerate(weapons):
        if gun is None: c.write('\t{ 0, 0, 0, 0, 0, 0 },\n')
        else: c.write('\t{ &fpm_w%d_gun, &fpm_w%d_graph, fpm_w%d_hands_graph_node, 1, %d, %s },\n' % (w_i, w_i, w_i, len(displays[w_i]), 'fpm_w%d_display_parts' % w_i if displays[w_i] else '0'))
    c.write('};\n')
    c.close()

    h = open(os.path.join(OUT, 'fpm_assets.h'), 'w')
    h.write('''/* generated by tools/export_fp_models.py; do not edit */
#ifndef FPM_ASSETS_H
#define FPM_ASSETS_H
#define FPM_TEXTURES %d
#define FPM_TEXTURE_SIZE %d
#define FPW_COUNT %d
#define FPM_MAX_PARTS 8
#define FPM_ANIMATION_TYPES %d
typedef struct { const unsigned char *pixels; const unsigned int *clut; } fpm_texture;      /* FPM_TEXTURE_SIZE square, 8-bit, the palette in the GS's order */
typedef struct { int first_triangle, triangle_count, texture; } fpm_part;
typedef struct
{
	int vertices, triangles, part_count, node_count;
	const float *position;                  /* 3 a vertex: the bind pose */
	const signed char *normal;              /* 3 a vertex, x 127 */
	const float *uv;                        /* 2 a vertex, in repeats of the texture */
	const unsigned char *node;              /* 2 a vertex: the nodes that skin it */
	const unsigned char *weight;            /* 1 a vertex: the first node's share, x 255 */
	const unsigned short *triangle;         /* 3 a triangle, counter-clockwise from outside */
	fpm_part parts[FPM_MAX_PARTS];
	const float *inverse_bind;              /* 12 a node: a 3x3 and a translation, for row vectors: p * m */
	const short *graph_node;                /* a model node's node in the animation graph, or -1 */
} fpm_model;
typedef struct
{
	short type, frames, frame_size, node_count, loop_frame, key_frame, second_key_frame;   /* type: 0 base, 1 overlay, 2 replacement */
	unsigned rotation_flags[2], translation_flags[2], scale_flags[2];      /* the nodes with each in the frames' data (bit n: node n) */
	int default_size;
	const unsigned char *defaults, *data;   /* the tag's own: a node's compressed rotation (4 shorts), translation (3 floats), scale (a float) where it has one */
} fpm_animation;
typedef struct
{
	int node_count;
	const short *parent;
	int animation_count;
	const fpm_animation *animations;
	short animation_of_type[FPM_ANIMATION_TYPES];     /* index into animations by kind (weapons.h: _first_person_weapon_animation_*), or -1 */
} fpm_graph;
/* a part of a weapon's own display (a transparent chicago shader, added to the picture, unlit, two sided): kind 0 static (texture: an index of fpm_textures),
   1 a digit of a number (the atlas' frames from atlas_first, `frames` of them: the digit is (counter / 10^place) %% frames of the counter round (limit * the
   weapon's value)), 2 the compass (the counter reads the heading); the geometry is in the gun's bind pose on one node of the model */
typedef struct { int kind, vertices, triangles, texture, atlas_first, frames, limit, place; const float *position, *uv; const unsigned short *triangle; int node; } fpm_display_part;
typedef struct { const fpm_model *gun; const fpm_graph *graph; const short *hands_graph_node; int present; int display_count; const fpm_display_part *display; } fpm_weapon;
#define FPM_ATLAS_W %d
#define FPM_ATLAS_H %d
#define FPM_ATLAS_FRAME %d
#define FPM_LOCAL_NORTH %sf                  /* the scenario's local north, radians (the compass part's) */
extern const unsigned char fpm_atlas_pixels[];
extern const unsigned int fpm_atlas_clut[];
extern const fpm_texture fpm_textures[FPM_TEXTURES];
extern const fpm_model fpm_hands;
extern const fpm_weapon fpm_weapons[FPW_COUNT];
#endif
''' % (len(textures.list), TEX, len(WEAPONS), ANIMATION_TYPES, ATLAS_W, ATLAS_H, FRAME, repr(local_north)))
    h.close()
    print('wrote', os.path.abspath(OUT))


if __name__ == '__main__':
    main()
