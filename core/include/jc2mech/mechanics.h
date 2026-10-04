#pragma once

#include <algorithm>
#include <cstdint>

#include "jc2mech/host.h"

namespace jc2 {

enum class MoveState : std::uint8_t {
    OnFoot,
    Falling,
    Grappling,    // reeling in toward the anchor
    Parachuting,
    WallCling,    // arrived on a wall (CReeledInController)
    Skydive,      // JC2's base jump pose (CSkyDiveController); the only free fall the chute opens from
    CeilingHang,  // arrived on a ceiling (CReeledHangController)
    WallJump,     // pushing off a wall (REELED_IN_JUMP, grpl_reel_bounce)
    WallDrop,     // letting go of a wall (REELED_IN_RELEASE_DROP, grpl_reeled_release)
    Landing,      // the chute landing roll (land_chute_roll)
};

const char* ToString(MoveState s);

// JC2 values (Steam JustCause2.exe, image base 0x400000). "dev+X" is the grapple device
// [[char+0x484]+0xD0]; addresses are the static floats the code reads. Fields marked APPROX are
// not recovered yet.
struct Tuning {
    float tickHz = 75.0f;  // JC2 runs 2 substeps per frame, dt ~0.0129..0.0137 s; sandbox only
    float gravity = 9.82f;  // 00e48bc0

    // Grapple (reel physics 00515cb0)
    float grappleRange = 120.0f;       // dev+0xD8
    float reelStartSpeed = 11.0f;      // dev+0x38
    // Hook flight, fitted to the F presses in session4/5 (fire -> anchor at 0.19-0.24 s +
    // distance / 264-280 m/s, max residual 35 ms): the arm's wind-up, then the hook's speed.
    // APPROX: from the logs, not from CGrapplingHook's code.
    float hookWindup = 0.2f;
    float hookSpeed = 270.0f;
    float reelGroundDelay = 0.28f;     // anchor -> reel start when fired from the ground (session4)
    float reelAccelRate = 3.5f;        // 0106d8ac: speed *= 1 + rate * dt
    float reelMaxSpeed = 40.0f;        // dev+0xE0
    float floorMinNormalY = 0.6433f;   // dev+0xF4: anchor normal.y >= this is a floor
    float ceilingMaxNormalY = -0.7668f;  // dev+0xF0: <= this is a ceiling, between is a wall
    float reelStallTime = 0.35f;       // dev+0x28 (APPROX use: give up when blocked this long)

    // Leaving a wall (session5 Space x3, session4 Ctrl; JC2 moves Rico by the clips' root motion,
    // these are fits to it). W/A/S/D do nothing on a wall in JC2 (session5: D held, no motion; a
    // jump with D held is the same as without). The jump: 0.2 s crouch in place, then ~11 m/s
    // straight out from the wall (sinking ~2 m/s) until the skydive takes over at 0.45 s.
    float wallJumpCrouch = 0.2f;
    float wallJumpTime = 0.45f;
    float wallJumpOut = 11.0f;
    float wallJumpSink = 2.0f;
    // The drop: 0.3 s sliding ~6 m/s down and ~2 m/s out (grpl_reeled_release), then a fall.
    float wallDropTime = 0.3f;
    float wallDropDown = 6.0f;
    float wallDropOut = 2.0f;

    // Chute landing roll (land_chute_roll, 0.97 s): APPROX, the horizontal speed runs out over it.
    float landRollTime = 0.97f;

    // Skydive (CSkyDiveController 00619510). The W/S and A/D smoothing is JC2's (00517090,
    // 007687c0); the speeds it maps to are APPROX until a skydive recording is fitted.
    float skydiveDelay = 0.6f;         // JC2 logs: CBaseJump -> CSkyDive ~0.64 s after leaving a ledge
    float skydiveMinHeight = 8.0f;     // APPROX: only with this much air below
    float diveInputRate = 1.5f;        // 00e55cd8: global W/S axis follows the keys
    float diveRate = 4.0f;             // 00e251b4: [char+0xA34]+0x148 follows the axis
    float steerInputRate = 4.0f;       // 00e251b4
    float steerRate = 3.0f;            // 00e23ad4: [char+0xA34]+0x14C
    // Dive blend 0 (W) / 0.5 (none) / 1 (S) -> target speeds, m/s. APPROX.
    float skydiveHoriz[3] = {34.0f, 22.0f, 12.0f};
    float skydiveFall[3] = {48.0f, 32.0f, 20.0f};  // terminal fall speeds (quadratic drag)
    float skydiveHorizAccel = 6.0f;    // APPROX: m/s^2 toward the target horizontal speed
    float skydiveTurnRate = 1.4f;      // APPROX: rad/s at full A/D

