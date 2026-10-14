#!/usr/bin/env python3
"""Bake the Master Chief scene out of a Halo Xbox map into C arrays for the PS2 program.

Reads characters\\cyborg\\cyborg and the assault rifle from the map (HALO_MAP, default
assets/maps/bloodgulch.map), plays the "stand rifle move-front" cycle with the left-hand
adjustment, and writes src/assets.c + src/assets.h:

  - the Chief's armour and visor meshes, the rifle mesh (positions, normals, UVs, nodes, weights)
  - 20 frames of skinning matrices (one 4x3 per node) and the rifle's matrix per frame
  - the Chief's colour-changed base map and the rifle's base map, each as an 8-bit palettised
    512x512 texture (the GS has no DXT), palettes already in the GS's CSM1 order
  - the visor's gold reflection cube map (CPU-lit at run time)
"""
import math, os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import halo_scene as H            # loads the map, meshes, materials, skeleton, animation

OUT = os.path.join(HERE, '..', 'src')

def cstr_array(name, data, ctype='unsigned char', align=64):
    out = ['const %s %s[%d] __attribute__((aligned(%d))) = {' % (ctype, name, len(data), align)]
    line = []
    for i, v in enumerate(data):
        line.append(str(int(v)) if ctype != 'float' else repr(float(v)))
        if len(line) == 24:
            out.append(','.join(line) + ','); line = []
    if line: out.append(','.join(line))
    out.append('};')
    return '\n'.join(out) + '\n'

def flat(a): return np.asarray(a).reshape(-1)

def oriented(P, N, T):
    """triangles wound counter-clockwise seen from outside (against the vertex normals)"""
    T = np.array(T)
    e1 = P[T[:, 1]] - P[T[:, 0]]; e2 = P[T[:, 2]] - P[T[:, 0]]
    fn = np.cross(e1, e2)
    vn = N[T[:, 0]] + N[T[:, 1]] + N[T[:, 2]]
    flip = (fn * vn).sum(1) < 0
    T[flip] = T[flip][:, [0, 2, 1]]
    return T, int(flip.sum())

def mesh_blob(parts, with_nodes):
    """concatenate parts; returns per-vertex arrays and a triangle list per part"""
    P, N, UV, ND, WG, tris, base = [], [], [], [], [], [], 0
    spans = []
    for p in parts:
        T, nf = oriented(p['P'], p['N'], p['T'])
        print('  part: %d verts %d tris (%d flipped)' % (len(p['P']), len(T), nf))
        P.append(p['P']); N.append(p['N']); UV.append(p['UV'])
        if with_nodes: ND.append(p['NODES']); WG.append(p['WGT'])
        tris.append(T + base)
        spans.append((sum(len(t) for t in tris[:-1]), len(T)))
        base += len(p['P'])
    P = np.concatenate(P); N = np.concatenate(N); UV = np.concatenate(UV)
    T = np.concatenate(tris)
    r = dict(P=P, N=N, UV=UV, T=T, spans=spans)
    if with_nodes: r['ND'] = np.concatenate(ND); r['WG'] = np.concatenate(WG)
    return r

def gs_palette(pal):          # pal: (256,3) uint8 -> RGBA words in CSM1 order, alpha 0x80 (opaque)
    out = np.zeros(256, dtype='<u4')
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        r, g, b = (int(x) for x in pal[i])
        out[j] = r | (g << 8) | (b << 16) | (0x80 << 24)
    return out

def quantise(rgb01):
    # gamma encoded here: the program lights in the same space (the GS multiplies colour by texel / 128)
    im = Image.fromarray((np.clip(rgb01, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8))
    q = im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:768], dtype=np.uint8).reshape(-1, 3)
    if len(pal) < 256: pal = np.vstack([pal, np.zeros((256 - len(pal), 3), np.uint8)])
    return np.array(q, dtype=np.uint8), pal

def chief_texture():
    m = H.MATS[H.M_ARMOR]
    n = 512
    uu = (np.arange(n) + 0.5) / n
    u, v = np.meshgrid(uu, uu)
    base = H.sample(m['base'], u, v); mp = H.sample(m['multi'], u, v)
    det = H.sample(m['detail'], u * m['dscale'], v * m['dscale'])
    alb = base * (0.65 + 0.7 * det.mean(-1, keepdims=True))
    mask = mp[..., 2:3]
    alb = alb * (1 - mask) + alb * H.CHANGE * 1.55 * mask       # primary change colour, as in the renderer
    alb = alb + mp[..., 1:2] * np.array([0.1, 1.0, 0.2]) * 0.6   # self-illumination (the green lights)
    alb = alb + mp[..., 0:1] * 0.06                              # a little of the specular mask
    return quantise(alb)

def rifle_texture():
    m = H.MATS[H.M_GUN]
    return quantise(m['base'])

