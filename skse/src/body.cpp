#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string_view>
#include <vector>

#include "animation.h"
#include "pose.h"

// Rico's body in the states core drives, posed by the plugin from JC2's clips (rico_body.bin,
// baked by tools/anim/build_anims.py) after the behavior graph, which stays in its jump fall
// state underneath (animation.cpp). Every bone of the clips is written, so what the graph plays
// there does not show.
//
// Skydive: JC2's ANIM_SKYDIVE (kc001_base.asb) blends four clips with weights set every frame by
// 00853f70 from the skydive state ([char+0xA34]): steer +0x14C > 0 (A) weights freefall_trn_lft,
// < 0 (D) freefall_trn_rgt; the dive weight 0076d010 = 2 * (0.5 - dive +0x148) once 1 - dive
// > 0.51 weights birdsuit_fwd (W, head down; S has no clip: the pose stays neutral and only the
// physics change); mhfreefall gets 1 - the rest (0 below 0.01). APPROX: which instance is which
// clip is read from the weights' meaning (the .asb lists the four in another order), and the
// clips loop on their own clocks (JC2's blend node may sync them).
// A fall turns into the skydive through falling_to_freefall (ANIM_JUMP_FALLING_TO_SKYDIVE).
// Chute: rico_open_chute / rico_reel_open, holding the last frame; visuals.cpp then blends into
// the flight pose. Reel: reel_in_start while a reel fired from the ground waits to start, then
// grpl_reel_flight_part2, played once and held (its .asb play mode is 0, not the loop mode 1 of
// ANIM_SKYDIVE / ANIM_FALL).
// Wall (BaseStateMachine.afsm): grpl_reel_impact, then grpl_reeled_timeblend, a time-blend clip
// whose frame is the look direction (frame 0 far right, 10 into the wall, 20 left; S_REELED_IN_IDLE
// is a CVelocityTimeVariableState): here the frame follows the camera, rate-limited (APPROX: JC2's
// time variable is not decoded, the frame is picked so the right hand points where the camera
// looks). Without a gun JC2 layers grpl_reeled_timeblend_unarmed on the right arm (the hand
// stays on the wire); baked merged as wall_idle_unarmed. grpl_reel_bounce for the jump off and
// grpl_reeled_release for the drop. Ceiling: reel_impact_horizontal, reel_impact_to_hang, then
// ANIM_HANG_IDLE, Rico hanging by the left hand from the anchor (the helicopter hang): two
// time-blend clips whose frames are the swing sideways (heli_default_timeblend_hori, frame 20
// centered) and fore / aft (_diag). APPROX: JC2 drives them from the swing's velocity; here they
// sway slowly about the middle, blended half and half. heli_default_exit_to_fall when it lets go. Chute landing: land_chute_roll.
// On a wall or ceiling with a weapon or spell drawn in the right hand, the right arm is left to
// the behavior graph, so it holds it as Skyrim does.
// Each source cross-fades in over its .asb blend time (property 57ab7197; APPROX 0.1 / 0.2 s for
// the arrival and landing clips, not read yet); back to the graph's own pose over kBlendSeconds.

