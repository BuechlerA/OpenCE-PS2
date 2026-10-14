#!/usr/bin/env python3
"""Bake the map's structure BSP into C arrays for the PS2 program: src/map_assets.c + src/map_assets.h.

  - every material's vertices merged into one array: position, texture coordinates (they tile) and a colour
  - the triangles wound counter-clockwise from outside, sorted into spans, one per base map
  - each environment shader's base map as an 8-bit palettised texture (the GS has no DXT), 256 pixels at
    most on a side unless many triangles use it, palette already in the GS's CSM1 order
  - the shader's detail maps (primary and micro) as 8-bit grey textures, 128 pixels a side: the GS
    multiplies the picture by them (a pass whose palette alpha is the grey: x 2 d, "double biased multiply")
  - the lightmap pages as 8-bit grey textures of the brightest channel (a pass the same way), the unused
    texels (a red and yellow checker) filled in from their neighbours, each page at least 64 wide for the
    GS; a textured vertex's colour is the lightmap's hue there (colour / brightest channel), which the
    base pass multiplies in. Vertices of other shaders (lights, the teleporter shield) get a flat colour
    from a small table.
  - the triangles grouped into chunks (a grid of CHUNK_CELL units on a side; each with a bounding sphere), so
    that the EE skips what is out of view: a span is a run of batches, each the triangles of one chunk
  - the scenery: the rocks, plants, trees and bases of the scenario's scenery block (not the vehicles), at
    their lowest level of detail, placed in the world, lit from the lightmap at the nearest vertex of the map; their
    base maps are cut to 64 pixels (128 for the biggest rock), foliage cut out by a palette entry of alpha 0
  - a plane for each triangle (its normal and offset), so that the EE can drop the triangles that face away from the
    camera before it transforms their vertices; the sky's planes always face it
  - bump: each environment shader's bump map (a palette of normals) as a grey "emboss" texture, 128 pixels a side: the
    normal against a light from the upper left in the texture's own space, so that the pass that multiplies by it (as
    a detail map does) gives the relief; and, for shaders with a reflection cube map, the cube (16x16 a face) and a
    normal for each vertex, for the colour the EE works out per vertex and adds
  - the sky: the sky model's dome with its gradient as a 64x64 texture, drawn first around the camera (its vertices
    are transformed by the view without its translation: they come last, from MAP_SKY_FIRST_VERT on)
  - where the Chief stands: the ground height under MAP_CHIEF_X/Y

  HALO_MAP (default assets/maps/bloodgulch.map), CHIEF_X, CHIEF_Y, CHIEF_BELOW (the highest ground that
  counts) and LM_SCALE (default 1) in the environment.
"""
import os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bsp import MapFile, load_bsp, oriented, ground_height, unswizzle
from dxt import decode_dxt1

MAP = os.environ.get('HALO_MAP', os.path.join(HERE, '..', '..', '..', '..', '..', 'assets', 'maps', 'bloodgulch.map'))
OUT = os.path.join(HERE, '..', 'src')
# open ground among the shrubs, ferns and boulders of the valley's west side, with grass and a clear sky behind (the
# base's front, (46.9, -89.0) with CHIEF_BELOW 1, was the first choice)
CHIEF_X = float(os.environ.get('CHIEF_X', 68.5))
CHIEF_Y = float(os.environ.get('CHIEF_Y', -97.5))
CHIEF_BELOW = float(os.environ.get('CHIEF_BELOW', 6.0))
LM_SCALE = float(os.environ.get('LM_SCALE', 1.0))
TAG_BASE = 0x803a6000
BASE_MAP = 0x88                    # offsets in a shader_environment: the base map's tag reference ...
PRIMARY_DETAIL, SECONDARY_DETAIL, MICRO_DETAIL = 0xB4, 0xD4, 0xF8   # ... and each detail map's scale, then its reference
DETAIL_SIZE = int(os.environ.get('DETAIL_SIZE', 128))
SCENERY_SIZE = int(os.environ.get('SCENERY_SIZE', 64))      # a scenery base map's longest side, at most
GROUND_SIZE = int(os.environ.get('GROUND_SIZE', 256))
SUN = None
EMBOSS_LIGHT = (-0.45, -0.45, 0.77)       # in the bump map's tangent space
EMBOSS_GAIN = float(os.environ.get('EMBOSS_GAIN', 2.4))
BUMP_SCALE_AT, BUMP_MAP_AT, REFLECTION_CUBE_AT = 0x124, 0x128, 0x324    # in a shader_environment
CHUNK_CELL = float(os.environ.get('CHUNK_CELL', 12))
BIG_TRIANGLES = 1000               # a base map under this many triangles is cut to 256 pixels

def flat(a): return np.asarray(a).reshape(-1)

def cstr_array(name, data, ctype='unsigned char', align=64):
    out = ['const %s %s[%d] __attribute__((aligned(%d))) = {' % (ctype, name, len(data), align)]
    line = []
    for i, v in enumerate(data):
        line.append(str(int(v)) if ctype != 'float' else repr(float(v)) + 'f')
        if len(line) == 24:
            out.append(','.join(line) + ','); line = []
    if line: out.append(','.join(line))
    out.append('};')
    return '\n'.join(out) + '\n'

def gs_palette(pal, clear_last=False):          # (256, 3) uint8 -> RGBA words in the GS's CSM1 order, alpha 0x80 (opaque)
    out = np.zeros(256, dtype='<u4')
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        r, g, b = (int(x) for x in pal[i])
        out[j] = r | (g << 8) | (b << 16) | ((0 if clear_last and i == 255 else 0x80) << 24)     # entry 255 alpha 0: cut out
    return out

