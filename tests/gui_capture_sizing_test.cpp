#include "../common/gui_capture_sizing.hpp"

#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        dayz::runtime_probe::GuiCaptureSizing sizing;
        const auto require = [](bool condition, const char* reason) {
            if (!condition)
                throw std::runtime_error(reason);
        };
        require(sizing.ObserveBackBuffer(1600, 1600), "initial backbuffer was not observed");
        sizing.Captured(1600, 1600);
        require(!sizing.Accepts(320, 320), "auxiliary target replaced the primary GUI capture");
        require(!sizing.ObserveBackBuffer(1600, 1600), "unchanged buffer reset target selection");
        require(!sizing.Accepts(1280, 1280), "smaller target accepted before actual resize");
        require(sizing.ObserveBackBuffer(1280, 1280), "primary downsize did not reset selection");
        require(sizing.Accepts(1280, 1280), "smaller primary target rejected after resize");
        sizing.Captured(1280, 1280);
        require(!sizing.Accepts(640, 640), "resize removed auxiliary-target protection");
        require(sizing.ObserveBackBuffer(1600, 1024), "equal-area aspect change did not reset selection");
        require(sizing.Accepts(1600, 1024), "new aspect was rejected");
    }
    catch (const std::exception& error)
    {
        std::cerr << "gui_capture_sizing_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gui_capture_sizing_test: all checks passed\n";
}
