#pragma once

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

// Small pose helpers shared by the plugin's hand-posed parts (visuals.cpp: canopy, Rico's chute
// pose; grapple.cpp: the grapple arm): quaternions (x, y, z, w), bone poses, and the clip file
// format the build tools write (canopy_anim.bin, rico_arm.bin; see tools/anim/build_models.py).
namespace animation {

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Quat Nlerp(Quat a, const Quat& b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) a = {-a.x, -a.y, -a.z, -a.w};
    Quat r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return {r.x / n, r.y / n, r.z / n, r.w / n};
}

inline RE::NiMatrix3 ToMatrix(const Quat& q) {
    RE::NiMatrix3 m;
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m.entry[0][0] = 1 - 2 * (y * y + z * z); m.entry[0][1] = 2 * (x * y - z * w);     m.entry[0][2] = 2 * (x * z + y * w);
    m.entry[1][0] = 2 * (x * y + z * w);     m.entry[1][1] = 1 - 2 * (x * x + z * z); m.entry[1][2] = 2 * (y * z - x * w);
    m.entry[2][0] = 2 * (x * z - y * w);     m.entry[2][1] = 2 * (y * z + x * w);     m.entry[2][2] = 1 - 2 * (x * x + y * y);
    return m;
}

struct Pose {
    RE::NiPoint3 t;
    Quat q;
    float s = 1.0f;  // uniform scale (Ni nodes have no per-axis scale)
};

inline Pose Lerp(const Pose& a, const Pose& b, float t) { return {a.t + (b.t - a.t) * t, Nlerp(a.q, b.q, t), a.s + (b.s - a.s) * t}; }

inline Quat FromMatrix(const RE::NiMatrix3& m) {
    const auto& e = m.entry;
    Quat q;
    const float trace = e[0][0] + e[1][1] + e[2][2];
    if (trace > 0.0f) {
        const float k = 0.5f / std::sqrt(trace + 1.0f);
        q = {(e[2][1] - e[1][2]) * k, (e[0][2] - e[2][0]) * k, (e[1][0] - e[0][1]) * k, 0.25f / k};
    } else if (e[0][0] > e[1][1] && e[0][0] > e[2][2]) {
        const float k = 2.0f * std::sqrt(1.0f + e[0][0] - e[1][1] - e[2][2]);
        q = {0.25f * k, (e[0][1] + e[1][0]) / k, (e[0][2] + e[2][0]) / k, (e[2][1] - e[1][2]) / k};
    } else if (e[1][1] > e[2][2]) {
        const float k = 2.0f * std::sqrt(1.0f + e[1][1] - e[0][0] - e[2][2]);
        q = {(e[0][1] + e[1][0]) / k, 0.25f * k, (e[1][2] + e[2][1]) / k, (e[0][2] - e[2][0]) / k};
    } else {
        const float k = 2.0f * std::sqrt(1.0f + e[2][2] - e[0][0] - e[1][1]);
        q = {(e[0][2] + e[2][0]) / k, (e[1][2] + e[2][1]) / k, 0.25f * k, (e[1][0] - e[0][1]) / k};
    }
    return q;
}

inline Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// An additive delta (from identity) at weight w on top of a pose (Havok: rotation post-multiplied,
// translation added).
inline Pose AddDelta(const Pose& base, const Pose& delta, float w) {
    const Quat q = Nlerp(Quat{}, delta.q, w);
    return {base.t + delta.t * w, Mul(base.q, q), base.s * (1.0f + (delta.s - 1.0f) * w)};
}

// Rotation by angle about a unit axis.
inline RE::NiMatrix3 AxisAngle(const RE::NiPoint3& axis, float angle) {
    const float h = 0.5f * angle, s = std::sin(h);
    return ToMatrix(Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(h)});
}

// Turns `bone` (rigidly, with its children) so the direction from..to points along want.
inline void TurnBone(RE::NiAVObject* bone, RE::NiPoint3 now, RE::NiPoint3 want, float w) {
    if (now.Unitize() <= 1e-3f || want.Unitize() <= 1e-3f || !bone->parent) return;
    RE::NiPoint3 axis = now.Cross(want);
    const float s = axis.Unitize();
    if (s <= 1e-5f) return;
    const RE::NiMatrix3 q = AxisAngle(axis, std::atan2(s, now.Dot(want)) * w);
    const RE::NiMatrix3& pr = bone->parent->world.rotate;
    bone->local.rotate = pr.Transpose() * q * pr * bone->local.rotate;
    RE::NiUpdateData update;
    bone->Update(update);
}

// Recomputes the skeleton's world transforms from its locals, from the root bone down. Every
// pose change ends with this: refreshing only the changed bone's subtree left arm bones with
// stale world transforms under the chute (the forearm stayed on the handle).
inline void UpdateSkeleton(RE::NiAVObject* root3d) {
    static const RE::BSFixedString kRoot("NPC Root [Root]");
    RE::NiAVObject* bone = root3d->GetObjectByName(kRoot);
    RE::NiUpdateData update;
    (bone ? bone : root3d)->Update(update);
}

