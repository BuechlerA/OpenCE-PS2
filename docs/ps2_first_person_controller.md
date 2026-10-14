# The first-person controller on the PS2

The Master Chief sample's map scene (`port/ps2/samples/masterchief`) is first person: a DualShock walks the
player over Blood Gulch, and the Chief stands where the orbit used to circle him. The controller is Halo's own:
the input shaping, the look, the biped's physics and the camera are ported from `source/`, and every number comes
from the map's tags. Checked in PCSX2 only (a scripted walk and an idle pad; no one has held a DualShock to it).

## Where each part comes from

| Part | Original | Here |
| --- | --- | --- |
| Stick dead zone | `input_xbox.c` `fix_dead_zone`, 9000 of 32767 on each axis | `fp_shape_input` |
| Stick circle to square, d-pad override, default preset (left moves, right looks) | `input_abstraction.c` `input_abstraction_update` | `fp_shape_input` |
| Look: the look function, yaw 120 and pitch 60 degrees a second, the acceleration of a pegged stick (up to 3x after 1 s above 0.85), the stronger-axis stretch, pitch limit 85.5 degrees | `player_control.c` `get_local_player_input_blob`, `player_control_modify_desired_angles` | `look` (per frame, scaled by its length; the original does it per 30 Hz tick) |
| Physics: 30 Hz, speeds and accelerations (run, sneak, air), slope speed scale, gravity 0.00357 a tick squared, jump 0.07, crouch | `bipeds.c` `biped_update_physics`, `biped_jump`, `physics.c` | `tick` (30 Hz, the camera interpolates between ticks) |
| Camera: eye = feet + 0.62 (0.35 crouched, blended over 0.2 s), facing = the aim, 70 degrees across a 4:3 picture | `first_person_camera.c`, `bipeds.c` `biped_get_sight_position` | `fp_camera`, `fp_vertical_half_fov_degrees` |

The numbers are read from the map by `tools/export_map.py` (`player_tunings`) into `MAP_*` in `map_assets.h`: the
globals tag's player-control block (look function `0, .05, .1, .25, .58, 1`; acceleration time 1, scale 3, pegging
0.85; rates 120 and 60) and player-information block (run 2.25 forward, 2.0 back and sideways, acceleration 0.32;
sneak 0.9, 0.65, 0.6, 0.16; airborne acceleration 0.035), and the player's biped tag (`characters\cyborg\cyborg`:
camera heights, jump velocity, collision size, slope angles). The exporter asserts the biped fields it reads are
in the plausible range, because their offsets come from the source's struct, not from a header it could check
(`BIPED_*_AT` in the exporter were checked against the Chief's tag).

Two things the decompiled source does not contain, and what was done: `runtime_downhill_k0` and its kin (the
slope speed thresholds) are computed by a tag postprocess that is not in the source, so they are taken to be the
sine of the tag's falloff and cutoff angles, and the minimum ground normal the cosine of the maximum slope angle
(the field's documented meaning); the player's speed is the globals', not the animation graph's.

## Buttons

DualShock to the Xbox's, as the Xbox's default layout has them: left stick moves, right stick looks, d-pad moves
at full throttle (overriding the stick, as the original's), Cross jumps (the Xbox's A), L3 crouches (the left
stick's click). Nothing else is used: the sample has no weapon, grenades or actions.

`src/pad.c` is libpad on port 1: it loads the console's `rom0:SIO2MAN` and `rom0:PADMAN`, asks the DualShock for
its sticks (`PAD_MMODE_DUALSHOCK`, locked), and treats a digital pad as having no sticks (its stick bytes are not
at rest). A DualShock's sticks rest at 127, one step below the dead zone's middle, so it does not drift.

## Walking on the map

The original sweeps a pill through the collision BSP. The sample has no collision BSP, only the render BSP, so
the player walks on that: `export_map.py` writes the structure BSP's triangles alone (no scenery, no sky) and
a grid over x, y (4-unit cells) of the triangles near each cell (5,503 triangles, 32 x 37 cells, about 110 KB). A
ground query is a vertical ray: the highest up-facing triangle at or below a height. The player stands on the
ground and its pill (radius 0.2) fits where the ground under the centre and eight points of its circle is at most
a step (0.3: the radius on a 45 degree slope, plus a stride) above the feet, and not above the terrain at all
when it is missing (the edge of the map, the far side of a cliff that rises more than a step). A move that does
not fit is tried along each axis (so walls slide). A slope steeper than the biped's maximum (45 degrees) cannot
be climbed, and standing on one slides the player down it. A drop of more than 0.12 is fallen, not walked.

Not ported: the collision against the scenery (rocks, plants, bases are drawn but not solid), the Chief and the
bases' doors; the pitch autolevel; the unit camera's own pitch limits; the first-person weapon (hands and rifle);
the zoom; the stun penalties; melee and the other actions; ceilings (a jump goes through an overhang); the
slipping physics of the original on steep ground. A fall into the void puts the player back at the start.

## Checking it

- `cc -O1 -I src -o /tmp/fp_controller_test tools/fp_controller_test.c src/fp_controller.c src/map_assets.c -lm`,
  run in `port/ps2/samples/masterchief` after `export_map.py`: the dead zone and the square stick, the look rates
  and their acceleration, the pitch stop, the run speed (2.23 of 2.25 on a slope), the jump (0.65 peak, lands), the
  crouch, and 36 headings x 60 s of holding forward: finite, never off the ground, never out of the map.
- `make FP_SCRIPT=1` plays a scripted walk (stand, run, turn, run uphill, jump, crouch; a 10 s loop) in place
  of the pad, for PCSX2 without anyone at it. `make EXTRA_CFLAGS=-DPAD_DEBUG` prints what libpad sees twice a second.
- The frame rate of the scripted walk, `make FP_SCRIPT=1 EXTRA_CFLAGS="-DPROFILE -DFIRST_PERSON"`: 60 fps, the EE busy
  7.5 to 11.6 ms of 16.7 (the orbit: 8 to 14), the widest views drawing 3,600 triangles. The builds for stills
  (`-DDEBUG_SECONDS`), the profile and the mip audit keep the orbit camera so that builds see the same views, unless
  `-DFIRST_PERSON` is added; `make ORBIT_CAMERA=1` asks for the orbit in any build.

## Pitfalls met

- The Chief's passes (VU1's microprogram, the EE's visor and display) do not clip: with the camera behind him the
  perspective divide mirrored him across the screen, and close up or far to a side his vertices passed the GS's
  2048-pixel limit and wrapped. The orbit never showed either. `chief_in_view` (main.c) tests his bounding sphere
  against the near plane and the pixel limit, and when it fails he is drawn through a view that sends every vertex to
  one point (degenerate triangles; VU1 still runs its stream, which ends with the FINISH the map's queue waits
  for). He vanishes when the eye is within about 0.7 units of him: the player does not collide with him.
- The walk can read texture levels the orbit never does (close to the ground, along the cliffs): `tools/mip_floor.json`
  leaves out 433 KB of them (the metal panels, the boulder, the flag base). The default assets are the orbit's, so
  those textures may look softer up close (not compared). `MIP_FLOOR=0 python3 tools/export_map.py` keeps every
  level and still fits: 1.32 MB of the 4 MB left free, against 1.76 MB.
- The Chief's highlight is worked out once every four frames (`SPEC_SPREAD`); a camera turning at 360 degrees a
  second lags it by up to 6 degrees a frame times four. Not visible in the scripted walk.