def mip_sizes(w, h):
    """the size of each mip level: halved while both sides stay at least 16 by 8 (the GS moves 16 bytes at a time), 7 levels at most"""
    out = [(w, h)]
    while len(out) < 7 and out[-1][0] // 2 >= 16 and out[-1][1] // 2 >= 8:
        out.append((out[-1][0] // 2, out[-1][1] // 2))
    return out

def quantise(rgb01, size, mips=True):
    """rgb01 (h, w, 3) floats in the game's own (display) encoding -> (indices of each mip level, one after the other,
    palette, level count), cut down to size = (w, h); every level takes the palette of the first"""
    im = Image.fromarray((np.clip(rgb01, 0, 1) * 255 + 0.5).astype(np.uint8))
    sizes = mip_sizes(*size) if mips else [size]
    q = None; data = []
    for sz in sizes:
        lv = im if im.size == sz else im.resize(sz, Image.BOX)
        if q is None:
            q = lv.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE); idx = q
        else:
            idx = lv.quantize(palette=q, dither=Image.Dither.NONE)
        data.append(np.array(idx, dtype=np.uint8).reshape(-1))
    pal = np.array(q.getpalette()[:768], dtype=np.uint8).reshape(-1, 3)
    if len(pal) < 256: pal = np.vstack([pal, np.zeros((256 - len(pal), 3), np.uint8)])
    return np.concatenate(data), pal, len(sizes)

def quantise_cutout(rgb01, alpha, size):
    """a picture with a cut-out: 255 colours (taken where it is opaque) and index 255 for what is not, each mip level
    from the picture's own average, the colour of what is opaque spread into what is not (a bilinear filter reads it)"""
    sizes = mip_sizes(*size)
    q = None; data = []
    for sz in sizes:
        a = np.asarray(Image.fromarray(alpha.astype(np.float32)).resize(sz, Image.BOX), dtype=np.float64)
        c = np.stack([np.asarray(Image.fromarray((rgb01[..., k] * alpha).astype(np.float32)).resize(sz, Image.BOX), dtype=np.float64) for k in range(3)], -1)
        c = c / np.maximum(a, 1e-3)[..., None]
        opaque = a >= 0.5
        fill = c[opaque].mean(0) if opaque.any() else np.array([0.3, 0.3, 0.3])
        c[~opaque] = fill
        im = Image.fromarray((np.clip(c, 0, 1) * 255 + 0.5).astype(np.uint8))
        if q is None:
            q = im.quantize(255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE); idx = q
        else:
            idx = im.quantize(palette=q, dither=Image.Dither.NONE)
        ix = np.array(idx, dtype=np.uint8); ix[~opaque] = 255
        data.append(ix.reshape(-1))
    pal = np.array(q.getpalette()[:765], dtype=np.uint8).reshape(-1, 3)
    pal = np.vstack([pal, np.zeros((256 - len(pal), 3), np.uint8)])
    return np.concatenate(data), pal, len(sizes)

# the lightmaps' unused texels are a red and yellow checker: pure (31, 0, 0) and (31, 63, 0) in 5-6-5
def lightmap_valid(img):
    q = np.round(img * np.array([31, 63, 31])).astype(int)
    checker = (q[..., 0] == 31) & (q[..., 2] == 0) & ((q[..., 1] == 0) | (q[..., 1] == 63))
    return ~checker

def lightmap_at(img, valid, u, v):
    """bilinear, over the valid texels only; (n, 3). u, v in [0, 1], the texel centres at (i + 0.5) / size"""
    h, w = valid.shape
    x = u * w - 0.5; y = v * h - 0.5
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx = x - x0; fy = y - y0
    acc = np.zeros((len(u), 3)); wsum = np.zeros(len(u))
    for dx, dy, wt in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        xi = np.clip(x0 + dx, 0, w - 1); yi = np.clip(y0 + dy, 0, h - 1)
        wt = wt * valid[yi, xi]
        acc += img[yi, xi] * wt[:, None]; wsum += wt
    out = acc / np.maximum(wsum, 1e-9)[:, None]
    for i in np.nonzero(wsum < 1e-6)[0]:       # all four texels unused: the nearest valid one
        cx, cy = int(round(x[i])), int(round(y[i]))
        for r in range(1, 8):
            ys, xs = np.mgrid[max(cy - r, 0):min(cy + r + 1, h), max(cx - r, 0):min(cx + r + 1, w)]
            good = valid[ys, xs]
            if good.any():
                d = (ys - y[i]) ** 2 + (xs - x[i]) ** 2; d = np.where(good, d, 1e9)
                k = np.unravel_index(np.argmin(d), d.shape); out[i] = img[ys[k], xs[k]]; break
        else:
            out[i] = (0.5, 0.5, 0.5)
    return out

def dilate(img, valid, steps=3):
    """fill the unused texels from the used ones around them (the lightmap pass's bilinear filter reaches them)"""
    out = img.copy(); known = valid.copy()
    for _ in range(steps):
        pk = np.pad(known, 1); po = np.pad(out, ((1, 1), (1, 1), (0, 0)))
        acc = np.zeros_like(out); cnt = np.zeros(known.shape)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            k = pk[1 + dy:1 + dy + known.shape[0], 1 + dx:1 + dx + known.shape[1]]
            o = po[1 + dy:1 + dy + known.shape[0], 1 + dx:1 + dx + known.shape[1]]
            acc += o * k[..., None]; cnt += k
        fill = ~known & (cnt > 0)
        out[fill] = acc[fill] / cnt[fill][:, None]
        known |= fill
    out[~known] = img[valid].mean(0) if valid.any() else 0.5
    return out

def grey_max(img01, w, h):
    """the brightest channel of the lightmap times LM_SCALE, 8 bits"""
    return (np.clip(img01.max(2) * LM_SCALE, 0, 1) * 255 + 0.5).astype(np.uint8)

def gs_gray_clut():
    """alpha = index, in the GS's CSM1 order (colour irrelevant: the passes that use it only blend by alpha)"""
    out = np.zeros(256, dtype='<u4')
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        out[j] = i | (i << 8) | (i << 16) | (i << 24)
    return out

def grey(img01, size):
    """(h, w, 3) floats -> 8-bit luminance, box-filtered to size x size, then each mip level one after the other; and the level count"""
    l = np.einsum('ijk,k->ij', img01, np.array([0.299, 0.587, 0.114]))
    im = Image.fromarray((np.clip(l, 0, 1) * 255 + 0.5).astype(np.uint8))
    sizes = mip_sizes(size, size)
    data = [np.array(im if im.size == sz else im.resize(sz, Image.BOX), dtype=np.uint8).reshape(-1) for sz in sizes]
    if len(data) >= 3:           # the last level is the average: far away a detail map tiles visibly, so it fades to nothing
        data[-1] = np.full_like(data[-1], int(round(float(data[0].mean()))))
    return np.concatenate(data), len(sizes)

SKY_MODEL = None     # the scenario's sky tag's model, read from the map

def sky_dome(mf):
    """the sky dome's vertices, UVs and triangles (wound to face the camera inside it), and its gradient as a picture"""
    scenario = [a for (g, n), a in mf.tags.items() if g == b'scnr'][0] - TAG_BASE
    c, a, _ = struct.unpack_from('<iII', mf.td, scenario + 48)
    sky_tag = mf.ref(a - TAG_BASE)                                      # the scenario's first sky reference
    sky_addr = mf.tags[(b'sky ', sky_tag)] - TAG_BASE
    model = mf.ref(sky_addr)
    part = mf.model_parts(model, 0)[0]                                  # geometry 0 is the dome
    shader = part['shader']
    so = mf.tags[shader] - TAG_BASE
    count, maps, _ = struct.unpack_from('<iII', mf.td, so + 84); maps -= TAG_BASE
    pics = [mf.ref(maps + i * 220 + 108) for i in range(count)]
    best = [p for p in pics if mf.bitmaps(p)[0]['fmt'] in (10, 11)] or pics[-1:]
    img = mf.rgb(mf.bitmaps(best[0])[0])
    P, T = part['pos'], part['tris']
    n = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    flip = (n * P[T].mean(1)).sum(1) > 0                                # facing away from the middle: turn it round
    T = T.copy(); T[flip] = T[flip][:, [0, 2, 1]]
    return P, part['uv'] * np.array(part['scale']), T, img

SCENERY_SKIP = ('floor_arrow', 'doorblinker', 'scenery\\teleporter_shield\\teleporter', 'small beacon')   # markers of the game, not of the place (a beacon is 10 cm across, 470 triangles)
SUN_DIRECTION = (0.6, -0.5, 0.7)

def rotation(yaw, pitch, roll):
    cy, sy, cp, sp, cr, sr = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch), np.cos(roll), np.sin(roll)
    rz = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]])
    ry = np.array([[cp, 0, sp], [0, 1, 0], [-sp, 0, cp]])
    rx = np.array([[1, 0, 0], [0, cr, -sr], [0, sr, cr]])
    return rz @ ry @ rx

