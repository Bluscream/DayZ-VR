#pragma once

#include "math.hpp"

namespace vr::pose
{
    struct TwoBoneChain
    {
        Transform start;
        Transform middle;
        Transform end;
    };

    struct TwoBoneTarget
    {
        Vec3 position;
        Quaternion orientation{};
        // A point in the same model space as the chain, not a direction.
        Vec3 pole_position{0.0f, 1.0f, 0.0f};
        float twist_radians{};
        float weight{1.0f};
        bool match_end_orientation{true};
    };

    struct TwoBoneConstraints
    {
        // Bend is zero for a straight chain and approaches pi when folded back.
        float minimum_bend_radians{};
        float maximum_bend_radians{kPi - 1.0e-3f};
        // Maximum model-space rotation applied at the shoulder relative to the
        // authored pose. Pi preserves the unconstrained legacy behavior.
        float maximum_start_correction_radians{kPi};
        // 1 disables soft reach. Values below 1 start easing before full extension.
        float soften_start_ratio{1.0f};
        float epsilon{kDefaultEpsilon};
    };

    enum class SolveStatus : std::uint8_t
    {
        solved,
        invalid_input,
        degenerate_chain
    };

    struct TwoBoneResult
    {
        // Apply start correction to start and all descendants. Then apply middle
        // correction to middle and its descendants. Both are model-space deltas.
        Quaternion start_correction{};
        Quaternion middle_correction{};
        Quaternion end_correction{};

        Vec3 solved_middle_position{};
        Vec3 solved_end_position{};
        float upper_length{};
        float lower_length{};
        float requested_distance{};
        float effective_distance{};
        float bend_radians{};
        bool target_reachable{};
        SolveStatus status{SolveStatus::invalid_input};

        [[nodiscard]] bool Succeeded() const noexcept { return status == SolveStatus::solved; }
    };

    struct TwistDistribution
    {
        Quaternion proximal{};
        Quaternion distal{};
    };

    [[nodiscard]] TwoBoneResult SolveTwoBone(const TwoBoneChain& chain,
        const TwoBoneTarget& target,
        const TwoBoneConstraints& constraints = {}) noexcept;

    // Splits a known axial rotation between roll bones. proximal_weight is clamped
    // to [0, 1], with the remainder assigned to the distal roll bone.
    [[nodiscard]] TwistDistribution DistributeTwist(const Vec3& local_axis,
        float twist_radians, float proximal_weight) noexcept;
}
