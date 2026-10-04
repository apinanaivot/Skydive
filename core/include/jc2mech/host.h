#pragma once

#include <cstdint>

#include "jc2mech/math.h"

namespace jc2 {

struct HitInfo {
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
    // Filled by RaycastAnchor only (host extension, not JC2): an id for a hit on something that
    // moves (0 = the static world), and whether the grapple pulls it in (a loose object).
    std::uint32_t anchor = 0;
    bool pullable = false;
};

// Sampled once per fixed tick. "Pressed" flags are edge events: hosts must latch
// them until the next tick consumes them, or presses between ticks get lost.
struct InputState {
    float moveX = 0.0f;  // strafe, -1..1
    float moveY = 0.0f;  // forward, -1..1
    bool grapplePressed = false;
    bool grappleHeld = false;
    bool parachutePressed = false;
    bool jumpPressed = false;
    bool dropPressed = false;  // let go of a wall (Ctrl in JC2)
};

struct CameraState {
    Vec3 position;
    Vec3 forward{0.0f, 0.0f, 1.0f};
    float fovDeg = 70.0f;
};

// The only way core talks to an engine. Implemented by sandbox, replay and skse.
class IHost {
public:
    virtual ~IHost() = default;

    virtual bool Raycast(Vec3 from, Vec3 dir, float maxDist, HitInfo& out) const = 0;
    // The grapple's ray: also tags hits on moving things (HitInfo::anchor, pullable).
    virtual bool RaycastAnchor(Vec3 from, Vec3 dir, float maxDist, HitInfo& out) { return Raycast(from, dir, maxDist, out); }
    // Where the point hit as anchor `id` is now (it moves with its object); false once it's gone.
    virtual bool AnchorPoint(std::uint32_t id, Vec3& out) const {
        (void)id;
        (void)out;
        return false;
    }
    // Moves anchor id's object toward `to` at `speed` m/s (0 stops it).
    virtual void PullAnchor(std::uint32_t id, Vec3 to, float speed) {
        (void)id;
        (void)to;
        (void)speed;
    }

    virtual Vec3 GetPos() const = 0;
    virtual Vec3 GetVel() const = 0;
    virtual void SetVel(Vec3 v) = 0;
    virtual bool IsGrounded() const = 0;
    // The body has reached water (host extension, not JC2): ends the skydive and the chute, and
    // neither starts in it.
    virtual bool InWater() const { return false; }
    // Half size of the body; the reel stops this far off the anchor surface.
    virtual Vec3 GetHalfExtents() const { return {0.35f, 0.9f, 0.35f}; }

    virtual InputState GetInput() const = 0;
    virtual CameraState GetCamera() const = 0;
    virtual void SetCamera(const CameraState& c) = 0;
};

} // namespace jc2
