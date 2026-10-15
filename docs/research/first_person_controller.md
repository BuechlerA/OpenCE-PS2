# The first-person controller on the PS2

> **The sample this describes is not in the tree.** The Master Chief / Blood Gulch sample (`port/ps2/samples/masterchief`) was removed from `main` when the repository was cleaned up for the game's port. Its code, tools and README are in the tag `pre-ps2-cleanup` (`git worktree add ../sample pre-ps2-cleanup`). What follows is research: measurements and pitfalls that apply to the game's renderer.

The Master Chief sample's map scene (`port/ps2/samples/masterchief`) is first person: a DualShock walks the
player over Blood Gulch; with no hand on the pad a demo walks the player from the front of one fortress to the
Chief waiting in the middle of the valley (see "The demo"). The controller is Halo's own:
the input shaping, the look, the biped's physics and the camera are ported from `source/`, and every number comes
from the map's tags. Checked in PCSX2 only (the demo and an idle pad; no one has held a DualShock to it, nor run it on hardware).

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
stick's click). The weapons' buttons are under "Weapons, zoom and autolevel".

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
  for 0.12 s at four marks along the path (20, 45, 70, 85 percent). It fires a second's burst of the assault rifle at 33 and 80 percent
  of the path (the trigger, as the right trigger: 255). At the goal it looks at the Chief (his chest: the ground at his feet is the top
  of his capsule, which the first version aimed at, a head too high, so that no shot hit him) and plays the weapons for 15 s, by the pad's
  buttons, one press 0.1 s long: a burst of the rifle at him (1.2 s in), the melee blow (3.0), a reload (4.3: 2.8 s, the rifle's
  87 frames), the switch to the pistol (7.8: put away in 12 ticks, readied in 33), the zoom (9.8), five shots a press each (the pistol's
  trigger is latched), the zoom off (13.6); then the demo starts over (a walk of about 20 s and the 15 s: 35 s). The pistol has one
  zoom level (2x). The sniper rifle, which the task asked for if it was feasible, is not: the player carries two weapons (the rifle and
  the pistol, `fp_init`) and its scope has an overlay of the HUD that is not drawn here.
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
too (see "The demo"). The player carries the assault rifle and the pistol. Each button is seen by the
weapons once, as an edge, on the next tick. The lens eases to the weapon's field of view over 0.18 s (`first_person_camera.c`'s
transition time) and the look rates are divided by the magnification (`player_control.c`); the program recomputes the focal length, the
clip planes, VU0's constants and the mip levels as the lens moves. The mip floor (`tools/mip_floor.json`) trims levels the orbit never
reads, so a zoom shows them blurrier: `MIP_FLOOR=0 python3 tools/export_map.py` keeps them.

**Autolevel** is the original's "autocenter" controller option (`player_ui_autolevel_enabled`), off in a new profile (the profile is
zeroed): `player_control.c` levels the pitch toward the unit camera's autolevel (0 for the Chief) after the player has walked forward
for `MAP_MIN_AUTOLEVEL_TICKS` (15) ticks without looking up or down, by `|pitch| * 0.6366 * look_autolevel_scale (0.5) * speed` a tick.
It is here, off, toggled by Select, where the original has a menu.

## The arms and the gun

`src/fp_view.c` (30 Hz, no PS2 code) is `source/interface/first_person_weapons.c`: the state the weapon's animation is in (idle, posing after
10 to 15 s idle, fire, reload, melee, ready, put away), the animation's frame a tick, the moving overlay, the sway overlay (position, turning
and the push-back of a shot, eased as `accelerate_to_position` does) and the 3 to 6 ticks of interpolation between animations. It makes the
node matrices of the weapon's animation graph in the camera's space (x forward, y left, z up). `fp_weapon.c`'s events (`fpv_event`) drive it,
and the reload and ready times of the weapon logic are the animations' frame counts. `tools/export_fp_models.py` bakes the first-person
models (the arms and each weapon's gun: positions, normals, UVs, two bone indices and a weight a vertex, inverse bind matrices from the
default pose) and their 17 textures into `src/fpm_assets.[ch]` (gitignored); `tools/fp_view_reference.py` is an independent numpy
version of the animation decode and the skinning (`fp_view_test` with a weapon, type and frame prints the same vertices: they agree to
six decimals). The graph's animations are uncompressed frames (per node a rotation of four int16, a translation of three floats and a scale,
by the node's flags; only unflagged components take the default pose).