    // Parachute (CParachuteController update 005a9950)
    float chuteMass = 90.0f;           // 010776cc = 1/90
    float chuteAirOffset = 3.5f;       // 010776f4, squared: q = |v + 12.25 * forward|^2
    float chuteDragScale = 3.0f;       // 010776f0
    float chuteSideDragScale = 10.0f;  // 010776ec
    float chuteLiftScale = 1.7f;       // 010776e8
    float chuteThrust = 1.0f;          // 010776e4
    float chuteDiveThrust = 400.0f;    // 010776e0, when airflow.R >= 0.7 (00e30748)
    float chuteOpenMaxFall = 30.0f;    // 010776fc (-30)
    float chuteOpenConvert = 0.4f;     // 010776f8: share of fall speed turned horizontal on open
    float pitchGain = 1.2f, headGain = 1.0f, bankGain = 1.2f;            // 011fcedc/e0/e4
    float pitchInput = 0.65f, headInput = 0.75f, bankInput = 0.65f;      // 011fcef8/fc/f00
    float dampMin = 0.2f, dampMax = 1.8f;                                // 011fcef0 / 011fcee8
    float headDamp = 0.95f;                                              // 010776bc

    // Opening the chute mid-reel (CDeployParachuteWhileReelingAction 005819e0): the speed is
    // split into horizontal and upward parts, each clamp(share * speed (+ rise), min, max).
    float deployShare = 0.5f;          // 01079a4c
    float deployMinSpeed = 28.0f;      // 01079a48
    float deployMaxSpeed = 50.0f;      // 01079a44

    // Grapple while parachuting (tether branch of 005a9950, CReelInAction check 005c2df0).
    float tetherGain = 50.0f;          // dev+0xF8: pull force = gain * rope length (N)
    float tetherMaxSpeed = 40.0f;      // dev+0xFC
    float tetherPullTurn = 1.4f;       // 010776dc: heading pull toward the anchor
    float tetherReelSpeed = 1.0f;      // dev+0x40: at or below this speed the chute closes and reels
    // The rope drops when its direction leaves a cone around the forward axis of the skeleton
    // root bone (005ef1f0 -> 0082cbe0, chute limits 0106ce6c..78). Half-angles in degrees for
    // the rope pointing right / left / up / down; in between they blend by direction.
    float releaseRight = 65.0f, releaseLeft = 65.0f, releaseUp = 70.0f, releaseDown = 85.0f;
    // Fitted to the 16 session4/5 chute drops (animation pose, not in the exe): the root bone is
    // tilted ~5.8 deg from the chute frame and the rope is measured from this far up it.
    float releaseOriginUp = 0.38f;

    // Pulling loose objects (host extension, not JC2's code; APPROX): the hook on a pullable
    // object drags it toward the player; held, then let go aimed elsewhere, it pulls the two
    // together (JC2's tether).
    float pullSpeed = 18.0f;         // m/s
    float pullTime = 1.5f;           // s, at most
    float pullStopDistance = 2.0f;   // m from the player
    float linkTime = 3.0f;           // s, at most
    float linkStopDistance = 1.0f;   // m apart
    float pullHoldTime = 10.0f;      // s the rope stays held before it pulls to the player

    // Host options (not JC2): either mechanic can be turned off; the skydive stays.
    bool grappleEnabled = true;
    bool chuteEnabled = true;
};

struct GrappleState {
    bool attached = false;
    Vec3 anchor;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float speed = 0.0f;      // reel speed, dev+0x18
    float stallTimer = 0.0f;
    float bestDist = 0.0f;   // closest the reel has come to its target (stall check)
    float tetherTimer = 0.0f;  // time on the tether under the chute (diagnostic)
    bool justArrived = false;  // the tick after an arrival starts from rest
    float startDelay = 0.0f;   // ground start: the reel waits this long after the hook lands
    std::uint32_t anchorId = 0;  // host anchor the rope follows (0 = static)
};

// The hook in flight (fired, not yet hit). The rope attaches when it lands.
struct HookState {
    bool flying = false;
    Vec3 from, anchor;   // fired from the body position toward the anchor
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float time = 0.0f;        // since the fire press
    float flightTime = 0.0f;  // wind-up + distance / speed
    std::uint32_t anchorId = 0;  // host anchor the hook follows (0 = static)
    bool pullable = false;       // lands on a loose object: pulls it instead of reeling
    // 0 while the arm winds up, then 0..1 along from -> anchor.
    float Travel(const Tuning& t) const {
        const float fly = flightTime - t.hookWindup;
        return fly > 0.0f ? std::clamp((time - t.hookWindup) / fly, 0.0f, 1.0f) : 1.0f;
    }
};

// Parachute state, [[char+0xA34]+0x170]: angles +0x30/34/38, rates +0x3C/40/44.
struct ChuteState {
    float pitch = 0.0f, heading = 0.0f, bank = 0.0f;
    float pitchRate = 0.0f, headRate = 0.0f, bankRate = 0.0f;
    // Debug outputs of the last tick.
    float q = 0.0f, dragAngle = 0.0f, liftAngle = 0.0f;
    Vec3 lift, drag;
};

// [char+0xA34] +0x148 / +0x14C and their smoothed key axes (statics 01077c3c / 011fd0ac).
struct SkydiveState {
    float dive = 0.5f;         // 0 = W (head down), 0.5 = neutral, 1 = S (flat, slow)
    float diveInput = 0.5f;    // persists between skydives like JC2's static
    float steer = 0.0f;        // -1..1, + = left
    float steerInput = 0.0f;
    float heading = 0.0f;      // body heading, atan2(-fwd.x, -fwd.z) like the chute
    float airTime = 0.0f;      // time in Falling (the base jump delay)
};

// On a wall or ceiling after a reel, and the short moves off it (WallJump, WallDrop). Also the
// chute landing roll's clock.
struct ClingState {
    Vec3 anchor;                     // where the reel arrived (JC2 puts the character origin here)
    Vec3 normal{0.0f, 1.0f, 0.0f};   // surface normal
    float time = 0.0f;               // in the current state
    Vec3 rollVel;                    // Landing: horizontal speed at touchdown
    std::uint32_t anchorId = 0;      // a moving surface: the body moves with it
};

// A loose object on the hook (Tuning::pull*). Held: the rope stays taut while the grapple key is
// held; let go aimed at another surface, it links the two (Link), else the object comes to the
// player (ToPlayer).
struct PullState {
    enum class Phase : std::uint8_t { None, Held, ToPlayer, Link };
    Phase phase = Phase::None;
    std::uint32_t a = 0, b = 0;  // anchors (b = 0: the static world)
    Vec3 pointA, pointB;
    bool bPullable = false;
    float time = 0.0f;
};

enum class SurfaceClass : std::uint8_t { Floor, Wall, Ceiling };

class Mechanics {
public:
    explicit Mechanics(const Tuning& t = {}) : tuning_(t) {}