def scenery_pieces(mf, lit_xy, lit_rgb):
    """the scenario's scenery as pieces in the world: dict(P, UV, C (0..255, 128 = the picture as it is), T, bitmap (the base map's
    name, or None for a flat colour), alpha (the base map is cut out))"""
    td = mf.td
    scenario = [a for (g, n), a in mf.tags.items() if g == b'scnr'][0] - TAG_BASE
    def block(o):
        c, a, _ = struct.unpack_from('<iII', td, scenario + o); return c, a - TAG_BASE
    pc, pa = block(540)
    palette = [mf.ref(pa + i * 48) for i in range(pc)]
    count, ia = block(528)
    sun = np.array(SUN_DIRECTION) / np.linalg.norm(SUN_DIRECTION)
    models = {}
    pieces = []
    for i in range(count):
        o = ia + i * 72
        t = struct.unpack_from('<h', td, o)[0]
        if t < 0 or any(k in palette[t] for k in SCENERY_SKIP): continue
        pos = np.array(struct.unpack_from('<3f', td, o + 8)); yaw, pitch, roll = struct.unpack_from('<3f', td, o + 20)
        if palette[t] not in models:
            model = mf.ref(mf.tags[(b'scen', palette[t])] - TAG_BASE + 40)
            models[palette[t]] = (model, mf.model_parts(model, mf.model_lod_geometry(model)))
        model, parts = models[palette[t]]
        R = rotation(yaw, pitch, roll)
        # the lightmap's colour beside the object: the nearest vertex of the map
        d = ((lit_xy - pos[:2]) ** 2).sum(1)
        ambient = np.clip(lit_rgb[np.argmin(d)], 0.05, 1.0)
        for part in parts:
            group, sname = part['shader']
            so = mf.tags[(group, sname)] - TAG_BASE
            bitmap, factor, flat = None, 1.0, None
            if group == b'soso':
                bitmap = mf.ref(so + 0xA4)
                detail = mf.ref(so + 0xDC)
                if detail:                                   # the detail map multiplies by 2 d: its average goes into the base map
                    factor = float(np.clip(2 * mf.rgb(mf.bitmaps(detail)[0]).mean(), 0.4, 2.0))
                uvs = np.array(struct.unpack_from('<2f', td, so + 0x9c))
            elif group == b'senv':
                bitmap = mf.ref(so + BASE_MAP); uvs = np.ones(2)
            elif 'beacon' in sname.lower():
                flat = (0.2, 0.35, 1.0) if 'blue' in sname.lower() else (1.0, 0.2, 0.15); uvs = np.ones(2)
            else:
                continue                                      # glass, shields, glows
            P = part['pos']; N = part['normal']; T = part['tris']
            if len(T) == 0: continue
            T, _ = oriented(P, N, T)
            alpha = group == b'soso' and mf.bitmaps(bitmap)[0]['fmt'] in (15, 16)      # a shader_model's alpha cuts it out; an environment shader's does something else
            Pw = P @ R.T + pos; Nw = N @ R.T
            light = np.clip(Nw @ sun, 0, 1)
            col = 128 * 2 * ambient[None, :] * (0.45 + 0.8 * light)[:, None]
            if flat is not None: col = np.tile(np.array(flat) * 255, (len(P), 1))
            pieces.append(dict(P=Pw, N=Nw, UV=part['uv'] * np.array(part['scale']) * uvs, C=np.clip(col, 0, 255), T=T, bitmap=bitmap, alpha=alpha, factor=factor,
                               name='%s #%d' % (palette[t], i)))
    return pieces

