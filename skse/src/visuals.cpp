#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "animation.h"
#include "pose.h"

// The JC2 parachute on the player: the skinned canopy (meshes/jc2mech/canopy.nif, bones
// "JC2Chute <bone>") posed every frame from JC2's canopy clips (canopy_anim.bin, baked by
// tools/anim/build_models.py), and Rico's hands pulled onto the canopy handles.
//
// JC2 plays chute_open_chute (or chute_reel_open mid-reel) when the chute opens, then poses the
// rig with two time-blend clips: chute_speed_TB (frame 0 = canopy forward, W; 15 = neutral;
// 30 = flared back, S) and chute_turn_TB (0 = left handle pulled, A; 30 = right, D). Here the
// clip times follow core's chute pitch and bank, which already follow the keys with JC2's
// damping, and the two poses are blended by how far each is from neutral (added, they pull a
// handle below the hips). APPROX: how JC2 combines them is not recovered. On top, the looping
// additive chute_add_idle flutters the cloth bones (the handles stay put) once the canopy is
// out of its opening clip. The opening clips also scale the rig: the canopy grows from 2% to full
// size (base, HarnessExt) while the handles carry the inverse, so the grips stay put. JC2 scales
// per axis; Ni has one scale, so the cloth bones get the geometric mean of theirs.

namespace animation {

namespace {

constexpr const char* kCanopyModel = "jc2mech/canopy.nif";
constexpr const char* kCanopyClips = "Data/meshes/jc2mech/canopy_anim.bin";
constexpr float kPitchRange = 0.65f;   // Tuning::pitchInput: full W / S
constexpr float kBankRange = 0.65f;    // Tuning::bankInput: full A / D
constexpr float kBlendToFlight = 0.3f;  // seconds from the opening clip into the flight pose
// Rico's flight pose (tools/anim/build_anims.py -> rico_chute.bin): JC2's rico_speed_TB /
// rico_turn_TB at a grid of chute pitch x bank, hands on the handles, interpolated every frame
// so the body follows the chute as smoothly as in JC2. It takes over from the opening clips
// (body.cpp) when they end: rico_open_chute 2.4 s, rico_reel_open 2.0 s.
constexpr const char* kRicoPoses = "Data/meshes/jc2mech/rico_chute.bin";
constexpr float kRicoOpenSeconds = 2.4f;
constexpr float kRicoReelOpenSeconds = 2.0f;

using CanopyClips = ClipSet;

CanopyClips g_clips;

// rico_chute.bin
struct ChutePoses {
    std::vector<std::string> bones;
    std::vector<float> grid;   // pitch and bank sample points, ascending
    std::vector<Pose> poses;   // pitch x bank x bones
    bool loaded = false;

    bool Load(const char* path) {
        std::ifstream in(path, std::ios::binary);
        auto u32 = [&] { std::uint32_t v = 0; in.read(reinterpret_cast<char*>(&v), 4); return v; };
        auto str = [&] { std::uint8_t n = 0; in.read(reinterpret_cast<char*>(&n), 1); std::string s(n, char{}); in.read(s.data(), n); return s; };
        char magic[4] = {};
        in.read(magic, 4);
        if (!in || std::string_view(magic, 4) != "JC2R" || u32() != 1) return false;
        // Counts checked before sizing anything by them (a damaged file).
        const std::uint32_t numBones = u32();
        if (!in || numBones == 0 || numBones > ClipSet::kMaxBones) return false;
        bones.resize(numBones);
        for (auto& b : bones) b = str();
        const std::uint32_t gridSize = u32();
        if (!in || gridSize < 2 || gridSize > 64) return false;
        grid.resize(gridSize);
        in.read(reinterpret_cast<char*>(grid.data()), static_cast<std::streamsize>(grid.size() * sizeof(float)));
        for (std::size_t i = 0; i + 1 < grid.size(); ++i) {
            if (!(grid[i] < grid[i + 1])) return false;  // ascending (Cell divides by the step)
        }
        poses.resize(grid.size() * grid.size() * bones.size());
        for (auto& p : poses) {
            float v[7];
            in.read(reinterpret_cast<char*>(v), sizeof(v));
            p = {{v[0], v[1], v[2]}, {v[3], v[4], v[5], v[6]}};
        }
        loaded = static_cast<bool>(in) && grid.size() >= 2;
        return loaded;
    }

