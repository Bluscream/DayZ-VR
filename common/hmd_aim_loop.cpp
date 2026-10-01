#include "hmd_aim_loop.hpp"

#include <algorithm>
#include <cmath>

namespace dayz::aim_loop
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        // Gain learning only trusts frames where the game visibly moved; smaller
        // deltas are dominated by tracking noise and count truncation.
        constexpr float kMinLearnCounts = 3.0f;
        constexpr float kMinLearnRadians = 0.0005f;
        constexpr float kLearnRate = 0.08f;
        // The learned gain may not drift more than this factor from the config.
        constexpr float kGainSpan = 16.0f;
        constexpr unsigned kFlipsBeforeBackoff = 3;

        float Clamp(float value, float low, float high) noexcept
        {
            return (std::min)((std::max)(value, low), high);
        }

        void Learn(AxisState& axis, float initialGain, float actual, bool wrap) noexcept
        {
            if (!axis.haveLast || std::fabs(axis.lastCounts) < kMinLearnCounts)
                return;
            float achieved = actual - axis.lastActual;
            if (wrap)
                achieved = WrapAngle(achieved);
            if (std::fabs(achieved) < kMinLearnRadians)
                return;
            const float observed = axis.lastCounts / achieved;
            // Opposite sign means the game moved against the push (external input
            // or a stance limit); never learn a sign flip from that.
            if ((observed < 0.0f) != (initialGain < 0.0f))
                return;
            const float magnitude = std::fabs(initialGain);
            const float sign = initialGain < 0.0f ? -1.0f : 1.0f;
            const float clamped = sign * Clamp(std::fabs(observed), magnitude / kGainSpan,
                magnitude * kGainSpan);
            axis.countsPerRadian += (clamped - axis.countsPerRadian) * kLearnRate;
        }

        float Control(AxisState& axis, const Config& config, float initialGain, float error,
            float actual, bool wrap) noexcept
        {
            if (config.adaptive)
                Learn(axis, initialGain, actual, wrap);
            axis.lastActual = actual;
            axis.haveLast = true;

            // Oscillation guard: repeated sign flips of a still-large error mean
            // the effective loop gain is too high for the current latency.
            if (std::fabs(error) > config.deadband * 4.0f && axis.lastCounts != 0.0f &&
                (error < 0.0f) != (axis.lastError < 0.0f))
            {
                if (++axis.flips >= kFlipsBeforeBackoff)
                {
                    axis.countsPerRadian *= 0.7f;
                    axis.flips = 0;
                }
            }
            else if (std::fabs(error) <= config.deadband)
                axis.flips = 0;
            axis.lastError = error;

            if (std::fabs(error) <= config.deadband)
            {
                axis.lastCounts = 0.0f;
                return 0.0f;
            }
            const float damping = Clamp(config.damping, 0.05f, 1.0f);
            const float limit = (std::max)(config.maxCountsPerFrame, 1.0f);
            const float counts = Clamp(error * axis.countsPerRadian * damping, -limit, limit);
            axis.lastCounts = counts;
            return counts;
        }
    }

    float WrapAngle(float radians) noexcept
    {
        return std::remainder(radians, 2.0f * kPi);
    }

    void Reset(State& state, const Config& config) noexcept
    {
        state = {};
        state.yaw.countsPerRadian = config.yawCountsPerRadian;
        state.pitch.countsPerRadian = config.pitchCountsPerRadian;
        state.initialized = true;
    }

    Output Step(State& state, const Config& config, float desiredYaw, float desiredPitch,
        float actualYaw, float actualPitch) noexcept
    {
        if (!state.initialized)
            Reset(state, config);
        Output output{};
        output.yawError = WrapAngle(desiredYaw - actualYaw);
        output.pitchError = desiredPitch - actualPitch;
        output.yawCounts = Control(state.yaw, config, config.yawCountsPerRadian,
            output.yawError, actualYaw, true);
        output.pitchCounts = Control(state.pitch, config, config.pitchCountsPerRadian,
            output.pitchError, actualPitch, false);
        return output;
    }
}
