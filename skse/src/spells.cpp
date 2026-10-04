#include "spells.h"

#include <algorithm>
#include <cmath>

#include "pose.h"
#include "skyrim_host.h"

namespace spells {

namespace {

constexpr float kMinInterval = 0.4f;       // s between fire-and-forget casts
constexpr float kConcentrationTick = 0.2f;  // s between concentration casts while held
constexpr float kAimHold = 0.4f;            // s the arm stays up after a cast
constexpr float kAimRate = 10.0f;           // 1/s, the arm in and out of the aim
constexpr float kAimDistance = 3000.0f;     // units along the camera the arm points at

// Melee swing (APPROX, procedural): the right arm rises over kSwingRaise of the swing, cuts down
// to kSwingCut, then fades back to the body pose; the hit lands at kSwingHitAt.
constexpr float kSwingSeconds = 0.55f;     // at weapon speed 1
constexpr float kSwingRaise = 0.4f, kSwingCut = 0.7f, kSwingHitAt = 0.55f;
constexpr float kSwingHigh = 2.1f;         // rad above the camera forward at the top
constexpr float kSwingLow = -0.8f;         // rad at the end of the cut
constexpr float kReachMargin = 60.0f;      // units added to the weapon's reach (both move fast)
constexpr float kHitCone = 0.5f;           // cos of the half-angle around the camera forward

bool g_held = false;     // Right Attack/Block
bool g_pressed = false;  // since the last update
bool g_readyPressed = false;
float g_charge = 0.0f;   // fire-and-forget: held this long
float g_cooldown = 0.0f;
float g_aimTimer = 0.0f;
float g_aim = 0.0f;      // arm aim weight
float g_swing = -1.0f;   // 0..1 through a melee swing, -1 none
float g_swingSeconds = kSwingSeconds;
bool g_swingHit = false;
float g_drawCheck = 0.0f;  // s until the draw / sheathe result is logged

RE::SpellItem* RightSpell(RE::PlayerCharacter* player) {
    if (!player->AsActorState()->IsWeaponDrawn()) return nullptr;
    RE::TESForm* form = player->GetEquippedObject(false);
    return form ? form->As<RE::SpellItem>() : nullptr;
}

RE::TESObjectWEAP* RightMelee(RE::PlayerCharacter* player) {
    if (!player->AsActorState()->IsWeaponDrawn()) return nullptr;
    RE::TESForm* form = player->GetEquippedObject(false);
    RE::TESObjectWEAP* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
    return weapon && weapon->IsMelee() && !weapon->IsHandToHandMelee() ? weapon : nullptr;
}

// Pays and casts; false (and the magicka bar flashes) when there isn't enough.
bool Cast(RE::PlayerCharacter* player, RE::SpellItem* spell, float costScale) {
    RE::ActorValueOwner* av = player->AsActorValueOwner();
    const float cost = spell->CalculateMagickaCost(player) * costScale;
    if (av->GetActorValue(RE::ActorValue::kMagicka) < cost) {
        RE::HUDMenu::FlashMeter(RE::ActorValue::kMagicka);
        return false;
    }
    RE::MagicCaster* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
    if (!caster) return false;
    RE::TESObjectREFR* target = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf ? player : nullptr;
    caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f, player);
    if (cost > 0.0f) av->DamageActorValue(RE::ActorValue::kMagicka, cost);
    g_aimTimer = kAimHold;
    return true;
}

RE::NiPoint3 CameraForward(const RE::NiCamera* cam) {
    // NiCamera looks down its local +X; +Y is up.
    return {cam->world.rotate.entry[0][0], cam->world.rotate.entry[1][0], cam->world.rotate.entry[2][0]};
}

// The swing's hit: the nearest living actor within the weapon's reach in front of the camera
// takes Skyrim's damage for this weapon (HitData::Populate: skill, perks, armor).
// APPROX: no stagger, block, hit sound or crime; the damage goes straight to health.
void Strike(RE::PlayerCharacter* player) {
    const RE::NiCamera* cam = RE::Main::WorldRootCamera();
    auto* lists = RE::ProcessLists::GetSingleton();
    if (!cam || !lists) return;
    const RE::NiPoint3 fwd = CameraForward(cam);
    const RE::NiPoint3 chest = player->GetPosition() + RE::NiPoint3{0.0f, 0.0f, 80.0f};
    const float reach = player->GetAttackReach() + kReachMargin;
    RE::Actor* best = nullptr;
    float bestDist = reach;
    for (const RE::ActorHandle& h : lists->highActorHandles) {
        const RE::NiPointer<RE::Actor> actor = h.get();
        if (!actor || actor.get() == player || actor->IsDead()) continue;
        RE::NiPoint3 to = actor->GetPosition() + RE::NiPoint3{0.0f, 0.0f, 80.0f} - chest;
        const float dist = to.Unitize();
        if (dist < bestDist && to.Dot(fwd) >= kHitCone) {
            best = actor.get();
            bestDist = dist;
        }
    }
    RE::InventoryEntryData* weapon = player->GetEquippedEntryData(false);
    if (!best || !weapon) return;
    RE::HitData hit{};
    hit.Populate(player, best, weapon);
    if (hit.totalDamage > 0.0f) best->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, hit.totalDamage);
    SKSE::log::info("melee: hit {:08X} at {:.0f} units for {:.1f}", best->GetFormID(), bestDist, hit.totalDamage);
}

