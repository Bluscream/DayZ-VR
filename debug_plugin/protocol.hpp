// Line protocol of the debug plugin, kept free of Windows and socket code so the
// host-side test can compile it natively.
//
//   get                 -> one JSON object with the full DayzVrDebugState
//   tunables            -> JSON object {"name": value, ...}
//   set <name> <value>  -> {"ok":true} or {"ok":false,"error":"..."}
//   action <UAName> <value> -> forces a DayZ input action through the engine hooks
//                          (value > 0.5 = held; 0 releases); same reply shape as set
//   recenter            -> {"ok":true}
//   haptic              -> {"ok":true}  (test vibration on the right controller)
//   dump_eyes           -> {"ok":true}  (writes dayz_openxr_eye0/1.bmp beside the exe)
//   ping                -> {"ok":true,"pong":true}
// Every reply is exactly one line.
#pragma once

#include "../common/dayz_vr_debug_api.h"

#include <charconv>
#include <cmath>
#include <type_traits>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace dayz::debug_protocol
{
    enum class CommandKind { Invalid, Get, Tunables, Set, Recenter, Haptic, DumpEyes, Ping, Action };

    struct Command
    {
        CommandKind kind{CommandKind::Invalid};
        std::string name;
        double value{};
    };

    inline std::string_view Trim(std::string_view text) noexcept
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
            text.front() == '\r' || text.front() == '\n'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
            text.back() == '\r' || text.back() == '\n'))
            text.remove_suffix(1);
        return text;
    }

    inline Command ParseCommand(std::string_view line)
    {
        Command command;
        line = Trim(line);
        const auto firstSpace = line.find_first_of(" \t");
        const std::string_view verb = line.substr(0, firstSpace);
        const bool takesArguments = verb == "set" || verb == "action";
        if (!takesArguments && firstSpace != std::string_view::npos)
            return command;
        if (verb == "get")
            command.kind = CommandKind::Get;
        else if (verb == "tunables")
            command.kind = CommandKind::Tunables;
        else if (verb == "recenter")
            command.kind = CommandKind::Recenter;
        else if (verb == "haptic")
            command.kind = CommandKind::Haptic;
        else if (verb == "dump_eyes")
            command.kind = CommandKind::DumpEyes;
        else if (verb == "ping")
            command.kind = CommandKind::Ping;
        else if (takesArguments && firstSpace != std::string_view::npos)
        {
            const std::string_view rest = Trim(line.substr(firstSpace + 1));
            const auto secondSpace = rest.find_first_of(" \t");
            if (secondSpace == std::string_view::npos)
                return command;
            const std::string_view name = rest.substr(0, secondSpace);
            const std::string valueText(Trim(rest.substr(secondSpace + 1)));
            double value{};
            const auto parsed = std::from_chars(valueText.data(), valueText.data() + valueText.size(), value);
            if (valueText.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != valueText.data() + valueText.size() || !std::isfinite(value))
                return command;
            command.kind = verb == "set" ? CommandKind::Set : CommandKind::Action;
            command.name.assign(name.data(), name.size());
            command.value = value;
        }
        return command;
    }

    inline void AppendString(std::string& out, std::string_view value)
    {
        constexpr char hex[] = "0123456789abcdef";
        out += '"';
        for (char c : value)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\')
            {
                out += '\\';
                out += c;
            }
            else if (byte < 0x20 || byte >= 0x80)
            {
                // Host labels are ASCII; escape arbitrary bytes rather than emit invalid UTF-8.
                out += "\\u00";
                out += hex[byte >> 4];
                out += hex[byte & 15];
            }
            else
                out += c;
        }
        out += '"';
    }

    template <typename Number>
    inline void AppendNumber(std::string& out, Number value)
    {
        if constexpr (std::is_floating_point_v<Number>)
        {
            if (!std::isfinite(value))
            {
                out += "null";
                return;
            }
        }
        char text[64]{};
        const auto result = std::to_chars(text, text + sizeof(text), value);
        if (result.ec == std::errc{})
            out.append(text, result.ptr);
        else
            out += "null";
    }

    inline void AppendArray(std::string& out, const float* values, std::size_t count)
    {
        out += '[';
        for (std::size_t index = 0; index < count; ++index)
        {
            if (index)
                out += ',';
            AppendNumber(out, values[index]);
        }
        out += ']';
    }

    template <typename Number>
    inline void AppendField(std::string& out, const char* name, Number value)
    {
        out += '"';
        out += name;
        out += "\":";
        AppendNumber(out, value);
        out += ',';
    }

    inline void AppendBool(std::string& out, const char* name, bool value)
    {
        out += '"';
        out += name;
        out += value ? "\":true," : "\":false,";
    }

    inline void AppendHand(std::string& out, const char* name, const DayzVrDebugHand& hand)
    {
        out += '"';
        out += name;
        out += "\":{";
        AppendBool(out, "grip_valid", hand.grip_valid != 0);
        AppendBool(out, "aim_valid", hand.aim_valid != 0);
        out += "\"grip_position\":";
        AppendArray(out, hand.grip_position, 3);
        out += ",\"grip_orientation\":";
        AppendArray(out, hand.grip_orientation, 4);
        out += ",\"aim_position\":";
        AppendArray(out, hand.aim_position, 3);
        out += ",\"aim_orientation\":";
        AppendArray(out, hand.aim_orientation, 4);
        out += "},";
    }

    inline std::string FormatState(const DayzVrDebugState& state)
    {
        std::string out;
        out.reserve(1500);
        out += '{';
        AppendField(out, "api_version", state.api_version);
        AppendBool(out, "hooks_active", state.hooks_active != 0);
        AppendBool(out, "openxr_initialized", state.openxr_initialized != 0);
        AppendBool(out, "session_running", state.session_running != 0);
        AppendField(out, "session_state", state.session_state);
        AppendBool(out, "window_focused", state.window_focused != 0);
        AppendBool(out, "gui_cursor_mode", state.gui_cursor_mode != 0);
        AppendBool(out, "gui_quad_visible", state.gui_quad_visible != 0);
        out += "\"build_profile\":";
        std::size_t profileLength{};
        while (profileLength < sizeof(state.build_profile) && state.build_profile[profileLength])
            ++profileLength;
        AppendString(out, {state.build_profile, profileLength});
        out += ',';
        AppendField(out, "present_count", state.present_count);
        AppendField(out, "stereo_apply_count", state.stereo_apply_count);
        AppendField(out, "rendered_eye", state.rendered_eye);
        AppendField(out, "host_fps", state.host_fps);
        AppendBool(out, "hmd_valid", state.hmd_valid != 0);
        out += "\"hmd_orientation\":";
        AppendArray(out, state.hmd_orientation, 4);
        out += ",\"hmd_position\":";
        AppendArray(out, state.hmd_position, 3);
        out += ',';
        AppendField(out, "hmd_yaw", state.hmd_yaw);
        AppendField(out, "hmd_pitch", state.hmd_pitch);
        AppendField(out, "hmd_roll", state.hmd_roll);
        AppendBool(out, "eyes_valid", state.eyes_valid != 0);
        AppendField(out, "eye_left_x", state.eye_left_x);
        AppendField(out, "eye_right_x", state.eye_right_x);
        AppendBool(out, "camera_directions_valid", state.camera_directions_valid != 0);
        out += "\"native_camera_direction\":";
        AppendArray(out, state.native_camera_direction, 3);
        out += ",\"render_camera_direction\":";
        AppendArray(out, state.render_camera_direction, 3);
        out += ',';
        AppendField(out, "pending_mouse_x", state.pending_mouse_x);
        AppendField(out, "pending_mouse_y", state.pending_mouse_y);
        AppendField(out, "aim_yaw_error", state.aim_yaw_error);
        AppendField(out, "aim_pitch_error", state.aim_pitch_error);
        AppendField(out, "aim_yaw_gain", state.aim_yaw_gain);
        AppendField(out, "aim_pitch_gain", state.aim_pitch_gain);
        AppendHand(out, "left_hand", state.hands[0]);
        AppendHand(out, "right_hand", state.hands[1]);
        AppendBool(out, "direct_input_active", state.direct_input_active != 0);
        AppendField(out, "direct_input_resolved", state.direct_input_resolved);
        AppendField(out, "direct_input_unresolved", state.direct_input_unresolved);
        AppendField(out, "direct_input_frames", state.direct_input_frames);
        AppendField(out, "direct_input_overrides", state.direct_input_overrides);
        AppendField(out, "direct_input_frame_seconds", state.direct_input_frame_seconds);
        out.back() = '}';
        return out;
    }

    // Converts "name=value\n" lines from DayzVrDebugHost::list_tunables into JSON.
    inline std::string FormatTunables(std::string_view lines)
    {
        std::string out{'{'};
        bool first = true;
        while (!lines.empty())
        {
            const auto newline = lines.find('\n');
            const std::string_view line = lines.substr(0, newline);
            lines = newline == std::string_view::npos ? std::string_view{} : lines.substr(newline + 1);
            const auto equals = line.find('=');
            if (line.empty() || equals == std::string_view::npos)
                continue;
            const auto valueText = Trim(line.substr(equals + 1));
            double value{};
            const auto parsed = std::from_chars(valueText.data(), valueText.data() + valueText.size(), value);
            if (equals == 0 || valueText.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != valueText.data() + valueText.size() || !std::isfinite(value))
                continue;
            if (!first)
                out += ',';
            first = false;
            AppendString(out, line.substr(0, equals));
            out += ':';
            AppendNumber(out, value);
        }
        out += '}';
        return out;
    }

    inline std::string FormatResult(int code, const char* unknownText, const char* rejectedText)
    {
        if (code == 0)
            return "{\"ok\":true}";
        std::string out{"{\"ok\":false,\"error\":"};
        AppendString(out, code == -1 ? unknownText : rejectedText);
        out += '}';
        return out;
    }
}
