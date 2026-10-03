// Host test for the fired-shot detector:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/shot_detector_test.cpp common/shot_detector.cpp -o build/shot_detector_test
#include "../common/shot_detector.hpp"

#include <iostream>
#include <stdexcept>

using dayz::shot::Detector;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }
}

int main()
{
    try
    {
        Detector detector;
        Expect(detector.Update(-1, "", -1, false) == 0, "no bridge data");
        Expect(detector.Update(10, "M4A1", 30, true) == 0, "first sample never fires");
        Expect(detector.Update(11, "M4A1", 29, true) == 1, "one round gone is a shot");
        Expect(detector.Update(11, "M4A1", 29, true) == 0, "same bridge frame is not a second shot");
        Expect(detector.Update(12, "M4A1", 29, true) == 0, "unchanged total");
        Expect(detector.Update(13, "M4A1", 27, true) == 0, "two rounds gone is not a shot (unload)");
        Expect(detector.Update(14, "M4A1", 26, true) == 1, "shot after an unload");
        Expect(detector.Update(15, "M4A1", 30, true) == 0, "reload adds rounds");
        Expect(detector.Update(16, "M4A1", -1, true) == 0, "magazine removed is not a shot");
        Expect(detector.Update(17, "M4A1", -1, false) == 1, "chambered round fired with no magazine");
        Expect(detector.Update(18, "Mosin9130", 4, true) == 0, "weapon swap resets");
        Expect(detector.Update(19, "Mosin9130", 4, false) == 1, "bolt action: chamber emptied");
        Expect(detector.Update(20, "Mosin9130", 3, true) == 0, "cycling the bolt keeps the total");
        Expect(detector.Update(21, "", -1, false) == 0, "empty hands");
        Expect(detector.Update(22, "Mosin9130", 2, true) == 0, "weapon back in hands starts fresh");
        Expect(detector.Update(30, "Mosin9130", 1, true) == 0, "bridge gap longer than three frames is ignored");
        Expect(detector.Update(31, "Mosin9130", 0, true) == 1, "shot after the gap");
        Expect(detector.Update(29, "Mosin9130", 0, false) == 0, "frame counter going backwards (restart) is ignored");
    }
    catch (const std::exception& error)
    {
        std::cerr << "shot_detector_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "shot_detector_test: all checks passed\n";
    return 0;
}
