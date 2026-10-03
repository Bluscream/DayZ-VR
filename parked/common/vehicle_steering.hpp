#pragma once

// Two-hand "steering wheel" from controller grip positions: the line between the
// left and right grips is the wheel's rim; its tilt (right hand lower than the left
// = wheel turned clockwise = steer right) maps linearly onto DayZ's steering value
// in [-1, 1]. Pure and host-testable; the OpenXR host publishes the result through
// the script bridge (vr.txt steer=) and the Enforce side applies it with
// Car.SetSteering while the local player drives.
namespace dayz::vehicle_steering
{
    struct Config
    {
        float wheelMaxDegrees{90.0f};  // rim tilt that means full lock
        float deadzone{0.05f};         // fraction of full lock ignored around centre
        bool invert{false};
    };

    struct Hands
    {
        float leftX{}, leftY{}, leftZ{};
        float rightX{}, rightY{}, rightZ{};
        bool leftValid{};
        bool rightValid{};
    };

    struct Result
    {
        float steer{};         // -1 (full left) .. 1 (full right)
        float wheelDegrees{};  // signed rim tilt, positive = clockwise
        bool valid{};          // both hands tracked and far enough apart
    };

    Result Compute(const Hands& hands, const Config& config) noexcept;
}
