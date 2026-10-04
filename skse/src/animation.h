#pragma once

#include "jc2mech/mechanics.h"

// The animation side: Rico's JC2 animations are posed by the plugin itself after the behavior
// graph (body.cpp, visuals.cpp, grapple.cpp), from clip files baked by tools/anim/build_anims.py.
namespace animation {

// Keeps the player's behavior graph in the jump fall state while core moves the body, so the
// rest of Skyrim treats the player as airborne (a reel started on the ground would otherwise keep
// running the locomotion clips) and body.cpp poses over a quiet base.
void UpdateGraph(RE::PlayerCharacter* player, jc2::MoveState state, bool driven);

// visuals.cpp: the JC2 canopy (tools/anim/build_models.py -> Data/meshes/jc2mech) hangs over the
// player while parachuting, posed from JC2's canopy clips by core's chute pitch and bank (Rico's
// matching body pose, JC2's rico_speed_TB / rico_turn_TB baked to rico_chute.bin, is interpolated
// here too). Also turns Rico's body (not the actor, so the camera stays free) to core's heading
// in the states where core steers (chute, skydive, reel). Call after each core frame.
void UpdateVisuals(RE::PlayerCharacter* player, const jc2::Mechanics& mech, float delta);
// From the player's animation update hook, after the behavior graph has posed the body.
void AfterAnimationUpdate(RE::PlayerCharacter* player);
// Diagnostics: logs the player's jump / fall / land animation events while core drives.
// Call after a game load (the player's graph is rebuilt).
void WatchGraphEvents(RE::PlayerCharacter* player);
void SetDriven(bool driven, jc2::MoveState state);
// Stops the jump fall clips' built-in landing trigger while core drives (see animation.cpp).
// Call once at plugin load.
void InstallClipHook();

// From the first-person camera state's update hook: under the chute the view rolls with the bank.
void AfterCameraUpdate(RE::PlayerCamera* camera);
// Forget attached nodes (the player's 3D is rebuilt on load).
void ResetVisuals();

// grapple.cpp: Rico's left arm (JC2's aim + forearm clips), the hook flying out and the wire.
// UpdateGrapple after each core frame, GrappleAfterAnimation after AfterAnimationUpdate.
void UpdateGrapple(RE::PlayerCharacter* player, const jc2::Mechanics& mech, float delta);
void GrappleAfterAnimation(RE::PlayerCharacter* player);
void ResetGrapple();

// sounds.cpp: JC2's grapple and parachute sounds on core's events. After each core frame.
void UpdateSounds(RE::PlayerCharacter* player, const jc2::Mechanics& mech, const jc2::Vec3& vel, float dt);
void ResetSounds();

// body.cpp: Rico's whole body in the skydive, chute opening and reel (JC2's clips, the skydive
// blended by core's dive and steer like JC2), cross-faded between states and back to the graph.
// UpdateBody after each core frame; PoseBody first in AfterAnimationUpdate, RememberBody after
// the chute flight pose (before the tilts).
void UpdateBody(const jc2::Mechanics& mech, float delta);
void PoseBody(RE::NiAVObject* root3d);
void RememberBody(RE::NiAVObject* root3d);
bool BodyPosed();
void ResetBody();

}  // namespace animation
