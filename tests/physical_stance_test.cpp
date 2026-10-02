// Host test for the physical stance mapping:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/physical_stance_test.cpp common/physical_stance.cpp -o build/physical_stance_test
#include "../common/physical_stance.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dayz::physical_stance;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }
}

int main()
{
    try
    {
        const Config config{};
        Expect(Desired(0.0f, Erect, config) == Erect, "standing stays erect");
        Expect(Desired(0.30f, Erect, config) == Erect, "small dip below the crouch threshold stays erect");
        Expect(Desired(0.36f, Erect, config) == Crouch, "dropping past the threshold crouches");
        Expect(Desired(0.30f, Crouch, config) == Crouch, "hysteresis keeps crouch just above the threshold");
        Expect(Desired(0.26f, Crouch, config) == Erect, "rising past threshold minus hysteresis stands up");
        Expect(Desired(0.90f, Erect, config) == Prone, "deep drop goes prone directly");
        Expect(Desired(0.90f, Crouch, config) == Prone, "crouch to prone");
        Expect(Desired(0.80f, Prone, config) == Prone, "prone hysteresis");
        Expect(Desired(0.50f, Prone, config) == Crouch, "prone back to crouch");
        Expect(Desired(0.10f, Prone, config) == Erect, "prone straight to erect when standing up fast");
        Expect(Desired(std::numeric_limits<float>::quiet_NaN(), Crouch, config) == Crouch, "NaN keeps the current stance");
        Config tight = config;
        tight.crouchDropMeters = 0.2f;
        tight.proneDropMeters = 0.1f;  // invalid order: prone must stay above crouch
        Expect(Desired(0.22f, Erect, tight) == Crouch, "config thresholds respected and ordered");
        Expect(Desired(0.30f, Erect, tight) == Prone, "prone clamped to crouch + 5 cm");
    }
    catch (const std::exception& error)
    {
        std::cerr << "physical_stance_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "physical_stance_test: all checks passed\n";
    return 0;
}
