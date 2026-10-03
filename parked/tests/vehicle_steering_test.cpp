// Host test for the two-hand steering wheel:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/vehicle_steering_test.cpp common/vehicle_steering.cpp -o build/vehicle_steering_test
#include "../common/vehicle_steering.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dayz::vehicle_steering;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }

    Hands Wheel(float leftY, float rightY, float width = 0.4f)
    {
        Hands hands;
        hands.leftX = -width / 2; hands.leftY = leftY; hands.leftZ = -0.4f;
        hands.rightX = width / 2; hands.rightY = rightY; hands.rightZ = -0.4f;
        hands.leftValid = hands.rightValid = true;
        return hands;
    }

    bool Near(float a, float b, float tolerance = 0.01f)
    {
        return std::fabs(a - b) <= tolerance;
    }
}

int main()
{
    try
    {
        Config config;
        Expect(!Compute(Hands{}, config).valid, "untracked hands are invalid");
        {
            Hands oneHand = Wheel(0.0f, 0.0f);
            oneHand.leftValid = false;
            Expect(!Compute(oneHand, config).valid, "one hand is not a wheel");
        }
        Expect(!Compute(Wheel(0.0f, 0.0f, 0.05f), config).valid, "hands together are not a wheel");
        {
            const Result level = Compute(Wheel(0.0f, 0.0f), config);
            Expect(level.valid && level.steer == 0.0f && Near(level.wheelDegrees, 0.0f), "level rim is centred");
        }
        {
            // Right hand 0.4 m lower than the left across 0.4 m: 45 degrees clockwise = half lock.
            const Result right = Compute(Wheel(0.2f, -0.2f), config);
            Expect(right.valid && Near(right.wheelDegrees, 45.0f) && right.steer > 0.0f, "clockwise tilt steers right");
            Expect(Near(right.steer, (0.5f - 0.05f) / 0.95f), "linear map after the deadzone");
            const Result left = Compute(Wheel(-0.2f, 0.2f), config);
            Expect(Near(left.steer, -right.steer), "symmetric to the left");
        }
        {
            Config narrow = config;
            narrow.wheelMaxDegrees = 45.0f;
            Expect(Near(Compute(Wheel(0.5f, -0.5f, 0.05f), narrow).steer, 1.0f), "tilt beyond the wheel range clamps at full lock");
        }
        {
            Config wide = config;
            wide.wheelMaxDegrees = 180.0f;
            Expect(Compute(Wheel(0.2f, -0.2f), wide).steer < Compute(Wheel(0.2f, -0.2f), config).steer,
                "larger wheel range gives gentler steering");
        }
        {
            // 2 degrees of tilt is inside the 5 % deadzone of a 90 degree wheel.
            const Result tiny = Compute(Wheel(0.0f, -0.4f * std::tan(2.0f / 57.2957795f)), config);
            Expect(tiny.valid && tiny.steer == 0.0f, "deadzone swallows small tilts");
        }
        {
            Config inverted = config;
            inverted.invert = true;
            Expect(Compute(Wheel(0.2f, -0.2f), inverted).steer < 0.0f, "invert flips the sign");
        }
        {
            Hands nan = Wheel(0.0f, 0.0f);
            nan.rightY = std::numeric_limits<float>::quiet_NaN();
            Expect(!Compute(nan, config).valid, "NaN position is invalid");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "vehicle_steering_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "vehicle_steering_test: all checks passed\n";
    return 0;
}
