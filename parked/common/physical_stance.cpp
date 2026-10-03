#include "physical_stance.hpp"

#include <algorithm>
#include <cmath>

namespace dayz::physical_stance
{
    int Desired(float headDrop, int current, const Config& config) noexcept
    {
        if (!std::isfinite(headDrop))
            return current;
        const float hysteresis = (std::max)(0.0f, config.hysteresisMeters);
        const float crouch = (std::max)(0.05f, config.crouchDropMeters);
        const float prone = (std::max)(crouch + 0.05f, config.proneDropMeters);
        // Going lower needs the full threshold; coming back up needs the threshold
        // minus hysteresis so the boundary is not crossed twice by one head bob.
        const float crouchUp = crouch - hysteresis;
        const float proneUp = prone - hysteresis;
        if (current == Prone)
            return headDrop >= proneUp ? Prone : (headDrop >= crouchUp ? Crouch : Erect);
        if (current == Crouch)
        {
            if (headDrop >= prone)
                return Prone;
            return headDrop >= crouchUp ? Crouch : Erect;
        }
        if (headDrop >= prone)
            return Prone;
        return headDrop >= crouch ? Crouch : Erect;
    }
}
