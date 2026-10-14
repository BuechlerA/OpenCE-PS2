# The first-person controller on the PS2

The Master Chief sample's map scene (`port/ps2/samples/masterchief`) is first person: a DualShock walks the
player over Blood Gulch; with no hand on the pad a demo walks the player from the front of one fortress to the
Chief waiting in the middle of the valley (see "The demo"). The controller is Halo's own:
the input shaping, the look, the biped's physics and the camera are ported from `source/`, and every number comes
from the map's tags. Checked in PCSX2 only (the demo and an idle pad; no one has held a DualShock to it).

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

## The demo

`src/fp_demo.c` plays the walk through the pad state the controller reads, so the dead zone, the slow start of a
turn, the acceleration, the slope's speed, the jump's arc and the walls are the controller's own.

- The route: the exporter's `DEMO_START` (the front of the north fortress, 68.5, -97.5), `DEMO_VIA` (three points that
  bend it east over the rise, down the valley and back west) and `DEMO_CHIEF` (62, -121, the middle between the two
  fortresses, where the Chief now stands, turned toward the start). They are env/constants at the top of
  `tools/export_map.py` and reach the program as `MAP_DEMO_*` in `map_assets.h`.
- The plan, at start-up: A* over the ground in 0.5-unit cells, leg by leg through the via points, on cells with a gentle
  slope (under 38 degrees) and room (ground, level, half a unit around), then the corners cut where the straight line is
  as walkable. 6 points, 45 units: planned in the first frames of the program (not timed alone).
- The pilot chases the point 1.5 units ahead on the path. The right stick turns by the heading's error through the
  look function backwards, never past 0.8 (a stick at the stop would peg and accelerate the turn), the left stick runs
  (slower in a bend, stopped to turn when far off), the eyes follow the ground ahead, and the jump button is pressed
  for 0.12 s at four marks along the path (20, 45, 70, 85 percent). At the goal it looks at the Chief for 3.5 s, then the
  demo starts over.
- Any stick past the dead zone or any button but Start takes the player over for good; Start restarts the demo.
  A demo that cannot plan, or a build with `FP_NO_DEMO`, leaves the pad alone in charge.
- The orbit builds (stills, profile, mip audit, `ORBIT_CAMERA`) keep the Chief where he stood for the orbit
  (`MAP_CHIEF_*`), so their views and the mip floor audit stay valid.

## Walking on the map: the collision BSP

The player collides with the map's own collision BSP, the one the original's biped is swept through
(`structure_bsp.collision_bsp`, `source/physics/collision_bsp_definitions.h`), not the render geometry.
`tools/bsp.py` (`load_collision_bsp`) reads its surfaces (4,916 for Blood Gulch: a plane, a flag byte, a material, and a ring of
edges, which it walks as `collision_surface_polygon` does), checks every surface is planar and wound the way its plane faces, and
`export_map.py` triangulates them (a fan: all are convex, 5,468 triangles on 2,704 vertices) and grids them over x, y in
4-unit cells (46 x 48 cells, about 100 KB). It is not the render BSP: it has the invisible walls and the sky's
ceiling at the map's edge (the collision box runs from -8 to 174 in x and -220 to -30 in y, the picture from 5 to 133 and
-190 to -43), and the surfaces the player can stand on and be stopped by are the ones the original's.

The player is a pill (radius 0.2, 0.7 tall standing, 0.5 crouched) swept through those triangles as two or three spheres a
radius apart (a few millimetres from a capsule): each sphere against each triangle in the swept test (the face, then the
edges as cylinders and the vertices as spheres), the surfaces one-sided as the collision BSP's are. A move slides along what
stops it (four times at most), stopping a millimetre short; the velocity loses what goes into the surface. A walkable surface
met (steeper than 45 degrees is not) is the ground: the player stands on it (the original's 1/128 push along the normal
keeps the contact), and its normal steers the walk along the slope. A walking player does not slide up a surface too steep to
stand on; it slides along the level line on it. A drop of under 0.12 is walked down (a probe below the feet), a larger one is a
fall. A sphere inside a surface after a move (the seam of two) is pushed out, and so is a player put down against a wall.
Standing still moves nothing: the push into the ground would, on an edge or a vertex whose normal is not the stored
ground's, leave a sideways part. The BSP's tree is not used: the triangles near the move come from the grid.

`fp_ground` (a vertical ray: the highest up-facing triangle under a point) is still there for putting the player on the ground
and for the demo's planner.

Not ported: the collision against the scenery (rocks, plants, bases are drawn but not solid), the Chief and the
bases' doors; standing up under a ceiling (the pill's height follows the crouch, the top of it is pushed out of a ceiling
rather than the stand being refused); the pitch autolevel; the unit camera's own pitch limits; the first-person weapon (hands
and rifle); the zoom; the stun penalties; melee and the other actions; the original's slipping physics on steep ground. A fall
into the void puts the player back at the start.

## Checking it

- `cc -O1 -I src -o /tmp/fp_controller_test tools/fp_controller_test.c src/map_assets.c -lm`,
  run in `port/ps2/samples/masterchief` after `export_map.py` (the test includes the controller): the dead zone and the
  square stick, the look rates and their acceleration, the pitch stop, the run speed (2.2 of 2.25 on a slope), the
  jump (0.65 peak, lands), the crouch, standing still for 3 s at 508 places over the valley (133 of them on slopes:
  no drift at all), 40 random players for 40 s each, random sticks, jumps and crouches (the deepest any sphere
  went into a surface: 0.0003 of a 0.2 radius, and never through the floor), and 36 headings x 60 s of holding forward:
  finite, never off the ground, never out of the map.
- `tools/fp_demo_test.c` (the command is in its header): the demo plans the walk, and the controller, given only the pad
  the demo makes, arrives next to the Chief, four times in 100 s, jumping four times a walk and facing him, never
  stuck; a touch of the pad takes over and Start restarts the demo.
- The demo is what PCSX2 shows with no one at the pad. `make FP_NO_DEMO=1` leaves the player standing, for the pad alone.
  `make EXTRA_CFLAGS=-DPAD_DEBUG` prints what libpad sees twice a second.
- The frame rate of the demo, `make EXTRA_CFLAGS="-DPROFILE -DFIRST_PERSON"`: 60 fps all the way, the EE busy
  8.5 to 12.2 ms of 16.7 (the orbit: 8 to 14), the widest views drawing 3,600 triangles. The builds for stills
  (`-DDEBUG_SECONDS`), the profile and the mip audit keep the orbit camera so that builds see the same views, unless
  `-DFIRST_PERSON` is added; `make ORBIT_CAMERA=1` asks for the orbit in any build.

## Pitfalls met

- The original presses the player into the ground each tick (a 1/128 push along the ground's normal) and its collision
  clips what the ground stops. Ported without the clip, the push's sideways part slid a player standing on a slope
  (0.4 units in 3 s at one place): the grounded velocity is now clipped to the ground plane.
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
  second lags it by up to 6 degrees a frame times four. Not visible in the demo.
