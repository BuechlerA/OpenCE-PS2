#!/usr/bin/env python3
"""Turn the sample's mip-level audit into tools/mip_floor.json, which export_map.py reads.

The GS picks a texture's mip level from the depth of the pixel alone: level = log2 (z) + K / 16. A texture whose
lowest requested level over the whole camera orbit is N never reads the N levels above it, so leaving them out of
video memory changes nothing in the picture. The audit build prints that lowest level for every texture and pass:

    python3 tools/export_map.py                 # with no mip_floor.json (or MIP_FLOOR=0): the full chains
    make EXTRA_CFLAGS=-DMIP_AUDIT
    python3 tools/ps2/pcsx2_run.py masterchief.elf --until "MIPAUDIT done" > audit.log      # from the repository root
    python3 tools/mip_audit.py audit.log        # from here; reads src/map_assets.c for the names and level counts
    python3 tools/export_map.py                 # now with the levels dropped

The report counts every vertex that reaches the GS, including those in the guard band off the screen and those the
near plane clips, so it errs toward keeping a level. It describes the sample's one camera path: a free camera, or the
Chief somewhere else, needs the audit run again (or the full chains).

Output: {"margin": ..., "base": {texture name: levels to drop}, "detail": {detail map name: levels to drop}}. The audit
is of the build that the current file made, and what it finds is dropped on top: run on a build that has already been
trimmed it finds nothing more to drop, which is the check that the table is complete (and the lowest levels it prints
then must all be at least 0: no pass reads below the level that is left).
"""
import json, math, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src', 'map_assets.c')
OUT = os.path.join(HERE, 'mip_floor.json')
MARGIN = 0.1      # levels: the GS's level of detail is fixed point, and Q has finite precision

def names_and_levels(text):
    """(base textures, detail maps) as [(name, levels)] in the order the program indexes them"""
    base = re.findall(r'\{ map_tex\d+_pixels, map_tex\d+_clut, \d+, \d+, (\d+), \d+ \},\s*/\* (.*?) \*/', text)
    detail = re.findall(r'\{ map_detail\d+_pixels, \d+, \d+, (\d+) \},\s*/\* (.*?) \*/', text)
    return [(n, int(l)) for l, n in base], [(n, int(l)) for l, n in detail]

def drop_for(lowest, levels):
    """levels to leave out: the whole ones below the lowest level asked for, less a margin; at least one level stays"""
    if levels <= 1: return 0
    return max(0, min(levels - 1, int(math.floor(lowest - MARGIN))))

def main():
    log = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    base, detail = names_and_levels(open(SRC).read())
    low_tex, low_detail = {}, {}
    for m in re.finditer(r'MIPAUDIT tex (\d+) drawn (\d+) base (\S+) d0 (\S+) d1 (\S+) bump (\S+)', log):
        low_tex[int(m.group(1))] = (int(m.group(2)), float(m.group(3)))
    for m in re.finditer(r'MIPAUDIT detail (\d+) (\S+)', log):
        low_detail[int(m.group(1))] = float(m.group(2))
    if 'MIPAUDIT done' not in log or len(low_tex) != len(base) or len(low_detail) != len(detail):
        sys.exit('the report is incomplete or from another map: %d of %d textures, %d of %d detail maps' % (len(low_tex), len(base), len(low_detail), len(detail)))
    # the audit is of the build the current mip_floor.json made, so what it finds is dropped on top of what that dropped
    prev = json.load(open(OUT)) if os.path.exists(OUT) else {'base': {}, 'detail': {}}
    out = {'margin': MARGIN, 'base': dict(prev['base']), 'detail': dict(prev['detail'])}
    print('base maps (levels in the full chain, the lowest level asked for, levels dropped)')
    for i, (name, levels) in enumerate(base):
        drawn, low = low_tex[i]
        d = drop_for(low if drawn else 99.0, levels)
        if d: out['base'][name] = out['base'].get(name, 0) + d
        print('  %-62s %d  %6.2f  +%d (%d in all)%s' % (name[-62:], levels, low, d, out['base'].get(name, 0), '' if drawn else '  (never drawn)'))
    print('detail and bump maps')
    for i, (name, levels) in enumerate(detail):
        d = drop_for(low_detail[i], levels)
        if d: out['detail'][name] = out['detail'].get(name, 0) + d
        print('  %-62s %d  %6.2f  +%d (%d in all)' % (name[-62:], levels, low_detail[i], d, out['detail'].get(name, 0)))
    with open(OUT, 'w') as f:
        json.dump(out, f, indent=1, sort_keys=True); f.write('\n')
    print('wrote', os.path.abspath(OUT))

if __name__ == '__main__':
    main()
