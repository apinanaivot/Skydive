#include "skyrim_host.h"

#include <cmath>
#include <xmmintrin.h>

#include "settings.h"

using jc2::Vec3;
using jc2::MoveState;

namespace {

// Skyrim world (Z-up) <-> core (Y-up), same scale.
Vec3 ToCore(float x, float y, float z) { return {x, z, -y}; }
RE::hkVector4 ToHavok(Vec3 v) { return {v.x, -v.z, v.y, 0.0f}; }

Vec3 FromHavok(const RE::hkVector4& h) {
    alignas(16) float f[4];
    _mm_store_ps(f, h.quad);
    return ToCore(f[0], f[1], f[2]);
}

SkyrimHost* g_hooked = nullptr;

// hkpCharacterState::Update (vfunc 6) of each Skyrim character state. The state computes the
// velocity the controller integrates this Havok step (in air: gravity plus air control toward
// the move input); core's velocity goes in after it.
template <class State>
struct StateUpdate {
    static void thunk(State* a_this, RE::hkpCharacterContext& a_context, const RE::hkpCharacterInput& a_input,
                      RE::hkpCharacterOutput& a_output) {
        func(a_this, a_context, a_input, a_output);
        if (g_hooked) g_hooked->OverrideOutput(a_context, a_output);
    }
    static inline REL::Relocation<decltype(thunk)> func;