    // Grid cell and fraction for a value (clamped to the grid).
    void Cell(float x, std::size_t& i, float& f) const {
        x = std::clamp(x, grid.front(), grid.back());
        i = 0;
        while (i + 2 < grid.size() && x > grid[i + 1]) ++i;
        f = (x - grid[i]) / (grid[i + 1] - grid[i]);
    }

    // Bilinear over the pitch x bank grid.
    Pose At(std::size_t bone, float pitch, float bank) const {
        std::size_t i, j;
        float fp, fb;
        Cell(pitch, i, fp);
        Cell(bank, j, fb);
        auto at = [&](std::size_t a, std::size_t b) -> const Pose& { return poses[(a * grid.size() + b) * bones.size() + bone]; };
        return Lerp(Lerp(at(i, j), at(i, j + 1), fb), Lerp(at(i + 1, j), at(i + 1, j + 1), fb), fp);
    }
};

ChutePoses g_ricoPoses;
RE::NiPointer<RE::NiAVObject> g_ricoRoot;    // the 3D g_ricoBones were looked up in
std::vector<RE::NiAVObject*> g_ricoBones;    // in g_ricoPoses.bones order

// What the animation hook needs from the last frame's core state.
struct ChuteVisual {
    bool active = false;
    bool firstPerson = false;
    bool fromReel = false;
    float openTime = 0.0f;  // since the chute opened
    float pitch = 0.0f, bank = 0.0f;
};
ChuteVisual g_chute;
// Mid-reel JC2 points the body along the rope (its -U is the reel direction): the pitch of the
// rope, applied like the chute tilt. Not while the reel waits to start on the ground.
struct ReelVisual {
    bool active = false;
    float pitch = 0.0f;
};
ReelVisual g_reel;
// The body faces core's heading by a yaw on the root bone and the canopy, not by turning the
// actor: the actor's yaw stays the mouse's, so the third-person camera can look around (and the
// grapple aim with it) while core steers. Faded in and out over kYawBlendSeconds.
constexpr float kYawBlendSeconds = 0.2f;
constexpr float kLookCap = 80.0f * 0.0174533f;  // first person under the chute, either side
constexpr float kFirstPersonBack = 25.0f;        // units the canopy sits back in first person
struct YawVisual {
    float target = 0.0f;  // actor-style yaw (data.angle.z) the body should face
    float weight = 0.0f;
};
YawVisual g_yaw;
// First person under the chute: once the mouse rests for kRecenterDelay, the view eases back to
// the chute's heading, level (the look is told apart from our own writes by comparing the angles
// with what was left last frame). The ease ramps in over kRecenterRamp so it starts gently.
constexpr float kRecenterDelay = 0.6f;  // s without looking around
constexpr float kRecenterRamp = 0.5f;   // s to full ease rate
constexpr float kRecenterRate = 2.5f;   // 1/s
struct LookVisual {
    bool valid = false;
    float idle = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;  // the angles left last frame
};
LookVisual g_look;
jc2::MoveState g_prevState = jc2::MoveState::OnFoot;
// On a wall or ceiling the JC2 clips are authored around the character origin JC2 puts at the
// cling point (grpl_reeled_timeblend: hips ~0.8 m out from it at its height; reel_impact_to_hang
// and the hang idle: hanging by the left hand at the origin, hips ~0.8 m under it). Core's body
// stops a capsule's width off the surface, so Rico's root bone is moved by the difference (Skyrim
// world units), eased in and out.
constexpr float kOffsetSeconds = 0.15f;
struct OffsetVisual {
    RE::NiPoint3 target, offset;
};
OffsetVisual g_offset;

RE::NiPointer<RE::NiNode> g_canopyModel;   // loaded once
RE::NiPointer<RE::NiAVObject> g_canopy;    // the clone hanging on the player
// Held, so a rebuilt player 3D can't leave a freed node to detach from.
RE::NiPointer<RE::NiNode> g_canopyParent;
std::vector<RE::NiAVObject*> g_canopyBones;  // in g_clips.bones order

RE::NiAVObject* Canopy() {
    if (!g_canopyModel) {
        static bool missing = false;  // not looked up again every frame
        if (missing) return nullptr;
        RE::BSModelDB::DBTraits::ArgsType args;
        if (RE::BSModelDB::Demand(kCanopyModel, g_canopyModel, args) != RE::BSResource::ErrorCode::kNone || !g_canopyModel) {
            SKSE::log::warn("{} not found (run tools/anim/build_models.py)", kCanopyModel);
            missing = true;
            return nullptr;
        }
        if (!g_clips.loaded && !g_clips.Load(kCanopyClips)) SKSE::log::warn("{} missing or bad: canopy stays in its bind pose", kCanopyClips);
    }
    if (!g_canopy) {
        g_canopy.reset(static_cast<RE::NiAVObject*>(g_canopyModel->Clone()));
        g_canopyBones.clear();
        for (const auto& name : g_clips.bones) g_canopyBones.push_back(g_canopy->GetObjectByName(RE::BSFixedString(name.c_str())));
    }
    return g_canopy.get();
}

void AttachCanopy(RE::PlayerCharacter* player, bool show) {
    RE::NiNode* root = player->Get3D(false) ? ModelParent(player) : nullptr;
    if (!show || !root) {
        if (g_canopy && g_canopyParent) g_canopyParent->DetachChild(g_canopy.get());
        g_canopyParent.reset();
        return;
    }
    RE::NiAVObject* canopy = Canopy();
    if (!canopy || g_canopyParent.get() == root) return;
    if (g_canopyParent) g_canopyParent->DetachChild(canopy);
    root->AttachChild(canopy, true);
    g_canopyParent.reset(root);
}

void PoseCanopy() {
    const std::size_t n = g_clips.bones.size();
    const auto* open = g_clips.Find(g_chute.fromReel ? "reel_open" : "open");
    const auto* speed = g_clips.Find("speed");
    const auto* turn = g_clips.Find("turn");
    const auto* idle = g_clips.Find("add_idle");  // optional (older canopy_anim.bin has none)
    if (!g_clips.loaded || !open || !speed || !turn || g_canopyBones.size() != n) return;

    const float pitch = std::clamp(g_chute.pitch / kPitchRange, -1.0f, 1.0f);
    const float bank = std::clamp(g_chute.bank / kBankRange, -1.0f, 1.0f);
    const float fs = 15.0f + 15.0f * pitch, ft = 15.0f - 15.0f * bank;
    const float sum = std::fabs(pitch) + std::fabs(bank);
    const float turnWeight = sum > 1e-6f ? std::fabs(bank) / sum : 0.0f;  // same rule as chute_ik.py
    const float tOpen = g_chute.openTime;
    const float blend = std::clamp((tOpen - open->Duration()) / kBlendToFlight, 0.0f, 1.0f);
    const float idleFrame = idle && idle->Duration() > 0.0f ? std::fmod(tOpen, idle->Duration()) / idle->frameDuration : 0.0f;
    for (std::size_t b = 0; b < n; ++b) {
        RE::NiAVObject* node = g_canopyBones[b];
        if (!node) continue;
        const Pose flight = Lerp(CanopyClips::Sample(*speed, b, n, fs), CanopyClips::Sample(*turn, b, n, ft), turnWeight);
        const Pose opening = CanopyClips::Sample(*open, b, n, tOpen / open->frameDuration);
        Pose p = blend >= 1.0f ? flight : Lerp(opening, flight, blend);
        if (idle && blend > 0.0f) p = AddDelta(p, CanopyClips::Sample(*idle, b, n, idleFrame), blend);
        node->local.translate = p.t;
        node->local.rotate = ToMatrix(p.q);
        node->local.scale = p.s;
    }
}

// The chute's pitch and bank in the actor's yaw frame (Skyrim: X right, Y forward, Z up;
// column vectors). JC2 builds the frame as Z(bank) * X(pitch) * Y(heading) in row-vector form, so
// the heading is the outer yaw that data.angle.z already applies; the rest is the frame at
// heading 0. JC2 turns the whole character with this frame (Rico banks into turns and tips with
// W/S); Skyrim keeps actors upright, so it goes on the skeleton root and the canopy instead.
void PoseRico(RE::NiAVObject* root3d) {
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (!g_ricoPoses.Load(kRicoPoses)) SKSE::log::warn("{} missing or bad: Rico keeps the opening clip's end pose (run tools/anim/build_anims.py)", kRicoPoses);
    }
    if (!g_ricoPoses.loaded || !root3d) return;
    const float openSeconds = g_chute.fromReel ? kRicoReelOpenSeconds : kRicoOpenSeconds;
    const float w = std::clamp((g_chute.openTime - openSeconds) / kBlendToFlight, 0.0f, 1.0f);
    if (w <= 0.0f) return;
    if (g_ricoRoot.get() != root3d) {
        g_ricoRoot.reset(root3d);
        g_ricoBones.clear();
        for (const auto& name : g_ricoPoses.bones) g_ricoBones.push_back(root3d->GetObjectByName(RE::BSFixedString(name.c_str())));
    }
    const float pitch = std::clamp(g_chute.pitch / kPitchRange, -1.0f, 1.0f);
    const float bank = std::clamp(g_chute.bank / kBankRange, -1.0f, 1.0f);
    for (std::size_t b = 0; b < g_ricoBones.size(); ++b) {
        RE::NiAVObject* node = g_ricoBones[b];
        if (!node) continue;
        // The grid has the hands solved onto the handles; the grapple arm reaches off the left one
        // with IK afterwards (grapple.cpp).
        Pose p = g_ricoPoses.At(b, pitch, bank);
        if (w < 1.0f) p = Lerp({node->local.translate, FromMatrix(node->local.rotate)}, p, w);
        node->local.translate = p.t;
        node->local.rotate = ToMatrix(p.q);
    }
}

// Rotation about Skyrim's Z turning a body that faces the actor's yaw to face g_yaw.target
// (yaw a faces (sin a, cos a), so the body turns by a - target, counterclockwise).
RE::NiMatrix3 Yaw(float actorYaw) {
    float d = g_yaw.target - actorYaw;
    d = std::remainder(d, 6.2831853f) * g_yaw.weight;
    const float phi = -d, c = std::cos(phi), s = std::sin(phi);
    RE::NiMatrix3 m;
    m.entry[0][0] = c; m.entry[0][1] = -s; m.entry[0][2] = 0.0f;
    m.entry[1][0] = s; m.entry[1][1] = c;  m.entry[1][2] = 0.0f;
    m.entry[2][0] = 0; m.entry[2][1] = 0;  m.entry[2][2] = 1.0f;
    return m;
}

RE::NiMatrix3 Tilt(float pitch, float bank) {
    const jc2::ChuteFrame f = jc2::MakeChuteFrame(pitch, 0.0f, bank);
    // core (x, y, z) -> Skyrim (x, -z, y); the frame rows are right (n), up (r), back (u).
    auto sky = [](const jc2::Vec3& v) { return RE::NiPoint3{v.x, -v.z, v.y}; };
    const RE::NiPoint3 right = sky(f.n), up = sky(f.r), fwd = sky(f.u) * -1.0f;
    RE::NiMatrix3 m;
    m.entry[0][0] = right.x; m.entry[0][1] = fwd.x; m.entry[0][2] = up.x;
    m.entry[1][0] = right.y; m.entry[1][1] = fwd.y; m.entry[1][2] = up.y;
    m.entry[2][0] = right.z; m.entry[2][1] = fwd.z; m.entry[2][2] = up.z;
    return m;
}

}  // namespace

