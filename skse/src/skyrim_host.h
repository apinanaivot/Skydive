#pragma once

#include <vector>

#include "jc2mech/mechanics.h"

// Skyrim side of jc2::IHost. All unit and axis conversion lives here: core works in JC2 meters
// with Y-up, Skyrim in game units with Z-up (Havok space = units * bhkWorld::GetWorldScale(),
// which is meters to within 0.02%). The mapping keeps handedness: core (x, y, z) = Skyrim
// (x, z, -y).
//
// Movement is driven through the player's bhkCharacterController. While core is in a state it
// moves the body itself (reel, chute, skydive, wall cling) the controller is forced into its
// in-air state with gravity off, and core's velocity replaces the output of the character
// state update on every Havok step (InstallHooks): Skyrim's air control otherwise blends it
// toward the move input and loses ~30% of the horizontal speed. Everything else (walking,
// normal jumps and falls) stays Skyrim's; core only watches so it can fire the hook.
class SkyrimHost final : public jc2::IHost {
public:
    struct Config {
        std::uint32_t grappleKey = 0x22;  // DirectInput scan code (G); 256+ are mouse buttons
        bool notify = true;               // print state changes to the console
    };
    Config config;

    // Hooks the character state updates. Call once at plugin load, on the instance that runs.
    void InstallHooks();

    // Reads the player's state at the start of a frame. False when there is nothing to drive.
    bool BeginFrame(RE::PlayerCharacter* player, float delta);
    // After each fixed tick: advances the shadow position while core drives the body, so the
    // ticks of one frame see each other's motion. Clears the latched presses.
    void AfterTick(jc2::MoveState s, float dt);
    // Hands the result to the controller.
    void EndFrame(jc2::MoveState s);
    // Gives the controller back to Skyrim (load game, plugin reset).
    void Release();

    // Input, called from the input event sink.
    void OnButton(const RE::ButtonEvent& e);
    // Input changes for this frame (stamina rules), between BeginFrame and the ticks.
    void ClearGrapplePress() { input_.grapplePressed = false; }
    void PressDrop() { input_.dropPressed = true; }
    void ScaleMove(float k) {
        input_.moveX *= k;
        input_.moveY *= k;
    }

    // IHost
    bool Raycast(jc2::Vec3 from, jc2::Vec3 dir, float maxDist, jc2::HitInfo& out) const override;
    bool RaycastAnchor(jc2::Vec3 from, jc2::Vec3 dir, float maxDist, jc2::HitInfo& out) override;
    bool AnchorPoint(std::uint32_t id, jc2::Vec3& out) const override;
    void PullAnchor(std::uint32_t id, jc2::Vec3 to, float speed) override;
    // Lets go of the held anchor nodes and bodies (call when core uses none).
    void DropAnchors() {
        if (!anchors_.empty()) anchors_.clear();
    }
    jc2::Vec3 GetPos() const override { return pos_; }
    jc2::Vec3 GetVel() const override { return vel_; }
    void SetVel(jc2::Vec3 v) override { vel_ = v; }
    bool IsGrounded() const override { return grounded_; }
    bool InWater() const override { return inWater_; }
    jc2::Vec3 GetHalfExtents() const override { return halfExtents_; }
    jc2::InputState GetInput() const override { return input_; }
    jc2::CameraState GetCamera() const override { return camera_; }
    void SetCamera(const jc2::CameraState&) override {}  // phase 7

    static bool Drives(jc2::MoveState s);
    // States where core sets only the horizontal speed and Skyrim keeps the body on the ground
    // (the chute landing roll).
    static bool GroundDrives(jc2::MoveState s) { return s == jc2::MoveState::Landing; }
    // From the state update hook: replaces the player's output velocity while core drives.
    void OverrideOutput(RE::hkpCharacterContext& ctx, RE::hkpCharacterOutput& out) const;
    // Before the player's update: keep the controller in air while core drives.
    void HoldInAir(RE::PlayerCharacter* player) const;
    // From the in-air Change hook: true keeps the player's controller in air (core drives).
    bool HoldsInAir(const RE::hkpCharacterContext& ctx) const;
    // A skydive that hits the ground: deals Skyrim's fall damage for the height fallen (meters,
    // from the highest point of the fall), times mult, and ragdolls the player along its motion
    // (with ragdoll off, only when the fall kills).
    void HardLanding(jc2::Vec3 vel, float dropMeters, float mult, bool ragdoll);
    // The controller's velocity at the start of the frame (diagnostics).
    jc2::Vec3 ControllerVel() const { return controllerVel_; }

private:
    RE::PlayerCharacter* player_ = nullptr;
    RE::bhkCharacterController* controller_ = nullptr;

    jc2::Vec3 pos_;  // body center
    jc2::Vec3 vel_;
    jc2::Vec3 controllerVel_;
    jc2::Vec3 halfExtents_{0.35f, 0.9f, 0.35f};
    bool grounded_ = false;
    bool inWater_ = false;
    // What the controller was handed last frame (core axes) and in which state, for what the
    // physics step took off it (CollisionLoss).
    jc2::Vec3 lastCmd_;
    jc2::MoveState lastState_ = jc2::MoveState::OnFoot;
    void CollisionLoss();
    bool CheckWater(RE::PlayerCharacter* player) const;
    jc2::InputState input_;
    jc2::CameraState camera_;

    bool driving_ = false;
    bool groundDriving_ = false;
    RE::hkVector4 driveVel_;  // core's velocity in Havok space, read by the state update hook
    float savedGravity_ = 1.0f;
    static constexpr float kMaxSpeed = 150.0f;  // m/s, a safety cap on what goes to the controller
    static constexpr float kMinLoss = 0.5f;     // m/s taken off by a collision before core keeps it
    // Fall damage is off while core drives and for kFallDamageGrace seconds after.
    static constexpr float kFallDamageGrace = 1.5f;
    void SetFallDamage(bool on);
    float frameDelta_ = 0.0f;
    float noFallDamage_ = 0.0f;
    bool fallDamageOff_ = false;
    float savedFallMin_ = 600.0f;
    RE::hkpCharacterStateType lastHavokState_ = RE::hkpCharacterStateType::kOnGround;

    // Grapple anchors on things that move (RaycastAnchor): the hit point in the hit node's frame,
    // and the rigid body when the object is loose (pullable). The newest kMaxAnchors are kept.
    struct Anchor {
        std::uint32_t id = 0;
        RE::NiPointer<RE::NiAVObject> node;
        RE::NiPoint3 local;  // in node's frame, game units
        RE::ObjectRefHandle ref;
        RE::NiPointer<RE::bhkRigidBody> body;  // pullable only
    };
    static constexpr std::size_t kMaxAnchors = 16;
    std::vector<Anchor> anchors_;
    std::uint32_t nextAnchor_ = 1;
    const Anchor* FindAnchor(std::uint32_t id) const;
    // Raycast, also returning what it hit.
    bool Pick(jc2::Vec3 from, jc2::Vec3 dir, float maxDist, jc2::HitInfo& out, const RE::hkpCollidable** hit,
              RE::COL_LAYER layer) const;
};