    void Tick(IHost& host, float dt);

    void Reset();

    // Replay only: start a tick from a recorded JC2 state.
    void Seed(MoveState s, const GrappleState& g, const ChuteState& c = {}, const SkydiveState& d = {}) {
        state_ = s;
        grapple_ = g;
        chute_ = c;
        skydive_ = d;
    }

    MoveState State() const { return state_; }
    const GrappleState& Grapple() const { return grapple_; }
    const HookState& Hook() const { return hook_; }
    const ChuteState& Chute() const { return chute_; }
    const SkydiveState& Skydive() const { return skydive_; }
    const ClingState& Cling() const { return cling_; }
    const PullState& Pull() const { return pull_; }
    SurfaceClass Classify(Vec3 normal) const;
    Tuning& GetTuning() { return tuning_; }
    const Tuning& GetTuning() const { return tuning_; }

private:
    bool TryFireGrapple(IHost& host);
    bool TickHook(const InputState& in, float dt);  // true on the tick the hook lands (rope attached)
    Vec3 TrackAnchors(const IHost& host, float dt);  // returns the cling surface's velocity
    void TickPull(IHost& host, const InputState& in, Vec3 pos, float dt);
    void Detach() { grapple_.attached = false; }
    Vec3 WallOut() const;  // level direction out of the cling surface
    void StartReel();
    bool RopeLeftCone(Vec3 pos, Vec3 vel, float dt) const;
    void OpenChute(Vec3& vel);
    Vec3 TickChute(const InputState& in, Vec3 pos, Vec3 vel, float dt);
    bool CanSkydive(const IHost& host, Vec3 pos) const;
    void EnterSkydive(Vec3 vel);
    Vec3 TickSkydive(const InputState& in, Vec3 vel, float dt);
    Vec3 TickReel(const IHost& host, Vec3 pos, float dt, bool& arrived);

    Tuning tuning_;
    MoveState state_ = MoveState::OnFoot;
    GrappleState grapple_;
    HookState hook_;
    ChuteState chute_;
    SkydiveState skydive_;
    ClingState cling_;
    PullState pull_;
    // Body forward (the character matrix's -U). The chute takes its starting angles from it.
    Vec3 bodyFwd_{0.0f, 0.0f, 1.0f};
};

// Rows of the chute's orientation (row-vector convention, world = local * M): N right, R up,
// U back (forward is -U). JC2 builds it as Z(bank) * X(pitch) * Y(heading) with its own
// approximate sin/cos.
struct ChuteFrame {
    Vec3 n, r, u;
};
ChuteFrame MakeChuteFrame(float pitch, float heading, float bank);

// JC2's parabolic sin/cos approximation (004dbce0 and friends).
void ApproxSinCos(float a, float& s, float& c);

// Runs a fixed-rate simulation inside a variable frame rate.
class FixedStepper {
public:
    // Returns how many fixed steps to run this frame.
    int Advance(float frameDt, float stepDt, int maxSteps = 8) {
        accumulator_ += frameDt;
        int steps = 0;
        while (accumulator_ >= stepDt && steps < maxSteps) {
            accumulator_ -= stepDt;
            ++steps;
        }
        if (steps == maxSteps) accumulator_ = 0.0f;  // drop time rather than spiral
        return steps;
    }

    float Alpha(float stepDt) const { return accumulator_ / stepDt; }

private:
    float accumulator_ = 0.0f;
};

} // namespace jc2
