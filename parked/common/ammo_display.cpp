#include "ammo_display.hpp"

#include <algorithm>
#include <string>

namespace dayz::ammo_display
{
    namespace
    {
        // Segment bits: a (top), b (upper right), c (lower right), d (bottom),
        // e (lower left), f (upper left), g (middle).
        enum : std::uint8_t
        {
            A = 1 << 0, B = 1 << 1, C = 1 << 2, D = 1 << 3, E = 1 << 4, F = 1 << 5, G = 1 << 6
        };

        constexpr std::uint8_t kDigits[10]{
            static_cast<std::uint8_t>(A | B | C | D | E | F),      // 0
            static_cast<std::uint8_t>(B | C),                      // 1
            static_cast<std::uint8_t>(A | B | D | E | G),          // 2
            static_cast<std::uint8_t>(A | B | C | D | G),          // 3
            static_cast<std::uint8_t>(B | C | F | G),              // 4
            static_cast<std::uint8_t>(A | C | D | F | G),          // 5
            static_cast<std::uint8_t>(A | C | D | E | F | G),      // 6
            static_cast<std::uint8_t>(A | B | C),                  // 7
            static_cast<std::uint8_t>(A | B | C | D | E | F | G),  // 8
            static_cast<std::uint8_t>(A | B | C | D | F | G),      // 9
        };

        std::uint8_t SegmentsFor(char character) noexcept
        {
            if (character >= '0' && character <= '9')
                return kDigits[character - '0'];
            if (character == '-')
                return G;
            return 0;
        }

        struct Rect
        {
            unsigned x0, y0, x1, y1;  // half-open
        };

        void Fill(Bitmap& bitmap, const Rect& rect, std::uint32_t colour) noexcept
        {
            const unsigned x1 = (std::min)(rect.x1, bitmap.width);
            const unsigned y1 = (std::min)(rect.y1, bitmap.height);
            for (unsigned y = rect.y0; y < y1; ++y)
                for (unsigned x = rect.x0; x < x1; ++x)
                    bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x] = colour;
        }

        // Geometry of one cell, all derived from the cell height so any size works.
        struct CellMetrics
        {
            unsigned thickness;  // segment thickness
            unsigned margin;     // blank border inside the cell
            unsigned width;      // cell width including the gap to the next cell
            unsigned glyphWidth; // width of the digit body
        };

        CellMetrics Metrics(unsigned cellHeight) noexcept
        {
            CellMetrics m{};
            m.thickness = (std::max)(2u, cellHeight / 8);
            m.margin = (std::max)(1u, cellHeight / 10);
            m.glyphWidth = (std::max)(m.thickness * 3, cellHeight * 11 / 20);
            m.width = m.glyphWidth + m.margin * 2;
            return m;
        }

        void DrawSegments(Bitmap& bitmap, unsigned cellX, unsigned cellHeight,
            std::uint8_t segments, std::uint32_t colour) noexcept
        {
            const CellMetrics m = Metrics(cellHeight);
            const unsigned x0 = cellX + m.margin;
            const unsigned x1 = x0 + m.glyphWidth;
            const unsigned y0 = m.margin;
            const unsigned y1 = cellHeight - m.margin;
            const unsigned midTop = (y0 + y1) / 2 - m.thickness / 2;
            const unsigned t = m.thickness;
            if (segments & A) Fill(bitmap, {x0, y0, x1, y0 + t}, colour);
            if (segments & D) Fill(bitmap, {x0, y1 - t, x1, y1}, colour);
            if (segments & G) Fill(bitmap, {x0, midTop, x1, midTop + t}, colour);
            if (segments & F) Fill(bitmap, {x0, y0, x0 + t, midTop + t}, colour);
            if (segments & B) Fill(bitmap, {x1 - t, y0, x1, midTop + t}, colour);
            if (segments & E) Fill(bitmap, {x0, midTop, x0 + t, y1}, colour);
            if (segments & C) Fill(bitmap, {x1 - t, midTop, x1, y1}, colour);
        }

        void DrawPlus(Bitmap& bitmap, unsigned cellX, unsigned cellHeight, std::uint32_t colour) noexcept
        {
            const CellMetrics m = Metrics(cellHeight);
            const unsigned t = m.thickness;
            const unsigned cx = cellX + m.margin + m.glyphWidth / 2;
            const unsigned cy = cellHeight / 2;
            const unsigned arm = m.glyphWidth / 2 - m.margin;
            Fill(bitmap, {cx - arm, cy - t / 2, cx + arm, cy - t / 2 + t}, colour);
            Fill(bitmap, {cx - t / 2, cy - arm, cx - t / 2 + t, cy + arm}, colour);
        }
    }

    unsigned CellWidth(unsigned cellHeight) noexcept
    {
        return Metrics((std::max)(8u, cellHeight)).width;
    }

    Bitmap Render(std::string_view text, unsigned cellHeight, std::uint32_t colour)
    {
        cellHeight = (std::max)(8u, cellHeight);
        const std::size_t clipped = text.size() < static_cast<std::size_t>(kMaxCells) ? text.size() : kMaxCells;
        const unsigned cells = static_cast<unsigned>(clipped);
        Bitmap bitmap{};
        bitmap.width = (std::max)(1u, cells) * CellWidth(cellHeight);
        bitmap.height = cellHeight;
        bitmap.pixels.assign(static_cast<std::size_t>(bitmap.width) * bitmap.height, 0u);
        for (unsigned cell = 0; cell < cells; ++cell)
        {
            const char character = text[cell];
            const unsigned cellX = cell * CellWidth(cellHeight);
            if (character == '+')
                DrawPlus(bitmap, cellX, cellHeight, colour);
            else
                DrawSegments(bitmap, cellX, cellHeight, SegmentsFor(character), colour);
        }
        return bitmap;
    }

    std::string FormatAmmo(int ammo, bool chamber)
    {
        if (ammo < 0)
            return chamber ? "+1" : "-";
        std::string text = std::to_string((std::min)(ammo, 999));
        if (chamber)
            text += "+1";
        return text;
    }

    std::uint32_t ColourFor(int ammo, bool chamber) noexcept
    {
        // Only the low-ammo thresholds matter; cap before adding the chamber so
        // even malformed bridge values cannot overflow a signed integer.
        const int total = (std::clamp)(ammo, 0, 6) + (chamber ? 1 : 0);
        if (total <= 0)
            return 0xFF4040FFu;  // red
        if (total <= 5)
            return 0xFF40C0FFu;  // amber
        return 0xFFFFFFFFu;      // white
    }
}
