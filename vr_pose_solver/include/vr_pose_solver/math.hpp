#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vr::pose
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kDefaultEpsilon = 1.0e-5f;

    struct Vec3
    {
        float x{};
        float y{};
        float z{};

        [[nodiscard]] constexpr Vec3 operator+() const noexcept { return *this; }
        [[nodiscard]] constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }
        [[nodiscard]] constexpr Vec3 operator+(const Vec3& rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
        [[nodiscard]] constexpr Vec3 operator-(const Vec3& rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
        [[nodiscard]] constexpr Vec3 operator*(float scalar) const noexcept { return {x * scalar, y * scalar, z * scalar}; }
        [[nodiscard]] constexpr Vec3 operator/(float scalar) const noexcept { return {x / scalar, y / scalar, z / scalar}; }

        constexpr Vec3& operator+=(const Vec3& rhs) noexcept
        {
            x += rhs.x; y += rhs.y; z += rhs.z;
            return *this;
        }
        constexpr Vec3& operator-=(const Vec3& rhs) noexcept
        {
            x -= rhs.x; y -= rhs.y; z -= rhs.z;
            return *this;
        }

        [[nodiscard]] constexpr float LengthSquared() const noexcept { return x * x + y * y + z * z; }
        [[nodiscard]] float Length() const noexcept { return std::sqrt(LengthSquared()); }
        [[nodiscard]] bool IsFinite() const noexcept
        {
            return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        }
    };

    [[nodiscard]] constexpr Vec3 operator*(float scalar, const Vec3& value) noexcept { return value * scalar; }
    [[nodiscard]] constexpr float Dot(const Vec3& lhs, const Vec3& rhs) noexcept
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }
    [[nodiscard]] constexpr Vec3 Cross(const Vec3& lhs, const Vec3& rhs) noexcept
    {
        return {
            lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
    }
    [[nodiscard]] inline Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback,
        float epsilon = kDefaultEpsilon) noexcept
    {
        const float lengthSquared = value.LengthSquared();
        if (!std::isfinite(lengthSquared) || lengthSquared <= epsilon * epsilon)
            return fallback;
        return value / std::sqrt(lengthSquared);
    }
    [[nodiscard]] inline Vec3 StableOrthogonal(const Vec3& unit) noexcept
    {
        const Vec3 axis = std::fabs(unit.x) < std::fabs(unit.y)
            ? (std::fabs(unit.x) < std::fabs(unit.z) ? Vec3{1, 0, 0} : Vec3{0, 0, 1})
            : (std::fabs(unit.y) < std::fabs(unit.z) ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
        return NormalizeOr(Cross(unit, axis), {0, 1, 0});
    }

    struct Quaternion
    {
        float x{};
        float y{};
        float z{};
        float w{1.0f};

        [[nodiscard]] static constexpr Quaternion Identity() noexcept { return {}; }
        [[nodiscard]] constexpr float LengthSquared() const noexcept { return x * x + y * y + z * z + w * w; }
        [[nodiscard]] bool IsFinite() const noexcept
        {
            return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
        }
        [[nodiscard]] Quaternion Normalized(float epsilon = kDefaultEpsilon) const noexcept;
        [[nodiscard]] constexpr Quaternion Conjugate() const noexcept { return {-x, -y, -z, w}; }
        [[nodiscard]] Quaternion Inverse(float epsilon = kDefaultEpsilon) const noexcept;
        [[nodiscard]] Vec3 Rotate(const Vec3& value) const noexcept;

        [[nodiscard]] static Quaternion FromAxisAngle(const Vec3& axis, float radians,
            float epsilon = kDefaultEpsilon) noexcept;
        [[nodiscard]] static Quaternion FromTo(const Vec3& from, const Vec3& to,
            float epsilon = kDefaultEpsilon) noexcept;
        [[nodiscard]] static Quaternion Slerp(const Quaternion& from, const Quaternion& to,
            float weight) noexcept;
    };

    [[nodiscard]] constexpr Quaternion operator*(const Quaternion& lhs, const Quaternion& rhs) noexcept
    {
        return {
            lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
            lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
            lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z};
    }

    struct Transform
    {
        Vec3 translation{};
        Quaternion rotation{};
    };

    [[nodiscard]] inline float ClampUnit(float value) noexcept
    {
        return std::clamp(value, -1.0f, 1.0f);
    }
}
