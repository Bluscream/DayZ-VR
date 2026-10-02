#include "vehicle_steering.hpp"

#include <algorithm>
#include <cmath>

namespace dayz::vehicle_steering
{
    namespace
    {
        constexpr float kMinHandDistance = 0.12f;  // metres; closer than this is not a wheel grip
        constexpr float kRadToDeg = 57.2957795f;
    }

    Result Compute(const Hands& hands, const Config& config) noexcept
    {
        Result result{};
        if (!hands.leftValid || !hands.rightValid)
            return result;
        const float dx = hands.rightX - hands.leftX;
        const float dy = hands.rightY - hands.leftY;
        const float dz = hands.rightZ - hands.leftZ;
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz))
            return result;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        if (std::sqrt(horizontal * horizontal + dy * dy) < kMinHandDistance)
            return result;
        // Right hand below the left: the rim turned clockwise, steer right (positive).
        const float tilt = std::atan2(-dy, horizontal) * kRadToDeg;
        const float maxDegrees = (std::max)(1.0f, config.wheelMaxDegrees);
        float steer = (std::clamp)(tilt / maxDegrees, -1.0f, 1.0f);
        const float deadzone = (std::clamp)(config.deadzone, 0.0f, 0.9f);
        if (std::fabs(steer) <= deadzone)
            steer = 0.0f;
        else
            steer = (steer - (steer > 0.0f ? deadzone : -deadzone)) / (1.0f - deadzone);
        if (config.invert)
            steer = -steer;
        result.steer = steer;
        result.wheelDegrees = tilt;
        result.valid = true;
        return result;
    }
}
