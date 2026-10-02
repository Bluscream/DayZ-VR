#include "../common/stereo_state.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace dayz::stereo_state;

    void Expect(bool condition, const char* reason)
    {
        if (!condition)
            throw std::runtime_error(reason);
    }

    void TestInvalidation()
    {
        UpdateEyePositions(-0.03f, 0, 0, 0.03f, 0, 0);
        UpdateHmdOrientation(0, 0, 0, 1);
        UpdateHmdPosition(1, 2, 3);
        UpdateCameraDirections(0, 0, -1, 0, 0, -1);
        UpdateAimOrientation(0, 1, 0, 0, true);
        InvalidateTracking();
        Expect(!GetEyePositions().valid && !GetHmdOrientation().valid &&
            !GetHmdPosition().valid && !GetCameraDirections().valid &&
            !GetAimOrientation().valid, "tracking loss left a valid stale pose");
        UpdateHmdOrientation(0, 0, 0, 1);
        Expect(GetHmdOrientation().valid && !GetAimOrientation().valid,
            "HMD recovery incorrectly revived the controller pose");
    }

    void TestCoherentPosePublication()
    {
        InvalidateTracking();
        std::atomic<bool> start{false};
        std::atomic<bool> failed{false};
        const auto writer = [&start](float sign) {
            start.wait(false);
            for (int i = 0; i < 30000; ++i)
            {
                UpdateAimOrientation(sign, 2 * sign, 3 * sign, 4 * sign, true);
                UpdateHmdOrientation(sign, 2 * sign, 3 * sign, 4 * sign);
                UpdateHmdPosition(sign, 2 * sign, 3 * sign);
                UpdateEyePositions(sign, 0, 0, -sign, 0, 0);
                UpdateCameraDirections(sign, 2 * sign, 3 * sign, -sign, -2 * sign, -3 * sign);
            }
        };
        std::jthread first(writer, 1.0f);
        std::jthread second(writer, -1.0f);
        std::jthread reader([&] {
            start.wait(false);
            for (int i = 0; i < 30000; ++i)
            {
                for (const auto pose : {GetAimOrientation(), GetHmdOrientation()})
                    if (pose.valid && (pose.y != 2 * pose.x || pose.z != 3 * pose.x ||
                        pose.w != 4 * pose.x))
                        failed = true;
                const auto position = GetHmdPosition();
                if (position.valid && (position.y != 2 * position.x || position.z != 3 * position.x))
                    failed = true;
                const auto eyes = GetEyePositions();
                if (eyes.valid && eyes.leftX != -eyes.rightX)
                    failed = true;
                const auto directions = GetCameraDirections();
                if (directions.valid && (directions.nativeY != 2 * directions.nativeX ||
                    directions.nativeZ != 3 * directions.nativeX ||
                    directions.renderX != -directions.nativeX ||
                    directions.renderY != -directions.nativeY ||
                    directions.renderZ != -directions.nativeZ))
                    failed = true;
            }
        });
        start = true;
        start.notify_all();
        first.join();
        second.join();
        reader.join();
        Expect(!failed, "reader observed pose components from different updates");
    }
}

int main()
{
    try
    {
        TestInvalidation();
        TestCoherentPosePublication();
    }
    catch (const std::exception& error)
    {
        std::cerr << "stereo_state_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "stereo_state_test: all checks passed\n";
}
