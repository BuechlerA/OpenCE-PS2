#!/usr/bin/env python3
"""Bake the first-person HUD of a Halo Xbox map into C for the PS2 sample: src/hud_assets.c + src/hud_assets.h.

The HUD is data: the weapon hud interface tags (wphi: statics, meters, numbers, overlays and crosshairs, the parent chain of each weapon's), the
grenade hud (grhi), the unit hud (unhi: the shield, health and motion sensor panels), the digits (hud#) and the interface bitmaps they name
(source/interface/hud_weapon.c, hud_unit.c, hud_draw.c, unit_hud_interface_definition.h; the sizes of the structs are the source's own).
This bakes, for the weapons the player carries (HUD_WEAPONS), the elements the single-player HUD draws (an element's use_on_map_type is a bit
of the map's flags: 0 always, 1 one local player, 2 several), flattened parent first as render_weapon_hud draws them, and every bitmap the
elements use, cropped to the sprites they draw, as GS textures:

  - an ordinary image (the statics, overlays, crosshairs, digits, backgrounds) is an alpha / intensity pair (ay8 and a8y8 interface formats; the map's
    are swizzled): 8 bits, a palette of (intensity, intensity, intensity, alpha) in the GS's CSM1 order, so vertex colour times the texture is the
    original's (tint * intensity, alpha * alpha)
  - a meter's image (hud_ammo_meters, hud_unit_meters) is a shape (alpha) and a fill order (intensity: a ramp along the meter): the Xbox's meter pixel
    shader shows a pixel as filled while its intensity is at most the value's alpha (alpha_multiplier * value + alpha_bias). It is kept as one 8-bit
    texture whose texel is 64 * (shape alpha in 4 levels) + (intensity / 4), and the program builds the palette for the value (the filled texels
    in the meter's colour, the others in the empty colour), so one quad draws a meter of any value
"""
import os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bsp import MapFile, unswizzle

MAP = os.environ.get('HALO_MAP', os.path.join(HERE, '..', '..', '..', '..', '..', 'assets', 'maps', 'bloodgulch.map'))
OUT = os.path.join(HERE, '..', 'src')
B = 0x803a6000
HUD_WEAPONS = ['assault rifle', 'pistol']                      # the player's weapons (fp_controller.c: the loadout), in the order of fpw_weapons
UNIT_HUD = 'ui\\hud\\cyborg'
GRENADE_HUD = 'ui\\hud\\default'
DIGITS = 'ui\\hud\\counter'
SINGLE_PLAYER_BIT = 1                                          # use_on_map_type: 0 all, 1 one local player, 2 several; the elements of 2 are left out
MAX_ELEMENTS_PER_HUD = 24


def cstr(name, data, ctype='unsigned char', align=16):
    out = ['const %s %s[%d] __attribute__((aligned(%d))) = {' % (ctype, name, max(len(data), 1), align)]
    line = []
    for v in data:
        line.append(repr(float(v)) + 'f' if ctype == 'float' else str(int(v)))
        if len(line) == 24: out.append(','.join(line) + ','); line = []
    if line: out.append(','.join(line))
    if not len(data): out.append('0')
    out.append('};')
    return '\n'.join(out) + '\n'


def gs_palette(pal4):
    """(256, 4) bytes r, g, b, a (a 0..255) -> the GS's CSM1 order, alpha at its 0..128 scale"""
    out = np.zeros(256, dtype='<u4')
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        r, g, b, a = (int(x) for x in pal4[i])
        out[j] = r | (g << 8) | (b << 16) | (((a + 1) >> 1) << 24)
    return out


