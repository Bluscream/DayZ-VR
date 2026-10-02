#pragma once

#include <cstdint>

namespace dayz::runtime_probe
{
    struct GuiCaptureSizing
    {
        std::uint32_t backWidth{};
        std::uint32_t backHeight{};
        std::uint64_t capturedArea{};

        // A resize starts a new target-selection generation. Auxiliary smaller
        // surfaces are still rejected within that generation.
        bool ObserveBackBuffer(std::uint32_t width, std::uint32_t height) noexcept
        {
            if (backWidth == width && backHeight == height)
                return false;
            backWidth = width;
            backHeight = height;
            capturedArea = 0;
            return true;
        }

        bool Accepts(std::uint32_t width, std::uint32_t height) const noexcept
        {
            return static_cast<std::uint64_t>(width) * height >= capturedArea;
        }

        void Captured(std::uint32_t width, std::uint32_t height) noexcept
        {
            capturedArea = static_cast<std::uint64_t>(width) * height;
        }
    };
}