_VECTORS = None
def vector_palette():
    """the engine's table of normals the bump maps' bytes index (source/bitmaps/bitmaps.c), as (256, 3) floats"""
    global _VECTORS
    if _VECTORS is None:
        import re
        src = open(os.path.join(HERE, '..', '..', '..', '..', '..', 'source', 'bitmaps', 'bitmaps.c')).read()
        body = src[src.index('global_vector_palette[NUMBER_OF_ENTRIES_IN_PALETTE]'):]
        body = body[:body.index('};')]
        words = [int(w, 16) for w in re.findall(r'0x[0-9A-Fa-f]{8}', body)]
        assert len(words) == 256, len(words)
        a = np.array(words, dtype=np.int64)
        _VECTORS = np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], 1) / 255.0 * 2 - 1
    return _VECTORS

def emboss_map(mf, name, size):
    """a bump map (8 bits, indexing the table of normals) as the grey a multiply pass turns into relief: 0.5 * (n . L) / L.z,
    stretched by EMBOSS_GAIN about 1 (flat), with its mip levels"""
    info = mf.bitmaps(name)[0]
    w, h = info['w'], info['h']
    raw = np.frombuffer(mf.full, dtype=np.uint8, count=w * h, offset=info['pixoff']).astype(np.uint32)
    if info['flags'] & 8: raw = unswizzle(raw, w, h)
    n = vector_palette()[raw.reshape(h, w).astype(np.int64)]
    light = np.array(EMBOSS_LIGHT) / np.linalg.norm(EMBOSS_LIGHT)
    e = (n @ light) / light[2]
    e = 1 + EMBOSS_GAIN * (e - 1)
    d = np.clip(e / 2, 0.1, 0.95)
    return grey(np.stack([d, d, d], -1), size)

