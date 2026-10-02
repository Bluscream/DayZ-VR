#pragma once

// CPU rasteriser for the controller-anchored ammo display: seven-segment digits plus
// a "+" glyph (chambered round) rendered into an RGBA8 bitmap that the host uploads
// into a small OpenXR quad swapchain. Pure, host-testable, no Windows dependency.
#include <cstdint>
#include <string_view>
#include <vector>

namespace dayz::ammo_display
{
    struct Bitmap
    {
        unsigned width{};
        unsigned height{};
        std::vector<std::uint32_t> pixels;  // row-major, 0xAABBGGRR as D3D R8G8B8A8 expects
    };

    // Characters understood: '0'..'9', '+', '-', ' ' (blank cell). Anything else is blank.
    constexpr unsigned kMaxCells = 5;

    // Width in pixels of one character cell and the full bitmap height for a cell size.
    unsigned CellWidth(unsigned cellHeight) noexcept;

    // Renders up to kMaxCells characters into a new bitmap of cells*CellWidth x cellHeight.
    // colour is 0xAABBGGRR; background is fully transparent black. Each lit segment is a
    // solid rectangle, so the result stays crisp at any quad size.
    Bitmap Render(std::string_view text, unsigned cellHeight, std::uint32_t colour);

    // Formats the magazine count and chamber state as DayZ shows it: "30+1", "7", "-".
    // ammo < 0 means no magazine (shows "-" or "+1" when only the chamber is loaded).
    std::string FormatAmmo(int ammo, bool chamber);

    // Suggested colour for a count: white, amber below 6 rounds, red when empty.
    std::uint32_t ColourFor(int ammo, bool chamber) noexcept;
}