void UpdateVisuals(RE::PlayerCharacter* player, const jc2::Mechanics& mech, float delta) {
    if (!player) return;
    const jc2::MoveState s = mech.State();
    const bool chute = s == jc2::MoveState::Parachuting;
    if (chute && g_prevState != jc2::MoveState::Parachuting) {
        g_chute.openTime = 0.0f;
        g_chute.fromReel = g_prevState == jc2::MoveState::Grappling;
    }
    g_chute.active = chute;
    if (chute) {
        g_chute.openTime += delta;
        g_chute.pitch = mech.Chute().pitch;
        g_chute.bank = mech.Chute().bank;
    }
    g_prevState = s;
    AttachCanopy(player, chute);

    // Face core's heading (core: forward = (-sin h, 0, -cos h); Skyrim yaw: forward = (sin, cos)).
    float heading = 0.0f;
    bool steer = true;
    const jc2::ClingState& cling = mech.Cling();
    switch (s) {
        case jc2::MoveState::WallCling:
        case jc2::MoveState::WallJump:
        case jc2::MoveState::WallDrop:
            // Into the wall: forward = -normal.
            heading = std::atan2(cling.normal.x, cling.normal.z);
            steer = cling.normal.x * cling.normal.x + cling.normal.z * cling.normal.z > 1e-4f;
            break;
        case jc2::MoveState::CeilingHang:
        case jc2::MoveState::Landing: steer = false; break;
        case jc2::MoveState::Parachuting: heading = mech.Chute().heading; break;
        case jc2::MoveState::Skydive: heading = mech.Skydive().heading; break;
        case jc2::MoveState::Grappling: {
            const jc2::Vec3 to = mech.Grapple().anchor;
            const RE::NiPoint3 p = player->GetPosition();
            const float scale = RE::bhkWorld::GetWorldScale();
            // core (x, y, z) = Skyrim (x, z, -y): direction to the anchor in core axes, from
            // about the body's middle
            const float dx = to.x - p.x * scale, dz = to.z + p.y * scale, dy = to.y - (p.z * scale + 1.0f);
            steer = dx * dx + dz * dz > 0.01f;
            heading = std::atan2(-dx, -dz);
            g_reel.active = mech.Grapple().startDelay <= 0.0f;
            g_reel.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
            break;
        }
        default: steer = false; break;
    }
    if (s != jc2::MoveState::Grappling) g_reel = {};
    if (steer) g_yaw.target = -heading;
    const bool yaw = s == jc2::MoveState::Parachuting || s == jc2::MoveState::Skydive || s == jc2::MoveState::Grappling ||
                     s == jc2::MoveState::WallCling || s == jc2::MoveState::WallJump || s == jc2::MoveState::WallDrop ||
                     s == jc2::MoveState::CeilingHang || s == jc2::MoveState::Landing;
    if (yaw && g_yaw.weight <= 0.0f) {
        // Rico takes the heading from wherever the actor faces now.
        if (!steer) g_yaw.target = player->data.angle.z;
    }
    g_yaw.weight = std::clamp(g_yaw.weight + (yaw ? delta : -delta) / kYawBlendSeconds, 0.0f, 1.0f);
    // First person under the chute: the head turns at most kLookCap either way from the heading.
    g_chute.firstPerson = chute && FirstPerson();
    if (g_chute.firstPerson) {
        RE::NiPoint3& angle = player->data.angle;
        const bool looked = g_look.valid && (std::fabs(std::remainder(angle.z - g_look.yaw, 6.2831853f)) > 1e-4f ||
                                             std::fabs(angle.x - g_look.pitch) > 1e-4f);
        g_look.idle = looked ? 0.0f : g_look.idle + delta;
        if (g_look.idle > kRecenterDelay) {
            const float ramp = std::clamp((g_look.idle - kRecenterDelay) / kRecenterRamp, 0.0f, 1.0f);
            const float k = (std::min)(1.0f, delta * kRecenterRate * ramp);
            angle.z += std::remainder(g_yaw.target - angle.z, 6.2831853f) * k;
            angle.x -= angle.x * k;
        }
        const float d = std::remainder(angle.z - g_yaw.target, 6.2831853f);
        if (std::fabs(d) > kLookCap) angle.z = g_yaw.target + std::copysign(kLookCap, d);
        g_look = {true, g_look.idle, angle.z, angle.x};
    } else {
        g_look = {};
    }

    // Root offset to the cling point (only while Rico is still against the surface).
    RE::NiPoint3 target;
    const bool onWall = s == jc2::MoveState::WallCling ||
                        (s == jc2::MoveState::WallJump && cling.time < mech.GetTuning().wallJumpCrouch);
    if (onWall || s == jc2::MoveState::CeilingHang) {
        const float scale = RE::bhkWorld::GetWorldScale();
        const RE::NiPoint3 origin{cling.anchor.x / scale, -cling.anchor.z / scale, cling.anchor.y / scale};
        target = origin - player->GetPosition();
    }
    g_offset.target = target;
    g_offset.offset += (target - g_offset.offset) * (std::min)(1.0f, delta / kOffsetSeconds);
}

