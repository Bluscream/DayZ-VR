// Host test for the input action override table:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/input_actions_test.cpp common/input_actions.cpp -o build/input_actions_test
#include "../common/input_actions.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace dayz::input_actions;

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
        Table table;
        Latched state;
        Expect(!table.Get("UAFire", state), "unknown action is absent");
        Expect(table.Set("UAFire", 1.0f, true), "set accepted");
        Expect(table.Get("UAFire", state) && !state.held && !state.press,
            "nothing is visible before the first latch");

        table.Latch();
        Expect(table.Get("UAFire", state), "present after latch");
        Expect(state.held && state.press && state.holdBegin && !state.release && state.value == 1.0f,
            "first latched frame is press + hold begin");

        table.Latch();
        Expect(table.Get("UAFire", state) && state.held && !state.press && !state.holdBegin,
            "second frame holds without a new press");

        // Setting again mid-frame changes nothing until the next latch.
        table.Set("UAFire", 0.0f, false);
        Expect(table.Get("UAFire", state) && state.held, "latched state is frozen within the frame");
        table.Latch();
        Expect(table.Get("UAFire", state) && !state.held && state.release && state.value == 0.0f,
            "release edge on the frame the key goes up");
        table.Latch();
        Expect(table.Get("UAFire", state) && !state.release, "release lasts one frame");

        // Clear keeps the entry for exactly one latch so the release is delivered.
        table.Set("UAGetOver", 1.0f, true);
        table.Latch();
        table.Clear("UAGetOver");
        table.Latch();
        Expect(table.Get("UAGetOver", state) && state.release && !state.held,
            "cleared action delivers its release");
        table.Latch();
        Expect(!table.Get("UAGetOver", state), "cleared action is gone after the release frame");

        // Clearing an action that was never held disappears on the next latch.
        table.Set("UAMoveForward", 0.4f, false);
        table.Latch();
        Expect(table.Get("UAMoveForward", state) && state.value == 0.4f && !state.held,
            "analogue value without hold");
        table.Clear("UAMoveForward");
        table.Latch();
        Expect(!table.Get("UAMoveForward", state), "unheld cleared action is dropped at once");

        // Handles: resolution by the native layer and lookups by handle and id.
        table.Set("UAMoveBack", 0.5f, false);
        Expect(!table.GetByHandle(0x1000, state), "unresolved handle misses");
        int unresolved = 0;
        table.ForEachUnresolved([&](std::string_view name) {
            ++unresolved;
            Expect(name == "UAMoveBack" || name == "UAFire", "only live names are unresolved");
        });
        Expect(unresolved == 2, "two unresolved names (UAFire lingers until cleared)");
        Expect(table.SetHandle("UAMoveBack", 0x1000, 7), "handle attached");
        Expect(!table.SetHandle("UAMoveBack", 0, 7), "null handle does not resolve");
        table.SetHandle("UAMoveBack", 0x1000, 7);
        table.Latch();
        Expect(table.GetByHandle(0x1000, state) && state.value == 0.5f, "lookup by handle");
        Expect(table.GetById(7, state) && state.value == 0.5f, "lookup by id");
        Expect(!table.GetById(8, state), "other ids miss");
        Expect(!table.GetByHandle(0, state), "null handle never matches");
        Expect(!table.SetHandle("nope", 1, 1), "unknown name cannot get a handle");

        // Limits.
        Expect(!table.Set("", 1.0f, true), "empty name rejected");
        Expect(!table.Set(std::string(Table::kNameLength, 'x'), 1.0f, true), "long name rejected");
        Table full;
        for (std::size_t index = 0; index < Table::kCapacity; ++index)
            Expect(full.Set("UA" + std::to_string(index), 1.0f, true), "fill the table");
        Expect(!full.Set("UAOverflow", 1.0f, true), "capacity enforced");
        Expect(full.Count() == Table::kCapacity, "count reports capacity");
        full.ClearAll();
        full.Latch();
        full.Latch();
        Expect(full.Count() == 0, "clear all empties the table after the release frame");

        Expect(&Global() == &Global(), "global table is a singleton");
        std::cout << "input_actions_test passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "input_actions_test failed: " << error.what() << '\n';
        return 1;
    }
}
