#include <algorithm>
#include <cmath>
#include <vector>

#include "animation.h"
#include "pose.h"

// The grapple on the player: Rico's left arm, the hook and the wire.
//
// JC2 fires the grapple on a left-arm layer (kc001_leftarm.asb): the upper arm is aimed at the
// target procedurally and the _con clips animate the forearm, hand and fingers on top
// (grpl_fire_ani_con on foot, grpl_freefall_fire_ani_con in the air, grpl_freefall_pull_con
// while the rope tethers the chute). Those are baked to rico_arm.bin (tools/anim/build_anims.py);
// here they go on the arm after the behavior graph, then the upper arm turns so the hand points
// at the hook. Mid-reel only the aim stays: the reel clip has the arm out already.
//
// The hook (hook.nif, tip along +Y) leaves the hand when core's wind-up ends and flies to the
// anchor at core's hook speed (HookState); the wire (rope.nif, two skinned end bones) is
// stretched from the hand to the hook. After a detach the hook flies back to the hand. A loose
// object on the hook (PullState) keeps the wire to the hand; two linked ones get the wire
// between them and the arm is free.
// APPROX: JC2's wire sags and its retract speed is not measured (kRetractSpeed).

namespace animation {

namespace {

constexpr const char* kHookModel = "jc2mech/hook.nif";
constexpr const char* kRopeModel = "jc2mech/rope.nif";
constexpr const char* kArmClips = "Data/meshes/jc2mech/rico_arm.bin";
constexpr float kArmBlendRate = 12.0f;    // 1/s, in and out of the arm layer
constexpr float kRetractSpeed = 150.0f;   // m/s, the hook back to the hand after a detach

enum class Phase { kNone, kFlying, kAttached, kHang, kRetract, kPull, kLink };

struct GrappleVisual {
    Phase phase = Phase::kNone;
    bool air = false;          // fired from the air (fire_air clip)
    bool tether = false;       // attached under the chute
    float fireTime = 0.0f;     // since the fire press
    float attachTime = 0.0f;   // since the hook landed
    float travel = 0.0f;       // 0..1 from the hand to the anchor while flying
    RE::NiPoint3 anchor;       // Skyrim units
    RE::NiPoint3 anchorB;      // kLink: the other end
    RE::NiPoint3 hook;         // where the hook was drawn last
    float arm = 0.0f;          // aim weight
    float clip = 0.0f;         // forearm clip weight
};
GrappleVisual g;
bool g_prevFlying = false;
float g_prevHookTime = 0.0f;

ClipSet g_armClips;
RE::NiPointer<RE::NiAVObject> g_armRoot;
std::vector<RE::NiAVObject*> g_armBones;

struct Model {
    const char* path;
    RE::NiPointer<RE::NiNode> source;
    RE::NiPointer<RE::NiAVObject> clone;
    RE::NiPointer<RE::NiNode> parent;  // held: the player's 3D can be rebuilt under us
    bool warned = false;

    RE::NiAVObject* Get() {
        if (!source) {
            if (warned) return nullptr;  // missing: not looked up again every frame
            RE::BSModelDB::DBTraits::ArgsType args;
            if (RE::BSModelDB::Demand(path, source, args) != RE::BSResource::ErrorCode::kNone || !source) {
                if (!warned) SKSE::log::warn("{} not found (run tools/anim/build_models.py)", path);
                warned = true;
                return nullptr;
            }
        }
        if (!clone) clone.reset(static_cast<RE::NiAVObject*>(source->Clone()));
        return clone.get();
    }

    // Under the player's root node or cell (shown) or detached.
    RE::NiAVObject* Show(RE::NiNode* root, bool show) {
        if (!show || !root) {
            if (clone && parent) parent->DetachChild(clone.get());
            parent.reset();
            return nullptr;
        }
        RE::NiAVObject* obj = Get();
        if (!obj) return nullptr;
        if (parent.get() != root) {
            if (parent) parent->DetachChild(obj);
            root->AttachChild(obj, true);
            parent.reset(root);
        }
        return obj;
    }

