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

## Weapons, zoom and autolevel

`src/fp_weapon.c` (30 Hz, no PS2 code) is the player's weapons, from `source/items/weapons.c`, the weapon parts of `source/units/units.c`
and the flight of `source/items/projectiles.c`, on numbers `tools/export_weapons.py` reads from the map's weapon, projectile and
first-person animation tags into `weapon_assets.[ch]` (generated, ignored by git): the nine hand weapons that have a first-person
model in Blood Gulch (assault rifle, pistol, sniper rifle, shotgun, rocket launcher, plasma rifle, plasma pistol, needler,
flamethrower): flags, ready and heat numbers, zoom levels and magnifications, movement penalties, each magazine (rounds, reload
and chamber times) and trigger (rate of fire and its ramps, error cone, charging, spew, heat and age per round, the projectile), each
projectile (velocity, range, gravity), and the first-person animation graph's table of animations by kind with each one's frame count and
key frame (the original times a reload, a ready and a put-away by those frames: the assault rifle's reload is 87 ticks, its ready 29).

Ported: the `weapon_update` loop (ready timer, triggers down, the secondary trigger's modes, the reload request, each magazine's
reloading and chambering, each trigger's state machine, the rate-of-fire and error ramps, `weapon_trigger_can_fire_again`'s cadence, heat
and overheating), firing (rounds, heat, age, misfire, the shot's random direction in its error cone, a shotgun's fan of pellets, the
plasma pistol's alternate shots), reload (a shotgun's shell by shell), zoom (`weapon_rotate_zoom_level`: none, the levels, none; the
magnification between the minimum and maximum by the level; the field of view over it; unzoomed on a switch and a melee), the unit's two
weapons and switching (the put-away animation's ticks, then the ready's, during which it cannot fire), melee (the animation busies the
weapon, a trace at the key frame) and a projectile's flight (its velocity and the player's along the shot, gravity, range) through the
map's collision geometry and the Chief's capsule (`fp_trace`: the first surface a segment crosses from its front): an impact is an event.
Not ported: effects, sound, damage (the Chief takes none), the weapon objects' own third-person animations (they time a recoil state that
only blocks a reload for a few ticks, and the assault rifle has no graph of its own), vehicle weapons, picking weapons up, grenades (the
button is read, the throw is not done), the flashlight, a projectile's detonation (a rocket's blast, a grenade's fuse) and the AI's use.

Through the controller (`fp_player`): the DualShock's right trigger fires (its pressure when libpad gives it, else the button), Square
reloads, Triangle switches weapons, Circle is melee, R3 zooms, L2 is the grenade, Select (Back) toggles the autolevel; the demo presses them
too (at the Chief: the pistol, zoomed, a few rounds). The player carries the assault rifle and the pistol. Each button is seen by the
weapons once, as an edge, on the next tick. The lens eases to the weapon's field of view over 0.18 s (`first_person_camera.c`'s
transition time) and the look rates are divided by the magnification (`player_control.c`); the program recomputes the focal length, the
clip planes, VU0's constants and the mip levels as the lens moves. The mip floor (`tools/mip_floor.json`) trims levels the orbit never
reads, so a zoom shows them blurrier: `MIP_FLOOR=0 python3 tools/export_map.py` keeps them.

**Autolevel** is the original's "autocenter" controller option (`player_ui_autolevel_enabled`), off in a new profile (the profile is
zeroed): `player_control.c` levels the pitch toward the unit camera's autolevel (0 for the Chief) after the player has walked forward
for `MAP_MIN_AUTOLEVEL_TICKS` (15) ticks without looking up or down, by `|pitch| * 0.6366 * look_autolevel_scale (0.5) * speed` a tick.
It is here, off, toggled by Select, where the original has a menu.

## Walking on the map: the collision BSP

The player collides with the map's own collision geometry, the geometry the original's biped is swept through, not the render
triangles. Three sources, all triangles in one array (`map_col_pos`, `map_col_tri`; 7,576 triangles; with the tree, 395 KB):

- **The structure BSP's collision BSP** (`structure_bsp.collision_bsp`, `source/physics/collision_bsp_definitions.h`).
  `tools/bsp.py` (`parse_collision_bsp`) reads its surfaces (4,916 for Blood Gulch: a plane, a flag byte, a material and a ring of
  edges, walked as `collision_surface_polygon` does; every one is planar, convex and wound the way its plane faces) and its
  tree, and `export_map.py` triangulates the surfaces (a fan: 5,468 triangles). It has the invisible walls and the sky's ceiling at the
  map's edge: its box runs from -8 to 174 in x and -220 to -30 in y, the picture from 5 to 133 and -190 to -43.
