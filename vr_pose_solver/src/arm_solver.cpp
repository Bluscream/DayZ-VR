#include <vr_pose_solver/arm_solver.hpp>

#include <algorithm>
#include <cmath>

namespace vr::pose
{
    namespace
    {
        bool IsValid(const TwoBoneChain& chain, const TwoBoneTarget& target,
            const TwoBoneConstraints& constraints) noexcept
        {
            return chain.start.translation.IsFinite() && chain.middle.translation.IsFinite() &&
                chain.end.translation.IsFinite() && chain.start.rotation.IsFinite() &&
                chain.middle.rotation.IsFinite() && chain.end.rotation.IsFinite() &&
                target.position.IsFinite() && target.orientation.IsFinite() &&
                target.pole_position.IsFinite() && std::isfinite(target.twist_radians) &&
                std::isfinite(target.weight) && std::isfinite(constraints.minimum_bend_radians) &&
                std::isfinite(constraints.maximum_bend_radians) &&
                std::isfinite(constraints.soften_start_ratio) &&
                std::isfinite(constraints.epsilon) && constraints.epsilon > 0.0f;
        }

        float SoftReach(float distance, float maximumDistance, float ratio,
            float epsilon) noexcept
        {
            const float startRatio = std::clamp(ratio, 0.0f, 1.0f);
            if (startRatio >= 1.0f - epsilon)
                return distance;
            const float start = maximumDistance * startRatio;
            if (distance <= start)
                return distance;
            const float range = maximumDistance - start;
            if (range <= epsilon)
                return std::min(distance, maximumDistance);
            const float normalized = std::clamp((distance - start) / range, 0.0f, 1.0f);
            return start + range * (1.0f - std::exp(-3.0f * normalized));
        }
    }