class Hud:
    def __init__(self, mf):
        self.mf, self.td = mf, mf.td
        self.images, self.image_key = [], {}                   # an image: (texture, x0, y0, x1, y1) in texels of the texture
        self.textures, self.texture_key = [], {}               # a texture: (group name, bitmap index, meter?) -> data
        self.groups = {}

    def u(self, f, o): return struct.unpack_from(f, self.td, o)

    def block(self, o):
        c, a, _ = self.u('<iII', o)
        return c, a - B

    # ----- bitmaps -----
    def group(self, name):
        if name not in self.groups:
            mf, td = self.mf, self.td
            a = mf.tags[(b'bitm', name)] - B
            typ, = self.u('<h', a)
            sc, sa = self.block(a + 0x54)
            sequences = []
            for s in range(sc):
                o = sa + s * 0x40
                first, count = self.u('<hh', o + 32)
                spc, spa = self.block(o + 0x34)
                sprites = []
                for j in range(spc):
                    e = spa + j * 0x20
                    bi, = self.u('<h', e)
                    x0, x1, y0, y1 = self.u('<4f', e + 8)
                    sprites.append((bi, x0, y0, x1, y1))
                sequences.append(dict(first=first, count=count, sprites=sprites))
            self.groups[name] = dict(type=typ, bitmaps=mf.bitmaps(name), sequences=sequences)
        return self.groups[name]

    def channels(self, info):
        """one bitmap -> (alpha, intensity) as uint8 (h, w): ay8 is one byte that is both, a8y8 alpha in the high byte, a8 alpha, y8 intensity"""
        w, h, fmt, po = info['w'], info['h'], info['fmt'], info['pixoff']
        if fmt in (0, 1, 2):
            raw = np.frombuffer(self.mf.full, dtype=np.uint8, count=w * h, offset=po).astype(np.uint32)
            if info['flags'] & 8: raw = unswizzle(raw, w, h)
            raw = raw.reshape(h, w).astype(np.uint8)
            if fmt == 0: return raw, np.full_like(raw, 255)
            if fmt == 1: return np.full_like(raw, 255), raw
            return raw, raw
        if fmt == 3:
            raw = np.frombuffer(self.mf.full, dtype='<u2', count=w * h, offset=po).astype(np.uint32)
            if info['flags'] & 8: raw = unswizzle(raw, w, h)
            raw = raw.reshape(h, w)
            return (raw >> 8).astype(np.uint8), (raw & 255).astype(np.uint8)
        raise ValueError('bitmap format %d' % fmt)

    def image(self, group_name, sequence, frame=0, meter=False):
        """the image of a sequence's frame: an index of self.images, its size in pixels is the clip's extent (hud_draw.c's hud_calculate_bitmap_bounds)"""
        g = self.group(group_name)
        seq = g['sequences'][sequence]
        if seq['sprites']:
            bi, x0, y0, x1, y1 = seq['sprites'][frame % len(seq['sprites'])]
        else:
            bi, x0, y0, x1, y1 = seq['first'] + frame % max(seq['count'], 1), 0.0, 0.0, 1.0, 1.0
        info = g['bitmaps'][bi]
        if g['type'] == 4:                                      # interface bitmaps: the clip is in pixels
            tx0, ty0, tx1, ty1 = x0, y0, x1, y1
            if seq['sprites'] == [] and (x1, y1) == (1.0, 1.0): tx0, ty0, tx1, ty1 = 0, 0, info['w'], info['h']
        else:
            tx0, ty0, tx1, ty1 = x0 * info['w'], y0 * info['h'], x1 * info['w'], y1 * info['h']
        tx0, ty0, tx1, ty1 = int(round(tx0)), int(round(ty0)), int(round(tx1)), int(round(ty1))
        key = (group_name, bi, meter)
        if key not in self.texture_key:
            self.texture_key[key] = len(self.textures)
            self.textures.append(dict(group=group_name, bitmap=bi, meter=meter, info=info, rects=[]))
        t = self.texture_key[key]
        ikey = (t, tx0, ty0, tx1, ty1)
        if ikey not in self.image_key:
            self.image_key[ikey] = len(self.images)
            self.images.append(dict(texture=t, rect=(tx0, ty0, tx1, ty1)))
            self.textures[t]['rects'].append((tx0, ty0, tx1, ty1))
        return self.image_key[ikey]

    def sequence_images(self, group_name, sequence, meter=False):
        """every frame of a sequence (a sprite each, or a bitmap each): the index of the first image and the count; consecutive"""
        g = self.group(group_name)
        seq = g['sequences'][sequence]
        n = max(len(seq['sprites']), seq['count'], 1)
        first = None
        ids = [self.image(group_name, sequence, f, meter) for f in range(n)]
        return ids

    # ----- tag structures -----
    def placement(self, o):
        ox, oy, sx, sy, mp = self.u('<2h2fh', o)
        return dict(x=ox, y=oy, sx=sx, sy=sy, mp=mp)

    def color(self, o):
        c, fc, per, dly, n, ff, ln, dc = self.u('<II2fhHfI', o)
        return dict(color=c, flash=fc, period=per, delay=dly, flashes=n, flags=ff, length=ln, disabled=dc)

    def static(self, o):
        """a static_hud_element_definition (0x68) -> dict or None (no bitmap)"""
        bm = self.mf.ref(o + 0x24)
        if not bm: return None
        seq, = self.u('<h', o + 0x24 + 16 + 0x20)
        return dict(placement=self.placement(o), colors=self.color(o + 0x34), image=self.image(bm, seq), bitmap=bm, sequence=seq)

    def meter(self, o):
        """a meter_hud_element_definition (0x68)"""
        bm = self.mf.ref(o + 0x24)
        if not bm: return None
        mn, mx, fl, em = self.u('<4I', o + 0x34)
        flags, minval, seq = self.u('<BBh', o + 0x44)
        amul, abias, scale = self.u('<BBh', o + 0x48)
        opacity, fade = self.u('<2f', o + 0x4C)
        disabled, = self.u('<I', o + 0x54)
        return dict(placement=self.placement(o), image=self.image(bm, seq, 0, True), bitmap=bm, sequence=seq, min_color=mn, max_color=mx, flash_color=fl, empty_color=em,
                    flags=flags, minimum=minval, alpha_multiplier=amul, alpha_bias=abias, value_scale=scale, opacity=opacity, fade=fade, disabled=disabled)

    def number(self, o):
        digits, nflags, frac = self.u('<bBb', o + 0x44)
        return dict(placement=self.placement(o), colors=self.color(o + 0x24), digits=digits, flags=nflags, fractional=frac)


    # ----- the grenade hud and the unit hud -----
    def grenade_hud(self, name):
        """grhi (hud_weapon.c: grenade_hud_interface_definition): background, count panel (a background, a number and its flash cutoff), overlays"""
        a = self.mf.tags[(b'grhi', name)] - B
        corner, = self.u('<h', a)
        out = dict(corner=corner, elements=[])
        s = self.static(a + 0x24)
        if s: out['elements'].append(dict(kind='static', state=0, **s))
        g = a + 0x24 + 0x68
        s = self.static(g)
        if s: out['elements'].append(dict(kind='static', state=1, **s))
        n = g + 0x68
        out['cutoff'], = self.u('<h', n + 0x54)
        num = self.number(n)
        out['elements'].append(dict(kind='number', state=2, weapon_flags=0, **num))
        ov = n + 0x54 + 4
        bm = self.mf.ref(ov); ic, ia = self.block(ov + 16)
        for j in range(ic):
            it = ia + j * 0x88
            frate, seq, typ = self.u('<h', it + 0x44)[0], self.u('<h', it + 0x48)[0], self.u('<h', it + 0x4A)[0]
            flags, = self.u('<I', it + 0x4C)
            out['elements'].append(dict(kind='overlay', state=3, placement=self.placement(it), colors=self.color(it + 0x24), frame_rate=frate, type=typ, flags=flags,
                                        images=self.sequence_images(bm, seq), bitmap=bm, sequence=seq))
        return out

    def unit_hud(self, name):
        """unhi (unit_hud_interface_definition.h): the shield and health panels (a background and a meter each, the health's colours by its cutoffs), the motion sensor's
        background and foreground (drawn in the bottom left corner whatever the tag's), the blip placement; states: 0 background, 1 shield background, 2 shield meter,
        3 health background, 4 health meter, 5 motion sensor background, 6 foreground"""
        a = self.mf.tags[(b'unhi', name)] - B
        corner, = self.u('<h', a)
        out = dict(corner=corner, elements=[], blip=self.placement(a + 0x35C))
        def add(kind, state, d, c=None):
            if d: out['elements'].append(dict(kind=kind, state=state, corner=corner if c is None else c, **d))
        add('static', 0, self.static(a + 0x24))
        add('static', 1, self.static(a + 0x8C))
        add('meter', 2, self.meter(a + 0x8C + 0x68))
        mn, mx = self.u('<4I', a + 0x8C + 0xD0)[:0] or (0, 0)
        add('static', 3, self.static(a + 0x17C))
        hm = self.meter(a + 0x17C + 0x68)
        hm['mid_color'], hm['max_cutoff'], hm['min_cutoff'] = self.u('<I2f', a + 0x17C + 0xD0)
        add('meter', 4, hm)
        add('static', 5, self.static(a + 0x26C), 2)             # the motion sensor: bottom left (hud_unit.c)
        add('static', 6, self.static(a + 0x26C + 0x68), 2)
        return out

    def digits(self, name):
        """hud# (hud_draw.c: hud_number_definition): the bitmap of the digits (a sequence of sprites: 0-9, decimal point, colon, minus, m, km) and the spacing"""
        a = self.mf.tags[(b'hud#', name)] - B
        bm = self.mf.ref(a)
        cw, sw, xo, yo, dw, colw = self.u('<6b', a + 16)
        return dict(images=self.sequence_images(bm, 0), character_width=cw, screen_width=sw, x_offset=xo, y_offset=yo, decimal_width=dw, colon_width=colw, bitmap=bm)

    # ----- the weapon hud -----
    def weapon_hud(self, name):
        """one wphi (and its parents) -> dict(levels=[dict(flags, cutoffs, corner, valid_crosshairs, crosshairs=[], elements=[])...]), root first"""
        mf = self.mf
        levels = []
        while name:
            a = mf.tags[(b'wphi', name)] - B
            fl, _pad, total, loaded, heat, age = self.u('<hhhhhh', a + 0x10)
            level = dict(name=name, flags=fl, total=total, loaded=loaded, heat=heat, age=age, corner=self.u('<h', a + 0x3C)[0], valid=self.u('<I', a + 0x9C)[0], crosshairs=[], elements=[])
            c, ba = self.block(a + 0x60)                         # statics
            for i in range(c):
                e = ba + i * 0xB4; st, rf, mt = self.u('<3h', e)
                if mt == 2: continue
                s = self.static(e + 0x24)
                if s: level['elements'].append(dict(kind='static', state=st, runtime=rf, **s))
            c, ba = self.block(a + 0x6C)                         # meters
            for i in range(c):
                e = ba + i * 0xB4; st, rf, mt = self.u('<3h', e)
                if mt == 2: continue
                m = self.meter(e + 0x24)
                if m: level['elements'].append(dict(kind='meter', state=st, runtime=rf, **m))
            c, ba = self.block(a + 0x78)                         # numbers
            for i in range(c):
                e = ba + i * 0xA0; st, rf, mt = self.u('<3h', e)
                if mt == 2: continue
                n = self.number(e + 0x24)
                n['weapon_flags'] = self.u('<H', e + 0x24 + 0x54)[0]
                level['elements'].append(dict(kind='number', state=st, runtime=rf, **n))
            c, ba = self.block(a + 0x90)                         # overlays
            for i in range(c):
                e = ba + i * 0x68; st, rf, mt = self.u('<3h', e)
                if mt == 2: continue
                bm = mf.ref(e + 0x24); ic, ia = self.block(e + 0x24 + 16)
                for j in range(ic):
                    it = ia + j * 0x88
                    frate, seq, typ = self.u('<h', it + 0x44)[0], self.u('<h', it + 0x48)[0], self.u('<h', it + 0x4A)[0]
                    flags, = self.u('<I', it + 0x4C)
                    level['elements'].append(dict(kind='overlay', state=st, runtime=rf, placement=self.placement(it), colors=self.color(it + 0x24), frame_rate=frate,
                                                  type=typ, flags=flags, images=self.sequence_images(bm, seq), bitmap=bm, sequence=seq))
            c, ba = self.block(a + 0x84)                         # crosshairs
            for i in range(c):
                e = ba + i * 0x68; ct, rf, mt = self.u('<3h', e)
                if mt == 2: continue
                bm = mf.ref(e + 0x24); ic, ia = self.block(e + 0x24 + 16)
                for j in range(ic):
                    it = ia + j * 0x6C
                    frate, seq = self.u('<2h', it + 0x44)
                    flags, = self.u('<I', it + 0x48)
                    g = self.group(bm)
                    is_sprite = not flags & 2                    # _hud_crosshair_not_a_sprite_bit
                    imgs = self.sequence_images(bm, seq) if (is_sprite and 0 <= seq < len(g['sequences'])) else []
                    level['crosshairs'].append(dict(type=ct, placement=self.placement(it), colors=self.color(it + 0x24), frame_rate=frate, flags=flags, images=imgs, bitmap=bm, sequence=seq))
            levels.append(level)
            name = mf.ref(a)
        return dict(name=name, levels=levels)


