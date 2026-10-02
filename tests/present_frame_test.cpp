#include "../dxgi/present_frame.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main()
{
    try
    {
        for (const unsigned flags : {0u, 8u, hooks::kPresentTestFlag, hooks::kPresentTestFlag | 8u})
        {
            std::string calls;
            const int result = hooks::TickAndPresent(flags, [&] { calls += 'T'; }, [&] {
                calls += 'P';
                return -7;
            });
            const std::string expected = (flags & hooks::kPresentTestFlag) != 0 ? "P" : "TP";
            if (result != -7 || calls != expected)
                throw std::runtime_error("test-present advanced a frame, altered the result, or did not forward once");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "present_frame_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "present_frame_test: all checks passed\n";
}
