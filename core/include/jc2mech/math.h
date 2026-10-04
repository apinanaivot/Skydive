#pragma once

#include <cmath>

namespace jc2 {

// Core works in JC2 conventions: meters, Y-up.
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Normalize(Vec3 v) {
    const float len = Length(v);
    return len > 1e-6f ? v / len : Vec3{};
}

inline Vec3 Horizontal(Vec3 v) { return {v.x, 0.0f, v.z}; }

// Moves `current` toward `target` by at most `maxDelta`.
inline Vec3 MoveTowards(Vec3 current, Vec3 target, float maxDelta) {
    const Vec3 d = target - current;
    const float len = Length(d);
    if (len <= maxDelta || len < 1e-6f) return target;
    return current + d * (maxDelta / len);
}

} // namespace jc2