def pow2(n):
    p = 8
    while p < n: p *= 2
    return p


def build_texture(h, t):
    """-> dict(width, height, kind, pixels (h*w uint8), clut (256 uint32 or None), origin (x, y), images adjust)"""
    info = h.groups[t['group']]['bitmaps'][t['bitmap']]
    alpha, inten = h.channels(info)
    x0 = min(r[0] for r in t['rects']); y0 = min(r[1] for r in t['rects'])
    x1 = max(r[2] for r in t['rects']); y1 = max(r[3] for r in t['rects'])
    x0, y0 = max(x0, 0), max(y0, 0); x1, y1 = min(x1, info['w']), min(y1, info['h'])
    W, H = pow2(x1 - x0), pow2(y1 - y0)
    a = np.zeros((H, W), np.uint8); y = np.zeros((H, W), np.uint8)
    a[:y1 - y0, :x1 - x0] = alpha[y0:y1, x0:x1]; y[:y1 - y0, :x1 - x0] = inten[y0:y1, x0:x1]
    if t['meter']:
        a4 = np.select([a < 32, a < 96, a < 176], [0, 1, 2], 3).astype(np.uint8)
        px = np.where(a4 > 0, (a4 << 6) | (y >> 2), 0).astype(np.uint8)
        return dict(width=W, height=H, kind=1, pixels=px, clut=None, origin=(x0, y0))
    pairs = np.stack([y, a], -1).reshape(-1, 2)
    uniq, inverse = np.unique(pairs, axis=0, return_inverse=True)
    if len(uniq) <= 256:
        pal = np.zeros((256, 4), np.uint8)
        pal[:len(uniq), 0] = pal[:len(uniq), 1] = pal[:len(uniq), 2] = uniq[:, 0]; pal[:len(uniq), 3] = uniq[:, 1]
        px = inverse.reshape(H, W).astype(np.uint8)
    else:
        rgba = np.stack([y, y, y, a], -1)
        q = Image.fromarray(rgba, 'RGBA').quantize(256, method=Image.Quantize.FASTOCTREE)
        pal4 = np.array(q.getpalette(rawmode='RGBA')[:1024], dtype=np.uint8).reshape(-1, 4)
        pal = np.zeros((256, 4), np.uint8); pal[:len(pal4)] = pal4
        px = np.array(q, dtype=np.uint8)
    return dict(width=W, height=H, kind=0, pixels=px, clut=gs_palette(pal), origin=(x0, y0))


