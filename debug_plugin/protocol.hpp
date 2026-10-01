// Line protocol of the debug plugin, kept free of Windows and socket code so the
// host-side test can compile it natively.
//
//   get                 -> one JSON object with the full DayzVrDebugState
//   tunables            -> JSON object {"name": value, ...}
//   set <name> <value>  -> {"ok":true} or {"ok":false,"error":"..."}
//   recenter            -> {"ok":true}
//   ping                -> {"ok":true,"pong":true}
// Every reply is exactly one line.
#pragma once

#include "../common/dayz_vr_debug_api.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace dayz::debug_protocol
{
    enum class CommandKind { Invalid, Get, Tunables, Set, Recenter, Ping };

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
        const auto firstSpace = line.find(' ');
        const std::string_view verb = line.substr(0, firstSpace);
        if (verb == "get")
            command.kind = CommandKind::Get;
        else if (verb == "tunables")
            command.kind = CommandKind::Tunables;
        else if (verb == "recenter")
            command.kind = CommandKind::Recenter;
        else if (verb == "ping")
            command.kind = CommandKind::Ping;
        else if (verb == "set" && firstSpace != std::string_view::npos)
        {
            const std::string_view rest = Trim(line.substr(firstSpace + 1));
            const auto secondSpace = rest.find(' ');
            if (secondSpace == std::string_view::npos)
                return command;
            const std::string_view name = rest.substr(0, secondSpace);
            const std::string valueText(Trim(rest.substr(secondSpace + 1)));
            char* end{};
            const double value = std::strtod(valueText.c_str(), &end);
            if (valueText.empty() || !end || *end != '\0')
                return command;
            command.kind = CommandKind::Set;
            command.name.assign(name.data(), name.size());
            command.value = value;
        }
        return command;
    }

    inline void AppendNumber(std::string& out, double value)
    {
        char text[64]{};
        std::snprintf(text, sizeof(text), "%.9g", value);
        out += text;
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

    inline void AppendField(std::string& out, const char* name, double value)
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
        out += "\"build_profile\":\"";
        for (const char* c = state.build_profile; *c && c < state.build_profile + sizeof(state.build_profile); ++c)
            if (*c != '"' && *c != '\\')
                out += *c;
        out += "\",";
        AppendField(out, "present_count", static_cast<double>(state.present_count));
        AppendField(out, "stereo_apply_count", static_cast<double>(state.stereo_apply_count));
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
        AppendHand(out, "left_hand", state.hands[0]);
        AppendHand(out, "right_hand", state.hands[1]);
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
            if (!first)
                out += ',';
            first = false;
            out += '"';
            out.append(line.data(), equals);
            out += "\":";
            out.append(line.data() + equals + 1, line.size() - equals - 1);
        }
        out += '}';
        return out;
    }

    inline std::string FormatResult(int code, const char* unknownText, const char* rejectedText)
    {
        if (code == 0)
            return "{\"ok\":true}";
        std::string out{"{\"ok\":false,\"error\":\""};
        out += code == -1 ? unknownText : rejectedText;
        out += "\"}";
        return out;
    }
}