    void Reset() {
        if (clone && parent) parent->DetachChild(clone.get());
        clone.reset();
        parent.reset();
    }
};
Model g_hook{kHookModel};
Model g_rope{kRopeModel};

RE::NiPoint3 ToSkyrim(const jc2::Vec3& v) {
    const float scale = RE::bhkWorld::GetWorldScale();
    return RE::NiPoint3{v.x, -v.z, v.y} / scale;
}

// Rotation taking +Y to dir (columns are the local axes in the parent frame).
RE::NiMatrix3 AlongY(RE::NiPoint3 dir) {
    dir.Unitize();
    RE::NiPoint3 x = dir.Cross({0.0f, 0.0f, 1.0f});
    if (x.SqrLength() < 1e-6f) x = {1.0f, 0.0f, 0.0f};
    x.Unitize();
    const RE::NiPoint3 z = x.Cross(dir);
    RE::NiMatrix3 m;
    m.entry[0][0] = x.x; m.entry[0][1] = dir.x; m.entry[0][2] = z.x;
    m.entry[1][0] = x.y; m.entry[1][1] = dir.y; m.entry[1][2] = z.y;
    m.entry[2][0] = x.z; m.entry[2][1] = dir.z; m.entry[2][2] = z.z;
    return m;
}

void LoadArmClips() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (!g_armClips.Load(kArmClips)) SKSE::log::warn("{} missing or bad: the grapple arm only aims (run tools/anim/build_anims.py)", kArmClips);
}

// The forearm, hand and finger clip at weight w (on top of the graph's pose).
void PoseArmClip(RE::NiAVObject* root3d, const char* name, float time, bool loop, float w) {
    LoadArmClips();
    const ClipSet::Clip* clip = g_armClips.loaded ? g_armClips.Find(name) : nullptr;
    if (!clip || w <= 0.0f) return;
    if (g_armRoot.get() != root3d) {
        g_armRoot.reset(root3d);
        g_armBones.clear();
        for (const auto& b : g_armClips.bones) g_armBones.push_back(root3d->GetObjectByName(RE::BSFixedString(b.c_str())));
    }
    const float duration = clip->Duration();
    if (loop && duration > 0.0f) time = std::fmod(time, duration);
    const float frame = time / clip->frameDuration;
    const std::size_t n = g_armClips.bones.size();
    for (std::size_t b = 0; b < g_armBones.size(); ++b) {
        RE::NiAVObject* node = g_armBones[b];
        if (!node) continue;
        Pose p = ClipSet::Sample(*clip, b, n, frame);
        if (w < 1.0f) p = Lerp({node->local.translate, FromMatrix(node->local.rotate)}, p, w);
        node->local.translate = p.t;
        node->local.rotate = ToMatrix(p.q);
    }
}

// Diagnostics, once a second while the arm aims: how far the forearm points off the target, and
// whether the twist bones (which carry the forearm's skin) sit on the forearm.
void LogArm(RE::NiAVObject* fore, RE::NiAVObject* hand, const RE::NiPoint3& target, float w) {
    static float last = 0.0f;
    const float now = static_cast<float>(GetTickCount64()) / 1000.0f;
    if (w < 0.5f || now - last < 1.0f) return;
    last = now;
    static const RE::BSFixedString kTwist("NPC L ForearmTwist2 [LLt2]");
    RE::NiAVObject* twist = fore->GetObjectByName(kTwist);
    RE::NiPoint3 a = hand->world.translate - fore->world.translate, b = target - hand->world.translate;
    a.Unitize();
    b.Unitize();
    const RE::NiPoint3 t = twist ? twist->world.translate : RE::NiPoint3{};
    const RE::NiPoint3 mid = (fore->world.translate + hand->world.translate) * 0.5f;
    SKSE::log::info("  arm: forearm {:.1f} deg off the target, elbow ({:.0f} {:.0f} {:.0f}) hand ({:.0f} {:.0f} {:.0f}) twist2 {:.1f} from the forearm's middle",
                    std::acos(std::clamp(a.Dot(b), -1.0f, 1.0f)) * 57.2958f, fore->world.translate.x, fore->world.translate.y,
                    fore->world.translate.z, hand->world.translate.x, hand->world.translate.y, hand->world.translate.z,
                    twist ? (t - mid).Length() : -1.0f);
}

// Turns the upper arm so the forearm (the grapple sits on the left wrist) points at target
// (weight w), keeping the elbow bend of the clip, as JC2's left-arm layer does (on foot, in the
// air and under the chute alike). Returns the hand position.
RE::NiPoint3 AimArm(RE::NiAVObject* root3d, const RE::NiPoint3& target, float w) {
    static const RE::BSFixedString kUpper("NPC L UpperArm [LUar]");
    static const RE::BSFixedString kFore("NPC L Forearm [LLar]");
    static const RE::BSFixedString kHand("NPC L Hand [LHnd]");
    RE::NiAVObject* upper = root3d->GetObjectByName(kUpper);
    RE::NiAVObject* fore = root3d->GetObjectByName(kFore);
    RE::NiAVObject* hand = root3d->GetObjectByName(kHand);
    if (!upper || !fore || !hand || !upper->parent) return hand ? hand->world.translate : root3d->world.translate;
    UpdateSkeleton(root3d);
    if (w > 0.0f) {
        // The arm turns rigidly about the shoulder, so the forearm turns by the same rotation.
        TurnBone(upper, hand->world.translate - fore->world.translate, target - hand->world.translate, w);
        UpdateSkeleton(root3d);
    }
    LogArm(fore, hand, target, w);
    return hand->world.translate;
}

}  // namespace