void AfterAnimationUpdate(RE::PlayerCharacter* player) {
    RE::NiAVObject* root3d = player ? player->Get3D(false) : nullptr;
    if (!root3d) return;
    PoseBody(root3d);
    if (g_chute.active) PoseRico(root3d);
    RememberBody(root3d);
    RE::NiUpdateData update;
    const bool offset = g_offset.offset.SqrLength() > 0.01f;
    const bool turned = g_chute.active || g_reel.active || g_yaw.weight > 0.0f || offset;
    if (!turned && !BodyPosed()) return;  // on foot: nothing of ours on the skeleton
    static const RE::BSFixedString kRoot("NPC Root [Root]");
    RE::NiAVObject* bone = root3d->GetObjectByName(kRoot);
    if (!turned) {
        if (bone) bone->Update(update);
        return;
    }
    // Turn Rico to core's heading, then tilt him by the chute's pitch and bank (or mid-reel along
    // the rope) about the actor origin (the point core moves). The graph rewrites the root bone
    // every update, so this is set each frame, after the body pose.
    RE::NiMatrix3 tilt;
    if (g_chute.active) tilt = Tilt(g_chute.pitch, g_chute.bank);
    else if (g_reel.active) tilt = Tilt(g_reel.pitch, 0.0f);
    const RE::NiMatrix3 turn = Yaw(player->data.angle.z) * tilt;
    if (bone) {
        bone->local.rotate = turn * bone->local.rotate;
        bone->local.translate = turn * bone->local.translate;
        if (offset && bone->parent) {
            const RE::NiTransform& pw = bone->parent->world;
            bone->local.translate += pw.rotate.Transpose() * g_offset.offset / (pw.scale > 0.0f ? pw.scale : 1.0f);
        }
        bone->Update(update);
    }
    if (!g_chute.active || !g_canopy || !g_canopyParent) return;
    // On the actor's root (or in world space in first person, see ModelParent), turned like Rico.
    const RE::NiTransform& rw = root3d->world;
    const RE::NiCamera* cam = RE::Main::WorldRootCamera();
    if (g_chute.firstPerson && cam) {
        // First person the view doesn't pitch, so a canopy pitched about the feet swings its
        // risers and handles through the face. Here it banks only, about the camera, and sits
        // kFirstPersonBack behind it, so the lines pass behind the head.
        const RE::NiMatrix3 body = rw.rotate * Yaw(player->data.angle.z);
        const RE::NiMatrix3 bank = Tilt(0.0f, g_chute.bank);
        const RE::NiMatrix3 roll = body * bank * body.Transpose();
        const RE::NiPoint3 pivot = cam->world.translate;
        const RE::NiPoint3 back = body * RE::NiPoint3{0.0f, -kFirstPersonBack, 0.0f};
        SetWorld(g_canopy.get(), g_canopyParent.get(), pivot + roll * (rw.translate + back - pivot), body * bank, rw.scale);
    } else {
        SetWorld(g_canopy.get(), g_canopyParent.get(), rw.translate, rw.rotate * turn, rw.scale);
    }
    PoseCanopy();
    g_canopy->Update(update);
}