def mask_texture():
    """the armour's specular mask (multipurpose red), as the 8-bit grey the second pass modulates: the
    shader's reflection weight 0.06 + 0.40 m and specular weight 0.15 + 1.1 m are both close to
    k (0.14 + m), so one map, (0.14 + m) / 1.14, serves both"""
    m = H.MATS[H.M_ARMOR]
    n = 512
    uu = (np.arange(n) + 0.5) / n
    u, v = np.meshgrid(uu, uu)
    sm = H.sample(m['multi'], u, v)[..., 0]
    t = np.clip((0.14 + sm) / 1.14, 0, 1)
    pal = np.stack([np.arange(256)] * 3, 1).astype(np.uint8)
    return np.round(t * 255).astype(np.uint8), pal

def rifle_mask_texture():
    """the rifle's specular mask, 256x256 to spare video memory, in the same form as the armour's"""
    m = H.MATS[H.M_GUN]
    n = 256
    uu = (np.arange(n) + 0.5) / n
    u, v = np.meshgrid(uu, uu)
    sm = H.sample(m['multi'], u, v)[..., 0]
    t = np.clip((0.14 + sm) / 1.14, 0, 1)
    pal = np.stack([np.arange(256)] * 3, 1).astype(np.uint8)
    return np.round(t * 255).astype(np.uint8), pal

def display_texture():
    """the rifle's ammo display: its base map plus the self-illumination (multipurpose green) in the
    shader's colour, which the program draws unlit"""
    m = H.MATS[H.M_DISP]
    glow = m['multi'][..., 1:2]
    so = H.TAGS[(b'soso', 'weapons\\assault rifle\\fp\\shaders\\display')] - H.B
    upper = np.array(struct.unpack_from('<3f', H.td, so + 0x84))
    return quantise(m['base'] + glow * upper * 0.9)

def visor_spec():
    m = H.MATS[H.M_VISOR]
    p = [p for p in H.chief if H.MATS[p['mat']]['visor']][0]
    return H.sample(m['multi'], p['UV'][:, 0], p['UV'][:, 1])[:, 0].astype(np.float32)

def frames():
    """per-frame skinning matrices (with the left-hand adjustment) and the rifle's matrix"""
    nf = H.ANIM['fc']
    skin = np.zeros((nf, 19, 12), np.float32); rif = np.zeros((nf, 12), np.float32)
    for f in range(nf):
        S, Wd = H.pose(f / nf)
        Mw = H.RH @ Wd[H.RH_NODE]
        tgt = H.AR_LH @ Mw
        Wd = H.left_arm_ik(Wd, tgt[3, :3], tgt[:3, :3])
        S = [H.INVBIND[i] @ Wd[i] for i in range(len(H.MN))]
        for i, M in enumerate(S): skin[f, i] = np.vstack([M[:3, :3], M[3, :3]]).reshape(-1)
        rif[f] = np.vstack([Mw[:3, :3], Mw[3, :3]]).reshape(-1)
    return skin, rif