namespace animation {

namespace {

constexpr const char* kBodyClips = "Data/meshes/jc2mech/rico_body.bin";
constexpr float kBlendSeconds = 0.2f;  // back to the graph
constexpr float kDiveFrom = 0.51f;      // 00e4e4c4: 1 - dive above this starts birdsuit_fwd
constexpr float kMinBaseWeight = 0.01f;  // 00f66934

enum class Source {
    kGraph, kSkydiveEnter, kSkydive, kChuteOpen, kChuteReelOpen, kReelStart, kReel,
    kWallImpact, kWallIdle, kWallIdleUnarmed, kWallJump, kWallDrop, kHangImpact, kHangEnter, kHang, kHangDiag, kHangDrop, kLandRoll,
};

// kc001_base.asb blend-in times.
float BlendSeconds(Source s) {
    switch (s) {
        case Source::kSkydiveEnter: return 0.3f;   // ANIM_JUMP_FALLING_TO_SKYDIVE
        case Source::kSkydive: return 0.4f;        // ANIM_SKYDIVE
        case Source::kChuteOpen: return 0.1f;      // ANIM_PULL_OPEN_PARACHUTE_VERTICAL
        case Source::kChuteReelOpen: return 0.2f;  // ANIM_UNFOLD_PARACHUTE_HORIZONTAL
        case Source::kReelStart: return 0.1f;      // ANIM_REEL_START
        case Source::kReel: return 0.2f;           // ANIM_REEL_FLIGHT
        case Source::kWallImpact:
        case Source::kHangImpact:
        case Source::kWallJump:
        case Source::kWallDrop:
        case Source::kLandRoll: return 0.1f;
        default: return kBlendSeconds;
    }
}

ClipSet g_clips;
const ClipSet::Clip* g_skydive[4]{};  // mhfreefall, trn_lft, trn_rgt, birdsuit_fwd
const ClipSet::Clip* g_skydiveEnter = nullptr;
const ClipSet::Clip* g_chuteOpen = nullptr;
const ClipSet::Clip* g_chuteReelOpen = nullptr;
const ClipSet::Clip* g_reelStart = nullptr;
const ClipSet::Clip* g_reel = nullptr;
// Arrival, cling and landing clips, by Source (nullptr where a source has none).
struct NamedClip {
    Source source;
    const char* name;
    bool loop;
    const ClipSet::Clip* clip = nullptr;
};
NamedClip g_more[] = {
    {Source::kWallImpact, "wall_impact", false}, {Source::kWallIdle, "wall_idle", false},
    {Source::kWallIdleUnarmed, "wall_idle_unarmed", false},
    {Source::kWallJump, "wall_jump", false},     {Source::kWallDrop, "wall_drop", false},
    {Source::kHangImpact, "hang_impact", false}, {Source::kHangEnter, "hang_enter", false},
    {Source::kHang, "hang", false},              {Source::kHangDiag, "hang_diag", false},               {Source::kHangDrop, "hang_drop", false},
    {Source::kLandRoll, "land_roll", false},
};

float ClipSeconds(Source s) {
    for (const auto& c : g_more) {
        if (c.source == s) return c.clip ? c.clip->Duration() : 0.0f;
    }
    return 0.0f;
}

RE::NiPointer<RE::NiAVObject> g_root;  // the 3D g_bones were looked up in
std::vector<RE::NiAVObject*> g_bones;   // in g_clips.bones order
std::vector<bool> g_rightArm;           // per g_bones: clavicle to fingers of the right arm

// grpl_reeled_timeblend: where the right hand points at each frame, degrees right of straight
// into the wall (measured from the clip, unwrapped; frame 10 looks into the wall).
constexpr float kWallLook[] = {215, 192, 169, 144, 118, 94, 72, 54, 39, 24, 10,
                               -5, -22, -38, -54, -65, -75, -86, -96, -103, -109};
constexpr float kWallLookRate = 25.0f;  // frames per second the look follows the camera
float g_wallFrame = 10.0f;
// Hang idle sway (frames of the 41-frame swing clips; 20 = still).
constexpr float kHangMid = 20.0f, kHangSway = 4.0f, kHangSwayPeriod = 3.0f;
bool g_rightHandBusy = false;  // weapon or spell drawn in the right hand

bool IsRightArmBone(std::string_view name) {
    constexpr std::string_view prefixes[] = {"npc r clavicle", "npc r upperarm", "npc r forearm", "npc r hand", "npc r finger"};
    for (std::string_view p : prefixes) {
        if (name.size() >= p.size() &&
            std::equal(p.begin(), p.end(), name.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
            return true;
        }
    }
    return false;
}

// Clip frame of grpl_reeled_timeblend looking along the camera, for a body facing into a wall
// with core normal n.
float WallLookFrame(const jc2::Vec3& n) {
    const RE::NiCamera* cam = RE::Main::WorldRootCamera();
    if (!cam) return 10.0f;
    // Skyrim XY: into the wall = -normal (core (x, y, z) = Skyrim (x, -z, y)); NiCamera looks down +X.
    const float fx = -n.x, fy = n.z;
    const float cx = cam->world.rotate.entry[0][0], cy = cam->world.rotate.entry[1][0];
    const float look = std::atan2(cx * fy - cy * fx, cx * fx + cy * fy) * 57.29578f;  // + = right
    constexpr int kN = static_cast<int>(std::size(kWallLook));
    if (look >= kWallLook[0]) return 0.0f;
    for (int i = 0; i + 1 < kN; ++i) {
        if (look >= kWallLook[i + 1]) return static_cast<float>(i) + (kWallLook[i] - look) / (kWallLook[i] - kWallLook[i + 1]);
    }
    return static_cast<float>(kN - 1);
}

Source g_source = Source::kGraph;
Source g_prevSource = Source::kGraph;
float g_time = 0.0f;   // in the current source
float g_blend = 1.0f;  // 0..1 from the snapshot into the current source
bool g_takeSnapshot = false;
std::vector<Pose> g_snapshot;  // the pose the cross-fade starts from
std::vector<Pose> g_last;      // last frame's body (before the chute / reel tilt)
float g_dive = 0.5f, g_steer = 0.0f;

bool Load() {
    static bool tried = false;
    if (tried) return g_clips.loaded;
    tried = true;
    if (!g_clips.Load(kBodyClips)) {
        SKSE::log::warn("{} missing or bad: Rico keeps Skyrim's fall pose (run tools/anim/build_anims.py)", kBodyClips);
        return false;
    }
    const char* names[4] = {"skydive", "skydive_left", "skydive_right", "skydive_dive"};
    for (int i = 0; i < 4; ++i) g_skydive[i] = g_clips.Find(names[i]);
    g_skydiveEnter = g_clips.Find("skydive_enter");
    g_chuteOpen = g_clips.Find("chute_open");
    g_chuteReelOpen = g_clips.Find("chute_reel_open");
    g_reelStart = g_clips.Find("reel_start");
    g_reel = g_clips.Find("reel");
    for (auto& c : g_more) {
        c.clip = g_clips.Find(c.name);
        if (!c.clip) SKSE::log::warn("{}: no clip {} (rebuild with tools/anim/build_anims.py)", kBodyClips, c.name);
    }
    SKSE::log::info("{}: {} clips, {} bones", kBodyClips, g_clips.clips.size(), g_clips.bones.size());
    return true;
}

float Frame(const ClipSet::Clip& c, float time, bool loop) {
    const float duration = c.Duration();
    if (loop && duration > 0.0f) time = std::fmod(time, duration);
    return time / c.frameDuration;
}

const ClipSet::Clip* SingleClip(Source s, bool& loop) {
    loop = false;
    switch (s) {
        case Source::kSkydiveEnter: return g_skydiveEnter;
        case Source::kChuteOpen: return g_chuteOpen;
        case Source::kChuteReelOpen: return g_chuteReelOpen;
        case Source::kReelStart: return g_reelStart;
        case Source::kReel: return g_reel;
        default:
            for (const auto& c : g_more) {
                if (c.source == s) {
                    loop = c.loop;
                    return c.clip;
                }
            }
            return nullptr;
    }
}

// JC2's skydive weights (00853f70, 0076d010): base, left, right, dive.
void SkydiveWeights(float dive, float steer, float w[4]) {
    w[1] = (std::max)(steer, 0.0f);
    w[2] = (std::max)(-steer, 0.0f);
    w[3] = 1.0f - dive > kDiveFrom ? (1.0f - dive - 0.5f) * 2.0f : 0.0f;
    w[0] = 1.0f - (w[1] + w[2]) - w[3];
    if (w[0] < kMinBaseWeight) w[0] = 0.0f;
}

// Weighted blend of up to four poses (normalized weights; quaternions sign-aligned to the first).
Pose Blend(const Pose* p, const float* w, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += w[i];
    if (sum <= 1e-6f) return p[0];
    Pose out{{0, 0, 0}, {0, 0, 0, 0}, 0.0f};
    for (int i = 0; i < n; ++i) {
        const float k = w[i] / sum;
        Quat q = p[i].q;
        if (q.x * p[0].q.x + q.y * p[0].q.y + q.z * p[0].q.z + q.w * p[0].q.w < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};
        out.t += p[i].t * k;
        out.q = {out.q.x + q.x * k, out.q.y + q.y * k, out.q.z + q.z * k, out.q.w + q.w * k};
        out.s += p[i].s * k;
    }
    const float len = std::sqrt(out.q.x * out.q.x + out.q.y * out.q.y + out.q.z * out.q.z + out.q.w * out.q.w);
    out.q = {out.q.x / len, out.q.y / len, out.q.z / len, out.q.w / len};
    return out;
}

Pose Current(const RE::NiAVObject* node) { return {node->local.translate, FromMatrix(node->local.rotate), node->local.scale}; }

}  // namespace

void UpdateBody(const jc2::Mechanics& mech, float delta) {
    if (mech.State() != jc2::MoveState::OnFoot) Load();  // the clip pointers below
    Source s = Source::kGraph;
    switch (mech.State()) {
        case jc2::MoveState::Skydive:
            // From a fall: falling_to_freefall first, until the skydive's blend would end it.
            if (g_source == Source::kGraph && g_skydiveEnter) s = Source::kSkydiveEnter;
            else if (g_source == Source::kSkydiveEnter && g_skydiveEnter &&
                     g_time < g_skydiveEnter->Duration() - BlendSeconds(Source::kSkydive)) s = Source::kSkydiveEnter;
            else s = Source::kSkydive;
            break;
        case jc2::MoveState::Parachuting:
            // Opened mid-reel keeps rico_reel_open for the whole flight (the clip holds its end).
            s = g_source == Source::kChuteReelOpen || g_source == Source::kReel || g_source == Source::kReelStart
                    ? Source::kChuteReelOpen : Source::kChuteOpen;
            break;
        case jc2::MoveState::Grappling:
            s = mech.Grapple().startDelay > 0.0f && g_source != Source::kReel ? Source::kReelStart : Source::kReel;
            break;
        case jc2::MoveState::WallCling:
            s = mech.Cling().time < ClipSeconds(Source::kWallImpact) ? Source::kWallImpact : Source::kWallIdle;
            break;
        case jc2::MoveState::CeilingHang: {
            const float t = mech.Cling().time, impact = ClipSeconds(Source::kHangImpact);
            s = t < impact ? Source::kHangImpact : t < impact + ClipSeconds(Source::kHangEnter) ? Source::kHangEnter : Source::kHang;
            break;
        }
        case jc2::MoveState::WallJump: s = Source::kWallJump; break;
        case jc2::MoveState::WallDrop: s = Source::kWallDrop; break;
        case jc2::MoveState::Landing: s = Source::kLandRoll; break;
        default: break;
    }
    // Letting go of a ceiling: heli_default_exit_to_fall plays out into the fall or skydive.
    if ((mech.State() == jc2::MoveState::Falling || mech.State() == jc2::MoveState::Skydive) &&
        (g_source == Source::kHang || g_source == Source::kHangEnter || g_source == Source::kHangImpact ||
         (g_source == Source::kHangDrop && g_time < ClipSeconds(Source::kHangDrop)))) {
        s = Source::kHangDrop;
    }
    if (s != g_source) {
        g_prevSource = g_source;
        g_source = s;
        g_time = 0.0f;
        g_blend = 0.0f;
        g_takeSnapshot = true;
    }
    g_time += delta;
    g_blend = (std::min)(1.0f, g_blend + delta / BlendSeconds(g_source));
    g_dive = mech.Skydive().dive;
    g_steer = mech.Skydive().steer;

    if (g_source == Source::kWallIdle) {
        const float want = WallLookFrame(mech.Cling().normal);
        if (g_time <= delta) g_wallFrame = want;
        const float step = kWallLookRate * delta;
        g_wallFrame += std::clamp(want - g_wallFrame, -step, step);
    }
    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    const bool cling = mech.State() == jc2::MoveState::WallCling || mech.State() == jc2::MoveState::CeilingHang;
    g_rightHandBusy = cling && player && player->AsActorState()->IsWeaponDrawn() && player->GetEquippedObject(false) != nullptr;
}

void PoseBody(RE::NiAVObject* root3d) {
    if (!root3d || (g_source == Source::kGraph && g_blend >= 1.0f && !g_takeSnapshot)) return;
    if (!Load()) return;
    if (g_root.get() != root3d) {
        g_root.reset(root3d);
        g_bones.clear();
        g_rightArm.clear();
        for (const auto& name : g_clips.bones) {
            g_bones.push_back(root3d->GetObjectByName(RE::BSFixedString(name.c_str())));
            g_rightArm.push_back(IsRightArmBone(name));
        }
        g_last.clear();
    }
    const std::size_t n = g_bones.size();
    if (g_takeSnapshot) {
        g_takeSnapshot = false;
        if (g_prevSource != Source::kGraph && g_last.size() == n) {
            g_snapshot = g_last;
        } else {
            g_snapshot.assign(n, Pose{});
            for (std::size_t b = 0; b < n; ++b) {
                if (g_bones[b]) g_snapshot[b] = Current(g_bones[b]);
            }
        }
    }
    if (g_snapshot.size() != n) return;

    bool loop = false;
    const ClipSet::Clip* single = SingleClip(g_source, loop);
    float singleFrame = single ? Frame(*single, g_time, loop) : 0.0f;
    // On the wall: the frame is the look direction; the right arm comes from the unarmed clip
    // (or stays the graph's with a weapon drawn).
    const ClipSet::Clip* rightArm = nullptr;
    if (g_source == Source::kWallIdle) {
        singleFrame = g_wallFrame;
        bool unused = false;
        rightArm = SingleClip(Source::kWallIdleUnarmed, unused);
    }
    // Hanging: the two swing clips at their sway frames.
    bool unused = false;
    const ClipSet::Clip* hangDiag = g_source == Source::kHang ? SingleClip(Source::kHangDiag, unused) : nullptr;
    float diagFrame = 0.0f;
    if (single && hangDiag) {
        singleFrame = kHangMid + kHangSway * std::sin(g_time * 6.2831853f / kHangSwayPeriod);
        diagFrame = kHangMid + kHangSway * std::sin(g_time * 6.2831853f / (kHangSwayPeriod * 1.3f) + 1.0f);
    }
    float w[4]{};
    float skyFrame[4]{};
    const bool skydive = g_source == Source::kSkydive && g_skydive[0] && g_skydive[1] && g_skydive[2] && g_skydive[3];
    if (skydive) {
        SkydiveWeights(g_dive, g_steer, w);
        for (int i = 0; i < 4; ++i) skyFrame[i] = Frame(*g_skydive[i], g_time, true);
    }
    // Smoothstep over the cross-fade.
    const float k = g_blend * g_blend * (3.0f - 2.0f * g_blend);
    for (std::size_t b = 0; b < n; ++b) {
        RE::NiAVObject* node = g_bones[b];
        if (!node || (g_rightHandBusy && g_rightArm[b])) continue;
        Pose p;
        if (rightArm && g_rightArm[b]) {
            p = ClipSet::Sample(*rightArm, b, n, singleFrame);
        } else if (skydive) {
            Pose ps[4];
            for (int i = 0; i < 4; ++i) ps[i] = ClipSet::Sample(*g_skydive[i], b, n, skyFrame[i]);
            p = Blend(ps, w, 4);
        } else if (single && hangDiag) {
            const Pose ps[2] = {ClipSet::Sample(*single, b, n, singleFrame), ClipSet::Sample(*hangDiag, b, n, diagFrame)};
            const float w2[2] = {0.5f, 0.5f};
            p = Blend(ps, w2, 2);
        } else if (single) {
            p = ClipSet::Sample(*single, b, n, singleFrame);
        } else {
            p = Current(node);  // the graph's own pose (fading back to it)
        }
        if (k < 1.0f) p = Lerp(g_snapshot[b], p, k);
        node->local.translate = p.t;
        node->local.rotate = ToMatrix(p.q);
    }
}

void RememberBody(RE::NiAVObject* root3d) {
    if (!root3d || g_root.get() != root3d || (g_source == Source::kGraph && g_blend >= 1.0f)) return;
    g_last.resize(g_bones.size());
    for (std::size_t b = 0; b < g_bones.size(); ++b) {
        if (g_bones[b]) g_last[b] = Current(g_bones[b]);
    }
}

bool BodyPosed() { return g_source != Source::kGraph || g_blend < 1.0f; }

void ResetBody() {
    g_root.reset();
    g_bones.clear();
    g_rightArm.clear();
    g_last.clear();
    g_snapshot.clear();
    g_source = g_prevSource = Source::kGraph;
    g_time = 0.0f;
    g_blend = 1.0f;
    g_takeSnapshot = false;
}

}  // namespace animation