def cube_faces(mf, name, res=16):
    """a cube map (six DXT1 faces) as (6, res, res, 3) bytes"""
    info = mf.bitmaps(name)[0]
    w, h = info['w'], info['h']
    stride = info['pixsize'] // 6
    faces = []
    for f in range(6):
        face = decode_dxt1(mf.full[info['pixoff'] + f * stride:info['pixoff'] + f * stride + w * h // 2], w, h)[..., :3]
        im = Image.fromarray(face).resize((res, res), Image.BOX)
        faces.append(np.asarray(im))
    return np.stack(faces)

def flat_color(shader):
    s = shader.lower()
    for key, c in (('yellow', (1.0, 0.85, 0.35)), ('black', (0.08, 0.08, 0.08)), ('red', (1.0, 0.2, 0.15)),
                   ('blue', (0.2, 0.35, 1.0)), ('green', (0.3, 1.0, 0.4)), ('moss', (0.33, 0.45, 0.22)),
                   ('shield', (0.4, 0.8, 1.0))):
        if key in s: return c
    return (0.7, 0.7, 0.7)

def main():
    mf = MapFile(MAP)
    mats = load_bsp(mf)
    lightmaps = {}
    def lightmap(group, index):
        if (group, index) not in lightmaps:
            img = mf.rgb(mf.bitmaps(group)[index])
            lightmaps[(group, index)] = (img, lightmap_valid(img))
        return lightmaps[(group, index)]

    # the lightmap's colour at every lit vertex of the map (where the scenery takes its light from)
    lit_xy, lit_rgb = [], []
    for m in mats:
        if m['group'] == b'senv' and m['lm'] is not None and m['lightmap'] >= 0:
            img, valid = lightmap(m['lightmap_group'], m['lightmap'])
            lu = (m['lm'][:, 2].view(np.int16) * 2.0 + 1.0) / 65535.0
            lv = (m['lm'][:, 3].view(np.int16) * 2.0 + 1.0) / 65535.0
            lit_xy.append(m['verts'][:, :2]); lit_rgb.append(lightmap_at(img, valid, lu, lv))
    lit_xy = np.concatenate(lit_xy).astype(np.float64); lit_rgb = np.concatenate(lit_rgb)
    pieces = scenery_pieces(mf, lit_xy, lit_rgb)
    print('scenery: %d pieces, %d triangles, %d vertices' % (len(pieces), sum(len(q['T']) for q in pieces), sum(len(q['P']) for q in pieces)))

    # the base map of each environment material, and how many triangles use it
    base_of, uses = [], {}
    base_names_bsp = set()
    for m in mats:
        name = mf.ref(mf.tags[(b'senv', m['shader'])] - TAG_BASE + BASE_MAP) if m['group'] == b'senv' else None
        base_of.append(name)
        if name: uses[name] = uses.get(name, 0) + len(m['tris']); base_names_bsp.add(name)
    extra_names, alpha_names, factors = set(), set(), {}
    for q in pieces:
        if q['bitmap']:
            uses[q['bitmap']] = uses.get(q['bitmap'], 0) + len(q['T'])
            if q['bitmap'] not in base_names_bsp: extra_names.add(q['bitmap'])
            if q['alpha']: alpha_names.add(q['bitmap'])
            factors[q['bitmap']] = q['factor']
    names = sorted(uses, key=lambda n: -uses[n])
    textures = []
    for name in names:
        info = mf.bitmaps(name)[0]
        w, h = info['w'], info['h']
        if name in extra_names:                                           # scenery: small
            cap = max(SCENERY_SIZE, 128 if 'rock large' in name else 0)
            while max(w, h) > cap: w //= 2; h //= 2
        elif name.endswith('blood ground'):
            while max(w, h) > GROUND_SIZE: w //= 2; h //= 2
        elif uses[name] < BIG_TRIANGLES and max(w, h) > 256:
            k = max(w, h) // 256; w //= k; h //= k
        w, h = max(w, 16), max(h, 16)
        picture = mf.rgb(info)
        if name in factors and factors[name] != 1.0 and name in extra_names: picture = np.clip(picture * factors[name], 0, 1)
        if name in alpha_names:
            idx, pal, levels = quantise_cutout(picture, mf.alpha(info), (w, h))
            textures.append(dict(name=name, w=w, h=h, pixels=idx, clut=gs_palette(pal, clear_last=True), levels=levels, alpha=1))
        else:
            idx, pal, levels = quantise(picture, (w, h))
            textures.append(dict(name=name, w=w, h=h, pixels=idx, clut=gs_palette(pal), levels=levels, alpha=0))
        print('  texture %-34s %3dx%-3d (from %dx%d) %5d triangles%s' % (name.split('\\')[-1], w, h, info['w'], info['h'], uses[name], ' (cut out)' if name in alpha_names else ''))
    tex_id = {n: i for i, n in enumerate(names)}

    # the detail maps of each base map's shader: (map id or -1, scale) for the primary and the micro
    detail_names, detail_of, bump_names, cubes, cube_names, refl_of = [], [], [], [], [], []
    for name in names:
        shader = next((m['shader'] for m, b in zip(mats, base_of) if b == name), None)
        if shader is None:
            detail_of.append([(-1, 0.0), (-1, 0.0), (-1, 0.0)]); refl_of.append((-1, 0.0, 0.0)); continue
        so = mf.tags[(b'senv', shader)] - TAG_BASE
        entry = []
        for off in (PRIMARY_DETAIL, MICRO_DETAIL):
            d = mf.ref(so + off + 4)
            scale = float(np.frombuffer(mf.td, dtype='<f4', count=1, offset=so + off)[0])
            if d and d not in detail_names: detail_names.append(d)
            entry.append((detail_names.index(d) if d else -1, scale))
        bump = mf.ref(so + BUMP_MAP_AT)
        bscale = float(np.frombuffer(mf.td, dtype='<f4', count=1, offset=so + BUMP_SCALE_AT)[0])
        if bump and mf.bitmaps(bump)[0]['fmt'] == 17:
            if bump not in bump_names: bump_names.append(bump)
            entry.append((-2 - bump_names.index(bump), bscale))                   # resolved below, once the details are counted
        else:
            entry.append((-1, 0.0))
        detail_of.append(entry)
        cube = mf.ref(so + REFLECTION_CUBE_AT)
        if cube:
            if cube not in cube_names: cube_names.append(cube); cubes.append(cube_faces(mf, cube))
            refl_of.append((cube_names.index(cube), 0.20, 0.80))
        else:
            refl_of.append((-1, 0.0, 0.0))
        print('  %-34s detail %s; bump %s; reflects %s' % (name.split('\\')[-1], ', '.join('%s x%g' % (detail_names[i].split('\\')[-1], sc) if i >= 0 else '-' for i, sc in entry[:2]),
              bump.split('\\')[-1] if bump else '-', cube.split('\\')[-1] if cube else '-'))
    details = [grey(mf.rgb(mf.bitmaps(n)[0]), DETAIL_SIZE) for n in detail_names]      # (pixels with their mips, levels)
    for n in bump_names:
        details.append(emboss_map(mf, n, DETAIL_SIZE)); detail_names.append(n + ' (emboss)')
    ndetail = len(detail_names) - len(bump_names)
    detail_of = [[(i if i >= -1 else ndetail + (-2 - i), sc) for i, sc in e] for e in detail_of]
    pages, page_id = [], {}          # lightmap pages in use: grey, padded; the scale from the page's own to the padded UV

    P, UV, LMUV, C, T, TEX, PAGE, NRM, base, flipped = [], [], [], [], [], [], [], [], 0, 0
    for m, name in zip(mats, base_of):
        v = m['verts']; n = m['normals']
        n = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        page = -1
        lmuv = np.zeros((len(v), 2))
        if name and m['lm'] is not None and m['lightmap'] >= 0:
            img, valid = lightmap(m['lightmap_group'], m['lightmap'])
            lu = (m['lm'][:, 2].view(np.int16) * 2.0 + 1.0) / 65535.0
            lv = (m['lm'][:, 3].view(np.int16) * 2.0 + 1.0) / 65535.0
            rgb = lightmap_at(img, valid, lu, lv)
            col = np.clip(rgb / np.maximum(rgb.max(1, keepdims=True), 1e-3) * 128, 0, 255)   # the hue: the GS multiplies by colour / 128
            key = (m['lightmap_group'], m['lightmap'])
            if key not in page_id:
                h_, w_ = img.shape[:2]
                W, H = max(64, w_), h_
                lum = grey_max(dilate(img, valid), w_, h_)
                padded = np.zeros((H, W), np.uint8); padded[:, :w_] = lum; padded[:, w_:] = lum[:, -1:]
                page_id[key] = len(pages); pages.append(dict(pixels=padded, w=W, h=H, scale=(w_ / W, 1.0), levels=1))
            page = page_id[key]
            sc = pages[page]['scale']
            lmuv = np.stack([lu * sc[0], lv * sc[1]], 1)
        elif name:
            col = np.full((len(v), 3), 128.0)
        else:
            col = np.tile(np.array(flat_color(m['shader'])) * 255, (len(v), 1))
        tris, nf = oriented(v[:, :3].astype(np.float64), n, m['tris']); flipped += nf
        P.append(v[:, :3]); UV.append(v[:, 6:8]); LMUV.append(lmuv); C.append(col); T.append(tris + base); NRM.append(n)
        TEX += [tex_id[name] if name else -1] * len(tris); PAGE += [page] * len(tris)
        base += len(v)
    PIECE = [-1] * len(TEX)                                              # which piece of scenery a triangle belongs to
    for k, q in enumerate(pieces):
        P.append(q['P']); UV.append(q['UV']); LMUV.append(np.zeros((len(q['P']), 2))); C.append(q['C']); T.append(q['T'] + base); NRM.append(q['N'])
        TEX += [tex_id[q['bitmap']] if q['bitmap'] else -1] * len(q['T']); PAGE += [-1] * len(q['T']); PIECE += [k] * len(q['T'])
        base += len(q['P'])
    # the sky, last: a texture of its own, no lightmap, neutral colour
    sky_p, sky_uv, sky_t, sky_img = sky_dome(mf)
    sky_idx, sky_pal, sky_levels = quantise(sky_img, (64, 64), mips=False)
    sky_tex = len(textures)
    textures.append(dict(name='sky gradient', w=64, h=64, pixels=sky_idx, clut=gs_palette(sky_pal), levels=sky_levels, alpha=0))
    detail_of.append([(-1, 0.0), (-1, 0.0), (-1, 0.0)]); refl_of.append((-1, 0.0, 0.0))
    sky_first_vertex = base
    P.append(sky_p); UV.append(sky_uv); LMUV.append(np.zeros((len(sky_p), 2))); C.append(np.full((len(sky_p), 3), 128.0)); T.append(sky_t + base); NRM.append(np.zeros((len(sky_p), 3)))
    TEX += [sky_tex] * len(sky_t); PAGE += [-1] * len(sky_t); PIECE += [-1] * len(sky_t)
    base += len(sky_p)
    SKYTRI = np.zeros(len(TEX), bool); SKYTRI[len(TEX) - len(sky_t):] = True
    P = np.concatenate(P).astype(np.float32); UV = np.concatenate(UV).astype(np.float32); LMUV = np.concatenate(LMUV).astype(np.float32)
    C = np.concatenate(C); T = np.concatenate(T); TEX = np.array(TEX); PAGE = np.array(PAGE); PIECE = np.array(PIECE)
    NRM = np.concatenate(NRM)
    nrm8 = np.zeros((len(NRM), 4), np.int8); nrm8[:, :3] = np.round(np.clip(NRM, -1, 1) * 127)
    assert base < 65536
    # chunks: the cell of a grid its centre falls in, numbered from 0
    terrain = ~SKYTRI & (PIECE < 0)
    centre = P[T[terrain]].astype(np.float64).mean(1)
    cell = np.floor((centre[:, :2] - centre[:, :2].min(0)) / CHUNK_CELL).astype(np.int64)
    _, inv = np.unique(cell[:, 0] * 100000 + cell[:, 1], return_inverse=True)
    CHUNK = np.zeros(len(T), np.int64); CHUNK[terrain] = inv
    nterrain = int(inv.max()) + 1
    CHUNK[PIECE >= 0] = nterrain + PIECE[PIECE >= 0]                    # a chunk of its own for each piece of scenery
    nchunks = nterrain + len(pieces) + 1                                 # the last chunk is the sky's: always in view
    sky_chunk = nchunks - 1
    CHUNK[SKYTRI] = sky_chunk
    assert nchunks < 1000
    key = np.where(TEX < 0, 1 << 20, TEX) * 1000 + (PAGE + 1)           # by base map, then lightmap page; the flat ones last
    key[SKYTRI] = -1                                                     # and the sky first
    order = np.argsort(key * 1000 + CHUNK, kind='stable')               # and by chunk within a span
    T = T[order]; TEX = TEX[order]; PAGE = PAGE[order]; key = key[order]; CHUNK = CHUNK[order]
    spans, batches = [], []          # spans: base map (-1: none), lightmap page (-1: none), first triangle, triangle count, first batch, batch count
    for k in sorted(set(key.tolist())):                                 # batches: chunk, first triangle, triangle count
        idx = np.nonzero(key == k)[0]; first = int(idx[0])
        runs = [0] + [i for i in range(1, len(idx)) if CHUNK[idx[i]] != CHUNK[idx[i - 1]]] + [len(idx)]
        spans += [int(TEX[first]), int(PAGE[first]), first, len(idx), len(batches) // 3, len(runs) - 1]
        for a, b in zip(runs[:-1], runs[1:]): batches += [int(CHUNK[idx[a]]), first + a, b - a]
    nspans = len(spans) // 6
    bounds = []                      # per chunk: centre, radius, how far away it is still drawn, three zeros
    for c in range(nchunks):
        if c == sky_chunk: bounds += [0.0, 0.0, 0.0, 1e9, 1e9, 0.0, 0.0, 0.0]; continue
        sel = T[CHUNK == c]
        if len(sel) == 0: bounds += [0.0, 0.0, -1e6, 0.0, 0.0, 0.0, 0.0, 0.0]; continue         # empty
        v = P[sel].reshape(-1, 3).astype(np.float64)
        ctr = (v.min(0) + v.max(0)) / 2
        r = float(np.linalg.norm(v - ctr, axis=1).max()) + 1e-3
        far = 1e9 if c < nterrain else float(np.clip(30 * r + 20, 35, 120))       # a small prop is not worth drawing far off
        bounds += [*ctr, r, far, 0.0, 0.0, 0.0]
    # each triangle's plane: front-facing when the camera is on the side its normal points to
    pa_, pb_, pc_ = (P[T[:, k]].astype(np.float64) for k in range(3))
    pn = np.cross(pb_ - pa_, pc_ - pa_); pl = np.linalg.norm(pn, axis=1, keepdims=True)
    pn = pn / np.maximum(pl, 1e-12)
    planes = np.concatenate([pn, -(pn * pa_).sum(1, keepdims=True)], 1)
    planes[pl[:, 0] < 1e-12] = (0, 0, 0, 1)                              # a degenerate triangle: left to the screen test
    planes[CHUNK == sky_chunk] = (0, 0, 0, 1)                            # the sky is seen from inside, always
    lm_order = sorted((i for i in range(nspans) if spans[i * 6 + 1] >= 0), key=lambda i: spans[i * 6 + 1])
    # how many texture repeats a world unit of surface spans, per base map: what the GS's level of detail needs (it
    # works from the depth alone). The median over the triangles, by their area.
    densities = []
    for t in range(len(textures)):
        sel = T[TEX == t]
        a, b, c = P[sel[:, 0]].astype(np.float64), P[sel[:, 1]].astype(np.float64), P[sel[:, 2]].astype(np.float64)
        wa = np.linalg.norm(np.cross(b - a, c - a), axis=1) / 2
        ua, ub, uc = UV[sel[:, 0]].astype(np.float64), UV[sel[:, 1]].astype(np.float64), UV[sel[:, 2]].astype(np.float64)
        uva = np.abs((ub[:, 0] - ua[:, 0]) * (uc[:, 1] - ua[:, 1]) - (uc[:, 0] - ua[:, 0]) * (ub[:, 1] - ua[:, 1])) / 2
        ok = wa > 1e-9
        rho = np.sqrt(uva[ok] / wa[ok]); order_ = np.argsort(rho); cw = np.cumsum(wa[ok][order_])
        densities.append(float(rho[order_][np.searchsorted(cw, cw[-1] / 2)]))
    print('  UV repeats a world unit:', ', '.join('%.3g' % d for d in densities))
    rgba = np.zeros((len(P), 4), np.uint8)
    rgba[:, :3] = np.round(C); rgba[:, 3] = 0x80
    zb = ground_height(P.astype(np.float64), T, CHIEF_X, CHIEF_Y, CHIEF_BELOW)
    print('%d materials, %d verts, %d tris (%d flipped), %d textures (%d KB of pixels), %d spans; ground under (%.1f, %.1f): %s'
          % (len(mats), len(P), len(T), flipped, len(textures), sum(len(t['pixels']) for t in textures) // 1024, nspans, CHIEF_X, CHIEF_Y, zb))
    print('%d detail maps (%d KB with mips), %d lightmap pages (%d KB), %d spans in the lightmap pass; %d chunks, %d batches'
          % (len(details), sum(len(d[0]) for d in details) // 1024, len(pages), sum(p['w'] * p['h'] for p in pages) // 1024, len(lm_order), nchunks, len(batches) // 3))
    assert zb is not None, 'no ground under the Chief'
    os.makedirs(OUT, exist_ok=True)
    c = open(os.path.join(OUT, 'map_assets.c'), 'w')
    c.write('/* generated by tools/export_map.py from the map\'s structure BSP; do not edit */\n#include "map_assets.h"\n\n')
    c.write(cstr_array('map_pos', flat(P), 'float', 16))
    c.write(cstr_array('map_uv', flat(UV), 'float', 16))
    c.write(cstr_array('map_lmuv', flat(LMUV), 'float', 16))
    c.write(cstr_array('map_color', flat(rgba), 'unsigned char', 16))
    c.write(cstr_array('map_tri', flat(T.astype(np.uint16)), 'unsigned short', 16))
    c.write(cstr_array('map_spans', spans, 'int', 16))
    c.write(cstr_array('map_batches', batches, 'int', 16))
    c.write(cstr_array('map_planes', flat(planes.astype(np.float32)), 'float', 16))
    c.write(cstr_array('map_normals', flat(nrm8), 'signed char', 16))
    c.write(cstr_array('map_chunks', bounds, 'float', 16))
    c.write(cstr_array('map_lm_order', lm_order, 'int', 16))
    c.write(cstr_array('map_gray_clut', flat(gs_gray_clut()), 'unsigned int', 64))
    for i, (d, _levels) in enumerate(details):
        c.write(cstr_array('map_detail%d_pixels' % i, flat(d), 'unsigned char', 64))
    for i, pg in enumerate(pages):
        c.write(cstr_array('map_page%d_pixels' % i, flat(pg['pixels']), 'unsigned char', 64))
    c.write('const map_gray_texture map_details[MAP_DETAILS] = {\n')
    for i, (d, levels) in enumerate(details):
        c.write('\t{ map_detail%d_pixels, %d, %d, %d },   /* %s */\n' % (i, DETAIL_SIZE, DETAIL_SIZE, levels, detail_names[i].replace('\\', '/')))
    c.write('};\nconst map_gray_texture map_lightmaps[MAP_LIGHTMAPS] = {\n')
    for i, pg in enumerate(pages):
        c.write('\t{ map_page%d_pixels, %d, %d, 1 },\n' % (i, pg['w'], pg['h']))
    c.write('};\nconst map_detail_data map_detail_of[MAP_TEXTURES] = {\n')
    for e in detail_of:
        c.write('\t{ { %d, %d, %d }, { %rf, %rf, %rf } },\n' % (e[0][0], e[1][0], e[2][0], e[0][1], e[1][1], e[2][1]))
    c.write('};\n')
    c.write('const map_reflection_data map_reflection_of[MAP_TEXTURES] = {\n')
    for cube, perp, par in refl_of: c.write('\t{ %d, %rf, %rf },\n' % (cube, perp, par))
    c.write('};\n')
    for i, cb in enumerate(cubes): c.write(cstr_array('map_cube%d' % i, flat(cb), 'unsigned char', 16))
    c.write('const unsigned char *const map_cubes[%d] = { %s };\n' % (max(len(cubes), 1), ', '.join('map_cube%d' % i for i in range(len(cubes))) or '0'))
    c.write(cstr_array('map_tex_density', densities, 'float', 16))
    for i, t in enumerate(textures):
        c.write(cstr_array('map_tex%d_pixels' % i, flat(t['pixels']), 'unsigned char', 64))
        c.write(cstr_array('map_tex%d_clut' % i, flat(t['clut'].astype(np.uint32)), 'unsigned int', 64))
    c.write('const map_texture_data map_textures[MAP_TEXTURES] = {\n')
    for i, t in enumerate(textures):
        c.write('\t{ map_tex%d_pixels, map_tex%d_clut, %d, %d, %d, %d },   /* %s */\n' % (i, i, t['w'], t['h'], t['levels'], t['alpha'], t['name'].replace('\\', '/')))
    c.write('};\n')
    c.close()
    h = open(os.path.join(OUT, 'map_assets.h'), 'w')
    h.write('''/* generated by tools/export_map.py; do not edit */
#ifndef MAP_ASSETS_H
#define MAP_ASSETS_H
#define MAP_VERTS %d
#define MAP_TRIS %d
#define MAP_TEXTURES %d
#define MAP_SPANS %d
#define MAP_LM_SPANS %d
#define MAP_SKY_FIRST_VERT %d
#define MAP_SKY_CHUNK %d
#define MAP_CHUNKS %d
#define MAP_BATCHES %d
#define MAP_DETAILS %d
#define MAP_LIGHTMAPS %d
#define MAP_CHIEF_X %rf
#define MAP_CHIEF_Y %rf
#define MAP_CHIEF_Z %rf
typedef struct { const unsigned char *pixels; const unsigned int *clut; int width, height, levels, alpha; } map_texture_data;   /* the mip levels follow one another; alpha: palette entry 255 is cut out */
typedef struct { const unsigned char *pixels; int width, height, levels; } map_gray_texture;   /* 8-bit grey, its value the alpha of map_gray_clut */
typedef struct { int detail[3]; float scale[3]; } map_detail_data;                    /* primary and micro detail map and bump (emboss) map of a base map, and their scales */
typedef struct { int cube; float perpendicular, parallel; } map_reflection_data;       /* the cube map a base map's shader reflects (-1: none) and its strength head-on and at a graze */
extern const float map_pos[], map_uv[];
extern const unsigned char map_color[];
extern const unsigned short map_tri[];
extern const float map_lmuv[], map_tex_density[];   /* texture repeats a world unit of surface, per base map */
extern const int map_spans[];                /* base map (-1: none), lightmap page (-1: none), first triangle, triangle count, first batch, batch count; MAP_SPANS times */
extern const float map_planes[];              /* a plane (normal, offset) for each triangle: front-facing if the camera is on its normal's side */
extern const int map_batches[];              /* chunk, first triangle, triangle count; MAP_BATCHES times: a span's triangles by chunk */
extern const float map_chunks[];             /* centre x, y, z, radius, the distance beyond which it is not drawn, 3 unused; MAP_CHUNKS times */
extern const int map_lm_order[];             /* the spans that have a lightmap page, by page */
extern const unsigned int map_gray_clut[];
extern const map_gray_texture map_details[], map_lightmaps[];
extern const map_detail_data map_detail_of[];
extern const map_reflection_data map_reflection_of[];
extern const signed char map_normals[];       /* x, y, z (x 127) and a pad, for each vertex */
extern const unsigned char *const map_cubes[];   /* the reflected cube maps: six faces of 16x16 RGB */
extern const map_texture_data map_textures[];
#endif
''' % (len(P), len(T), len(textures), nspans, len(lm_order), sky_first_vertex, sky_chunk, nchunks, len(batches) // 3, len(details), len(pages), CHIEF_X, CHIEF_Y, float(zb)))
    h.close()
    print('wrote', os.path.abspath(OUT))

if __name__ == '__main__':
    main()
