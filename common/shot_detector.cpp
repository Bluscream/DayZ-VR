#include "shot_detector.hpp"

namespace dayz::shot
{
    namespace
    {
        int Total(int ammo, bool chamber) noexcept
        {
            return (ammo < 0 ? 0 : ammo) + (chamber ? 1 : 0);
        }
    }

    int Detector::Update(int frame, std::string_view weapon, int ammo, bool chamber) noexcept
    {
        if (frame < 0 || weapon.empty())
        {
            // No bridge data or empty hands: forget the history so the next weapon
            // starts fresh and the first sample of it never fires.
            haveSample_ = false;
            weapon_.clear();
            lastFrame_ = frame;
            return 0;
        }
        if (frame == lastFrame_)
            return 0;  // same bridge sample seen again
        const int total = Total(ammo, chamber);
        const bool sameWeapon = haveSample_ && weapon == weapon_;
        const bool continuous = sameWeapon && frame > lastFrame_ && frame - lastFrame_ <= 3;
        const int shots = continuous && lastTotal_ - total == 1 ? 1 : 0;
        weapon_.assign(weapon.data(), weapon.size());
        lastTotal_ = total;
        lastFrame_ = frame;
        haveSample_ = true;
        return shots;
    }
}