- **The scenery's collision models** (`coll` tags: the scenery's `collision_model` reference, the nodes block at 0x28C, each node's
  `bsps` block, one collision BSP per permutation), placed as the renderer places the scenery (position, yaw, pitch, roll,
  permutation 0). 27 of the map's scenery objects have one (boulders, trees, the bases' props, the plants' trunks): 1,964
  triangles in the world. Their nodes with surfaces are all the first, at the object's origin.
- **The Chief**, a capsule of the player's own size (`capsule_mesh`: 12 sides, 3 rings a hemisphere, 144 triangles) at the demo Chief's
  feet. The Chief does not move in this scene, so he is geometry; a Chief that moved would be tested as the pill against a pill.

**The BSP's own tree** finds the structure's triangles. The exporter writes the nodes (a plane, a back child and a front child;
a child with the top bit set is a leaf, all ones is none), the planes (`distance = n . p - d`, as `bsp3d_test_point`), and for each
leaf the surfaces at the bottom of its bsp2d references' 2D trees (13,707 entries over 5,999 leaves) instead of the 2D trees
themselves (the controller wants the surfaces near a box, not which surface a point on a plane is in), and each surface's first
triangle. A query is a box: down the tree, both children where the box straddles a node's plane, one where it is wholly to a
side, and every surface of every leaf reached. The objects are found through a grid over x, y (4-unit cells), as the tree is the
structure's alone. Each triangle is gathered once (a stamp), 20 to 60 for a move.

The player is a pill (radius 0.2, 0.7 tall standing, 0.5 crouched) swept through those triangles as two or three spheres a
radius apart (a few millimetres from a capsule): each sphere against each triangle in the swept test (the face, then the
edges as cylinders and the vertices as spheres), the surfaces one-sided as the collision BSP's are. A move slides along what
stops it (four times at most), stopping a millimetre short; the velocity loses what goes into the surface. A walkable surface
met (steeper than 45 degrees is not) is the ground: the player stands on it (the original's 1/128 push along the normal
keeps the contact), and its normal steers the walk along the slope. A walking player does not slide up a surface too steep to
stand on; it slides along the level line on it. A drop of under 0.12 is walked down (a probe below the feet), a larger one is a
fall. A sphere inside a surface after a move (the seam of two) is pushed out, and so is a player put down against a wall.
Standing still moves nothing: the push into the ground would, on an edge or a vertex whose normal is not the stored
ground's, leave a sideways part.

**Standing up** is refused where the taller pill would be inside a surface: each tick the crouch moves toward its target, and a
step toward standing that puts a sphere more than 4 millimetres into a surface is not taken (the crouch stays at what fits).

`fp_ground` (a vertical ray: the highest up-facing triangle within 8 units under a point, from the same tree and grid) is
still there for putting the player on the ground and for the demo's planner, which asks `fp_ground_in` for only the step
it will accept (a few tenths: a far smaller box, a fraction of the leaves; planning takes 0.3 s of the EE at start-up, 1.3 s
before that and the cell cache).

Not ported: the other objects' collision (vehicles, items, the bipeds that are not the Chief), a Chief who moves (he would be
tested as a pill against a pill), the scenery's permutations other than the first, the 2D trees of the leaves (flattened to
the surface lists), the unit camera's own pitch limits (the Chief's are the defaults, 85.5 degrees); the first-person weapon's
model (hands and gun: not drawn yet); the stun penalties; the original's slipping physics on steep ground. A fall into the void
puts the player back at the start.

## Checking it

- `cc -O1 -I src -o /tmp/fp_controller_test tools/fp_controller_test.c src/map_assets.c -lm`,
  run in `port/ps2/samples/masterchief` after `export_map.py` (the test includes the controller): the dead zone and the
  square stick, the look rates and their acceleration, the pitch stop, the run speed (2.2 of 2.25 on a slope), the
  jump (0.65 peak, lands), the crouch, standing still for 3 s at 508 places over the valley (133 of them on slopes:
  no drift at all), the ground the tree finds against every triangle of the map at 4,000 random points (no
  difference), walking into the Chief for 12 s (stops 0.39 from him: two radii), walking into 29 walls of scenery (none
  got through), standing up under a low ceiling (refused, the crouch stays at what fits), 40 random players for 40 s each,
  random sticks, jumps and crouches (the deepest any sphere went into a surface, against every triangle: 0.0005 of a 0.2
  radius, and never through the floor), and 36 headings x 60 s of holding forward: finite, never off the ground, never
  out of the map.
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
