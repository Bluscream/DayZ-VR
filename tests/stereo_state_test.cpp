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

    void TestFrameRecords()
    {
        InvalidateTracking();
        FrameRecord record{};
        Expect(!PresentedRecord(0, record), "no record before the first game frame");
        Expect(!CurrentFrame().valid, "no current frame before the first game frame");
        UpdateHmdOrientation(0, 0.5f, 0, 0.5f);
        UpdateHmdPosition(1, 2, 3);
        ViewPose left{}; left.px = -0.03f; left.fovLeft = -0.8f;
        ViewPose right{}; right.px = 0.03f; right.fovRight = 0.8f;
        UpdateHmdViews(left, right);
        const std::uint64_t first = BeginGameFrame(true);
        const FrameRecord current = CurrentFrame();
        Expect(current.valid && current.index == first && current.position.x == 1,
            "current frame carries the frozen sample");
        Expect(current.views[0].px == -0.03f && current.views[1].fovRight == 0.8f,
            "current frame carries both view poses");
        Expect(RenderedEye() == (first & 1u), "eye follows the frame index when alternating");
        // A newer sample must not change the frozen frame.
        UpdateHmdPosition(9, 9, 9);
        Expect(CurrentFrame().position.x == 1, "frame sample is frozen until the next frame");
        const std::uint64_t second = BeginGameFrame(true);
        Expect(second == first + 1 && CurrentFrame().position.x == 9, "next frame takes the new sample");
        Expect(RenderedEye() != (first & 1u), "eye alternates per frame");
        Expect(PresentedRecord(0, record) && record.index == second, "lag 0 is the latest frame");
        Expect(PresentedRecord(1, record) && record.index == first && record.position.x == 1,
            "lag 1 is the previous frame with its own sample");
        Expect(!PresentedRecord(5, record), "a lag beyond the filed frames is unknown");
        Expect(LatestFrameIndex() == second, "latest index reported");
        for (int i = 0; i < 40; ++i)
            BeginGameFrame(false);
        Expect(RenderedEye() == 0, "non-alternating frames stay on eye 0");
        Expect(PresentedRecord(15, record) && record.index == LatestFrameIndex() - 15, "ring keeps 16 frames");
        Expect(!PresentedRecord(16, record), "ring depth is 16");
        InvalidateTracking();
        Expect(!CurrentFrame().valid, "invalidation clears the current frame");
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
        TestFrameRecords();
        TestCoherentPosePublication();
    }
    catch (const std::exception& error)
    {
        std::cerr << "stereo_state_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "stereo_state_test: all checks passed\n";
}