void AfterCameraUpdate(RE::PlayerCamera* camera) {
    // First person under the chute the view rolls with Rico's bank (not his pitch): the camera
    // root turns about the body's forward axis, the way a head on the banked body would. The
    // first-person state sets the camera root's local transform every update; this goes after.
    if (!g_chute.firstPerson || !camera || !camera->cameraRoot || std::fabs(g_chute.bank) < 1e-4f) return;
    RE::NiNode* node = camera->cameraRoot.get();
    const float t = g_yaw.target, c = std::cos(t), s = std::sin(t);
    RE::NiMatrix3 yaw;  // faces actor yaw t
    yaw.entry[0][0] = c;  yaw.entry[0][1] = s; yaw.entry[0][2] = 0.0f;
    yaw.entry[1][0] = -s; yaw.entry[1][1] = c; yaw.entry[1][2] = 0.0f;
    yaw.entry[2][0] = 0;  yaw.entry[2][1] = 0; yaw.entry[2][2] = 1.0f;
    const RE::NiMatrix3 roll = yaw * Tilt(0.0f, g_chute.bank) * yaw.Transpose();
    // World = parent * local; rolled in world terms (the local was just set, the world is stale).
    if (node->parent) {
        const RE::NiMatrix3& pr = node->parent->world.rotate;
        node->local.rotate = pr.Transpose() * roll * pr * node->local.rotate;
    } else {
        node->local.rotate = roll * node->local.rotate;
    }
    RE::NiUpdateData update;
    node->Update(update);
}

void ResetVisuals() {
    ResetBody();
    g_canopy.reset();
    g_canopyParent.reset();
    g_canopyBones.clear();
    g_ricoRoot.reset();
    g_ricoBones.clear();
    g_offset = {};
    g_chute = {};
    g_reel = {};
    g_yaw = {};
    g_look = {};
    g_prevState = jc2::MoveState::OnFoot;
}

}  // namespace animation