    static void Install() {
        REL::Relocation<std::uintptr_t> vtbl{State::VTABLE[0]};
        func = vtbl.write_vfunc(6, thunk);
    }
};

// hkpCharacterState::Change (vfunc 7) of the in-air state: decides the next state (on ground when
// Havok finds support). While core drives, Havok switched the player to on-ground mid-air about a
// second into every chute flight; core took that as a landing. The state stays in air instead.
struct InAirChange {
    static void thunk(RE::bhkCharacterStateInAir* a_this, RE::hkpCharacterContext& a_context,
                      const RE::hkpCharacterInput& a_input, RE::hkpCharacterOutput& a_output) {
        if (g_hooked && g_hooked->HoldsInAir(a_context)) return;
        func(a_this, a_context, a_input, a_output);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

}  // namespace

bool SkyrimHost::HoldsInAir(const RE::hkpCharacterContext& ctx) const {
    return driving_ && controller_ && &ctx == &controller_->context;
}

void SkyrimHost::InstallHooks() {
    g_hooked = this;
    // In air is the one core forces; the others cover a step where Havok switched state first
    // (touching ground mid-reel, the jump state on the frame of a jump).
    StateUpdate<RE::bhkCharacterStateInAir>::Install();
    StateUpdate<RE::bhkCharacterStateOnGround>::Install();
    StateUpdate<RE::bhkCharacterStateJumping>::Install();
    REL::Relocation<std::uintptr_t> inAir{RE::bhkCharacterStateInAir::VTABLE[0]};
    InAirChange::func = inAir.write_vfunc(7, InAirChange::thunk);
    SKSE::log::info("character state update hooks installed");
}

void SkyrimHost::OverrideOutput(RE::hkpCharacterContext& ctx, RE::hkpCharacterOutput& out) const {
    if (!controller_ || &ctx != &controller_->context) return;
    if (groundDriving_) {
        // Havok x / y are horizontal; Skyrim's own vertical (gravity, slopes) stays.
        alignas(16) float o[4], d[4];
        _mm_store_ps(o, out.velocity.quad);
        _mm_store_ps(d, driveVel_.quad);
        out.velocity = RE::hkVector4{d[0], d[1], o[2], 0.0f};
        return;
    }
    if (!driving_) return;
    out.velocity = driveVel_;
    // Skyrim drops the player onto the ground state once a second (a timer, not Havok's Change);
    // each time it may run its landing. Back to in air on the next physics step.
    ctx.currentState = RE::hkpCharacterStateType::kInAir;
    controller_->wantState = RE::hkpCharacterStateType::kInAir;
}

void SkyrimHost::HoldInAir(RE::PlayerCharacter* player) const {
    if (!driving_ || !player || player->GetCharController() != controller_ || !controller_) return;
    controller_->context.currentState = RE::hkpCharacterStateType::kInAir;
    controller_->wantState = RE::hkpCharacterStateType::kInAir;
}

bool SkyrimHost::Drives(MoveState s) {
    return s == MoveState::Grappling || s == MoveState::Parachuting || s == MoveState::Skydive ||
           s == MoveState::WallCling || s == MoveState::CeilingHang || s == MoveState::WallJump ||
           s == MoveState::WallDrop;
}

bool SkyrimHost::BeginFrame(RE::PlayerCharacter* player, float delta) {
    player_ = player;
    frameDelta_ = delta;
    controller_ = player ? player->GetCharController() : nullptr;
    if (!controller_ || !player->GetParentCell() || !player->GetParentCell()->GetbhkWorld()) return false;

    const float scale = RE::bhkWorld::GetWorldScale();
    const RE::NiPoint3 feet = player->GetPosition();
    pos_ = ToCore(feet.x * scale, feet.y * scale, feet.z * scale) + Vec3{0.0f, halfExtents_.y, 0.0f};

    RE::hkVector4 v;
    controller_->GetLinearVelocityImpl(v);
    controllerVel_ = FromHavok(v);
    // While core drives, its own velocity is the truth. Skyrim's in-air state pulls the velocity
    // we set toward the move input (air control); reading that back every frame compounds it and
    // kills the chute's glide. Only what a collision took off is kept (CollisionLoss).
    if (!driving_) vel_ = controllerVel_;
    else CollisionLoss();
    const RE::hkpCharacterStateType havokState = controller_->context.currentState;
    // While core drives, the controller is held in air, so ground contact comes from a short ray
    // below the feet (falling or level only, so a reel can leave the ground).
    if (driving_) {
        jc2::HitInfo hit;
        const Vec3 feetCore = pos_ - Vec3{0.0f, halfExtents_.y, 0.0f};
        grounded_ = vel_.y <= 0.5f && Raycast(feetCore + Vec3{0.0f, 0.2f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.35f, hit) &&
                    hit.normal.y > 0.5f;
    } else {
        grounded_ = havokState == RE::hkpCharacterStateType::kOnGround;
    }
    // Water only matters in the air (the skydive and chute end in it, and don't start).
    inWater_ = !grounded_ && CheckWater(player);
    if (driving_ && havokState != lastHavokState_) {
        // Diagnostics: Havok switching the state under core (e.g. to on-ground mid-air).
        SKSE::log::info("  controller state {} -> {} while driving (want {}, flags {:#x})", static_cast<int>(lastHavokState_),
                        static_cast<int>(havokState), static_cast<int>(controller_->wantState), controller_->flags.underlying());
    }
    lastHavokState_ = havokState;

    if (RE::NiCamera* cam = RE::Main::WorldRootCamera()) {
        // NiCamera looks down its local +X.
        const RE::NiPoint3& t = cam->world.translate;
        const RE::NiMatrix3& r = cam->world.rotate;
        camera_.position = ToCore(t.x * scale, t.y * scale, t.z * scale);
        camera_.forward = jc2::Normalize(ToCore(r.entry[0][0], r.entry[1][0], r.entry[2][0]));
    }

    const RE::NiPoint2 move = RE::PlayerControls::GetSingleton()->data.moveInputVec;
    input_.moveX = std::clamp(move.x, -1.0f, 1.0f);
    input_.moveY = std::clamp(move.y, -1.0f, 1.0f);
    return true;
}

void SkyrimHost::AfterTick(MoveState s, float dt) {
    if (Drives(s)) pos_ += vel_ * dt;
    input_.grapplePressed = false;
    input_.parachutePressed = false;
    input_.jumpPressed = false;
    input_.dropPressed = false;
}

void SkyrimHost::EndFrame(MoveState s) {
    if (!controller_) return;
    const bool drive = Drives(s);

    if (drive && !driving_) savedGravity_ = controller_->gravity;
    if (drive) {
        // APPROX: the controller integrates the last tick's velocity over Havok's own step, so
        // positions can lag the fixed-tick result by up to a tick.
        controller_->context.currentState = RE::hkpCharacterStateType::kInAir;
        controller_->wantState = RE::hkpCharacterStateType::kInAir;
        controller_->gravity = 0.0f;
    } else if (driving_) {
        controller_->gravity = savedGravity_;
    }
    // No fall damage while core moves the body and for a moment after it lets go (a reel ends at
    // full speed next to the ground): Skyrim's minimum damaging fall height goes up meanwhile.
    // Writing fallStartHeight instead failed: it reads 0 on the ground, so its units can't be
    // told apart, and the wrong guess turned every landing into a 4000-unit fall.
    noFallDamage_ = drive ? kFallDamageGrace : (std::max)(0.0f, noFallDamage_ - frameDelta_);
    SetFallDamage(noFallDamage_ <= 0.0f);
    // A bad velocity (a NaN, or an anchor that jumped across the world) must never reach Havok.
    if (!std::isfinite(vel_.x) || !std::isfinite(vel_.y) || !std::isfinite(vel_.z)) {
        SKSE::log::warn("non-finite velocity from core in {}, zeroed", jc2::ToString(s));
        vel_ = {};
    } else if (const float speed = jc2::Length(vel_); speed > kMaxSpeed) {
        vel_ = vel_ * (kMaxSpeed / speed);
    }
    // One more write on the way out, so Skyrim continues from core's last velocity (zero after
    // an arrival, the chute's velocity after closing it).
    driveVel_ = ToHavok(vel_);
    lastCmd_ = vel_;
    lastState_ = s;
    if (drive || driving_) controller_->SetLinearVelocityImpl(driveVel_);
    driving_ = drive;
    groundDriving_ = GroundDrives(s);
}

void SkyrimHost::CollisionLoss() {
    // Under the chute and in the skydive core carries its velocity from tick to tick, so a wall
    // or tree that stopped the body would be forgotten: the controller's velocity after the
    // physics step is what the collision left of last frame's command (gravity is off and the
    // state update's output is replaced, so nothing else changes it). Only a loss is kept.
    if (lastState_ != MoveState::Parachuting && lastState_ != MoveState::Skydive) return;
    const Vec3 d = controllerVel_ - lastCmd_;
    if (jc2::Length(d) < kMinLoss || jc2::Dot(d, lastCmd_) >= 0.0f) return;
    vel_ = vel_ + d;
    static float lastLog = -1.0f;
    const float now = static_cast<float>(GetTickCount64()) / 1000.0f;
    if (now - lastLog > 0.5f) {
        lastLog = now;
        SKSE::log::info("  {}: collision took {:.1f} m/s, now {:.1f} m/s", jc2::ToString(lastState_), jc2::Length(d), jc2::Length(vel_));
    }
}

bool SkyrimHost::CheckWater(RE::PlayerCharacter* player) const {
    if (player->AsActorState()->IsSwimming()) return true;
    // The feet below the surface of the water the player is in: the game sets the reference's
    // relevant water height when its body overlaps a water plane's volume (lakes and rivers at any
    // height); the cell's own water level is the fallback. Plain reads: TES::GetWaterHeight has no
    // Address Library id on 1.7.104 (it failed to load at the first airborne frame).
    const float surface = player->GetWaterHeight();
    return std::isfinite(surface) && surface > -1.0e6f && player->GetPosition().z < surface;
}

void SkyrimHost::SetFallDamage(bool on) {
    RE::Setting* min = RE::GameSettingCollection::GetSingleton()->GetSetting("fJumpFallHeightMin");
    if (!min || on == !fallDamageOff_) return;
    if (!on) {
        savedFallMin_ = min->data.f;
        min->data.f = 1.0e9f;
    } else {
        min->data.f = savedFallMin_;
    }
    fallDamageOff_ = !on;
}

void SkyrimHost::HardLanding(Vec3 vel, float dropMeters, float mult, bool ragdoll) {
    if (!player_) return;
    // Skyrim's fall damage: ((height - fJumpFallHeightMin) * fJumpFallHeightMult) ^
    // fJumpFallHeightExponent, in game units. The minimum is the saved one (raised while driving).
    auto* gmst = RE::GameSettingCollection::GetSingleton();
    auto get = [&](const char* name, float fallback) {
        const RE::Setting* s = gmst->GetSetting(name);
        return s ? s->data.f : fallback;
    };
    const float minUnits = fallDamageOff_ ? savedFallMin_ : get("fJumpFallHeightMin", 600.0f);
    const float units = dropMeters / RE::bhkWorld::GetWorldScale();
    const float over = units - minUnits;
    const float damage = over > 0.0f ? std::pow(over * get("fJumpFallHeightMult", 0.1f), get("fJumpFallHeightExponent", 1.65f)) * mult : 0.0f;
    SKSE::log::info("skydive landing: {:.1f} m fallen at {:.1f} m/s, fall damage {:.0f}", dropMeters, jc2::Length(vel), damage);
    // Ragdoll: knocked away from a point behind the motion, harder the faster the landing. Off by
    // option, except when the fall kills (Skyrim ragdolls the dead anyway).
    const bool kills = damage >= player_->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
    RE::AIProcess* process = player_->GetActorRuntimeData().currentProcess;
    if (process && (ragdoll || kills)) {
        Vec3 h{vel.x, 0.0f, vel.z};
        const float hs = jc2::Length(h);
        h = hs > 0.5f ? h * (1.0f / hs) : Vec3{};
        const RE::NiPoint3 feet = player_->GetPosition();
        const RE::NiPoint3 from{feet.x - h.x * 50.0f, feet.y + h.z * 50.0f, feet.z - 20.0f};
        process->KnockExplosion(player_, from, (std::min)(2.0f + 0.2f * jc2::Length(vel), 10.0f));
    }
    if (damage > 0.0f) player_->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, damage);
}

void SkyrimHost::Release() {
    if (driving_ && controller_) controller_->gravity = savedGravity_;
    noFallDamage_ = 0.0f;
    SetFallDamage(true);
    driving_ = false;
    groundDriving_ = false;
    controller_ = nullptr;
    input_ = {};
    anchors_.clear();
}

void SkyrimHost::OnButton(const RE::ButtonEvent& e) {
    std::uint32_t key = e.GetIDCode();
    switch (e.GetDevice()) {
        case RE::INPUT_DEVICE::kKeyboard: break;
        case RE::INPUT_DEVICE::kMouse: key += 256; break;
        default: key = 0xFFFFFFFF; break;
    }
    if (key == config.grappleKey) {
        input_.grappleHeld = e.IsPressed();
        input_.grapplePressed |= e.IsDown();
    }
    if (!e.IsDown()) return;

    const RE::BSFixedString& ev = e.QUserEvent();
    const auto* names = RE::UserEvents::GetSingleton();
    if (ev == names->jump) {
        // Jump on the ground (and off walls), the chute in the air, like JC2's space.
        input_.jumpPressed |= grounded_;
        input_.parachutePressed |= !grounded_;
    } else if (ev == names->sneak) {
        input_.dropPressed = true;
    }
}

bool SkyrimHost::Raycast(Vec3 from, Vec3 dir, float maxDist, jc2::HitInfo& out) const {
    return Pick(from, dir, maxDist, out, nullptr, RE::COL_LAYER::kLOS);
}

bool SkyrimHost::RaycastAnchor(Vec3 from, Vec3 dir, float maxDist, jc2::HitInfo& out) {
    // The projectile layer also hits loose objects and people (line of sight passes through
    // them); line of sight as the fallback keeps the old static hits.
    const RE::hkpCollidable* collidable = nullptr;
    if (!Pick(from, dir, maxDist, out, &collidable, RE::COL_LAYER::kProjectile) &&
        !Pick(from, dir, maxDist, out, &collidable, RE::COL_LAYER::kLOS)) {
        return false;
    }
    if (!collidable) return true;
    // Fixed bodies are the static world (anchor 0). Anything else that moves gets an anchor on the
    // node it drives (an actor's bone, a loose object), or its reference's root.
    const RE::hkpRigidBody* rigid = nullptr;
    if (collidable->broadPhaseHandle.type == static_cast<std::int8_t>(RE::hkpWorldObject::BroadPhaseType::kEntity)) {
        rigid = collidable->GetOwner<RE::hkpRigidBody>();
    }
    using Motion = RE::hkpMotion::MotionType;
    const Motion motion = rigid ? rigid->motion.type.get() : Motion::kFixed;
    RE::TESObjectREFR* ref = RE::TESHavokUtilities::FindCollidableRef(*collidable);
    if (motion == Motion::kFixed && !(ref && ref->As<RE::Actor>())) return true;
    RE::NiAVObject* node = RE::TESHavokUtilities::FindCollidableObject(*collidable);
    if (!node && ref) node = ref->Get3D();
    if (!node) return true;

    Anchor a;
    a.id = nextAnchor_++;
    a.node.reset(node);
    if (ref) a.ref = ref->CreateRefHandle();
    const float scale = RE::bhkWorld::GetWorldScale();
    const RE::NiPoint3 world{out.point.x / scale, -out.point.z / scale, out.point.y / scale};
    const RE::NiTransform& w = node->world;
    a.local = (w.rotate.Transpose() * (world - w.translate)) / (w.scale > 0.0f ? w.scale : 1.0f);
    // Loose objects (not people or animals, not too heavy) are pulled in.
    const bool dynamic = motion == Motion::kDynamic || motion == Motion::kSphereInertia || motion == Motion::kBoxInertia ||
                         motion == Motion::kThinBoxInertia;
    if (dynamic && !(ref && ref->As<RE::Actor>()) && rigid->userData) {
        alignas(16) float inertia[4];
        _mm_store_ps(inertia, rigid->motion.inertiaAndMassInv.quad);
        const float mass = inertia[3] > 0.0f ? 1.0f / inertia[3] : 1e9f;
        out.pullable = mass <= Settings::Get().pullMaxMass;
        if (out.pullable) a.body.reset(reinterpret_cast<RE::bhkRigidBody*>(rigid->userData));
        SKSE::log::info("grapple: loose object {:08X} mass {:.1f}{}", ref ? ref->GetFormID() : 0, mass, out.pullable ? " (pulled)" : " (too heavy)");
    } else {
        SKSE::log::info("grapple: moving anchor on {:08X} (motion {})", ref ? ref->GetFormID() : 0, static_cast<int>(motion));
    }
    out.anchor = a.id;
    if (anchors_.size() >= kMaxAnchors) anchors_.erase(anchors_.begin());
    anchors_.push_back(std::move(a));
    return true;
}

const SkyrimHost::Anchor* SkyrimHost::FindAnchor(std::uint32_t id) const {
    for (const Anchor& a : anchors_) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

bool SkyrimHost::AnchorPoint(std::uint32_t id, Vec3& out) const {
    const Anchor* a = FindAnchor(id);
    if (!a || !a->node || !a->node->parent) return false;
    if (a->ref) {
        const RE::NiPointer<RE::TESObjectREFR> ref = a->ref.get();
        if (!ref || ref->IsDisabled() || ref->IsDeleted() || !ref->Is3DLoaded()) return false;
    }
    const RE::NiTransform& w = a->node->world;
    const RE::NiPoint3 p = w.translate + w.rotate * (a->local * w.scale);
    const float scale = RE::bhkWorld::GetWorldScale();
    out = ToCore(p.x * scale, p.y * scale, p.z * scale);
    return true;
}

void SkyrimHost::PullAnchor(std::uint32_t id, Vec3 to, float speed) {
    const Anchor* a = FindAnchor(id);
    Vec3 at;
    if (!a || !a->body || !AnchorPoint(id, at)) return;
    // Only a body still in a Havok world (an unloaded or picked up object's isn't).
    const RE::hkpRigidBody* rigid = a->body->GetRigidBody();
    if (!rigid || !rigid->world) return;
    const Vec3 v = jc2::Normalize(to - at) * speed;
    a->body->SetLinearVelocity(ToHavok(v));
}

bool SkyrimHost::Pick(Vec3 from, Vec3 dir, float maxDist, jc2::HitInfo& out, const RE::hkpCollidable** hit, RE::COL_LAYER layer) const {
    if (!player_) return false;
    RE::TESObjectCELL* cell = player_->GetParentCell();
    RE::bhkWorld* world = cell ? cell->GetbhkWorld() : nullptr;
    if (!world) return false;

    const Vec3 to = from + jc2::Normalize(dir) * maxDist;
    RE::bhkPickData pick;
    pick.rayInput.from = ToHavok(from);
    pick.rayInput.to = ToHavok(to);
    // The layer in the player's own collision group, so the ray skips the player.
    RE::CFilter filter;
    player_->GetCollisionFilterInfo(filter);
    pick.rayInput.filterInfo.filter = (filter.filter & 0xFFFF0000) | static_cast<std::uint32_t>(layer);

    world->PickObject(pick);
    if (!pick.rayOutput.HasHit()) return false;

    const float f = pick.rayOutput.hitFraction;
    out.point = from + (to - from) * f;
    out.normal = jc2::Normalize(FromHavok(pick.rayOutput.normal));
    out.distance = maxDist * f;
    if (hit) *hit = pick.rayOutput.rootCollidable;
    return true;
}
