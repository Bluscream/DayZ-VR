#pragma once

// Detects a fired round from the script bridge's ammo readback (magazine count plus
// chamber flag, ten samples a second). A shot is one round leaving the weapon
// without the weapon changing: the total (magazine + chamber) drops by exactly one.
// Reloads, chambering, weapon swaps and bridge gaps never count. Pure, host-testable;
// the OpenXR host pulses the controller haptics for every detected shot.
#include <string>
#include <string_view>

namespace dayz::shot
{
    class Detector
    {
    public:
        // frame: the bridge frame counter (-1 while the bridge has no data). Returns the
        // number of shots detected in this sample (0 or 1; a drop of several rounds is
        // treated as an unload or a magazine swap, not as shots).
        int Update(int frame, std::string_view weapon, int ammo, bool chamber) noexcept;

    private:
        int lastFrame_{-1};
        std::string weapon_;
        int lastTotal_{-1};
        bool haveSample_{};
    };
}
