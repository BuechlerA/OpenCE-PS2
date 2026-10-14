#!/usr/bin/env python3
"""Bake the hand weapons' numbers out of a Halo Xbox map into C for the first-person weapon logic: src/weapon_assets.c + src/weapon_assets.h.

For each hand weapon that has a first-person model in the map (HALO_MAP, default assets/maps/bloodgulch.map) the weapon tag's own fields
(source/items/weapon_definitions.h: the weapon, its magazines and triggers, the zoom, the movement penalty), the projectiles its
triggers fire (source/items/projectile_definitions.h), and the first-person animation graph's table of the animations by kind
(source/items/weapons.h: _first_person_weapon_animation_*) with each one's frame count and key frame: the original times a weapon's
reload, ready and put-away by those frame counts (weapon_get_first_person_animation_time).

The offsets in the tags are the sums of the structs' sizes in the source; the ones the source's asserts pin (weapon_type 0x4E2, the
first-person animations reference 0x46C, the render values read below) are checked by reading values a weapon must have.
"""
import os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bsp import MapFile

MAP = os.environ.get('HALO_MAP', os.path.join(HERE, '..', '..', '..', '..', '..', 'assets', 'maps', 'bloodgulch.map'))
OUT = os.path.join(HERE, '..', 'src')
B = 0x803a6000

# the hand weapons, in the order the program lists them (the tag's name is weapons\<name>\<name>)
WEAPONS = ['assault rifle', 'pistol', 'sniper rifle', 'shotgun', 'rocket launcher', 'plasma rifle', 'plasma pistol', 'needler', 'flamethrower']

# offsets: the weapon definition starts after the object (0x17C) and item (0x18C) definitions at 0x308
W = dict(flags=0x308, label=0x30C, secondary_trigger_mode=0x32C, maximum_alternate_shots_loaded=0x32E, ready_time=0x338, heat=0x34C,
         zoom_level_count=0x3DA, zoom_min=0x3DC, zoom_max=0x3E0, movement_penalty_mode=0x3FC, forward_penalty=0x400, sideways_penalty=0x404,
         age_heat_recovery_penalty=0x440, age_rate_of_fire_penalty=0x444, age_misfire_start=0x448, age_misfire_chance=0x44C,
         fp_model=0x45C, fp_animations=0x46C, weapon_type=0x4E2, magazines=0x4F0, triggers=0x4FC)
MAGAZINE_SIZE, TRIGGER_SIZE = 112, 276
ANIMATION_TYPES = 28          # NUMBER_OF_FIRST_PERSON_WEAPON_ANIMATIONS
ANIMATION_NAMES = ['idle', 'posing', 'primary_fire', 'moving', 'overlays', 'light_on', 'light_off', 'reload_while_empty', 'reload_while_full', 'overheated',
                   'ready', 'put_away', 'overcharged', 'melee', 'secondary_fire', 'overcharged_jitter', 'throw_grenade', 'ammunition', 'primary_misfire',
                   'secondary_misfire', 'throw_grenade_overheated', 'overheating', 'overheating_again', 'shotgun_enter', 'shotgun_exit_empty',
                   'shotgun_exit_full', 'overheated_exit', 'overheated_supercharge_enter']
PROJECTILE_BASE = 0x17C       # the projectile definition follows the object definition


