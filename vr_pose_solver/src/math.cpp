#include <vr_pose_solver/math.hpp>

namespace vr::pose
{
    Quaternion Quaternion::Normalized(float epsilon) const noexcept
    {
        const float lengthSquared = LengthSquared();
        if (!std::isfinite(lengthSquared) || lengthSquared <= epsilon * epsilon)
            return Identity();
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        return {x * inverseLength, y * inverseLength, z * inverseLength, w * inverseLength};
    }

    Quaternion Quaternion::Inverse(float epsilon) const noexcept
    {
        const float lengthSquared = LengthSquared();
        if (!std::isfinite(lengthSquared) || lengthSquared <= epsilon * epsilon)
            return Identity();
        const Quaternion conjugate = Conjugate();
        return {conjugate.x / lengthSquared, conjugate.y / lengthSquared,
            conjugate.z / lengthSquared, conjugate.w / lengthSquared};
    }

    Vec3 Quaternion::Rotate(const Vec3& value) const noexcept
    {
        const Quaternion q = Normalized();
        const Vec3 imaginary{q.x, q.y, q.z};
        const Vec3 twiceCross = 2.0f * Cross(imaginary, value);
        return value + q.w * twiceCross + Cross(imaginary, twiceCross);
    }

    Quaternion Quaternion::FromAxisAngle(const Vec3& axis, float radians, float epsilon) noexcept
    {
        const Vec3 normalizedAxis = NormalizeOr(axis, {}, epsilon);
        if (normalizedAxis.LengthSquared() <= epsilon * epsilon || !std::isfinite(radians))
            return Identity();
        const float half = radians * 0.5f;
        const float sine = std::sin(half);
        return {normalizedAxis.x * sine, normalizedAxis.y * sine,
            normalizedAxis.z * sine, std::cos(half)};
    }

    Quaternion Quaternion::FromTo(const Vec3& from, const Vec3& to, float epsilon) noexcept
    {
        const Vec3 fromUnit = NormalizeOr(from, {}, epsilon);
        const Vec3 toUnit = NormalizeOr(to, {}, epsilon);
        if (fromUnit.LengthSquared() <= epsilon * epsilon || toUnit.LengthSquared() <= epsilon * epsilon)
            return Identity();

        const float cosine = ClampUnit(Dot(fromUnit, toUnit));
        if (cosine >= 1.0f - epsilon)
            return Identity();
        if (cosine <= -1.0f + epsilon)
            return FromAxisAngle(StableOrthogonal(fromUnit), kPi, epsilon);

        const Vec3 axis = Cross(fromUnit, toUnit);
        return Quaternion{axis.x, axis.y, axis.z, 1.0f + cosine}.Normalized(epsilon);
    }

    Quaternion Quaternion::Slerp(const Quaternion& from, const Quaternion& to, float weight) noexcept
    {
        const float t = std::clamp(weight, 0.0f, 1.0f);
        Quaternion a = from.Normalized();
        Quaternion b = to.Normalized();
        float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (cosine < 0.0f)
        {
            b = {-b.x, -b.y, -b.z, -b.w};
            cosine = -cosine;
        }

        if (cosine > 0.9995f)
        {
            return Quaternion{
                a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
                a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)}.Normalized();
        }

        const float angle = std::acos(ClampUnit(cosine));
        const float sine = std::sin(angle);
        if (std::fabs(sine) <= kDefaultEpsilon)
            return a;
        const float fromWeight = std::sin((1.0f - t) * angle) / sine;
        const float toWeight = std::sin(t * angle) / sine;
        return Quaternion{
            a.x * fromWeight + b.x * toWeight,
            a.y * fromWeight + b.y * toWeight,
            a.z * fromWeight + b.z * toWeight,
            a.w * fromWeight + b.w * toWeight}.Normalized();
    }
}
