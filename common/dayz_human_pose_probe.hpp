#pragma once

#include <cstdint>

namespace dayz::human_pose_probe
{
    // Installs the opt-in DayZ single-bone validation hook. The implementation
    // rejects every executable except an exact generated PE profile and
    // leaves the game untouched unless [human_pose_probe] enabled=true.
    bool Initialize(std::uintptr_t moduleBase, std::uint32_t peTimestamp,
        std::uint32_t imageSize) noexcept;
    bool IsActive() noexcept;
}