def main():
    mf = MapFile(MAP)
    td = mf.td

    def u(fmt, o):
        return struct.unpack_from(fmt, td, o)

    def block(o):
        c, a, _ = u('<iII', o)
        return c, a - B

    projectiles, projectile_index = [], {}

    def projectile(name):
        if not name: return -1
        if name not in projectile_index:
            a = mf.tags[(b'proj', name)] - B + PROJECTILE_BASE
            f, = u('<I', a)
            timer_start, impact_noise = u('<2h', a + 4)
            collision_radius, arming_time = u('<2f', a + 0x24)
            timer_lo, timer_hi, min_velocity, max_range, air_gravity, air_lo, air_hi = u('<7f', a + 0x40)
            water_gravity, water_lo, water_hi, initial_velocity, final_velocity, guided = u('<6f', a + 0x5C)
            projectile_index[name] = len(projectiles)
            projectiles.append(dict(name=name, flags=f, collision_radius=collision_radius, arming_time=arming_time, timer_lo=timer_lo, timer_hi=timer_hi,
                                    min_velocity=min_velocity, max_range=max_range, air_gravity=air_gravity, air_lo=air_lo, air_hi=air_hi,
                                    initial_velocity=initial_velocity, final_velocity=final_velocity, guided=guided))
        return projectile_index[name]

    weapons = []
    for name in WEAPONS:
        tag = 'weapons\\%s\\%s' % (name, name)
        a = mf.tags[(b'weap', tag)] - B
        w = dict(name=name, tag=tag)
        w['flags'], = u('<I', a + W['flags'])
        w['label'] = td[a + W['label']:a + W['label'] + 32].split(b'\0')[0].decode()
        w['secondary_trigger_mode'], w['maximum_alternate_shots_loaded'] = u('<2h', a + W['secondary_trigger_mode'])
        w['ready_time'], = u('<f', a + W['ready_time'])
        w['heat'] = u('<5f', a + W['heat'])       # recovery threshold, overheated, detonation, overheated explosion fraction, loss per second
        w['zoom_level_count'], = u('<h', a + W['zoom_level_count'])
        w['zoom_min'], w['zoom_max'] = u('<2f', a + W['zoom_min'])
        w['movement_penalty_mode'], = u('<h', a + W['movement_penalty_mode'])
        w['forward_penalty'], w['sideways_penalty'] = u('<2f', a + W['forward_penalty'])
        w['age'] = u('<4f', a + W['age_heat_recovery_penalty'])
        w['weapon_type'], = u('<h', a + W['weapon_type'])
        assert td[a:a + 4] != b'\0\0\0\0'
        mc, ma = block(a + W['magazines']); tc, ta = block(a + W['triggers'])
        assert 0 <= mc <= 2 and 0 <= tc <= 2, (name, mc, tc)
        w['magazines'] = []
        for i in range(mc):
            m = ma + i * MAGAZINE_SIZE
            fl, = u('<I', m)
            rps, initial, maximum, loaded = u('<4h', m + 4)
            reload_time, = u('<f', m + 0x14)
            rounds_reloaded, = u('<h', m + 0x18)
            chamber_time, = u('<f', m + 0x1C)
            w['magazines'].append(dict(flags=fl, recharged_per_second=rps, total_initial=initial, total_maximum=maximum, loaded_maximum=loaded,
                                       reload_time=reload_time, rounds_reloaded=rounds_reloaded, chamber_time=chamber_time))
        w['triggers'] = []
        for i in range(tc):
            t = ta + i * TRIGGER_SIZE
            d = dict(flags=u('<I', t)[0])
            d['rof'] = u('<5f', t + 4)               # initial, final, acceleration time, deceleration time, blurred
            d['magazine'], d['rounds_per_shot'], d['minimum_rounds_loaded'], d['rounds_between_tracers'] = u('<4h', t + 0x20)
            d['charging_time'], d['charged_time'] = u('<2f', t + 0x48)
            d['overcharged_action'], = u('<h', t + 0x50)
            d['charged_illumination'], d['spew_time'] = u('<2f', t + 0x54)
            d['distribution_function'], d['projectiles_per_shot'] = u('<2h', t + 0x6C)
            d['distribution_angle'], = u('<f', t + 0x70)
            d['error_inner_cone'], d['error_lower'], d['error_upper'] = u('<3f', t + 0x78)
            d['first_person_offset'] = u('<3f', t + 0x84)
            d['projectile'] = projectile(mf.ref(t + 0x94))
            d['ejection_port_recovery_time'], d['illumination_recovery_time'] = u('<2f', t + 0xA4)
            d['heat_per_round'], d['age_per_round'] = u('<2f', t + 0xB8)
            d['overloading_time'], = u('<f', t + 0xC4)
            d['runtime'] = u('<6f', t + 0xF0)        # illumination recovery, ejection port recovery, rate of fire accel/decel, error accel/decel, per tick
            w['triggers'].append(d)
        # the first-person animations: the graph's table of animation indices by kind, and each animation's frames and key frame
        g = mf.tags[(b'antr', mf.ref(a + W['fp_animations']))] - B
        nodes_c, _na = block(g + 0x68)
        ac, aa = block(g + 0x74)
        fc, fa = block(g + 0x48)
        assert fc == 1
        cnt, addr = block(fa + 0x10)
        table = [u('<h', addr + i * 2)[0] for i in range(min(cnt, ANIMATION_TYPES))]
        w['animations'] = []
        for t in range(ANIMATION_TYPES):
            i = table[t] if t < len(table) else -1
            if i < 0: w['animations'].append((0, 0)); continue
            o = aa + i * 0xB4
            w['animations'].append((u('<h', o + 0x22)[0], u('<h', o + 0x34)[0]))
        w['fp_model'] = mf.ref(a + W['fp_model'])
        w['fp_graph'] = mf.ref(a + W['fp_animations'])
        w['fp_nodes'] = nodes_c
        print('%-16s %s: %d magazine(s), %d trigger(s), zoom x%d (%.1f-%.1f), %d first-person animations, fp model %s' %
              (name, w['label'], mc, tc, w['zoom_level_count'], w['zoom_min'], w['zoom_max'], sum(1 for f, _k in w['animations'] if f), w['fp_model']))
        weapons.append(w)

    os.makedirs(OUT, exist_ok=True)
    f = lambda x: repr(float(x)) + 'f'
    c = open(os.path.join(OUT, 'weapon_assets.c'), 'w')
    c.write('/* generated by tools/export_weapons.py from the map\'s weapon, projectile and first-person animation tags; do not edit */\n#include "weapon_assets.h"\n\n')
    c.write('const fpw_projectile fpw_projectiles[FPW_PROJECTILES] = {\n')
    for p in projectiles:
        c.write('\t{ "%s", 0x%XU, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s },\n' % (p['name'].replace('\\', '/'), p['flags'], f(p['collision_radius']), f(p['arming_time']), f(p['timer_lo']),
                f(p['timer_hi']), f(p['min_velocity']), f(p['max_range']), f(p['air_gravity']), f(p['air_lo']), f(p['air_hi']), f(p['initial_velocity']), f(p['final_velocity']), f(p['guided'])))
    c.write('};\n\nconst fpw_weapon fpw_weapons[FPW_WEAPONS] = {\n')
    for w in weapons:
        c.write('\t{ /* %s */\n\t\t"%s", 0x%XU, %d, %d, %s, { %s },\n' % (w['name'], w['label'], w['flags'], w['secondary_trigger_mode'], w['maximum_alternate_shots_loaded'], f(w['ready_time']), ', '.join(f(x) for x in w['heat'])))
        c.write('\t\t%d, %s, %s, %d, %s, %s, %d, { %s },\n' % (w['zoom_level_count'], f(w['zoom_min']), f(w['zoom_max']), w['movement_penalty_mode'], f(w['forward_penalty']), f(w['sideways_penalty']), w['weapon_type'], ', '.join(f(x) for x in w['age'])))
        c.write('\t\t%d, {\n' % len(w['magazines']))
        for m in w['magazines']:
            c.write('\t\t\t{ 0x%XU, %d, %d, %d, %d, %s, %d, %s },\n' % (m['flags'], m['recharged_per_second'], m['total_initial'], m['total_maximum'], m['loaded_maximum'], f(m['reload_time']), m['rounds_reloaded'], f(m['chamber_time'])))
        c.write('\t\t}, %d, {\n' % len(w['triggers']))
        for t in w['triggers']:
            c.write('\t\t\t{ 0x%XU, { %s }, %d, %d, %d, %d, %s, %s, %d, %s, %s, %d, %d, %s, %s, %s, %s, { %s }, %d, %s, %s, %s, %s, %s, %s, { %s } },\n' % (
                t['flags'], ', '.join(f(x) for x in t['rof']), t['magazine'], t['rounds_per_shot'], t['minimum_rounds_loaded'], t['rounds_between_tracers'],
                f(t['charging_time']), f(t['charged_time']), t['overcharged_action'], f(t['charged_illumination']), f(t['spew_time']),
                t['distribution_function'], t['projectiles_per_shot'], f(t['distribution_angle']), f(t['error_inner_cone']), f(t['error_lower']), f(t['error_upper']),
                ', '.join(f(x) for x in t['first_person_offset']), t['projectile'], f(t['ejection_port_recovery_time']), f(t['illumination_recovery_time']),
                f(t['heat_per_round']), f(t['age_per_round']), f(t['overloading_time']), f(0), ', '.join(f(x) for x in t['runtime'])))
        c.write('\t\t}, { %s }\n\t},\n' % ', '.join('{ %d, %d }' % a for a in w['animations']))
    c.write('};\n')
    c.close()

    h = open(os.path.join(OUT, 'weapon_assets.h'), 'w')
    h.write('''/* generated by tools/export_weapons.py; do not edit */
#ifndef WEAPON_ASSETS_H
#define WEAPON_ASSETS_H
#define FPW_WEAPONS %d
#define FPW_PROJECTILES %d
#define FPW_ANIMATION_TYPES %d
''' % (len(weapons), len(projectiles), ANIMATION_TYPES))
    for i, w in enumerate(weapons): h.write('#define FPW_%s %d\n' % (w['name'].upper().replace(' ', '_'), i))
    h.write('/* source/items/weapons.h: the first-person animation kinds, the index of a weapon\'s table of animations */\nenum {\n')
    for i, n in enumerate(ANIMATION_NAMES): h.write('\tFPW_ANIMATION_%s = %d,\n' % (n.upper(), i))
    h.write('};\n')
    h.write('''typedef struct { short frames, key_frame; } fpw_animation;       /* 0 frames: the weapon has none of this kind */
typedef struct
{
	unsigned flags;                         /* weapon_magazine_definition: 1 << 0 (wasted), 1 << 1 (every shot chambered) */
	short recharged_per_second, total_initial, total_maximum, loaded_maximum;
	float reload_time;
	short rounds_reloaded;
	float chamber_time;
} fpw_magazine;
typedef struct
{
	unsigned flags;                         /* weapon_trigger_definition flags: source/items/weapon_definitions.h */
	float rate_of_fire[5];                  /* initial, final, acceleration time, deceleration time, blurred (a second, or rounds a second) */
	short magazine, rounds_per_shot, minimum_rounds_loaded, rounds_between_tracers;
	float charging_time, charged_time;
	short overcharged_action;
	float charged_illumination, spew_time;
	short projectile_distribution_function, projectiles_per_shot;
	float projectile_distribution_angle, error_inner_cone, error_lower, error_upper;   /* radians */
	float first_person_offset[3];
	short projectile;                       /* index into fpw_projectiles, or -1 */
	float ejection_port_recovery_time, illumination_recovery_time, heat_per_round, age_per_round, overloading_time, unused;
	float runtime[6];                       /* per tick: illumination recovery, ejection port recovery, rate of fire acceleration and deceleration, error acceleration and deceleration */
} fpw_trigger;
''')
    h.write('''typedef struct
{
	const char *name;
	unsigned flags;
	float collision_radius, arming_time, timer_lower, timer_upper, minimum_velocity, maximum_range, air_gravity_scale, air_damage_lower, air_damage_upper;
	float initial_velocity, final_velocity, guided_angular_velocity;     /* the tag's own: world units a tick, radians a tick */
} fpw_projectile;
typedef struct
{
	const char *label;
	unsigned flags;
	short secondary_trigger_mode, maximum_alternate_shots_loaded;
	float ready_time;
	float heat[5];                          /* recovery threshold, overheated threshold, detonation threshold, overheated explosion fraction, loss per second */
	short zoom_level_count;
	float zoom_magnification_minimum, zoom_magnification_maximum;
	short movement_penalty_mode;
	float forward_movement_penalty, sideways_movement_penalty;
	short weapon_type;
	float age[4];                           /* heat recovery penalty, rate of fire penalty, misfire start, misfire chance */
	short magazine_count;
	fpw_magazine magazines[2];
	short trigger_count;
	fpw_trigger triggers[2];
	fpw_animation animations[FPW_ANIMATION_TYPES];
} fpw_weapon;
extern const fpw_projectile fpw_projectiles[FPW_PROJECTILES];
extern const fpw_weapon fpw_weapons[FPW_WEAPONS];
#endif
''')
    h.close()
    print('wrote', os.path.abspath(OUT))


if __name__ == '__main__':
    main()