void UpdateGrapple(RE::PlayerCharacter* player, const jc2::Mechanics& mech, float delta) {
    if (!player) return;
    const jc2::HookState& hook = mech.Hook();
    const jc2::GrappleState& rope = mech.Grapple();
    const jc2::MoveState s = mech.State();

    // A new shot: the hook started flying, or re-fired while one was in the air.
    if (hook.flying && (!g_prevFlying || hook.time < g_prevHookTime)) {
        g.phase = Phase::kFlying;
        g.fireTime = 0.0f;
        g.air = s != jc2::MoveState::OnFoot;
    }
    g_prevFlying = hook.flying;
    g_prevHookTime = hook.time;

    const bool attached = rope.attached && (s == jc2::MoveState::Grappling || s == jc2::MoveState::Parachuting);
    const jc2::PullState& pull = mech.Pull();
    // Hanging under a ceiling or on a wall Rico holds the wire: the hook stays in.
    const bool hanging = (s == jc2::MoveState::CeilingHang || s == jc2::MoveState::WallCling) && !hook.flying;
    if (hanging) {
        if (g.phase != Phase::kHang) g.attachTime = 0.0f;
        g.phase = Phase::kHang;
        g.anchor = ToSkyrim(mech.Cling().anchor);
        g.tether = false;
    } else if (hook.flying) {
        g.anchor = ToSkyrim(hook.anchor);
        g.travel = hook.Travel(mech.GetTuning());
    } else if (attached) {
        if (g.phase != Phase::kAttached) g.attachTime = 0.0f;
        g.phase = Phase::kAttached;
        g.anchor = ToSkyrim(rope.anchor);
        g.tether = s == jc2::MoveState::Parachuting;
    } else if (pull.phase == jc2::PullState::Phase::Link) {
        g.phase = Phase::kLink;
        g.anchor = ToSkyrim(pull.pointA);
        g.anchorB = ToSkyrim(pull.pointB);
    } else if (pull.phase != jc2::PullState::Phase::None) {
        if (g.phase != Phase::kPull) g.attachTime = 0.0f;
        g.phase = Phase::kPull;
        g.anchor = ToSkyrim(pull.pointA);
        g.tether = false;
    } else if (g.phase == Phase::kFlying || g.phase == Phase::kAttached || g.phase == Phase::kHang || g.phase == Phase::kPull ||
               g.phase == Phase::kLink) {
        g.phase = Phase::kRetract;
    }
    g.fireTime += delta;
    g.attachTime += delta;

    const bool armOn = g.phase == Phase::kFlying || g.phase == Phase::kAttached || g.phase == Phase::kPull;
    const bool clipOn = g.phase == Phase::kFlying || (g.phase == Phase::kAttached && g.tether);
    const float k = (std::min)(1.0f, delta * kArmBlendRate);
    g.arm += ((armOn ? 1.0f : 0.0f) - g.arm) * k;
    g.clip += ((clipOn ? 1.0f : 0.0f) - g.clip) * k;
    if (g.phase == Phase::kRetract) {
        const float step = kRetractSpeed / RE::bhkWorld::GetWorldScale() * delta;
        const RE::NiPoint3 to = player->GetPosition() + RE::NiPoint3{0.0f, 0.0f, 100.0f};
        RE::NiPoint3 d = to - g.hook;
        if (d.Length() <= step) g.phase = Phase::kNone;
        else g.hook += d * (step / d.Length());
    }
}

