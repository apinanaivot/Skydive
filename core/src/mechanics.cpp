#include "jc2mech/mechanics.h"

#include <algorithm>
#include <cmath>

// Parachute free glide, the tether under the chute (pull, steering and when the rope drops),
// opening the chute mid-reel and grapple reel-in follow JC2 and were checked tick by tick against
// recorded JC2 runs (see docs/progress.md). Arrival behaviours beyond the state change and wall
// jumps are approximations, marked APPROX.

namespace jc2 {

namespace {

constexpr float kPi = 3.14159274f;
constexpr float kTwoPi = 6.28318548f;
constexpr float kHalfPi = 1.57079637f;
constexpr float kInvPi = 0.318309873f;
constexpr float kRadToDeg = 57.2957802f;

float Wrap(float a) {
    while (a < -kPi) a += kTwoPi;
    while (a > kPi) a -= kTwoPi;
    return a;
}

// Piecewise-linear lookup over [x[i], x[i+1]).
template <int N>
float Lookup(const float (&x)[N], const float (&y)[N], float v, float outside) {
    for (int i = 0; i + 1 < N; ++i) {
        if (x[i] <= v && v < x[i + 1]) return y[i] - (y[i] - y[i + 1]) * (x[i] - v) / (x[i] - x[i + 1]);
    }
    return outside;
}

// Canopy drag coefficient by angle between airflow and the canopy's back axis, degrees (00843900).
float DragCoefficient(float deg) {
    static const float x[] = {0, 4, 8, 12, 16, 20, 24, 90, 180};
    static const float y[] = {0.032f, 0.052f, 0.0904f, 0.124f, 0.194f, 0.25f, 0.328f, 0.8f, 1.0f};
    return Lookup(x, y, std::fabs(deg), 0.0f);
}

// Lift coefficient by angle of attack, degrees (0056fca0).
float LiftCoefficient(float deg) {
    static const float x[] = {-8, -4, 0, 4, 8, 12, 16, 20, 54, 80, 180};
    static const float y[] = {-0.54f, -0.2f, 0.2f, 0.57f, 0.92f, 1.21f, 1.43f, 1.4f, 1.0f, 0.0f, 0.0f};
    return Lookup(x, y, deg, y[0]);
}

float Parabola(float p) {
    const float s = p < 0.0f ? -1.0f : 1.0f;
    const float y = (1.0f - kInvPi * s * p) * p * kInvPi * 4.0f;
    return (y * y * s - y) * 0.225f + y;
}

// Skeleton root bone rows in the chute frame (fitted, see Tuning::releaseOriginUp).
constexpr float kRootBone[3][3] = {
    {0.99902f, 0.04209f, -0.01352f},
    {-0.04069f, 0.99503f, 0.09092f},
    {0.01728f, -0.09028f, 0.99577f},
};

// world = local.x * n + local.y * r + local.z * u
Vec3 ToWorld(const ChuteFrame& f, Vec3 l) { return f.n * l.x + f.r * l.y + f.u * l.z; }
Vec3 ToLocal(const ChuteFrame& f, Vec3 w) { return {Dot(w, f.n), Dot(w, f.r), Dot(w, f.u)}; }

}  // namespace

void ApproxSinCos(float a, float& s, float& c) {
    float p = a - static_cast<float>(static_cast<int>(kInvPi * a * 0.5f)) * kTwoPi;
    if (kPi - p < 0.0f) p -= kTwoPi;
    if (p + kPi < 0.0f) p += kTwoPi;
    s = Parabola(p);
    float p2 = kHalfPi + p;
    if (kPi - p2 < 0.0f) p2 -= kTwoPi;
    c = Parabola(p2);
}

ChuteFrame MakeChuteFrame(float pitch, float heading, float bank) {
    float sp, cp, sh, ch, sb, cb;
    ApproxSinCos(pitch, sp, cp);
    ApproxSinCos(heading, sh, ch);
    ApproxSinCos(bank, sb, cb);
    // X(pitch) * Y(heading)
    const Vec3 xy0{ch, 0.0f, -sh};
    const Vec3 xy1{sp * sh, cp, sp * ch};
    const Vec3 xy2{cp * sh, -sp, cp * ch};
    // Z(bank) * that
    const Vec3 r0 = xy0 * cb + xy1 * sb, r1 = xy0 * -sb + xy1 * cb, r2 = xy2;

    // JC2 stores the orientation as a normalized quaternion, which also re-orthonormalizes the
    // slightly skewed matrix from the approximate sin/cos. Matrix -> quaternion:
    float qx, qy, qz, qw;
    const float trace = r0.x + r1.y + r2.z + 1.0f;
    if (trace > 1e-4f) {
        const float k = 0.5f / std::sqrt(trace);
        qw = 0.25f / k;
        qx = (r1.z - r2.y) * k;
        qy = (r2.x - r0.z) * k;
        qz = (r0.y - r1.x) * k;
    } else if (r1.y < r0.x && r2.z < r0.x) {
        const float s = std::sqrt(r0.x - r1.y - r2.z + 1.0f) * 2.0f, k = 1.0f / s;
        qx = s * 0.25f;
        qy = (r1.x + r0.y) * k;
        qz = (r2.x + r0.z) * k;
        qw = (r1.z - r2.y) * k;
    } else if (r2.z < r1.y) {
        const float s = std::sqrt(r1.y + 1.0f - r0.x - r2.z) * 2.0f, k = 1.0f / s;
        qy = s * 0.25f;
        qx = (r1.x + r0.y) * k;
        qz = (r2.y + r1.z) * k;
        qw = (r2.x - r0.z) * k;
    } else {
        const float s = std::sqrt(r2.z + 1.0f - r0.x - r1.y) * 2.0f, k = 1.0f / s;
        qz = s * 0.25f;
        qx = (r2.x + r0.z) * k;
        qy = (r2.y + r1.z) * k;
        qw = (r0.y - r1.x) * k;
    }
    const float len2 = qx * qx + qy * qy + qz * qz + qw * qw;
    if (len2 > 0.0f) {
        const float inv = 1.0f / std::sqrt(len2);
        qx *= inv; qy *= inv; qz *= inv; qw *= inv;
    }

    // ... and back.
    const float z2 = qz * 2.0f, zz = qz * z2, y2 = qy * 2.0f, wx = qw * qx * 2.0f, xx = qx * qx * 2.0f;
    return {
        {1.0f - (zz + qy * y2), qw * z2 + qx * y2, qx * z2 - qw * y2},
        {qx * y2 - qw * z2, 1.0f - (zz + xx), wx + qy * z2},
        {qw * y2 + qx * z2, qy * z2 - wx, 1.0f - (qy * y2 + xx)},
    };
}

const char* ToString(MoveState s) {
    switch (s) {
        case MoveState::OnFoot: return "OnFoot";
        case MoveState::Falling: return "Falling";
        case MoveState::Grappling: return "Grappling";
        case MoveState::Parachuting: return "Parachuting";
        case MoveState::WallCling: return "WallCling";
        case MoveState::Skydive: return "Skydive";
        case MoveState::CeilingHang: return "CeilingHang";
        case MoveState::WallJump: return "WallJump";
        case MoveState::WallDrop: return "WallDrop";
        case MoveState::Landing: return "Landing";
    }
    return "?";
}

void Mechanics::Reset() {
    state_ = MoveState::OnFoot;
    grapple_ = {};
    hook_ = {};
    chute_ = {};
    skydive_ = {};
    cling_ = {};
    pull_ = {};
    bodyFwd_ = {0.0f, 0.0f, 1.0f};
}

SurfaceClass Mechanics::Classify(Vec3 normal) const {
    const float y = std::clamp(normal.y, -1.0f, 1.0f);
    if (y >= tuning_.floorMinNormalY) return SurfaceClass::Floor;
    if (y <= tuning_.ceilingMaxNormalY) return SurfaceClass::Ceiling;
    return SurfaceClass::Wall;
}

bool Mechanics::TryFireGrapple(IHost& host) {
    // The hook flies first (HookState); the rope attaches when it lands (Tick).
    const CameraState cam = host.GetCamera();
    HitInfo hit;
    if (!host.RaycastAnchor(cam.position, cam.forward, tuning_.grappleRange, hit)) return false;
    // A new shot lets go of an object on the hook.
    if (pull_.phase != PullState::Phase::None) host.PullAnchor(pull_.a, pull_.pointA, 0.0f);
    pull_ = {};
    hook_ = {};
    hook_.anchorId = hit.anchor;
    hook_.pullable = hit.pullable;
    hook_.flying = true;
    hook_.from = host.GetPos();
    hook_.anchor = hit.point;
    hook_.normal = Normalize(hit.normal);
    hook_.flightTime = tuning_.hookWindup + Length(hit.point - hook_.from) / tuning_.hookSpeed;
    return true;
}

bool Mechanics::TickHook(const InputState& in, float dt) {
    if (!hook_.flying) return false;
    hook_.time += dt;
    if (hook_.time < hook_.flightTime) return false;
    hook_.flying = false;
    if (hook_.pullable) {
        // A loose object: it comes to the player (or waits for a second target while held).
        pull_ = {};
        pull_.phase = in.grappleHeld ? PullState::Phase::Held : PullState::Phase::ToPlayer;
        pull_.a = hook_.anchorId;
        pull_.pointA = hook_.anchor;
        return false;
    }
    grapple_.attached = true;
    grapple_.anchorId = hook_.anchorId;
    grapple_.anchor = hook_.anchor;
    grapple_.normal = hook_.normal;
    grapple_.tetherTimer = 0.0f;
    grapple_.justArrived = false;
    grapple_.startDelay = 0.0f;
    StartReel();
    return true;
}

Vec3 Mechanics::TrackAnchors(const IHost& host, float dt) {
    // Anchors on moving things follow them; a vanished one drops the hook or rope.
    auto track = [&](std::uint32_t& id, Vec3& point) {
        if (id == 0) return true;
        Vec3 now;
        if (!host.AnchorPoint(id, now)) {
            id = 0;
            return false;
        }
        point = now;
        return true;
    };
    if (hook_.flying && !track(hook_.anchorId, hook_.anchor)) hook_.flying = false;
    if (grapple_.attached && !track(grapple_.anchorId, grapple_.anchor)) Detach();
    const bool cling = state_ == MoveState::WallCling || state_ == MoveState::CeilingHang ||
                       state_ == MoveState::WallJump || state_ == MoveState::WallDrop;
    if (!cling || cling_.anchorId == 0 || dt <= 0.0f) return {};
    const Vec3 before = cling_.anchor;
    return track(cling_.anchorId, cling_.anchor) ? (cling_.anchor - before) * (1.0f / dt) : Vec3{};
}

void Mechanics::TickPull(IHost& host, const InputState& in, Vec3 pos, float dt) {
    const Tuning& t = tuning_;
    PullState& p = pull_;
    if (p.phase == PullState::Phase::None) return;
    p.time += dt;
    if (!host.AnchorPoint(p.a, p.pointA)) {
        p = {};
        return;
    }
    switch (p.phase) {
        case PullState::Phase::Held: {
            const bool timeout = p.time > t.pullHoldTime;
            if (in.grappleHeld && !timeout) break;
            // Let go aimed at another surface: the two are pulled together; else to the player.
            p.time = 0.0f;
            p.phase = PullState::Phase::ToPlayer;
            const CameraState cam = host.GetCamera();
            HitInfo hit;
            if (!timeout && host.RaycastAnchor(cam.position, cam.forward, t.grappleRange, hit) &&
                Length(hit.point - p.pointA) > 2.0f * t.linkStopDistance) {
                p.phase = PullState::Phase::Link;
                p.b = hit.anchor;
                p.pointB = hit.point;
                p.bPullable = hit.pullable;
            }
            break;
        }
        case PullState::Phase::ToPlayer:
            if (Length(pos - p.pointA) < t.pullStopDistance || p.time > t.pullTime) {
                host.PullAnchor(p.a, pos, 0.0f);
                p = {};
            } else {
                host.PullAnchor(p.a, pos, t.pullSpeed);
            }
            break;
        case PullState::Phase::Link: {
            if (p.b != 0 && !host.AnchorPoint(p.b, p.pointB)) {
                p = {};
                break;
            }
            const bool done = Length(p.pointA - p.pointB) < t.linkStopDistance || p.time > t.linkTime;
            host.PullAnchor(p.a, p.pointB, done ? 0.0f : t.pullSpeed);
            if (p.bPullable) host.PullAnchor(p.b, p.pointA, done ? 0.0f : t.pullSpeed);
            if (done) p = {};
            break;
        }
        case PullState::Phase::None: break;
    }
}

Vec3 Mechanics::WallOut() const {
    // Straight out from the wall, level.
    const Vec3 h = Horizontal(cling_.normal);
    return Dot(h, h) > 1e-4f ? Normalize(h) : -bodyFwd_;
}

bool Mechanics::RopeLeftCone(Vec3 pos, Vec3 vel, float dt) const {
    const Tuning& t = tuning_;
    // Bone frame: the fitted root bone rotation applied to the canopy frame.
    const ChuteFrame f = MakeChuteFrame(chute_.pitch, chute_.heading, chute_.bank);
    const ChuteFrame b{ToWorld(f, {kRootBone[0][0], kRootBone[0][1], kRootBone[0][2]}),
                       ToWorld(f, {kRootBone[1][0], kRootBone[1][1], kRootBone[1][2]}),
                       ToWorld(f, {kRootBone[2][0], kRootBone[2][1], kRootBone[2][2]})};
    // JC2 tests where the rope will point next tick.
    const Vec3 origin = pos + b.r * t.releaseOriginUp + vel * dt;
    const Vec3 d = ToLocal(b, Normalize(grapple_.anchor - origin));
    const float r2 = d.x * d.x + d.y * d.y;
    if (r2 <= 0.0f) return true;
    // -cos of each half-angle, with JC2's approximate cosine; blended by the rope's direction
    // around the axis. Inside the cone the rope points forward (-U), so z stays below it.
    auto negCos = [](float deg) {
        float sn, cs;
        ApproxSinCos((180.0f + deg) * (kPi / 180.0f), sn, cs);
        return cs;
    };
    const float fx = negCos(d.x > 0.0f ? t.releaseRight : t.releaseLeft);
    const float fy = negCos(d.y > 0.0f ? t.releaseUp : t.releaseDown);
    const float limit = (1.0f - d.x * d.x / r2) * fy + (1.0f - d.y * d.y / r2) * fx;
    return !(d.z < limit);
}

void Mechanics::StartReel() {
    // Every reel starts at dev+0x38, also from the air or out of a tether.
    grapple_.speed = tuning_.reelStartSpeed;
    grapple_.stallTimer = 0.0f;
    grapple_.bestDist = 1e9f;
}

void Mechanics::OpenChute(Vec3& vel) {
    const Tuning& t = tuning_;
    // JC2 takes pitch and heading from the character's transform (004de960) and zeroes the
    // rates. Mid-reel the body points along the rope, so the canopy starts pitched up.
    chute_ = {};
    chute_.pitch = std::asin(std::clamp(bodyFwd_.y, -1.0f, 1.0f));
    chute_.heading = std::atan2(-bodyFwd_.x, -bodyFwd_.z);

    // Part of the fall speed (capped) turns into forward speed.
    if (vel.y < 0.0f) {
        const float capped = std::max(vel.y, -t.chuteOpenMaxFall);
        const Vec3 h = Normalize(Horizontal(vel));
        vel.x += h.x * -capped * t.chuteOpenConvert;
        vel.z += h.z * -capped * t.chuteOpenConvert;
        vel.y = (1.0f - t.chuteOpenConvert) * capped + (vel.y - capped);
    }
}

Vec3 Mechanics::TickChute(const InputState& in, Vec3 pos, Vec3 v, float dt) {
    const Tuning& t = tuning_;
    ChuteState& c = chute_;
    const ChuteFrame f = MakeChuteFrame(c.pitch, c.heading, c.bank);

    // Forward direction flattened to horizontal; used for the airflow offset and the clamp.
    const Vec3 fwd = -f.u;
    Vec3 fwdH = Horizontal(fwd);
    fwdH = Dot(fwdH, fwdH) > 0.0f ? Normalize(fwdH) : f.r;

    Vec3 force;
    c.q = 0.0f;
    c.lift = c.drag = {};
    if (Dot(v, v) > 0.0f) {
        const Vec3 air = v + fwdH * (t.chuteAirOffset * t.chuteAirOffset);
        c.q = Dot(air, air);
        const Vec3 a = Normalize(-v);  // airflow direction
        const float aR = Dot(a, f.r);
        c.dragAngle = std::acos(std::clamp(Dot(a, f.u), -1.0f, 1.0f)) * kRadToDeg;
        c.liftAngle = std::asin(std::clamp(aR, -1.0f, 1.0f)) * kRadToDeg;

        const Vec3 aSide = f.n * Dot(a, f.n);
        c.drag = (a - aSide) * (c.q * t.chuteDragScale * DragCoefficient(c.dragAngle)) +
                 aSide * (c.q * t.chuteSideDragScale);
        c.lift = Normalize(Cross(Cross(a, f.r), a)) * (c.q * t.chuteLiftScale * LiftCoefficient(c.liftAngle));
        const float thrust = aR >= 0.7f ? t.chuteDiveThrust * std::pow(aR, 16.0f) : t.chuteThrust;
        force = c.drag + c.lift + fwd * thrust;
    }

    Vec3 vn = v + force * (dt / t.chuteMass);
    vn.y -= t.gravity * dt;

    // Steering targets. `pull` is JC2's tether steering term (0 when free).
    float pull = 0.0f, pitchBias = 0.0f;
    if (grapple_.attached) {
        // Tether: a pull of gain * rope length toward the anchor, then a speed cap. It also
        // steers the canopy toward the anchor and pitches it up when the anchor is above.
        const Vec3 to = grapple_.anchor - pos;
        const float dist = Length(to);
        const Vec3 dir = Normalize(to);
        vn += dir * (t.tetherGain * dist * dt / t.chuteMass);
        const float s = Length(vn);
        if (s > t.tetherMaxSpeed) vn *= t.tetherMaxSpeed / s;
        pull = Wrap(std::atan2(-dir.x, -dir.z) - std::atan2(f.u.x, f.u.z)) * t.tetherPullTurn;
        pitchBias = std::clamp(1.0f + dir.y, 0.0f, 1.0f);
    }

    // No backwards horizontal motion.
    const float back = Dot(fwdH, vn);
    if (back < 0.0f) vn -= fwdH * back;

    // Angular update: damped rates driven toward input targets (lr = D - A, fb = W - S).
    const float lr = in.moveX, fb = in.moveY;
    const float ePitch = Wrap((pitchBias - fb) * t.pitchInput - c.pitch);
    const float eHead = Wrap((pull - lr) * t.headInput + std::atan2(f.u.x, f.u.z) - c.heading);
    const float eBank = Wrap((pull - lr) * t.bankInput - c.bank);
    const float span = t.dampMax - t.dampMin;
    const float kPitch = span * std::fabs(c.pitch) * (2.0f / kPi) + t.dampMin;
    const float kBank = span * std::fabs(c.bank) * (2.0f / kPi) + t.dampMin;
    c.pitchRate = c.pitchRate * std::exp(-kPitch * dt) + t.pitchGain * ePitch * dt;
    c.headRate = c.headRate * std::exp(-t.headDamp * dt) + t.headGain * eHead * dt;
    c.bankRate = c.bankRate * std::exp(-kBank * dt) + t.bankGain * eBank * dt;
    c.pitch += c.pitchRate * dt;
    c.heading += c.headRate * dt;
    c.bank += c.bankRate * dt;

    // JC2 stores the new velocity in the old canopy frame and reads it back with the new one,
    // so the velocity turns with the canopy. This is the steering.
    const ChuteFrame nf = MakeChuteFrame(c.pitch, c.heading, c.bank);
    return ToWorld(nf, ToLocal(f, vn));
}

bool Mechanics::CanSkydive(const IHost& host, Vec3 pos) const {
    HitInfo hit;
    return !host.Raycast(pos, {0.0f, -1.0f, 0.0f}, tuning_.skydiveMinHeight + host.GetHalfExtents().y, hit);
}

void Mechanics::EnterSkydive(Vec3 vel) {
    // JC2 keeps the smoothed key axes (statics) and restarts the pose blend.
    SkydiveState& s = skydive_;
    s.dive = 0.5f;
    s.steer = s.steerInput = 0.0f;
    const Vec3 h = Horizontal(vel);
    const Vec3 f = Dot(h, h) > 1.0f ? Normalize(h) : bodyFwd_;
    s.heading = std::atan2(-f.x, -f.z);
}

Vec3 Mechanics::TickSkydive(const InputState& in, Vec3 vel, float dt) {
    const Tuning& t = tuning_;
    SkydiveState& s = skydive_;
    // Key axes as in 00517090 / 007687c0: W/S map to 0..1 around 0.5, A/D to +-2, each smoothed
    // twice. moveY is W - S, moveX is D - A.
    s.diveInput += ((1.0f - in.moveY) * 0.5f - s.diveInput) * dt * t.diveInputRate;
    s.dive = std::clamp(s.dive + (s.diveInput - s.dive) * dt * t.diveRate, 0.0f, 1.0f);
    s.steerInput += (-2.0f * in.moveX - s.steerInput) * dt * t.steerInputRate;
    s.steer = std::clamp(s.steer + (s.steerInput - s.steer) * dt * t.steerRate, -1.0f, 1.0f);

    // APPROX from here: JC2 moves the skydiver with the blended animation's root motion.
    s.heading = Wrap(s.heading + s.steer * t.skydiveTurnRate * dt);
    const Vec3 fwd{-std::sin(s.heading), 0.0f, -std::cos(s.heading)};
    bodyFwd_ = fwd;
    auto blend = [&](const float (&v)[3]) {
        return s.dive < 0.5f ? v[0] + (v[1] - v[0]) * s.dive * 2.0f : v[1] + (v[2] - v[1]) * (s.dive - 0.5f) * 2.0f;
    };
    const Vec3 h = MoveTowards(Horizontal(vel), fwd * blend(t.skydiveHoriz), t.skydiveHorizAccel * dt);
    // Quadratic drag with the blend's terminal speed: falling faster than it slows down.
    const float r = vel.y / blend(t.skydiveFall);
    const float accel = vel.y < 0.0f ? t.gravity * (1.0f - r * r) : t.gravity * (1.0f + r * r);
    return {h.x, vel.y - accel * dt, h.z};
}

Vec3 Mechanics::TickReel(const IHost& host, Vec3 pos, float dt, bool& arrived) {
    const Tuning& t = tuning_;
    GrappleState& g = grapple_;
    arrived = false;

    // Aim so the body stops against the surface instead of at the anchor point itself.
    const Vec3 e = host.GetHalfExtents();
    const Vec3 n = g.normal;
    const float standoff = std::fabs(n.x) * e.x + std::fabs(n.y) * e.y + std::fabs(n.z) * e.z + 0.02f;
    const Vec3 to = g.anchor + n * standoff - pos;
    const float dist = Length(to);
    const Vec3 dir = Normalize(to);

    g.speed = std::min(g.speed * (1.0f + t.reelAccelRate * dt), t.reelMaxSpeed);

    // APPROX: give up when blocked (JC2 uses contact tests and timers in 00722cc0). Progress is
    // measured against the closest point so far: a host that advances its position between its
    // own physics steps (skse) moves the body a tick closer and back while it sits on a collider.
    if (dist < g.bestDist - 0.01f * g.speed * dt) {
        g.bestDist = dist;
        g.stallTimer = 0.0f;
    } else {
        g.stallTimer += dt;
    }

    if (dist - g.speed * dt < 0.0f) {
        arrived = true;
        return dt > 0.0f ? dir * (dist / dt) : Vec3{};
    }
    return dir * g.speed;
}

void Mechanics::Tick(IHost& host, float dt) {
    const Tuning& t = tuning_;
    const InputState in = host.GetInput();
    const bool grounded = host.IsGrounded();
    const bool water = host.InWater();
    const Vec3 pos = host.GetPos();
    Vec3 vel = host.GetVel();
    const Vec3 surfaceVel = TrackAnchors(host, dt);

    // Outside a reel the body stands upright and faces its horizontal motion (or the camera).
    if (state_ != MoveState::Grappling) {
        const Vec3 h = Horizontal(vel);
        bodyFwd_ = Dot(h, h) > 1.0f ? Normalize(h) : Normalize(Horizontal(host.GetCamera().forward));
    }

    // Grapple button: fire (or re-fire mid-reel; the old rope holds until the new hook lands).
    // Under the chute the rope becomes a tether.
    if (in.grapplePressed && t.grappleEnabled) TryFireGrapple(host);
    TickPull(host, in, pos, dt);
    if (TickHook(in, dt)) {
        if (grounded) grapple_.startDelay = t.reelGroundDelay;
        if (state_ != MoveState::Parachuting) state_ = MoveState::Grappling;
    }

    // Leaving a rope or the chute in the air: skydive when there is room for it (a reel cut
    // mid-air goes straight to CSkyDiveController in the JC2 logs).
    auto fall = [&] {
        if (grounded) {
            state_ = MoveState::OnFoot;
        } else if (!water && CanSkydive(host, pos)) {
            EnterSkydive(vel);
            state_ = MoveState::Skydive;
        } else {
            state_ = MoveState::Falling;
        }
    };

    switch (state_) {
        case MoveState::OnFoot:
            skydive_.airTime = 0.0f;
            if (!grounded) state_ = MoveState::Falling;
            break;

        case MoveState::Falling:
            skydive_.airTime += dt;
            if (grounded) {
                state_ = MoveState::OnFoot;
                skydive_.airTime = 0.0f;
            } else if (skydive_.airTime >= t.skydiveDelay && !water && CanSkydive(host, pos)) {
                // Off a ledge (CBaseJumpController) into the skydive. No chute before this.
                EnterSkydive(vel);
                state_ = MoveState::Skydive;
            } else {
                vel.y -= t.gravity * dt;
            }
            break;

        case MoveState::Skydive:
            if (grounded || water) {
                state_ = MoveState::OnFoot;
                skydive_.airTime = 0.0f;
            } else if (in.parachutePressed && t.chuteEnabled) {
                OpenChute(vel);
                state_ = MoveState::Parachuting;
            } else {
                vel = TickSkydive(in, vel, dt);
            }
            break;

        case MoveState::Grappling: {
            if (in.dropPressed || !grapple_.attached) {
                Detach();
                fall();
                break;
            }
            if (in.parachutePressed && !grounded && !water && t.chuteEnabled) {
                // Deploying mid-reel (CDeployParachuteWhileReelingAction): the speed is split
                // into forward and upward parts of at least 28 m/s each, so Rico shoots up.
                Detach();
                const float speed = Length(vel);
                const float rise = std::max(vel.y, 0.0f);
                const Vec3 h = Normalize(Horizontal(vel));
                const float fwd = std::clamp(t.deployShare * speed, t.deployMinSpeed, t.deployMaxSpeed);
                const float up = std::clamp(t.deployShare * speed + rise, t.deployMinSpeed, t.deployMaxSpeed);
                vel = h * fwd + Vec3{0.0f, up, 0.0f};
                OpenChute(vel);
                state_ = MoveState::Parachuting;
                break;
            }
            if (grapple_.startDelay > 0.0f) {
                // Fired from the ground: Rico braces before the reel pulls (reel_in_start).
                grapple_.startDelay -= dt;
                if (grounded) vel = {};
                else vel.y -= t.gravity * dt;
                break;
            }
            bool arrived = false;
            vel = TickReel(host, pos, dt, arrived);
            bodyFwd_ = Normalize(vel);
            if (arrived) {
                // CReelInAction 006b8890: floor -> on foot, wall -> CReeledInController (cling),
                // ceiling -> CReeledHangController (session4 t=40.7, normal (0, -1, 0)).
                Detach();
                cling_ = {};
                cling_.anchor = grapple_.anchor;
                cling_.normal = grapple_.normal;
                cling_.anchorId = grapple_.anchorId;
                switch (Classify(grapple_.normal)) {
                    case SurfaceClass::Floor: state_ = MoveState::OnFoot; break;
                    case SurfaceClass::Wall: state_ = MoveState::WallCling; break;
                    case SurfaceClass::Ceiling: state_ = MoveState::CeilingHang; break;
                }
                grapple_.justArrived = true;
                host.SetVel(vel);  // this tick lands exactly on the target
                return;
            }
            if (grapple_.stallTimer > t.reelStallTime) {
                // Stuck on something short of the anchor: the rope lets go (the hook reels back).
                Detach();
                vel = {};
                fall();
            }
            break;
        }

        case MoveState::Parachuting:
            if (water) {
                // Into water: the chute is gone and the host's swimming takes over.
                Detach();
                state_ = MoveState::OnFoot;
                skydive_.airTime = 0.0f;
                break;
            }
            if (grounded) {
                // Touchdown: the landing roll carries the horizontal speed out.
                Detach();
                cling_.time = 0.0f;
                cling_.rollVel = Horizontal(vel);
                vel = cling_.rollVel;
                state_ = MoveState::Landing;
                break;
            }
            if (grapple_.attached && (in.parachutePressed || Length(vel) <= t.tetherReelSpeed)) {
                // Closing the chute on a tether reels in. JC2 also closes it by itself when the
                // tether has slowed Rico to dev+0x40 or less (005c2df0, static anchors).
                state_ = MoveState::Grappling;
                StartReel();
                break;
            }
            if (in.parachutePressed || in.dropPressed) {
                // APPROX: closing the chute high up goes back to the skydive, so it can reopen.
                Detach();
                fall();
                break;
            }
            if (grapple_.attached) grapple_.tetherTimer += dt;
            vel = TickChute(in, pos, vel, dt);
            // After the chute update JC2 drops the rope once it points out of the release cone
            // (00503aa0 -> 005ef1f0), e.g. when Rico is about to pass the anchor.
            if (grapple_.attached && RopeLeftCone(pos, vel, dt)) Detach();
            break;

        case MoveState::WallCling:
            // JC2: only jump (Space), drop (Ctrl) and the grapple act here; W/A/S/D do nothing.
            // On a moving surface the body moves with it.
            vel = surfaceVel;
            cling_.time += dt;
            if (in.dropPressed) {
                cling_.time = 0.0f;
                state_ = MoveState::WallDrop;
            } else if (in.parachutePressed || in.jumpPressed) {
                cling_.time = 0.0f;
                state_ = MoveState::WallJump;
            }
            break;

        case MoveState::CeilingHang:
            // CReeledHangController 007024d0: input action 0xD (Ctrl) drops (HANG_TO_FALL);
            // APPROX: Space drops too.
            vel = surfaceVel;
            cling_.time += dt;
            if (in.dropPressed || in.parachutePressed || in.jumpPressed) fall();
            break;

        case MoveState::WallJump: {
            cling_.time += dt;
            const Vec3 out = WallOut();
            if (cling_.time < t.wallJumpCrouch) {
                vel = surfaceVel;
            } else {
                vel = out * t.wallJumpOut - Vec3{0.0f, t.wallJumpSink, 0.0f};
                bodyFwd_ = out;  // Rico turns away from the wall as he jumps
                if (cling_.time >= t.wallJumpTime) fall();
            }
            break;
        }

        case MoveState::WallDrop:
            cling_.time += dt;
            vel = WallOut() * t.wallDropOut - Vec3{0.0f, t.wallDropDown, 0.0f};
            if (cling_.time >= t.wallDropTime) fall();
            break;

        case MoveState::Landing: {
            cling_.time += dt;
            if (!grounded) {
                fall();
                break;
            }
            const float k = std::max(0.0f, 1.0f - cling_.time / t.landRollTime);
            vel = cling_.rollVel * k;
            if (cling_.time >= t.landRollTime) state_ = MoveState::OnFoot;
            break;
        }
    }

    if (grapple_.justArrived) {
        // The tick after an arrival starts from rest.
        grapple_.justArrived = false;
        if (state_ != MoveState::Grappling) vel = state_ == MoveState::Falling ? Vec3{0.0f, -t.gravity * dt, 0.0f} : Vec3{};
    }

    host.SetVel(vel);
}

} // namespace jc2
