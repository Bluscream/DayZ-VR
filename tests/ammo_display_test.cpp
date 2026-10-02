// Host test for the seven-segment ammo display rasteriser:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/ammo_display_test.cpp common/ammo_display.cpp -o build/ammo_display_test
#include "../common/ammo_display.hpp"

#include <iostream>
#include <stdexcept>

using namespace dayz::ammo_display;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }

    unsigned LitPixels(const Bitmap& bitmap, unsigned cell)
    {
        const unsigned cellWidth = bitmap.width / (std::max)(1u, bitmap.width / CellWidth(bitmap.height));
        unsigned lit = 0;
        for (unsigned y = 0; y < bitmap.height; ++y)
            for (unsigned x = cell * cellWidth; x < (cell + 1) * cellWidth && x < bitmap.width; ++x)
                lit += (bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x] >> 24) != 0;
        return lit;
    }
}

int main()
{
    try
    {
        Expect(FormatAmmo(30, true) == "30+1", "30 rounds plus chamber");
        Expect(FormatAmmo(7, false) == "7", "7 rounds");
        Expect(FormatAmmo(-1, false) == "-", "no magazine");
        Expect(FormatAmmo(-1, true) == "+1", "chamber only");
        Expect(FormatAmmo(5000, false) == "999", "clamped");
        Expect(ColourFor(0, false) == 0xFF4040FFu, "empty is red");
        Expect(ColourFor(3, false) == 0xFF40C0FFu, "low is amber");
        Expect(ColourFor(30, true) == 0xFFFFFFFFu, "full is white");

        const Bitmap eight = Render("8", 32, 0xFFFFFFFFu);
        const Bitmap one = Render("1", 32, 0xFFFFFFFFu);
        const Bitmap blank = Render(" ", 32, 0xFFFFFFFFu);
        Expect(eight.width == CellWidth(32) && eight.height == 32, "single cell size");
        Expect(LitPixels(eight, 0) > LitPixels(one, 0), "8 lights more segments than 1");
        Expect(LitPixels(one, 0) > 0, "1 lights two segments");
        Expect(LitPixels(blank, 0) == 0, "blank cell is transparent");

        const Bitmap text = Render("30+1", 48, 0xFF40C0FFu);
        Expect(text.width == 4 * CellWidth(48), "four cells");
        Expect(LitPixels(text, 2) > 0, "plus glyph drawn");
        bool colourSeen = false;
        for (std::uint32_t pixel : text.pixels)
            colourSeen = colourSeen || pixel == 0xFF40C0FFu;
        Expect(colourSeen, "requested colour used");

        const Bitmap tooLong = Render("1234567", 16, 0xFFFFFFFFu);
        Expect(tooLong.width == kMaxCells * CellWidth(16), "input clipped to kMaxCells");
    }
    catch (const std::exception& error)
    {
        std::cerr << "ammo_display_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ammo_display_test: all checks passed\n";
    return 0;
}
