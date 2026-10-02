#include "../common/projection_replay.hpp"

#include <iostream>
#include <stdexcept>

namespace
{
    void Expect(bool condition, const char* reason)
    {
        if (!condition)
            throw std::runtime_error(reason);
    }
}

int main()
{
    try
    {
        dayz::runtime_probe::ProjectionReplay replay;
        int firstContext{};
        int secondContext{};
        Expect(!replay.Consume(), "world render without an engine projection was allowed");
        replay.Publish(&firstContext, false);
        Expect(replay.Consume() == &firstContext, "fresh engine projection was not consumed");
        replay.Publish(&firstContext, true);
        replay.Publish(&firstContext, true);
        Expect(!replay.Consume(), "internal left/right replay armed another double render");
        replay.Publish(&secondContext, false);
        replay.Publish(&firstContext, true);
        Expect(replay.Consume() == &secondContext, "internal replay replaced a newer engine context");
        Expect(!replay.Consume(), "engine projection was consumed twice");
        replay.Publish(&firstContext, false);
        replay.Publish(nullptr, false);
        Expect(!replay.Consume(), "null engine projection left an older context armed");
    }
    catch (const std::exception& error)
    {
        std::cerr << "projection_replay_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "projection_replay_test: all checks passed\n";
}