`draw_first_person` in `main.c` draws them after the map and the Chief (and not while zoomed: `first_person_weapon_render_update` makes the weapon visible only with no zoom level; the first version drew it, small at the screen's foot, and a zoomed frame now costs the weapon's 3.3 ms less), with the depth test on and the arms and gun's own projection (near plane
0.01, clipped against it only when a vertex is behind it). A bone's skin matrix is its inverse bind matrix and its graph node's; a light
is carried into the bone's bind space instead of a normal out of it. On VU0 (macro mode; `NO_VU0` keeps the C path) the vertices take two matrix
products by their bones, the projection is one divide, and a normal is lit by three dot products, a clamp and the lights' colours, its index
into a gamma table coming out of the same registers: the EE packs the GS registers. The weapon costs 3.3 ms of the EE a frame (the C path:
5.3; the first version, which also lit every vertex, about 9). VRAM: all 17 textures stay resident, 1.1 MB (0.6 MB free).

## The weapon's own display

The assault rifle shows its rounds loaded **on the gun**, as the Xbox does: its first-person model has five parts that are `shader_transparent_chicago`
shaders (the `soso` parts are all the first version drew), added to the picture, unlit and two sided: the display's glow (`ar data`) and icons (`icons`: the
"x" and the bullet), a compass (`compass`: an arrow, 8 frames, for the heading against the scenario's `local_north`, `FPM_LOCAL_NORTH`) and two `numbers` quads, a
digit each. A shader with the numeric flag (bit 7 of its flags) and a counter limit (60 for the digits, 8 for the compass) picks the frame of its map from a function
of the weapon: `counter = round(limit * value)`, the digit of place p is `(counter / frames^p) % frames`
(`rasterizer_xbox_transparent_geometry.c`; here `fpv_display_frame`). The digits read the rounds loaded over the magazine's size; the tags' own function
modes for the rifle are (integrated light, primary ammunition, illumination, none) by the source's enum, which does not obviously give the digits the
ammunition, so what the program shows is what the screenshot of the Xbox shows (60 for a full magazine), not a proved reading of the tags. The graph has no
first-person "ammunition" animation for either gun (its table has 17 entries; the original's overlay of that kind is idle).

`tools/export_fp_models.py` reads the parts (geometry in the bind pose, UV with the map's scale and offset, the node), reuses the texture the soso `display` part already
holds (the same `display_fp` bitmap) and packs the numeric maps' frames (the digits 0-9 and the compass's 8, 64x64 DXT1 each) cut to 32x32 into one 4-bit atlas of
256x128 (16 KB with its palette; 18 of 32 frames used). A shader that is not added (the sniper's scope subscreen is alpha blended) is left out. Which of two digit
parts is the tens is found from the map's u axis in space (the part header's permutation index is -1). `fp_draw_display` (main.c) skins the 41 vertices on the EE (one node),
projects them in the weapon's view with a Z nudge of 40 toward the eye and draws each part as a list, added (`Cs + Cd`): +0.05 ms of the EE and 16 KB of video memory
(free 611 KB of 4096). The weapon is hidden while zoomed, so the display is too.

## Shots: the muzzle flash and the impact marks

`main.c`'s `fx_*` (FIRST_PERSON builds): small flat-coloured primitives, a few dozen a frame, each blend a REGLIST packet of RGBAQ and XYZ2 (no texture,
no lighting). They cost 0.03 ms of the EE a frame (the profile's "fp fx+hud" before the HUD below was added), which the weapon's 3.3 ms does not notice.

- **Marks.** Each `FPW_EVENT_IMPACT` (and a melee blow that hit) not on the Chief (`b == 2`: he takes no damage, and a quad on his capsule would
  float) puts a mark in a ring of 32 at the shot's position and normal: a dark scorch quad (alpha blended, 0.05 units from its centre to its side
  plus 0.006 a unit of distance, so that it stays a few pixels) for 5 s, fading in the last 1.5 s, and in its first 0.12 s a flash (added). The
  quads are drawn after the map and the Chief with the depth test on and a nudge toward the camera of 0.02 + 0.004 a unit of distance
  (the 16-bit Z buffer does not separate a coplanar quad further than that); a mark nearer than 0.3 or further than 60 units is not drawn.
- **Muzzle flash.** The trigger's `illumination` (the original's: 1 at a shot, down by 1/3 a tick for the rifle and the pistol, 0.1 s) scales an eight-pointed
  star (alternate long and short points: bright centre, clearer points, added) at the gun's tip, in the weapon's view. The tip is the centroid of
  the gun's bind-pose vertices within 4 mm of the largest x (+x is the barrel in every weapon's bind pose, checked on the nine), carried by that
  vertex's node: `fpv_skin_matrix` each frame. The star is drawn in front of the whole gun (Z just under the near plane): at its own depth it was behind
  the barrel's nearer parts and did not show (found in a PCSX2 still).
Not done: shell ejection, the Chief's reaction (a hit mark on his armour), impact effects by material (dust, sparks), the flash's light on the map.

