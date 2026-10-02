// Host test for the melee swing detector:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/melee_swing_test.cpp common/melee_swing.cpp -o build/melee_swing_test
#include "../common/melee_swing.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace dayz::melee;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }

    // Moves the hand along +x with the given speed profile (m/s per 10 ms sample)
    // and returns every event in order.
    std::vector<Swing> Run(SwingDetector& detector, const std::vector<float>& speeds, const Config& config)
    {
        std::vector<Swing> events;
        float x = 0.0f;
        constexpr float dt = 0.01f;
        detector.Update(x, 0.0f, 0.0f, dt, config);
        for (const float speed : speeds)
        {
            x += speed * dt;
            const Swing swing = detector.Update(x, 0.0f, 0.0f, dt, config);
            if (swing != Swing::None)
                events.push_back(swing);
        }
        return events;
    }

    std::vector<float> Bell(float peak, int samples)
    {
        std::vector<float> speeds;
        for (int i = 0; i < samples; ++i)
            speeds.push_back(peak * std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(samples)));
        return speeds;
    }

    std::vector<float> Rest(int samples)
    {
        return std::vector<float>(static_cast<std::size_t>(samples), 0.0f);
    }

    std::vector<float> Join(std::vector<float> a, const std::vector<float>& b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }
}

int main()
{
    try
    {
        const Config config{};
        {
            SwingDetector detector;
            Expect(Run(detector, Rest(100), config).empty(), "stationary hand never swings");
        }
        {
            SwingDetector detector;
            Expect(Run(detector, Bell(1.0f, 30), config).empty(), "slow movement below light_speed is ignored");
        }
        {
            SwingDetector detector;
            const auto events = Run(detector, Join(Bell(2.2f, 30), Rest(20)), config);
            Expect(events.size() == 1 && events[0] == Swing::Light, "moderate swing is one light attack");
        }
        {
            SwingDetector detector;
            const auto events = Run(detector, Join(Bell(4.5f, 30), Rest(20)), config);
            Expect(events.size() == 1 && events[0] == Swing::Heavy, "fast swing is one heavy attack");
        }
        {
            SwingDetector detector;
            // Two swings back to back without the hand coming to rest: the second is
            // within the cooldown and the hand never drops below rearm_speed.
            std::vector<float> profile = Join(Bell(2.5f, 20), Bell(2.5f, 20));
            Expect(Run(detector, profile, config).size() == 1, "no double fire within cooldown");
        }
        {
            SwingDetector detector;
            std::vector<float> profile = Join(Join(Bell(2.5f, 30), Rest(60)), Join(Bell(4.0f, 30), Rest(10)));
            const auto events = Run(detector, profile, config);
            Expect(events.size() == 2 && events[0] == Swing::Light && events[1] == Swing::Heavy,
                "second swing after rest and cooldown fires with its own class");
        }
        {
            SwingDetector detector;
            Config strict = config;
            strict.lightSpeed = 10.0f;
            Expect(Run(detector, Bell(4.5f, 30), strict).empty(), "thresholds are taken from the config");
        }
        {
            SwingDetector detector;
            detector.Update(0.0f, 0.0f, 0.0f, 0.01f, config);
            Expect(detector.Update(5.0f, 0.0f, 0.0f, 0.0f, config) == Swing::None, "zero dt does not divide by zero");
            Expect(std::isfinite(detector.Speed()), "speed stays finite on zero dt");
            Expect(detector.Update(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.01f, config) == Swing::None,
                "NaN sample is ignored");
            detector.Reset();
            Expect(detector.Speed() == 0.0f, "reset clears speed");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "melee_swing_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "melee_swing_test: all checks passed\n";
    return 0;
}
