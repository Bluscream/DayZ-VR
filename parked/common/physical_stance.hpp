#pragma once

// Physical crouch/prone from headset height. The head drop below the calibrated
// standing height (the recentred HMD position) selects the stance DayZ should be
// in; hysteresis around each threshold keeps small bobbing from toggling. Pure and
// host-testable; the OpenXR host taps DayZ's crouch/prone toggles to reach it.
namespace dayz::physical_stance
{
    enum Stance : int { Erect = 0, Crouch = 1, Prone = 2 };

    struct Config
    {
        float crouchDropMeters{0.35f};  // head this far below standing = crouch
        float proneDropMeters{0.85f};   // this far below = prone
        float hysteresisMeters{0.08f};  // must rise this much above a threshold to stand back up
    };

    // headDrop: standing height minus current head height (metres, positive = lower).
    // current: the stance the game reports now (bridge stance index).
    int Desired(float headDrop, int current, const Config& config) noexcept;
}