    TwoBoneResult SolveTwoBone(const TwoBoneChain& chain, const TwoBoneTarget& target,
        const TwoBoneConstraints& constraints) noexcept
    {
        TwoBoneResult result{};
        if (!IsValid(chain, target, constraints))
            return result;

        const float epsilon = constraints.epsilon;
        const Vec3 originalUpper = chain.middle.translation - chain.start.translation;
        const Vec3 originalLower = chain.end.translation - chain.middle.translation;
        result.upper_length = originalUpper.Length();
        result.lower_length = originalLower.Length();
        if (result.upper_length <= epsilon || result.lower_length <= epsilon)
        {
            result.status = SolveStatus::degenerate_chain;
            return result;
        }

        const Vec3 requested = target.position - chain.start.translation;
        result.requested_distance = requested.Length();
        const Vec3 fallbackForward = NormalizeOr(chain.end.translation - chain.start.translation,
            NormalizeOr(originalUpper, {1, 0, 0}, epsilon), epsilon);
        const Vec3 forward = NormalizeOr(requested, fallbackForward, epsilon);

        const float geometricMinimum = std::fabs(result.upper_length - result.lower_length);
        const float geometricMaximum = result.upper_length + result.lower_length;
        const float safeMinimum = std::min(geometricMaximum - epsilon,
            geometricMinimum + epsilon);
        const float safeMaximum = std::max(safeMinimum, geometricMaximum - epsilon);
        float distance = std::clamp(result.requested_distance, safeMinimum, safeMaximum);
        const bool withinGeometricReach = result.requested_distance >= geometricMinimum - epsilon &&
            result.requested_distance <= geometricMaximum + epsilon;

        distance = SoftReach(distance, safeMaximum, constraints.soften_start_ratio, epsilon);

        float minimumBend = std::clamp(constraints.minimum_bend_radians, 0.0f, kPi);
        float maximumBend = std::clamp(constraints.maximum_bend_radians, 0.0f, kPi);
        if (minimumBend > maximumBend)
            std::swap(minimumBend, maximumBend);
        const float bendCosine = ClampUnit((distance * distance -
            result.upper_length * result.upper_length - result.lower_length * result.lower_length) /
            (2.0f * result.upper_length * result.lower_length));
        const float requestedBend = std::acos(bendCosine);
        result.bend_radians = std::clamp(requestedBend, minimumBend, maximumBend);
        if (std::fabs(result.bend_radians - requestedBend) > epsilon)
        {
            const float distanceSquared = result.upper_length * result.upper_length +
                result.lower_length * result.lower_length + 2.0f * result.upper_length *
                result.lower_length * std::cos(result.bend_radians);
            distance = std::sqrt(std::max(0.0f, distanceSquared));
        }
        result.effective_distance = distance;
        result.target_reachable = withinGeometricReach &&
            std::fabs(distance - result.requested_distance) <= epsilon * 4.0f;

        Vec3 bendDirection = target.pole_position - chain.start.translation;
        bendDirection -= forward * Dot(bendDirection, forward);
        if (bendDirection.LengthSquared() <= epsilon * epsilon)
        {
            bendDirection = originalUpper - forward * Dot(originalUpper, forward);
            if (bendDirection.LengthSquared() <= epsilon * epsilon)
                bendDirection = StableOrthogonal(forward);
        }
        bendDirection = NormalizeOr(bendDirection, StableOrthogonal(forward), epsilon);
        if (std::fabs(target.twist_radians) > epsilon)
            bendDirection = Quaternion::FromAxisAngle(forward, target.twist_radians, epsilon).Rotate(bendDirection);

        const float shoulderCosine = ClampUnit((result.upper_length * result.upper_length +
            distance * distance - result.lower_length * result.lower_length) /
            (2.0f * result.upper_length * std::max(distance, epsilon)));
        const float shoulderSine = std::sqrt(std::max(0.0f, 1.0f - shoulderCosine * shoulderCosine));
        const Vec3 fullMiddle = chain.start.translation +
            forward * (shoulderCosine * result.upper_length) +
            bendDirection * (shoulderSine * result.upper_length);
        const Vec3 fullEnd = chain.start.translation + forward * distance;

        const Quaternion fullStartCorrection = Quaternion::FromTo(originalUpper,
            fullMiddle - chain.start.translation, epsilon);
        const Vec3 upperRotatedLower = fullStartCorrection.Rotate(originalLower);
        const Quaternion fullMiddleCorrection = Quaternion::FromTo(upperRotatedLower,
            fullEnd - fullMiddle, epsilon);

        const float weight = std::clamp(target.weight, 0.0f, 1.0f);
        result.start_correction = Quaternion::Slerp(Quaternion::Identity(), fullStartCorrection, weight);
        result.middle_correction = Quaternion::Slerp(Quaternion::Identity(), fullMiddleCorrection, weight);

        result.solved_middle_position = chain.start.translation + result.start_correction.Rotate(originalUpper);
        const Vec3 lowerAfterStart = result.start_correction.Rotate(originalLower);
        result.solved_end_position = result.solved_middle_position +
            result.middle_correction.Rotate(lowerAfterStart);

        if (target.match_end_orientation)
        {
            const Quaternion inheritedEnd = (result.middle_correction * result.start_correction *
                chain.end.rotation).Normalized(epsilon);
            const Quaternion fullEndCorrection = (target.orientation.Normalized(epsilon) *
                inheritedEnd.Inverse(epsilon)).Normalized(epsilon);
            result.end_correction = Quaternion::Slerp(Quaternion::Identity(), fullEndCorrection, weight);
        }

        result.status = SolveStatus::solved;
        return result;
    }

    TwistDistribution DistributeTwist(const Vec3& localAxis, float twistRadians,
        float proximalWeight) noexcept
    {
        const float weight = std::clamp(proximalWeight, 0.0f, 1.0f);
        return {
            Quaternion::FromAxisAngle(localAxis, twistRadians * weight),
            Quaternion::FromAxisAngle(localAxis, twistRadians * (1.0f - weight))};
    }
}