void GrappleAfterAnimation(RE::PlayerCharacter* player) {
    RE::NiAVObject* root3d = player ? player->Get3D(false) : nullptr;
    RE::NiNode* root = root3d ? ModelParent(player) : nullptr;  // the hook and wire hang here
    if (!root || (g.phase == Phase::kNone && g.arm < 0.01f)) {
        g_hook.Show(nullptr, false);
        g_rope.Show(nullptr, false);
        return;
    }

    // The arm: forearm clip, then the aim at the hook (in flight) or the anchor.
    if (g.clip > 0.01f) {
        if (g.phase == Phase::kAttached && g.tether) PoseArmClip(root3d, "pull", g.attachTime, true, g.clip);
        else PoseArmClip(root3d, g.air ? "fire_air" : "fire", g.fireTime, false, g.clip);
    }
    const RE::NiPoint3 target = g.phase == Phase::kRetract ? g.hook : g.anchor;
    const RE::NiPoint3 hand = AimArm(root3d, target, g.arm);

    // The hook: in the hand during the wind-up, then along the line to the anchor.
    bool showHook = true;
    RE::NiPoint3 dir = g.anchor - hand;
    switch (g.phase) {
        case Phase::kFlying:
            showHook = g.travel > 0.0f;
            g.hook = hand + (g.anchor - hand) * g.travel;
            break;
        case Phase::kAttached:
        case Phase::kHang:
        case Phase::kPull:
        case Phase::kLink: g.hook = g.anchor; break;
        case Phase::kRetract: dir = g.hook - hand; break;
        default: showHook = false; break;
    }
    RE::NiUpdateData update;
    if (RE::NiAVObject* h = g_hook.Show(root, showHook)) {
        SetWorld(h, root, g.hook, AlongY(dir));
        h->Update(update);
    }
    RE::NiAVObject* rope = g_rope.Show(root, showHook);
    if (!rope) return;
    rope->local = {};
    rope->Update(update);
    static const RE::BSFixedString kStart("JC2Rope Start"), kEnd("JC2Rope End");
    RE::NiAVObject* a = rope->GetObjectByName(kStart);
    RE::NiAVObject* b = rope->GetObjectByName(kEnd);
    if (!a || !b) return;
    const RE::NiPoint3 from = g.phase == Phase::kLink ? g.anchorB : hand;
    const RE::NiMatrix3 along = AlongY(g.hook - from);
    SetWorld(a, rope, from, along);
    SetWorld(b, rope, g.hook, along);
    rope->Update(update);
}

void ResetGrapple() {
    g = {};
    g_prevFlying = false;
    g_prevHookTime = 0.0f;
    g_hook.Reset();
    g_rope.Reset();
    g_armRoot.reset();
    g_armBones.clear();
}

}  // namespace animation
