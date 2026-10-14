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
  - where the Chief stands: the ground height under MAP_CHIEF_X/Y

  HALO_MAP (default assets/maps/bloodgulch.map), CHIEF_X, CHIEF_Y, CHIEF_BELOW (the highest ground that
  counts) and LM_SCALE (default 1) in the environment.
"""
import os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bsp import MapFile, load_bsp, oriented, ground_height

MAP = os.environ.get('HALO_MAP', os.path.join(HERE, '..', '..', '..', '..', '..', 'assets', 'maps', 'bloodgulch.map'))
OUT = os.path.join(HERE, '..', 'src')
# open ground 12 wu in front of the base whose flag is at (40.2, -79.1): flat, with room to orbit
CHIEF_X = float(os.environ.get('CHIEF_X', 46.9))
CHIEF_Y = float(os.environ.get('CHIEF_Y', -89.0))
CHIEF_BELOW = float(os.environ.get('CHIEF_BELOW', 1.0))
LM_SCALE = float(os.environ.get('LM_SCALE', 1.0))
TAG_BASE = 0x803a6000
BASE_MAP = 0x88                    # offsets in a shader_environment: the base map's tag reference ...
PRIMARY_DETAIL, SECONDARY_DETAIL, MICRO_DETAIL = 0xB4, 0xD4, 0xF8   # ... and each detail map's scale, then its reference
DETAIL_SIZE = int(os.environ.get('DETAIL_SIZE', 128))
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

def gs_palette(pal):          # (256, 3) uint8 -> RGBA words in the GS's CSM1 order, alpha 0x80 (opaque)
    out = np.zeros(256, dtype='<u4')
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        r, g, b = (int(x) for x in pal[i])
        out[j] = r | (g << 8) | (b << 16) | (0x80 << 24)
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

    # the base map of each environment material, and how many triangles use it
    base_of, uses = [], {}
    for m in mats:
        name = mf.ref(mf.tags[(b'senv', m['shader'])] - TAG_BASE + BASE_MAP) if m['group'] == b'senv' else None
        base_of.append(name)
        if name: uses[name] = uses.get(name, 0) + len(m['tris'])
    names = sorted(uses, key=lambda n: -uses[n])
    textures = []
    for name in names:
        info = mf.bitmaps(name)[0]
        w, h = info['w'], info['h']
        if uses[name] < BIG_TRIANGLES and max(w, h) > 256:
            k = max(w, h) // 256; w //= k; h //= k
        idx, pal, levels = quantise(mf.rgb(info), (w, h))
        textures.append(dict(name=name, w=w, h=h, pixels=idx, clut=gs_palette(pal), levels=levels))
        print('  texture %-34s %3dx%-3d (from %dx%d) %5d triangles' % (name.split('\\')[-1], w, h, info['w'], info['h'], uses[name]))
    tex_id = {n: i for i, n in enumerate(names)}

    # the detail maps of each base map's shader: (map id or -1, scale) for the primary and the micro
    detail_names, detail_of = [], []
    for name in names:
        shader = next(m['shader'] for m, b in zip(mats, base_of) if b == name)
        so = mf.tags[(b'senv', shader)] - TAG_BASE
        entry = []
        for off in (PRIMARY_DETAIL, MICRO_DETAIL):
            d = mf.ref(so + off + 4)
            scale = float(np.frombuffer(mf.td, dtype='<f4', count=1, offset=so + off)[0])
            if d and d not in detail_names: detail_names.append(d)
            entry.append((detail_names.index(d) if d else -1, scale))
        detail_of.append(entry)
        print('  %-34s detail %s' % (name.split('\\')[-1], ', '.join('%s x%g' % (detail_names[i].split('\\')[-1], sc) if i >= 0 else '-' for i, sc in entry)))
    details = [grey(mf.rgb(mf.bitmaps(n)[0]), DETAIL_SIZE) for n in detail_names]      # (pixels with their mips, levels)
    pages, page_id = [], {}          # lightmap pages in use: grey, padded; the scale from the page's own to the padded UV

    P, UV, LMUV, C, T, TEX, PAGE, base, flipped = [], [], [], [], [], [], [], 0, 0
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
        P.append(v[:, :3]); UV.append(v[:, 6:8]); LMUV.append(lmuv); C.append(col); T.append(tris + base)
        TEX += [tex_id[name] if name else -1] * len(tris); PAGE += [page] * len(tris)
        base += len(v)
    P = np.concatenate(P).astype(np.float32); UV = np.concatenate(UV).astype(np.float32); LMUV = np.concatenate(LMUV).astype(np.float32)
    C = np.concatenate(C); T = np.concatenate(T); TEX = np.array(TEX); PAGE = np.array(PAGE)
    assert base < 65536
    key = np.where(TEX < 0, 1 << 20, TEX) * 1000 + (PAGE + 1)           # by base map, then lightmap page; the flat ones last
    order = np.argsort(key, kind='stable')
    T = T[order]; TEX = TEX[order]; PAGE = PAGE[order]; key = key[order]
    spans = []                       # base map (-1: none), lightmap page (-1: none), first triangle, triangle count
    for k in sorted(set(key.tolist())):
        first = int(np.argmax(key == k))
        spans += [int(TEX[first]), int(PAGE[first]), first, int((key == k).sum())]
    nspans = len(spans) // 4
    lm_order = sorted((i for i in range(nspans) if spans[i * 4 + 1] >= 0), key=lambda i: spans[i * 4 + 1])
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
    print('%d detail maps (%d KB with mips), %d lightmap pages (%d KB), %d spans in the lightmap pass'
          % (len(details), sum(len(d[0]) for d in details) // 1024, len(pages), sum(p['w'] * p['h'] for p in pages) // 1024, len(lm_order)))
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
        c.write('\t{ { %d, %d }, { %rf, %rf } },\n' % (e[0][0], e[1][0], e[0][1], e[1][1]))
    c.write('};\n')
    c.write(cstr_array('map_tex_density', densities, 'float', 16))
    for i, t in enumerate(textures):
        c.write(cstr_array('map_tex%d_pixels' % i, flat(t['pixels']), 'unsigned char', 64))
        c.write(cstr_array('map_tex%d_clut' % i, flat(t['clut'].astype(np.uint32)), 'unsigned int', 64))
    c.write('const map_texture_data map_textures[MAP_TEXTURES] = {\n')
    for i, t in enumerate(textures):
        c.write('\t{ map_tex%d_pixels, map_tex%d_clut, %d, %d, %d },   /* %s */\n' % (i, i, t['w'], t['h'], t['levels'], t['name'].replace('\\', '/')))
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
#define MAP_DETAILS %d
#define MAP_LIGHTMAPS %d
#define MAP_CHIEF_X %rf
#define MAP_CHIEF_Y %rf
#define MAP_CHIEF_Z %rf
typedef struct { const unsigned char *pixels; const unsigned int *clut; int width, height, levels; } map_texture_data;   /* the mip levels follow one another */
typedef struct { const unsigned char *pixels; int width, height, levels; } map_gray_texture;   /* 8-bit grey, its value the alpha of map_gray_clut */
typedef struct { int detail[2]; float scale[2]; } map_detail_data;                    /* primary and micro detail map of a base map */
extern const float map_pos[], map_uv[];
extern const unsigned char map_color[];
extern const unsigned short map_tri[];
extern const float map_lmuv[], map_tex_density[];   /* texture repeats a world unit of surface, per base map */
extern const int map_spans[];                /* base map (-1: none), lightmap page (-1: none), first triangle, triangle count; MAP_SPANS times */
extern const int map_lm_order[];             /* the spans that have a lightmap page, by page */
extern const unsigned int map_gray_clut[];
extern const map_gray_texture map_details[], map_lightmaps[];
extern const map_detail_data map_detail_of[];
extern const map_texture_data map_textures[];
#endif
''' % (len(P), len(T), len(textures), nspans, len(lm_order), len(details), len(pages), CHIEF_X, CHIEF_Y, float(zb)))
    h.close()
    print('wrote', os.path.abspath(OUT))

if __name__ == '__main__':
    main()