def main():
    os.makedirs(OUT, exist_ok=True)
    print('chief')
    chief = mesh_blob(H.chief, True)
    print('rifle')
    rifle = mesh_blob([p for p in H.rifle if p['mat'] != H.M_DISP], False)   # the display has its own map: left out
    # armour first, then the visor (parts_of keeps the map's part order: armor, visor)
    names = [H.MATS[p['mat']]['visor'] for p in H.chief]
    assert names == [False, True], names
    (a_first, a_count), (v_first, v_count) = chief['spans']
    skin, rif = frames()
    dx = H.FINFO[:, 0]
    cum = np.concatenate([[0], np.cumsum(dx)])[:len(dx)].astype(np.float32)
    disp = [p for p in H.rifle if p['mat'] == H.M_DISP][0]
    dT, _ = oriented(disp['P'], disp['N'], disp['T'])
    di, dpal = display_texture()
    ci, cpal = chief_texture(); ri, rpal = rifle_texture(); mi, mpal = mask_texture(); rmi, rmpal = rifle_mask_texture()
    cube = (H.MATS[H.M_VISOR]['cube'] * 255).astype(np.uint8)       # (6,64,64,3)
    tint = np.array([H.MATS[H.M_VISOR]['perp'], H.MATS[H.M_VISOR]['par']], np.float32)
    lo, hi = H.lo, H.hi
    c = open(os.path.join(OUT, 'assets.c'), 'w')
    c.write('/* generated by tools/export_assets.py from characters\\cyborg\\cyborg and the assault rifle; do not edit */\n#include "assets.h"\n\n')
    def put(name, arr, ctype):
        c.write(cstr_array(name, flat(arr), ctype, 16 if ctype != 'unsigned char' else 64))
    put('chief_pos', chief['P'].astype(np.float32), 'float')
    put('chief_nrm', chief['N'].astype(np.float32), 'float')
    put('chief_uv', chief['UV'].astype(np.float32), 'float')
    put('chief_node', chief['ND'].astype(np.uint8), 'unsigned char')
    put('chief_weight', chief['WG'].astype(np.float32), 'float')
    put('chief_tri', chief['T'].astype(np.uint16), 'unsigned short')
    put('rifle_pos', rifle['P'].astype(np.float32), 'float')
    put('rifle_nrm', rifle['N'].astype(np.float32), 'float')
    put('rifle_uv', rifle['UV'].astype(np.float32), 'float')
    put('rifle_tri', rifle['T'].astype(np.uint16), 'unsigned short')
    put('display_pos', disp['P'].astype(np.float32), 'float')
    put('display_nrm', disp['N'].astype(np.float32), 'float')
    put('display_uv', disp['UV'].astype(np.float32), 'float')
    put('display_tri', dT.astype(np.uint16), 'unsigned short')
    put('display_tex_pixels', di, 'unsigned char')
    put('display_tex_clut', gs_palette(dpal).astype(np.uint32), 'unsigned int')
    put('anim_skin', skin, 'float')
    put('anim_rifle', rif, 'float')
    put('anim_scroll', cum, 'float')
    put('chief_tex_pixels', ci, 'unsigned char')
    put('chief_tex_clut', gs_palette(cpal).astype(np.uint32), 'unsigned int')
    put('rifle_tex_pixels', ri, 'unsigned char')
    put('rifle_tex_clut', gs_palette(rpal).astype(np.uint32), 'unsigned int')
    put('mask_tex_pixels', mi, 'unsigned char')
    put('mask_tex_clut', gs_palette(mpal).astype(np.uint32), 'unsigned int')
    put('armor_cube', (H.MATS[H.M_ARMOR]['cube'] * 255).astype(np.uint8), 'unsigned char')
    put('rifle_mask_pixels', rmi, 'unsigned char')
    put('rifle_mask_clut', gs_palette(rmpal).astype(np.uint32), 'unsigned int')
    put('rifle_grip_cube', (H.MATS[H.M_GRIP]['cube'] * 255).astype(np.uint8), 'unsigned char')
    put('rifle_gun_cube', (H.MATS[H.M_GUN]['cube'] * 255).astype(np.uint8), 'unsigned char')
    put('visor_spec', visor_spec(), 'float')
    put('visor_cube', cube, 'unsigned char')
    put('visor_tint', tint, 'float')
    c.close()
    h = open(os.path.join(OUT, 'assets.h'), 'w')
    # the scalars get an f suffix: the EE has no double FPU, so a double literal in a float expression
    # costs hundreds of cycles of software arithmetic
    h.write('''/* generated by tools/export_assets.py; do not edit */
#ifndef ASSETS_H
#define ASSETS_H
#define CHIEF_VERTS %d
#define CHIEF_ARMOR_FIRST_TRI %d
#define CHIEF_ARMOR_TRIS %d
#define CHIEF_VISOR_FIRST_TRI %d
#define CHIEF_VISOR_TRIS %d
#define RIFLE_VERTS %d
#define RIFLE_TRIS %d
#define DISPLAY_VERTS %d
#define DISPLAY_TRIS %d
#define RIFLE_GRIP_VERTS %d
#define RIFLE_GRIP_TRIS %d
#define ANIM_FRAMES %d
#define NODE_COUNT 19
#define ANIM_STRIDE %rf
#define MODEL_CENTER_X %rf
#define MODEL_CENTER_Y %rf
#define MODEL_CENTER_Z %rf
#define MODEL_RADIUS %rf
#define MODEL_FLOOR_Z %rf
extern const float chief_pos[], chief_nrm[], chief_uv[], chief_weight[];
extern const unsigned char chief_node[];
extern const unsigned short chief_tri[];
extern const float rifle_pos[], rifle_nrm[], rifle_uv[];
extern const unsigned short rifle_tri[], display_tri[];
extern const float display_pos[], display_nrm[], display_uv[];
extern const unsigned char display_tex_pixels[];
extern const unsigned int display_tex_clut[];
extern const float anim_skin[], anim_rifle[], anim_scroll[];
extern const unsigned char chief_tex_pixels[], rifle_tex_pixels[], mask_tex_pixels[], rifle_mask_pixels[], visor_cube[], armor_cube[], rifle_grip_cube[], rifle_gun_cube[];
extern const unsigned int chief_tex_clut[], rifle_tex_clut[], mask_tex_clut[], rifle_mask_clut[];
extern const float visor_tint[], visor_spec[];
#endif
''' % (len(chief['P']), a_first, a_count, v_first, v_count, len(rifle['P']), len(rifle['T']), len(disp['P']), len(dT), len(H.rifle[0]['P']), rifle['spans'][0][1],
       skin.shape[0], float(H.STRIDE), float(H.center[0]), float(H.center[1]), float(H.center[2]),
       float(H.radius), float(lo[2])))
    h.close()
    print('wrote', os.path.abspath(OUT))

if __name__ == '__main__':
    main()