def main():
    mf = MapFile(MAP)
    h = Hud(mf)
    weapons = {}
    for w in HUD_WEAPONS:
        tag = 'weapons\\%s\\%s' % (w, 'rocket_launcher' if w == 'rocket launcher' else 'flame thrower' if w == 'flamethrower' else w)
        weapons[w] = h.weapon_hud(tag)
        print(w, [(l['name'], len(l['elements']), len(l['crosshairs'])) for l in weapons[w]['levels']])
    grenade = h.grenade_hud(GRENADE_HUD)
    unit = h.unit_hud(UNIT_HUD)
    digits = h.digits(DIGITS)
    print('grenade hud: %d elements; unit hud: %d elements; digits: %d images' % (len(grenade['elements']), len(unit['elements']), len(digits['images'])))
    textures = [build_texture(h, t) for t in h.textures]
    total = 0
    for t, d in zip(h.textures, textures):
        total += d['width'] * d['height'] + (1024 if d['kind'] == 0 else 1024)
        print('  %-28s bitmap %d %s %dx%d' % (t['group'].split('\\')[-1], t['bitmap'], 'meter' if t['meter'] else '     ', d['width'], d['height']))
    print('%d images, %d textures, %d KB' % (len(h.images), len(textures), total // 1024))
    write(h, weapons, grenade, unit, digits, textures)


KIND = dict(static=0, meter=1, number=2, overlay=3, crosshair=4)


def write(h, weapons, grenade, unit, digits, textures):
    os.makedirs(OUT, exist_ok=True)
    refs = []                                                   # a flat list of image indices: an element's frames are consecutive in it

    def ref_range(ids):
        first = len(refs); refs.extend(ids); return first, len(ids)

    def f(x): return repr(float(x)) + 'f'

    def element(e, corner):
        """one hud_element as a designated initialiser, the fields that are not zero"""
        pl = e['placement']
        fields = dict(kind=KIND[e['kind']], state=e.get('state', 0), corner=e.get('corner', corner), x=pl['x'], y=pl['y'], scale_x=f(pl['sx']), scale_y=f(pl['sy']))
        if e['kind'] in ('static', 'meter'): first, count = ref_range([e['image']])
        elif e['kind'] == 'number': first, count = 0, 0
        else: first, count = ref_range(e['images'])
        fields.update(image_first=first, image_count=count)
        col = e.get('colors')
        if col:
            fields.update(color='0x%08XU' % col['color'], flash_color='0x%08XU' % col['flash'], disabled_color='0x%08XU' % col['disabled'], flash_period=f(col['period']),
                          flash_delay=f(col['delay']), flash_length=f(col['length']), flashes=col['flashes'], color_flags=col['flags'])
        if e['kind'] == 'meter':
            fields.update(min_color='0x%08XU' % e['min_color'], max_color='0x%08XU' % e['max_color'], meter_flash_color='0x%08XU' % e['flash_color'], empty_color='0x%08XU' % e['empty_color'],
                          mid_color='0x%08XU' % e.get('mid_color', 0), meter_flags=e['flags'], minimum_value=e['minimum'], alpha_multiplier=e['alpha_multiplier'], alpha_bias=e['alpha_bias'],
                          value_scale=e['value_scale'], opacity=f(e['opacity']), fade=f(e['fade']), max_cutoff=f(e.get('max_cutoff', 0.0)), min_cutoff=f(e.get('min_cutoff', 0.0)),
                          meter_disabled_color='0x%08XU' % e['disabled'])
        if e['kind'] == 'number':
            fields.update(digits=e['digits'], fractional_digits=e['fractional'], number_flags=e['flags'], weapon_flags=e.get('weapon_flags', 0))
        if e['kind'] == 'overlay':
            fields.update(frame_rate=e['frame_rate'], type=e['type'], flags='0x%XU' % e['flags'])
        if e['kind'] == 'crosshair':
            fields.update(frame_rate=e['frame_rate'], flags='0x%XU' % e['flags'])
        return '{ ' + ', '.join('.%s = %s' % (k, v) for k, v in fields.items() if str(v) not in ('0', '0.0f', '0x00000000U')) + ' }'

    c = open(os.path.join(OUT, 'hud_assets.c'), 'w')
    c.write('/* generated by tools/export_hud.py from the map\'s weapon, grenade and unit hud interfaces and their bitmaps; do not edit */\n#include "hud_assets.h"\n\n')
    for i, (t, d) in enumerate(zip(h.textures, textures)):
        c.write('/* %s bitmap %d%s, from (%d, %d) */\n' % (t['group'].replace('\\', '/'), t['bitmap'], ' (a meter)' if t['meter'] else '', d['origin'][0], d['origin'][1]))
        c.write(cstr('hud_texture%d_pixels' % i, d['pixels'].reshape(-1), 'unsigned char', 64))
        if d['clut'] is not None: c.write(cstr('hud_texture%d_clut' % i, d['clut'], 'unsigned int', 64))
    c.write('const hud_texture hud_textures[HUD_TEXTURES] = {\n')
    for i, d in enumerate(textures):
        c.write('\t{ %d, %d, %d, hud_texture%d_pixels, %s },\n' % (d['width'], d['height'], d['kind'], i, 'hud_texture%d_clut' % i if d['clut'] is not None else '0'))
    c.write('};\n\n/* the images: a texture and a rectangle of it in texels (relative to the crop the texture holds) */\nconst hud_image hud_images[HUD_IMAGES] = {\n')
    for im in h.images:
        t = textures[im['texture']]; ox, oy = t['origin']; x0, y0, x1, y1 = im['rect']
        c.write('\t{ %d, %d, %d, %d, %d },\n' % (im['texture'], x0 - ox, y0 - oy, x1 - ox, y1 - oy))
    c.write('};\n\n')

    out_elements = []
    def emit_list(sym, elems, corner):
        lines = [element(e, corner) for e in elems]
        out_elements.append((sym, lines))
        return len(lines)

    weapon_levels = []
    for w_i, w in enumerate(HUD_WEAPONS):
        wl = []
        for l_i, lv in enumerate(weapons[w]['levels']):
            es = 'hud_w%d_l%d_elements' % (w_i, l_i); cs = 'hud_w%d_l%d_crosshairs' % (w_i, l_i)
            ne = emit_list(es, lv['elements'], lv['corner'])
            nc = emit_list(cs, [dict(kind='crosshair', state=x['type'], **{k: v for k, v in x.items() if k != 'type'}) for x in lv['crosshairs']], 4)     # crosshairs: the window's middle
            wl.append((lv, es, ne, cs, nc))
        weapon_levels.append(wl)
    ng = emit_list('hud_grenade_elements', grenade['elements'], grenade['corner'])
    nu = emit_list('hud_unit_elements', unit['elements'], unit['corner'])
    for sym, lines in out_elements:
        c.write('static const hud_element %s[%d] = {\n' % (sym, max(len(lines), 1)))
        for ln in lines: c.write('\t%s,\n' % ln)
        if not lines: c.write('\t{ 0 }\n')
        c.write('};\n')
    c.write(cstr('hud_refs', refs, 'short'))
    c.write('\n')
    for w_i, wl in enumerate(weapon_levels):
        c.write('static const hud_level hud_w%d_levels[%d] = {\n' % (w_i, len(wl)))
        for lv, es, ne, cs, nc in wl:
            c.write('\t{ %d, %d, %d, %d, %d, %d, 0x%XU, %s, %d, %s, %d },     /* %s */\n' % (lv['flags'], lv['total'], lv['loaded'], lv['heat'], lv['age'], lv['corner'], lv['valid'], es, ne, cs, nc, lv['name'].replace('\\', '/')))
        c.write('};\n')
    c.write('const hud_weapon hud_weapons[HUD_WEAPONS] = {\n')
    for w_i, wl in enumerate(weapon_levels): c.write('\t{ %d, hud_w%d_levels },     /* %s */\n' % (len(wl), w_i, HUD_WEAPONS[w_i]))
    c.write('};\n\n')
    c.write('const hud_element *const hud_grenade = hud_grenade_elements;\nconst int hud_grenade_count = %d;\nconst short hud_grenade_flash_cutoff = %d;\n' % (ng, grenade['cutoff']))
    c.write('const hud_element *const hud_unit = hud_unit_elements;\nconst int hud_unit_count = %d;\n' % nu)
    bp = unit['blip']
    c.write('const short hud_blip_x = %d, hud_blip_y = %d;\n' % (bp['x'], bp['y']))
    c.write('const hud_digits hud_counter = { %d, %d, %d, %d, %d, { %s } };\n' % (digits['character_width'], digits['screen_width'], digits['x_offset'], digits['y_offset'], digits['decimal_width'], ', '.join(str(i) for i in digits['images'])))
    c.close()

    hh = open(os.path.join(OUT, 'hud_assets.h'), 'w')
    hh.write("""/* generated by tools/export_hud.py; do not edit */
#ifndef HUD_ASSETS_H
#define HUD_ASSETS_H
#define HUD_TEXTURES %d
#define HUD_IMAGES %d
#define HUD_WEAPONS %d
#define HUD_DIGIT_IMAGES %d
/* a texture: 8-bit texels and a palette (kind 0), or a meter's (kind 1: texel = 64 * the shape's alpha in 4 levels + the fill order / 4; the program makes the palette);
   width and height are powers of 2 */
typedef struct { short width, height, kind; const unsigned char *pixels; const unsigned int *clut; } hud_texture;
typedef struct { short texture, x0, y0, x1, y1; } hud_image;           /* a rectangle of a texture in texels: also its size on the screen in pixels */
enum { HUD_STATIC = 0, HUD_METER, HUD_NUMBER, HUD_OVERLAY, HUD_CROSSHAIR };
/* an element of a hud (hud_weapon.c, hud_unit.c): `state` is the weapon hud state it reads (0 total ammunition, 1 rounds loaded, 2 heat, 3 age, 4 and 5 the secondary magazine), a
   crosshair's type (0 aim, 1 zoom, ...), the grenade hud's part (0 background, 1 count panel background, 2 number, 3 overlay) or the unit hud's (0 background, 1 shield background,
   2 shield meter, 3 health background, 4 health meter, 5 and 6 the motion sensor's background and foreground); `corner` is hud_definitions.h's hud_anchor */
typedef struct
{
	unsigned char kind, state;
	short corner, x, y;
	float scale_x, scale_y;
	short image_first, image_count;                  /* into hud_refs: the images (a sprite sequence's frames) */
	unsigned color, flash_color, disabled_color;
	float flash_period, flash_delay, flash_length;
	short flashes; unsigned short color_flags;
	unsigned min_color, max_color, meter_flash_color, empty_color, mid_color, meter_disabled_color;
	unsigned char meter_flags, minimum_value, alpha_multiplier, alpha_bias; short value_scale;
	float opacity, fade, max_cutoff, min_cutoff;
	signed char digits, fractional_digits; unsigned short number_flags, weapon_flags;
	short frame_rate, type; unsigned flags;
} hud_element;
typedef struct { short flags, total_ammo, loaded_ammo, heat, age, corner; unsigned valid_crosshairs; const hud_element *elements; int element_count; const hud_element *crosshairs; int crosshair_count; } hud_level;
typedef struct { int level_count; const hud_level *levels; } hud_weapon;      /* the weapon's own hud first, then its parents */
typedef struct { signed char character_width, screen_width, x_offset, y_offset, decimal_width; short images[HUD_DIGIT_IMAGES]; } hud_digits;
extern const hud_texture hud_textures[HUD_TEXTURES];
extern const hud_image hud_images[HUD_IMAGES];
extern const short hud_refs[];
extern const hud_weapon hud_weapons[HUD_WEAPONS];
extern const hud_element *const hud_grenade, *const hud_unit;
extern const int hud_grenade_count, hud_unit_count;
extern const short hud_grenade_flash_cutoff, hud_blip_x, hud_blip_y;
extern const hud_digits hud_counter;
#endif
""" % (len(textures), len(h.images), len(HUD_WEAPONS), len(digits['images'])))
    hh.close()
    print('wrote', os.path.abspath(OUT))


if __name__ == '__main__':
    main()