## The HUD

The HUD is the original's, from its tags: `tools/export_hud.py` reads the weapon hud interfaces of the weapons the player carries (`wphi`: the assault rifle's
and the pistol's, each with its parents `ui\hud\master rounds` and `ui\hud\master`), the grenade hud (`grhi`), the unit hud (`unhi`: `ui\hud\cyborg`), the digits
(`hud#`: `ui\hud\counter`) and the interface bitmaps they name, into `src/hud_assets.[ch]` (generated, ignored by git; the sizes of the tag structs are the
source's: `weapon_hud_interface_definition.h`, `unit_hud_interface_definition.h`, `hud_weapon.c`, `hud_draw.c`). `src/fp_hud.c` (30 Hz, no PS2 code, tested on the
host) is `hud_weapon.c`, `hud_unit.c` and `hud_draw.c`: it makes the list of quads a frame draws, in the original's order: the crosshairs, the weapon's hud (statics,
meters, numbers, overlays; the parent's first, the state flags and flashing of each state: total ammunition, rounds loaded, heat, age, the secondary magazine), the grenade
hud, the unit hud (the shield and health meters with their backgrounds, the motion sensor's two backgrounds). `main.c` draws the quads.

What is on the screen for the rifle, as in the Xbox screenshot it was checked against: at the top left the total rounds in three digits (leading zeros as the tag says) with the
ammunition icon, the grenade count in its own panel, and under them the magazine as three rows of ticks, one for each round loaded (the flash colour when it is low, the
panel's background red when the rounds carried run out); at the top right the shield bar (a gradient) and the health ticks; at the bottom left the motion sensor's
disc and its "15m"; at the middle the reticle (the pistol's zoomed one is the scope's "2x"). The loaded rounds are also on the gun (see "The weapon's own display").

- **Layout.** An element is placed from its corner of the window by its offset in pixels (`hud_calculate_point`, the scale is 1). The window is the Xbox's title safe frame
  (`RASTERIZER_FRAME_BOUNDS_*`, 48, 36 to 592, 444 of 640 x 480), here 48, 34 to 592, 414 of the display's 640 x 448 lines; the buffer is 512 wide, so x is scaled by 0.8.
- **Images and textures.** An image is a rectangle of a bitmap (a sprite of its sequence). The bitmaps are alpha / intensity (`ay8`, `a8y8`, swizzled): 8-bit textures with a
  palette of (intensity, intensity, intensity, alpha), cropped to the sprites used and padded to powers of 2: 18 textures, 139 KB, 180 KB with their palettes (free video memory
  611 KB before, 432 KB after). The vertex colour is the element's (half: the GS doubles it).
- **Meters.** The meter bitmaps (`hud_ammo_meters`, `hud_unit_meters`) hold a shape (alpha) and a fill order (intensity: a ramp along the meter; the ammunition's ticks are
  4 n + 2 for the n-th round, which is what the Xbox's `alpha_multiplier 4, alpha_bias 2` of the rifle's meter compares a value with: `min_alpha = 4 * rounds + 2`; the
  pistol's is 20 a round, the shield's 1 * 198 + 55). A texel is drawn filled while its fill order is at most that threshold. Here a meter's texture holds, per texel,
  64 * (the shape's alpha in 4 levels) + (the fill order / 4), and its palette is made for the value (`fph_meter_palette`: the filled texels in the gradient from the
  meter's minimum colour to its maximum, the flash region in the flash colour, the rest in the empty colour) and sent to the texture's place in video memory
  (an image transfer in the draw queue, 1 KB, only when it changed): one quad draws a meter of any value. The Xbox's meter pixel shader is a register combiner whose blend
  constants (source constant colour, destination source alpha) I could not tie to the texture's alpha, so the shape is the alpha of an ordinary blend: it reads on a bright ground
  as well as a dark one (an added blend washed out on the desert's sand), which may differ from the Xbox's on a bright ground.
- **A reticle's colours have alpha 0 in the tag** (`0x002896FF`), and it is visible in the Xbox's screenshot, so a crosshair is drawn opaque.
- **Cost** (measured before `main`'s later speed-ups were merged in: with them the EE is busy 7.5 to 13.5 ms over the walk, 60 fps, free video memory 563 KB, and the weapon, effects and HUD cost what they did). 0.24 ms of the EE a frame for about 20 quads (the HUD's own state is made at 30 Hz, but the draw packets are made every frame; a quad is a texture setup and six
  vertices); the EE is busy 15.0 to 15.4 ms of 16.67 in the fortress, up from 14.7 to 15.2, and the frame rate there stays at 59 to 60.
- **Not done**: the motion sensor's blips and sweep (the Chief does not move: its backgrounds are drawn), the unit's integrated light meter (the flashlight), the multitexture
  overlays of the static elements (scan lines on some), the autoaim's red reticle (no target), the zoomed view's extra elements (the sniper's range numbers), the grenade
  throw's "no grenade" state, the damage indicators, the messages and waypoints, the HUD's sounds, and the multiplayer elements (the exporter leaves out an element used only
  on several local players). The shield and health are full (the Chief takes no damage) and the grenade count is 2.

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
the surface lists), the unit camera's own pitch limits (the Chief's are the defaults, 85.5 degrees); the stun penalties; the original's slipping physics on steep ground. A fall into the void
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
- `tools/fp_view_test.c`: the state machine (a reload of 86 ticks against the animation's 87 frames, the pistol readied in 33 of 35, the idle pose
  after 10 to 15 s) and, with a weapon, an animation type and a frame, the skinned vertices for `fp_view_reference.py`.
- `tools/fp_demo_test.c` (the command is in its header): the demo plans the walk, and the controller, given only the pad
  the demo makes, arrives next to the Chief, four times in 100 s, jumping four times a walk and facing him, never
  stuck; a touch of the pad takes over and Start restarts the demo.
- `tools/fp_hud_test.c` (`cc -O1 -I src -o /tmp/fp_hud_test tools/fp_hud_test.c src/fp_hud.c src/hud_assets.c -lm && /tmp/fp_hud_test [prefix]`): the quads of
  the full rifle (the crosshair first, centred; three meters; the loaded meter's threshold 4 * rounds + 2), and, given a prefix, a software composite of the quads with
  the baked textures into `<prefix>_<scenario>.ppm` (full, 29 rounds, low, empty, reload, pistol, pistol zoomed) for looking at: it is how the layout was
  compared with the screenshot of the Xbox before PCSX2.
- `tools/fp_demo_test.c` also counts what the weapons did (rifle 203 shots and pistol 20 in 150 s, four reloads started and finished, four blows, put aways
  and readies, zooms in and out, 84 impacts on the Chief), and prints the first walk's events with their times.
- **Stills of the demo's states**: `make EXTRA_CFLAGS="-DFIRST_PERSON -DFP_STILL_SECONDS=22.95f"` plays that many seconds of the demo at start-up in 1/60 s steps (the
  events print as `STILL <time>: event ...` with the final position and weapon state) and holds the frame; `rm src/main.o` first, since make does not see
  the flag. The host test's events and the EE's agree to 0.02 s (melee 22.75 and 22.77 s, reload start 24.02 and 24.07). Times of the states
  in the first walk (the arrival at about 19.7 s): rifle burst 20.9-22.0, melee 22.75, reload 24.07-26.90, put away 27.57, ready 27.97, zoom 29.57, pistol shots 30.1-32.5, zoom
  off 33.37. PCSX2 with `-datapath` on a copy of the configuration with the console log on (`tools/ps2/pcsx2_run.py` does it, without a window) shows the
  printed lines; the window capture is as for the orbit.
- The demo is what PCSX2 shows with no one at the pad. `make FP_NO_DEMO=1` leaves the player standing, for the pad alone.
  `make EXTRA_CFLAGS=-DPAD_DEBUG` prints what libpad sees twice a second.
- The frame rate of the demo, `make EXTRA_CFLAGS="-DPROFILE -DFIRST_PERSON"`: 59 to 60 fps, the EE busy
  14.5 to 15.4 ms of 16.7 with the weapon (its draw 3.3 ms, the marks, flash and HUD 0.02 to 0.07: 60 fps in the open stretches and the dwell, a vsync missed in 60 frames,
  59.0 fps, in the fortress's, as before the effects, and 12.4 to 13.1 ms at the Chief; a zoomed frame draws no weapon) (the orbit: 8 to 14), the widest views drawing 3,600 triangles. The builds for stills
  (`-DDEBUG_SECONDS`), the profile and the mip audit keep the orbit camera so that builds see the same views, unless
  `-DFIRST_PERSON` is added; `make ORBIT_CAMERA=1` asks for the orbit in any build.

## Pitfalls met

- **The packet heap is 5 MB, not 6.** With the first-person scene's tables (the weapons' models and animations, the HUD's textures, the demo's planner grid) the program's static data
  is 20 MB, and gsKit's 6 MB heap no longer fits the EE's 32 MB: its allocation fails and every packet is written through a NULL heap, which PCSX2 reports as a stream of
  `TLB Miss, ... addr=0x30000010 [store]` while the picture shows only the sky and the Chief. A 5 MB heap runs the whole demo with none; do not raise it without checking
  for those lines (`tools/ps2/pcsx2_run.py`).

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
