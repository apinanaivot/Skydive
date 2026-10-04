#pragma once

#include "jc2mech/mechanics.h"

// The right hand while core moves the body. Skyrim's behavior graph sits in its jump fall state
// then (animation.cpp), which refuses attacks, casting and drawing, so the plugin does them:
// - on a wall or ceiling, casts the right hand's spell on Right Attack (fire-and-forget after the
//   charge time, concentration while held, Skyrim's magicka cost), the arm aimed along the camera;
// - anywhere it drives, swings a melee weapon (a procedural right-arm cut; the nearest actor in
//   reach in front takes the weapon's Skyrim damage) and draws / sheathes on Ready Weapon.
// APPROX: no casting or attack animations, only the arm; no stagger, block or hit sounds.
namespace spells {

void OnButton(const RE::ButtonEvent& e);
// After each core frame.
void Update(RE::PlayerCharacter* player, jc2::MoveState state, float delta);
// From the animation hook, after the body and grapple arm are posed.
void PoseArm(RE::PlayerCharacter* player);
void Reset();

}  // namespace spells
