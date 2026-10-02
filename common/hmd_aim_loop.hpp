#pragma once

// Closed-loop head aim: instead of converting HMD deltas to mouse counts with a
// fixed scale, each frame compares where the HMD points with where DayZ's own
// camera actually points and injects just enough mouse counts to close the gap.
//
// This is needed because DayZ scales mouse deltas by frame time: the same scale
// gives ~0.8x head tracking at 67 fps and ~2.9x at 15 fps, so no constant can be
// right. The loop is a damped proportional controller with an online estimate
// of the game's counts-per-radian, so it also absorbs sensitivity settings and
// stance-dependent turn limits without integral wind-up.
//
// Pure functions, no Windows dependencies: tests/hmd_aim_loop_test.cpp drives it
// with a simulated game.
namespace dayz::aim_loop
{
    struct Config
    {
        // Initial guess of signed mouse counts per radian for each axis (the old
        // hmd_mouse_*_scale values keep working as the starting point).
        float yawCountsPerRadian{-600.0f};
        float pitchCountsPerRadian{-600.0f};
        // Fraction of the remaining error corrected per frame. Below 1 keeps the
        // loop stable with the one or two frames of input latency DayZ adds.
        float damping{0.5f};
        // Hard limit on counts injected per frame per axis.
        float maxCountsPerFrame{400.0f};
        // Errors below this (radians) are ignored, so the camera is not jittered
        // by tracking noise once it is on target.
        float deadband{0.0015f};
        // Learn counts-per-radian from what the game actually did last frame.
        bool adaptive{true};
    };

    struct AxisState
    {
        float countsPerRadian{};
        float lastCounts{};
        float lastActual{};
        bool haveLast{};
        // Consecutive frames where the error changed sign while still large;
        // used to detect oscillation and back the gain off.
        unsigned flips{};
        float lastError{};
        // Consecutive frames at the output limit without the error shrinking: the
        // target is unreachable (stance/pitch clamp, turn-rate cap) and hammering
        // the limit only makes the view judder.
        unsigned saturatedFrames{};
        float saturatedStartError{};
        bool stalled{};
    };

    struct State
    {
        AxisState yaw;
        AxisState pitch;
        bool initialized{};
    };

    struct Output
    {
        float yawCounts{};
        float pitchCounts{};
        float yawError{};
        float pitchError{};
    };

    void Reset(State& state, const Config& config) noexcept;

    // desired*/actual* are angles in radians in the same frame (OpenXR space).
    // Yaw errors are wrapped to [-pi, pi]; pitch is treated as absolute.
    Output Step(State& state, const Config& config, float desiredYaw, float desiredPitch,
        float actualYaw, float actualPitch) noexcept;

    float WrapAngle(float radians) noexcept;
}