// A world transform as the local transform of a child of `parent`.
inline void SetWorld(RE::NiAVObject* node, const RE::NiAVObject* parent, const RE::NiPoint3& pos, const RE::NiMatrix3& rot,
                     float scale = 1.0f) {
    const RE::NiMatrix3 inv = parent->world.rotate.Transpose();
    const float s = parent->world.scale > 0.0f ? parent->world.scale : 1.0f;
    node->local.translate = (inv * (pos - parent->world.translate)) / s;
    node->local.rotate = inv * rot;
    node->local.scale = scale / s;
}

inline bool FirstPerson() {
    const RE::PlayerCamera* cam = RE::PlayerCamera::GetSingleton();
    return cam && cam->IsInFirstPerson();
}

// Where the plugin's own models (canopy, hook, wire) hang: the player's third-person root, or in
// first person, where Skyrim hides that root and everything under it, the player's cell (placed
// in world space by SetWorld).
inline RE::NiNode* ModelParent(RE::PlayerCharacter* player) {
    if (!player) return nullptr;
    if (FirstPerson()) {
        RE::TESObjectCELL* cell = player->GetParentCell();
        const auto* loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
        return loaded ? loaded->cell3D.get() : nullptr;
    }
    RE::NiAVObject* root3d = player->Get3D(false);
    return root3d ? root3d->AsNode() : nullptr;
}

// canopy_anim.bin / rico_arm.bin ("JC2C")
struct ClipSet {
    struct Clip {
        std::string name;
        std::uint32_t frames = 0;
        float frameDuration = 1.0f / 30.0f;
        std::vector<Pose> poses;  // frames x bones
        float Duration() const { return (frames - 1) * frameDuration; }
    };
    std::vector<std::string> bones;
    std::vector<Clip> clips;
    bool loaded = false;

    bool Load(const char* path) {
        loaded = Read(path);
        if (!loaded) {
            bones.clear();  // nothing half-read stays for Find
            clips.clear();
        }
        return loaded;
    }
    static constexpr std::uint32_t kMaxBones = 1024, kMaxClips = 256, kMaxFrames = 100000;

private:
    bool Read(const char* path) {
        bones.clear();
        clips.clear();
        std::ifstream in(path, std::ios::binary);
        auto u32 = [&] { std::uint32_t v = 0; in.read(reinterpret_cast<char*>(&v), 4); return v; };
        auto str = [&] { std::uint8_t n = 0; in.read(reinterpret_cast<char*>(&n), 1); std::string s(n, '\0'); in.read(s.data(), n); return s; };
        char magic[4] = {};
        in.read(magic, 4);
        const std::uint32_t version = in ? u32() : 0;
        if (!in || std::string_view(magic, 4) != "JC2C" || (version != 1 && version != 2)) return false;
        // Counts are checked before anything is sized by them: a damaged file must not allocate
        // gigabytes or leave a clip with no frames (Sample indexes frame 0).
        const std::uint32_t numBones = u32();
        if (!in || numBones == 0 || numBones > kMaxBones) return false;
        bones.resize(numBones);
        for (auto& b : bones) b = str();
        const std::uint32_t numClips = u32();
        if (!in || numClips > kMaxClips) return false;
        clips.resize(numClips);
        for (auto& c : clips) {
            c.name = str();
            c.frames = u32();
            in.read(reinterpret_cast<char*>(&c.frameDuration), 4);
            if (!in || c.frames == 0 || c.frames > kMaxFrames || !(c.frameDuration > 0.0f && c.frameDuration < 10.0f)) return false;
            c.poses.resize(static_cast<std::size_t>(c.frames) * bones.size());
            for (auto& p : c.poses) {
                float v[8] = {0, 0, 0, 0, 0, 0, 1, 1};
                in.read(reinterpret_cast<char*>(v), version >= 2 ? 32 : 28);  // v2 adds the scale
                p = {{v[0], v[1], v[2]}, {v[3], v[4], v[5], v[6]}, v[7]};
            }
            if (!in) return false;
        }
        return true;
    }

public:
    const Clip* Find(std::string_view name) const {
        for (const auto& c : clips) {
            if (c.name == name) return &c;
        }
        return nullptr;
    }

    // Bone pose at a fractional frame (clamped).
    static Pose Sample(const Clip& c, std::size_t bone, std::size_t numBones, float frame) {
        frame = std::clamp(frame, 0.0f, static_cast<float>(c.frames - 1));
        const auto f0 = static_cast<std::size_t>(frame);
        const std::size_t f1 = std::min<std::size_t>(f0 + 1, c.frames - 1);
        return Lerp(c.poses[f0 * numBones + bone], c.poses[f1 * numBones + bone], frame - static_cast<float>(f0));
    }
};

}  // namespace animation
