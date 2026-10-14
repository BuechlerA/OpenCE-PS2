#!/usr/bin/env python3
"""Bake the map's structure BSP into C arrays for the PS2 program: src/map_assets.c + src/map_assets.h.

  - every material's vertices merged into one array: position, texture coordinates (they tile) and a colour
  - the triangles wound counter-clockwise from outside, sorted into spans, one per base map
  - each environment shader's base map as an 8-bit palettised texture (the GS has no DXT), 256 pixels at
    most on a side unless many triangles use it, palette already in the GS's CSM1 order
  - the colour of a textured vertex is the lightmap sampled at the vertex (the GS multiplies the texel by
    colour / 128, so the lightmap's full white is 2x: the shader's overbright), without the lightmap's
    unused texels (a red and yellow checker). Vertices of other shaders (lights, the teleporter shield)
    get a flat colour from a small table.
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
BASE_MAP = 0x88                    # offset of the base map's tag reference in a shader_environment
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

def quantise(rgb01, size):
    """rgb01 (h, w, 3) floats in the game's own (display) encoding -> (indices, palette), cut down to size = (w, h)"""
    im = Image.fromarray((np.clip(rgb01, 0, 1) * 255 + 0.5).astype(np.uint8))
    if im.size != size: im = im.resize(size, Image.BOX)
    q = im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:768], dtype=np.uint8).reshape(-1, 3)
    if len(pal) < 256: pal = np.vstack([pal, np.zeros((256 - len(pal), 3), np.uint8)])
    return np.array(q, dtype=np.uint8), pal

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
        idx, pal = quantise(mf.rgb(info), (w, h))
        textures.append(dict(name=name, w=w, h=h, pixels=idx, clut=gs_palette(pal)))
        print('  texture %-34s %3dx%-3d (from %dx%d) %5d triangles' % (name.split('\\')[-1], w, h, info['w'], info['h'], uses[name]))
    tex_id = {n: i for i, n in enumerate(names)}

    P, UV, C, T, TEX, base, flipped = [], [], [], [], [], 0, 0
    for m, name in zip(mats, base_of):
        v = m['verts']; n = m['normals']
        n = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        if name and m['lm'] is not None and m['lightmap'] >= 0:
            img, valid = lightmap(m['lightmap_group'], m['lightmap'])
            lu = (m['lm'][:, 2].view(np.int16) * 2.0 + 1.0) / 65535.0
            lv = (m['lm'][:, 3].view(np.int16) * 2.0 + 1.0) / 65535.0
            col = np.clip(lightmap_at(img, valid, lu, lv) * 128 * LM_SCALE * 2, 0, 255)      # the GS: texel * colour / 128
        elif name:
            col = np.full((len(v), 3), 128.0)
        else:
            col = np.tile(np.array(flat_color(m['shader'])) * 255, (len(v), 1))
        tris, nf = oriented(v[:, :3].astype(np.float64), n, m['tris']); flipped += nf
        P.append(v[:, :3]); UV.append(v[:, 6:8]); C.append(col); T.append(tris + base)
        TEX += [tex_id[name] if name else -1] * len(tris)
        base += len(v)
    P = np.concatenate(P).astype(np.float32); UV = np.concatenate(UV).astype(np.float32)
    C = np.concatenate(C); T = np.concatenate(T); TEX = np.array(TEX)
    assert base < 65536
    order = np.argsort(np.where(TEX < 0, 1 << 20, TEX), kind='stable')     # the flat ones last
    T = T[order]; TEX = TEX[order]
    spans = []
    for t in sorted(set(TEX.tolist()), key=lambda t: 1 << 20 if t < 0 else t):
        spans += [t, int(np.argmax(TEX == t)), int((TEX == t).sum())]
    rgba = np.zeros((len(P), 4), np.uint8)
    rgba[:, :3] = np.round(C); rgba[:, 3] = 0x80
    zb = ground_height(P.astype(np.float64), T, CHIEF_X, CHIEF_Y, CHIEF_BELOW)
    print('%d materials, %d verts, %d tris (%d flipped), %d textures (%d KB of pixels), %d spans; ground under (%.1f, %.1f): %s'
          % (len(mats), len(P), len(T), flipped, len(textures), sum(t['w'] * t['h'] for t in textures) // 1024, len(spans) // 3, CHIEF_X, CHIEF_Y, zb))
    assert zb is not None, 'no ground under the Chief'
    os.makedirs(OUT, exist_ok=True)
    c = open(os.path.join(OUT, 'map_assets.c'), 'w')
    c.write('/* generated by tools/export_map.py from the map\'s structure BSP; do not edit */\n#include "map_assets.h"\n\n')
    c.write(cstr_array('map_pos', flat(P), 'float', 16))
    c.write(cstr_array('map_uv', flat(UV), 'float', 16))
    c.write(cstr_array('map_color', flat(rgba), 'unsigned char', 16))
    c.write(cstr_array('map_tri', flat(T.astype(np.uint16)), 'unsigned short', 16))
    c.write(cstr_array('map_spans', spans, 'int', 16))
    for i, t in enumerate(textures):
        c.write(cstr_array('map_tex%d_pixels' % i, flat(t['pixels']), 'unsigned char', 64))
        c.write(cstr_array('map_tex%d_clut' % i, flat(t['clut'].astype(np.uint32)), 'unsigned int', 64))
    c.write('const map_texture_data map_textures[MAP_TEXTURES] = {\n')
    for i, t in enumerate(textures):
        c.write('\t{ map_tex%d_pixels, map_tex%d_clut, %d, %d },   /* %s */\n' % (i, i, t['w'], t['h'], t['name'].replace('\\', '/')))
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
#define MAP_CHIEF_X %rf
#define MAP_CHIEF_Y %rf
#define MAP_CHIEF_Z %rf
typedef struct { const unsigned char *pixels; const unsigned int *clut; int width, height; } map_texture_data;
extern const float map_pos[], map_uv[];
extern const unsigned char map_color[];
extern const unsigned short map_tri[];
extern const int map_spans[];                /* texture (-1: none), first triangle, triangle count; MAP_SPANS times */
extern const map_texture_data map_textures[];
#endif
''' % (len(P), len(T), len(textures), len(spans) // 3, CHIEF_X, CHIEF_Y, float(zb)))
    h.close()
    print('wrote', os.path.abspath(OUT))

if __name__ == '__main__':
    main()
