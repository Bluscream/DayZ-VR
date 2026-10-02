// Native test for the debug plugin's line protocol. Build and run on the host:
//   g++ -std=c++20 -Wall -Wextra -Werror tests/debug_protocol_test.cpp -o build/debug_protocol_test
//   ./build/debug_protocol_test
#include "../debug_plugin/protocol.hpp"

#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace dayz::debug_protocol;

namespace
{
    void Expect(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(what);
    }

    void TestParse()
    {
        Expect(ParseCommand("get\r\n").kind == CommandKind::Get, "get");
        Expect(ParseCommand("  tunables ").kind == CommandKind::Tunables, "tunables");
        Expect(ParseCommand("recenter").kind == CommandKind::Recenter, "recenter");
        Expect(ParseCommand("haptic").kind == CommandKind::Haptic, "haptic");
        Expect(ParseCommand("haptic now").kind == CommandKind::Invalid, "haptic takes no arguments");
        Expect(ParseCommand("ping").kind == CommandKind::Ping, "ping");
        const Command set = ParseCommand("set stereo.hmd_mouse_yaw_scale -300.5\n");
        Expect(set.kind == CommandKind::Set, "set kind");
        Expect(set.name == "stereo.hmd_mouse_yaw_scale", "set name");
        Expect(set.value == -300.5, "set value");
        Expect(ParseCommand("set").kind == CommandKind::Invalid, "set without args");
        Expect(ParseCommand("set name").kind == CommandKind::Invalid, "set without value");
        Expect(ParseCommand("set name abc").kind == CommandKind::Invalid, "set non-numeric");
        Expect(ParseCommand("set name 1x").kind == CommandKind::Invalid, "set trailing junk");
        for (const auto* invalid : {"set x nan", "set x inf", "set x 1e999", "get extra", "ping extra"})
            Expect(ParseCommand(invalid).kind == CommandKind::Invalid, "reject invalid command");
        Expect(ParseCommand("set\tx\t1").kind == CommandKind::Set, "tab-separated command");
        const Command action = ParseCommand("action UAMoveForward 0.75");
        Expect(action.kind == CommandKind::Action && action.name == "UAMoveForward" && action.value == 0.75,
            "action with name and value");
        Expect(ParseCommand("action UAFire").kind == CommandKind::Invalid, "action without value");
        Expect(ParseCommand("action").kind == CommandKind::Invalid, "action without args");
        Expect(ParseCommand("action UAFire x").kind == CommandKind::Invalid, "action non-numeric");
        Expect(ParseCommand(std::string("set x 1\0junk", 12)).kind == CommandKind::Invalid, "embedded nul");
        Expect(ParseCommand("").kind == CommandKind::Invalid, "empty");
        Expect(ParseCommand("GET").kind == CommandKind::Invalid, "verbs are case-sensitive");
    }

    void TestFormatState()
    {
        DayzVrDebugState state{};
        state.struct_size = sizeof(state);
        state.api_version = DAYZ_VR_DEBUG_API_VERSION;
        state.hooks_active = 1;
        state.direct_input_active = 1;
        state.direct_input_overrides = 42;
        state.session_state = 5;
        state.host_fps = 71.5;
        state.hmd_yaw = -3.0975f;
        state.hmd_orientation[3] = 1.0f;
        state.hands[1].aim_valid = 1;
        state.hands[1].aim_position[2] = -0.25f;
        std::strcpy(state.build_profile, "DayZ_x64 1.29.163709");
        const std::string json = FormatState(state);
        Expect(json.front() == '{' && json.back() == '}', "object braces");
        Expect(json.find("\"hooks_active\":true") != std::string::npos, "bool true");
        Expect(json.find("\"direct_input_active\":true") != std::string::npos, "direct input flag");
        Expect(json.find("\"direct_input_overrides\":42") != std::string::npos, "direct input counter");
        Expect(json.find("\"openxr_initialized\":false") != std::string::npos, "bool false");
        Expect(json.find("\"session_state\":5") != std::string::npos, "integer");
        Expect(json.find("\"host_fps\":71.5") != std::string::npos, "double");
        Expect(json.find("\"build_profile\":\"DayZ_x64 1.29.163709\"") != std::string::npos, "string");
        Expect(json.find("\"hmd_orientation\":[0,0,0,1]") != std::string::npos, "array");
        Expect(json.find("\"right_hand\":{\"grip_valid\":false,\"aim_valid\":true") != std::string::npos, "hand");
        Expect(json.find(",}") == std::string::npos && json.find(",]") == std::string::npos, "no dangling commas");
        Expect(json.find('\n') == std::string::npos, "single line");
    }

    void TestHostileState()
    {
        DayzVrDebugState state{};
        std::memset(state.build_profile, 'x', sizeof(state.build_profile));
        state.host_fps = std::numeric_limits<double>::infinity();
        state.present_count = std::numeric_limits<decltype(state.present_count)>::max();
        auto json = FormatState(state);
        Expect(json.find(std::string(sizeof(state.build_profile), 'x')) != std::string::npos, "bounded full label");
        Expect(json.find("\"host_fps\":null") != std::string::npos, "nonfinite is null");
        Expect(json.find("18446744073709551615") != std::string::npos, "integer precision");
        std::strcpy(state.build_profile, "a\"\\\n");
        json = FormatState(state);
        Expect(json.find("a\\\"\\\\\\u000a") != std::string::npos, "escaped label");
        Expect(FormatTunables("x=nan\ny=1e999\nz=1,2\n") == "{}", "invalid tunable values");
        Expect(FormatResult(-1, "a\n", "r").find("a\\u000a") != std::string::npos, "escaped error");
    }

    void TestFormatTunables()
    {
        Expect(FormatTunables("a.b=1\nc.d=-0.5\n") == "{\"a.b\":1,\"c.d\":-0.5}", "tunables json");
        Expect(FormatTunables("") == "{}", "empty tunables");
        Expect(FormatTunables("broken\nx=2") == "{\"x\":2}", "skips malformed lines");
    }

    void TestFormatResult()
    {
        Expect(FormatResult(0, "u", "r") == "{\"ok\":true}", "ok");
        Expect(FormatResult(-1, "unknown tunable", "r") == "{\"ok\":false,\"error\":\"unknown tunable\"}", "unknown");
        Expect(FormatResult(-2, "u", "value out of range") == "{\"ok\":false,\"error\":\"value out of range\"}", "rejected");
    }
}

int main()
{
    TestParse();
    TestFormatState();
    TestHostileState();
    TestFormatTunables();
    TestFormatResult();
    std::cout << "debug_protocol_test: all checks passed\n";
    return 0;
}
