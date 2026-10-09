#!/usr/bin/env python3
"""The tag cache peak each map would have on the PS2, from the measurements
of map_budget.py and converter_prototype.py (docs/ps2_port_plan.md):

    python tools/ps2/map_budget.py --iso IMAGE --json budget.json
    python tools/ps2/converter_prototype.py --iso IMAGE --json prototype.json
    python tools/ps2/budget_projection.py budget.json prototype.json [--tolerance "0.25 deg, 2 mm"]

The peak is the shared tags plus the structure BSP that is largest after
the changes. The changes, and what each needs:

converter and the PS2 renderer (no change to the game's code):
- model vertices from 32 to 16 bytes, BSP vertices from 40 to 20 (formats
  the PS2 renderer reads);
- animations in the engine's keyframe format (measured, at the tolerance);
- no super-high-only geometry (measured; its vertices at 16 bytes);
- script nodes trimmed to those the scripts made (measured).

changes to the game's code:
- collision BSPs with 16-bit indices (measured; physics/collision_bsp*.c);
- sound permutations without their name, subtitle and unused fields (124
  to 44 bytes) and mouth data at 4 bits (sound code);
- script nodes from 20 to 12 bytes (hs);
- font glyphs at 4 bits (text drawing).

The tag names (all but a quarter, kept for the few lookups by name) are a
further option, shown apart.
"""

import argparse
import json
from pathlib import Path

M = 1048576
SOUND_PERMUTATION_SAVING = 80 / 124


def blocks(r):
    return {f'{group}{path}': n for group, path, n in r['blocks']}


def project(budget, prototype, tolerance):
    b = blocks(budget)
    a = prototype['animations']
    lod = prototype['super_high_lod']
    scripts = prototype['scripts'] or dict(maximum=0, count=0, element_size=0)
    collision = prototype['collision']

    converter = dict(
        model_vertices=budget['before_first_tag']['model_vertices'] * 0.5,
        animations=a['uncompressed_bytes'] - a['keyframes'][tolerance],
        super_high_lod=lod['removable_vertex_bytes'] * 0.5 + lod['removable_index_bytes'],
        script_trim=(scripts['maximum'] - scripts['count']) * scripts['element_size'],
    )
    code = dict(
        collision_models=sum(v[0] - v[1] for v in collision['models'].values()),
        sounds=b.get('snd!/pitch_ranges/permutations', 0) * SOUND_PERMUTATION_SAVING
        + b.get('snd!/pitch_ranges/permutations/mouth_data', 0) * 0.5,
        script_nodes=scripts['count'] * scripts['element_size'] * 0.4,
        fonts=b.get('font/pixels', 0) * 0.5,
    )
    names = budget['before_first_tag']['names'] * 0.75

    def bsp_after(i, with_code):
        bsp = budget['bsps'][i]
        vertices = dict(bsp['blocks']).get('/lightmaps/materials/compressed_vertex_data', 0) * 0.5
        coll = sum(v[0] - v[1] for v in collision['structure_bsps'][i]['blocks'].values()) if with_code else 0
        return bsp['size'] - vertices - coll

    count = len(budget['bsps'])
    xbox = budget['tag_data_size'] + max((b['size'] for b in budget['bsps']), default=0)
    without_code = budget['tag_data_size'] - sum(converter.values()) + max(
        (bsp_after(i, False) for i in range(count)), default=0)
    with_code = budget['tag_data_size'] - sum(converter.values()) - sum(code.values()) + max(
        (bsp_after(i, True) for i in range(count)), default=0)
    return dict(map=budget['map'], xbox=xbox, without_code=without_code, with_code=with_code,
                with_names=with_code - names, converter=converter, code=code)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('budget', type=Path)
    parser.add_argument('prototype', type=Path)
    parser.add_argument('--tolerance', default='0.25 deg, 2 mm')
    args = parser.parse_args()
    budgets = {r['map']: r for r in json.loads(args.budget.read_text())}
    rows = [project(budgets[p['map']], p, args.tolerance)
            for p in json.loads(args.prototype.read_text()) if p['map'] in budgets]
    print(f"animations at {args.tolerance}; MB")
    print(f"{'map':18} {'Xbox':>6} {'converter':>9} {'+code':>6} {'+names':>6}   "
          f"{'model':>5} {'anim':>5} {'LOD':>5} {'hs':>5} | {'collM':>5} {'snd':>5} {'hs':>5} {'font':>5}")
    for r in rows:
        c, k = r['converter'], r['code']
        print(f"{r['map']:18} {r['xbox'] / M:6.2f} {r['without_code'] / M:9.2f} {r['with_code'] / M:6.2f} "
              f"{r['with_names'] / M:6.2f}   " + ' '.join(f'{v / M:5.2f}' for v in c.values()) + ' | '
              + ' '.join(f'{v / M:5.2f}' for v in k.values()))


if __name__ == '__main__':
    main()
