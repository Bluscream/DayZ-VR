#pragma once

#include <algorithm>
#include <cstdint>
#include <openxr/openxr.h>

namespace dayz::xr
{
    constexpr bool ValidStereoViews(std::uint32_t count, XrViewStateFlags flags) noexcept
    {
        constexpr auto valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        return count == 2 && (flags & valid) == valid;
    }

    // Controller poses (grip/aim spaces, action state) may be read whenever the XR
    // session is focused and the views are tracked; desktop focus is irrelevant.
    constexpr bool TrackingAllowed(XrSessionState session, bool rendering, bool validViews) noexcept
    {
        return session == XR_SESSION_STATE_FOCUSED && rendering && validViews;
    }

    // Keyboard/mouse injection into the game additionally needs the game window
    // to be the desktop foreground, so an unfocused DayZ never receives held keys.
    constexpr bool InputAllowed(XrSessionState session, bool rendering, bool validViews,
        bool gameAttached, bool desktopFocused) noexcept
    {
        return TrackingAllowed(session, rendering, validViews) &&
            (!gameAttached || desktopFocused);
    }

    inline float AdvanceInputClock(XrTime& previous, XrTime now) noexcept
    {
        const double seconds = previous == 0 ? 0.0 :
            (static_cast<double>(now) - static_cast<double>(previous)) * 1e-9;
        previous = now;
        return static_cast<float>((std::clamp)(seconds, 0.0, 0.1));
    }
}
