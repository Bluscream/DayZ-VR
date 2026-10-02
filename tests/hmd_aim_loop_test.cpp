// Native test for the closed-loop head aim. Build and run on the host:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/hmd_aim_loop_test.cpp common/hmd_aim_loop.cpp -o build/hmd_aim_loop_test
#include "../common/hmd_aim_loop.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dayz::aim_loop;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }

    // A game whose true counts-per-radian differs from the configured guess and
    // that applies injected counts one frame late, like DayZ's input path.
    struct FakeGame
    {
        float trueCountsPerRadian;
        float yaw{};
        float pitch{};
        float pendingYaw{};
        float pendingPitch{};
        float pitchLimit{1.5f};
        void Apply(const Output& out)
        {
            yaw += WrapAngle(pendingYaw / trueCountsPerRadian);
            pitch = (std::min)((std::max)(pitch + pendingPitch / trueCountsPerRadian, -pitchLimit), pitchLimit);
            pendingYaw = out.yawCounts;
            pendingPitch = out.pitchCounts;
        }
    };

    // Returns frames needed until both errors stay inside tolerance.
    int Converge(FakeGame& game, State& state, const Config& config, float desiredYaw,
        float desiredPitch, float tolerance, int maxFrames)
    {
        int settled = 0;
        for (int frame = 0; frame < maxFrames; ++frame)
        {
            const Output out = Step(state, config, desiredYaw, desiredPitch, game.yaw, game.pitch);
            game.Apply(out);
            if (std::fabs(WrapAngle(desiredYaw - game.yaw)) < tolerance &&
                std::fabs(desiredPitch - game.pitch) < tolerance)
            {
                if (++settled >= 5)
                    return frame;
            }
            else
                settled = 0;
        }
        return -1;
    }

    void TestConvergesWithWrongGain()
    {
        for (float trueGain : {-150.0f, -600.0f, -2400.0f})
        {
            FakeGame game{trueGain};
            Config config{};
            State state{};
            Reset(state, config);
            const int frames = Converge(game, state, config, 0.8f, -0.3f, 0.01f, 300);
            Expect(frames >= 0, "did not converge");
            Expect(frames < 120, "converged too slowly");
            Expect(std::fabs(state.yaw.countsPerRadian - trueGain) < std::fabs(trueGain) * 0.5f,
                "yaw gain not learned");
        }
    }

    void TestWrapAround()
    {
        FakeGame game{-600.0f};
        game.yaw = 3.0f;
        Config config{};
        State state{};
        Expect(Converge(game, state, config, -3.0f, 0.0f, 0.01f, 300) >= 0, "wrap path");
    }

    void TestPitchLimitNoWindup()
    {
        FakeGame game{-600.0f};
        Config config{};
        State state{};
        for (int frame = 0; frame < 200; ++frame)
            game.Apply(Step(state, config, 0.0f, 2.0f, game.yaw, game.pitch));
        Expect(std::fabs(game.pitch - game.pitchLimit) < 0.01f, "held at limit");
        // Releasing the target must come back promptly: no integral wind-up.
        Expect(Converge(game, state, config, 0.0f, 0.0f, 0.01f, 150) >= 0, "release from limit");
    }

    void TestDeadbandSilence()
    {
        Config config{};
        State state{};
        Reset(state, config);
        const Output out = Step(state, config, 0.001f, -0.001f, 0.0f, 0.0f);
        Expect(out.yawCounts == 0.0f && out.pitchCounts == 0.0f, "deadband");
    }
}

namespace
{
    // A game that caps how far the view may turn per frame (DayZ's turn-rate cap):
    // saturated pushes must not inflate the learned gain, and the loop must still
    // settle within tolerance afterwards.
    void TestTurnRateCapDoesNotInflateGain()
    {
        Config config{};
        config.yawCountsPerRadian = -600.0f;
        config.pitchCountsPerRadian = -600.0f;
        config.maxCountsPerFrame = 400.0f;
        State state{};
        const float trueCountsPerRadian = -3600.0f;
        const float maxTurnPerFrame = 0.02f;
        float yaw = 0.0f;
        float pending = 0.0f;
        Output out{};
        for (int frame = 0; frame < 600; ++frame)
        {
            float delta = pending / trueCountsPerRadian;
            delta = (std::min)((std::max)(delta, -maxTurnPerFrame), maxTurnPerFrame);
            yaw = WrapAngle(yaw + delta);
            out = Step(state, config, 2.2f, 0.0f, yaw, 0.0f);
            pending = out.yawCounts;
        }
        Expect(std::fabs(out.yawError) < 0.01f, "turn-rate-capped game must still converge");
        Expect(std::fabs(state.yaw.countsPerRadian) < std::fabs(trueCountsPerRadian) * 1.5f,
            "saturated frames must not inflate the learned gain");
    }

    // A target the game can never reach (beyond its pitch clamp): after a short
    // saturated run the loop must back off instead of pushing the limit forever.
    void TestUnreachableTargetBacksOff()
    {
        Config config{};
        config.yawCountsPerRadian = -600.0f;
        config.pitchCountsPerRadian = -600.0f;
        config.maxCountsPerFrame = 400.0f;
        State state{};
        FakeGame game{-600.0f};
        game.pitchLimit = 0.5f;
        Output out{};
        for (int frame = 0; frame < 200; ++frame)
        {
            out = Step(state, config, 0.0f, 1.4f, game.yaw, game.pitch);
            game.Apply(out);
        }
        Expect(std::fabs(out.pitchCounts) <= config.maxCountsPerFrame * 0.125f + 0.001f,
            "unreachable target must reduce the output to the stall share");
        // Once the target comes back inside the clamp the loop follows it again.
        for (int frame = 0; frame < 200; ++frame)
        {
            out = Step(state, config, 0.0f, 0.2f, game.yaw, game.pitch);
            game.Apply(out);
        }
        Expect(std::fabs(out.pitchError) < 0.02f, "loop must recover after a stall");
    }
}

int main()
{
    try
    {
        TestConvergesWithWrongGain();
        TestWrapAround();
        TestPitchLimitNoWindup();
        TestDeadbandSilence();
        TestTurnRateCapDoesNotInflateGain();
        TestUnreachableTargetBacksOff();
    }
    catch (const std::exception& error)
    {
        std::cerr << "hmd_aim_loop_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hmd_aim_loop_test: all checks passed\n";
    return 0;
}
