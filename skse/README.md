# skse (phase 6)

Players see the mod as **Skydive**: the plugin builds as `Skydive.dll` (SKSE name Skydive, log
`Skydive.log`, settings `Skydive.ini`); the code, target and asset folders keep `jc2mech`.

The Skyrim host: an SKSE plugin on CommonLibSSE-NG v9.3.0 (FetchContent) that runs `jc2::Mechanics`
on the player. Separate CMake project: it needs vcpkg for CommonLib's dependencies (the VS-bundled
vcpkg, manifest in this folder).

## Build

From this folder, with the CMake that comes with Visual Studio 2022 (workload "Desktop development
with C++"; not on PATH: `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`):

```
cmake --preset vs2022
cmake --build --preset release --parallel
```

Output: `build/skse/Release/Skydive.dll`. The build copies the DLL to
`Data/SKSE/Plugins` of the Steam Skyrim (`SKYRIM_DATA_DIR` in the preset), plus `Skydive.ini` if
none is there yet. The first build compiles CommonLib (~15 min).

## JC2 assets (animations, canopy, hook, sounds)

Rico's animations, the canopy, hook, wire and sounds are converted from the local JC2 install by the tools in
`tools/anim` (Python 3 with numpy, matplotlib, lz4); they are JC2 data, so they are installed
straight into Skyrim's Data folder and never committed:

```
python tools/re/jc2arc.py unsarc "^(?!km\d)(.*(chute|parach|freefall|birdsuit|grpl|reel|fall|roof).*\.ban|.*\.bsk|gae0[1-9].*|gea0[1-9].*|wea04.*|hook_.*dds)$"
python tools/anim/bsa.py "Skyrim - Animations.bsa" "actors.character.(character assets.skeleton.hkx|animations.mt_jumpfall)" extracted/skyrim
python tools/re/jc2arc.py unsarc "^(grpl_|reel_in_start|la_grpl|rico_(speed|turn)_tb)|chute_add_idle"
python tools/re/jc2arc.py unsarc "^heli_default_(timeblend|exit)"
python tools/anim/build_anims.py
python tools/anim/build_models.py
python tools/anim/build_sounds.py
```

- `build_anims.py`: retargets Rico's clips onto the Skyrim skeleton (`retarget.py`; check one
  with `preview.py <clip.ban> out.png`) into the files the plugin plays itself, in
  `meshes/jc2mech`: `rico_body.bin` (whole body: the four skydive clips, rico_open_chute,
  rico_reel_open, reel_in_start, grpl_reel_flight_part2), `rico_chute.bin` (the chute flight
  pose grid) and `rico_arm.bin` (the grapple arm clips). Removes the OAR mod older versions
  installed (`meshes/actors/character/animations/OpenAnimationReplacer/JC2Mech`).
- `build_models.py`: `meshes/jc2mech/canopy.nif` (skinned to the JC2 canopy rig) +
  `canopy_anim.bin` (the canopy clips the plugin plays) + `hook.nif` + `rope.nif` (the wire,
  stretched between two bones), textures in `textures/jc2mech`.
- `build_sounds.py`: JC2's Parachute.fsb (grapple + chute sounds, MPEG) decoded with miniaudio
  (`pip install miniaudio`) to `sound/fx/jc2mech/*.wav`.

## In game

Needs SKSE64 2.3.1+ (runtime 1.7.104) and Address Library All in One v13+. Start with
`skse64_loader.exe`. Log: `Documents/My Games/Skyrim Special Edition/SKSE/Skydive.log`.

- Grapple: G (`iGrappleKey` in the ini), aims along the camera, range 120 m
- Falling from a height turns into the skydive (W dives faster, S flattens out, A/D turn)
- Space while skydiving or mid-reel: open the chute; Space under the chute closes it (on a tether
  it reels in). There is no chute from a short fall, as in JC2.
- Sneak: let go (rope, chute, wall)
- Move keys steer the chute
- Hitting the ground in the skydive ragdolls you with Skyrim's fall damage for the height fallen
- Options: F10 (`iMenuKey`) opens the built-in options overlay (Esc closes; the game is frozen
  while it is open); changes are saved to `Skydive.ini` ([Features]: grapple / chute on or off;
  [Stamina]: immersive stamina costs)
- On a wall or ceiling with a spell in the right hand: Right Attack casts it

## How it hooks in

- Tick: `PlayerCharacter::Update` (vfunc 0xAD), `FixedStepper` at `Tuning::tickHz` (75 Hz)
- Input: `BSInputDeviceManager` event sink, presses latched until the next tick; move axes from
  `PlayerControls::data.moveInputVec`
- `Raycast`: `bhkWorld::PickObject`, line-of-sight layer, player's collision group skipped
- `GetPos` / `GetVel` / `IsGrounded`: player position (feet + half height), `bhkCharacterController`
  velocity and on-ground state
- `SetVel`: only in the states where core moves the body (reel, chute, skydive, wall cling):
  controller forced in-air, gravity off, fall height reset, and core's velocity replaces the
  output of `hkpCharacterState::Update` (in air / on ground / jumping vtables) on every Havok
  step, so Skyrim's air control can't eat the horizontal speed. Walking and normal falls stay
  Skyrim's.
- Animations: while core drives, the plugin keeps the behavior graph in the jump fall state
  (sends `JumpFall` when `bInJumpState` is false) and stops the fall clips' landing trigger
  (`hkbClipGenerator::Update` hook, `src/animation.cpp`). After the player's animation update
  (vfunc 0x7D) it writes Rico's bones itself: `src/body.cpp` (skydive blended by core's dive /
  steer with JC2's weights, chute opening, reel, 0.2 s cross-fades), `src/visuals.cpp` (chute
  flight pose grid, the chute / reel tilt, and the canopy NIF cloned onto the player's 3D root
  and posed from the baked JC2 canopy clips), `src/grapple.cpp` (grapple arm, hook, wire). The
  player is turned to core's heading (chute, skydive, reel).
- Options overlay (`src/menu.cpp`): Dear ImGui (vcpkg) drawn from a hook on the swap chain's
  Present; game input swapped for an empty event list at the input device manager's send call
  while it is open.
- Fall damage: off while core drives and 1.5 s after (`fJumpFallHeightMin` raised meanwhile).
- Landing events (`JumpLand`, `JumpDown`, `JumpLandEnd`, `JumpLandDirectional`) sent to the
  player's graph are dropped while core drives (NotifyAnimationGraph hook).
- Camera: `Main::WorldRootCamera()` (looks down local +X); `SetCamera` is phase 7. First person
  under the chute: look capped at 80 deg from the heading, and the camera root rolls with the
  chute bank after `PlayerCamera::Update` (vfunc 2). The canopy, hook and wire hang in the
  player's cell in first person (the third-person root is hidden there).
- Units: Havok space (units * world scale) is meters; core (x, y, z) = Skyrim (x, z, -y)