// Where the swinging arm points (forearm direction) at swing time u, and its weight.
RE::NiPoint3 SwingDirection(const RE::NiCamera* cam, float u, float& weight) {
    const float a = u < kSwingRaise ? kSwingHigh * (u / kSwingRaise)
                    : u < kSwingCut ? kSwingHigh + (kSwingLow - kSwingHigh) * ((u - kSwingRaise) / (kSwingCut - kSwingRaise))
                                    : kSwingLow;
    weight = std::clamp((std::min)(u / 0.15f, (1.0f - u) / 0.25f), 0.0f, 1.0f);
    const RE::NiPoint3 fwd = CameraForward(cam);
    const RE::NiPoint3 up{cam->world.rotate.entry[0][1], cam->world.rotate.entry[1][1], cam->world.rotate.entry[2][1]};
    return fwd * std::cos(a) + up * std::sin(a);
}

}  // namespace

void OnButton(const RE::ButtonEvent& e) {
    const auto* names = RE::UserEvents::GetSingleton();
    if (e.QUserEvent() == names->readyWeapon) {
        g_readyPressed |= e.IsDown();
        return;
    }
    if (e.QUserEvent() != names->rightAttack) return;
    g_held = e.IsPressed();
    g_pressed |= e.IsDown();
}

void Update(RE::PlayerCharacter* player, jc2::MoveState state, float delta) {
    const bool pressed = g_pressed;
    const bool readyPressed = g_readyPressed;
    g_pressed = g_readyPressed = false;
    g_cooldown = (std::max)(0.0f, g_cooldown - delta);
    g_aimTimer = (std::max)(0.0f, g_aimTimer - delta);
    if (!player) return;
    const bool driven = SkyrimHost::Drives(state);
    const bool cling = state == jc2::MoveState::WallCling || state == jc2::MoveState::CeilingHang;

    // Draw / sheathe: Skyrim's Ready Weapon handler refuses in the air, and the graph sits in its
    // jump fall state while core drives (animation.cpp), so the plugin asks for it.
    const RE::WEAPON_STATE weaponState = player->AsActorState()->GetWeaponState();
    if (readyPressed && driven && (weaponState == RE::WEAPON_STATE::kDrawn || weaponState == RE::WEAPON_STATE::kSheathed)) {
        player->DrawWeaponMagicHands(weaponState == RE::WEAPON_STATE::kSheathed);
        g_drawCheck = 1.0f;
    }
    if (g_drawCheck > 0.0f && (g_drawCheck -= delta) <= 0.0f) {
        SKSE::log::info("draw / sheathe in the air: weapon state {} a second later", static_cast<int>(player->AsActorState()->GetWeaponState()));
    }

    // Melee: the graph refuses attacks in the jump fall state too, so the swing is the plugin's.
    RE::TESObjectWEAP* weapon = driven ? RightMelee(player) : nullptr;
    if (weapon && pressed && g_swing < 0.0f) {
        g_swing = 0.0f;
        g_swingHit = false;
        const float speed = weapon->GetSpeed();
        g_swingSeconds = kSwingSeconds / (speed > 0.1f ? speed : 1.0f);
    }
    if (g_swing >= 0.0f) {
        g_swing += delta / g_swingSeconds;
        if (!g_swingHit && g_swing >= kSwingHitAt) {
            g_swingHit = true;
            if (weapon) Strike(player);
        }
        if (g_swing >= 1.0f || !weapon) g_swing = -1.0f;
    }

    RE::SpellItem* spell = cling ? RightSpell(player) : nullptr;
    if (!spell) {
        g_charge = 0.0f;
    } else if (spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
        // Concentration: cost is per second, cast in ticks while held.
        if (g_held && g_cooldown <= 0.0f && Cast(player, spell, kConcentrationTick)) g_cooldown = kConcentrationTick;
        if (g_held) g_aimTimer = kAimHold;
    } else {
        // Fire and forget: charge while held, cast on release once charged (or on a tap with no
        // charge time).
        if (pressed) g_charge = 0.0f;
        if (g_held) {
            g_charge += delta;
            g_aimTimer = kAimHold;
        }
        const bool charged = g_charge >= spell->GetChargeTime();
        if ((!g_held && (pressed || g_charge > 0.0f) && charged) && g_cooldown <= 0.0f) {
            if (Cast(player, spell, 1.0f)) g_cooldown = kMinInterval;
            g_charge = 0.0f;
        } else if (!g_held && !pressed) {
            g_charge = 0.0f;
        }
    }
    const float k = (std::min)(1.0f, delta * kAimRate);
    g_aim += ((spell && g_aimTimer > 0.0f ? 1.0f : 0.0f) - g_aim) * k;
}

