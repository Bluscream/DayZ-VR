#include "../common/xr_frame_policy.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void Expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    using namespace dayz::xr;
    constexpr auto allValid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    Expect(ValidStereoViews(2, allValid), "stereo with valid position/orientation");
    for (const auto flags : {XrViewStateFlags{0}, XR_VIEW_STATE_POSITION_VALID_BIT,
        XR_VIEW_STATE_ORIENTATION_VALID_BIT})
        Expect(!ValidStereoViews(2, flags), "partial tracking must not expose invalid fields");
    Expect(!ValidStereoViews(1, allValid) && !ValidStereoViews(0, allValid), "view count mismatch");
    for (const auto session : {XR_SESSION_STATE_UNKNOWN, XR_SESSION_STATE_READY,
        XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_STOPPING,
        XR_SESSION_STATE_EXITING, XR_SESSION_STATE_LOSS_PENDING})
        Expect(!InputAllowed(session, true, true, true, true), "unfocused XR input must be released");
    Expect(InputAllowed(XR_SESSION_STATE_FOCUSED, true, true, true, true), "active game input");
    Expect(!InputAllowed(XR_SESSION_STATE_FOCUSED, false, true, true, true), "no-render releases input");
    Expect(!InputAllowed(XR_SESSION_STATE_FOCUSED, true, false, true, true), "invalid views release input");
    Expect(!InputAllowed(XR_SESSION_STATE_FOCUSED, true, true, true, false), "desktop focus loss releases input");
    Expect(InputAllowed(XR_SESSION_STATE_FOCUSED, true, true, false, false), "probe can still locate controllers");
    XrTime previous{};
    Expect(AdvanceInputClock(previous, 1000000000) == 0.0f, "initial clock");
    Expect(std::fabs(AdvanceInputClock(previous, 1020000000) - 0.02f) < 0.00001f, "clock advances without aiming");
    Expect(AdvanceInputClock(previous, 2000000000) == 0.1f, "long pause bounded");
    Expect(AdvanceInputClock(previous, 1000000000) == 0.0f, "backwards time bounded");
    std::cout << "xr_frame_policy_test: tracking, focus and timing cases passed\n";
}