void PoseArm(RE::PlayerCharacter* player) {
    const bool swinging = g_swing >= 0.0f;
    if ((g_aim < 0.01f && !swinging) || !player) return;
    RE::NiAVObject* root3d = player->Get3D(false);
    const RE::NiCamera* cam = RE::Main::WorldRootCamera();
    if (!root3d || !cam) return;
    static const RE::BSFixedString kUpper("NPC R UpperArm [RUar]");
    static const RE::BSFixedString kFore("NPC R Forearm [RLar]");
    static const RE::BSFixedString kHand("NPC R Hand [RHnd]");
    RE::NiAVObject* upper = root3d->GetObjectByName(kUpper);
    RE::NiAVObject* fore = root3d->GetObjectByName(kFore);
    RE::NiAVObject* hand = root3d->GetObjectByName(kHand);
    if (!upper || !fore || !hand) return;
    animation::UpdateSkeleton(root3d);
    RE::NiPoint3 want;
    float w = g_aim;
    if (swinging) {
        want = SwingDirection(cam, g_swing, w);
    } else {
        want = cam->world.translate + CameraForward(cam) * kAimDistance - hand->world.translate;
    }
    animation::TurnBone(upper, hand->world.translate - fore->world.translate, want, w);
    animation::UpdateSkeleton(root3d);
}

void Reset() {
    g_held = g_pressed = g_readyPressed = false;
    g_charge = g_cooldown = g_aimTimer = g_aim = g_drawCheck = 0.0f;
    g_swing = -1.0f;
    g_swingHit = false;
}

}  // namespace spells
