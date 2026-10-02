#include "debug_bridge.hpp"

#include "dayz_runtime_probe.hpp"
#include "dayz_vr_debug_api.h"
#include "dayz_input_hooks.hpp"
#include "logging.hpp"
#include "openxr_host.hpp"
#include "stereo_state.hpp"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <cstdlib>
#include <string>

namespace
{
    std::once_flag g_once;
    HMODULE g_plugin{};
    DayzVrDebugHost g_host{};

    std::wstring ExecutableDirectory()
    {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path))
            return L"";
        std::wstring directory(path, length);
        const auto separator = directory.find_last_of(L"\\/");
        return separator == std::wstring::npos ? L"" : directory.substr(0, separator + 1);
    }

    bool ReadBoolean(const std::wstring& ini, const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"debug", key, fallback ? L"true" : L"false", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    void CopyPose(const XrPosef& pose, float (&position)[3], float (&orientation)[4]) noexcept
    {
        position[0] = pose.position.x;
        position[1] = pose.position.y;
        position[2] = pose.position.z;
        orientation[0] = pose.orientation.x;
        orientation[1] = pose.orientation.y;
        orientation[2] = pose.orientation.z;
        orientation[3] = pose.orientation.w;
    }

    void FillHand(const XrSpaceLocation& grip, const XrSpaceLocation& aim,
        DayzVrDebugHand& out) noexcept
    {
        constexpr XrSpaceLocationFlags kValid =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        out.grip_valid = (grip.locationFlags & kValid) == kValid;
        out.aim_valid = (aim.locationFlags & kValid) == kValid;
        CopyPose(grip.pose, out.grip_position, out.grip_orientation);
        CopyPose(aim.pose, out.aim_position, out.aim_orientation);
    }

    void GetState(void*, DayzVrDebugState* out)
    {
        if (!out)
            return;
        DayzVrDebugState state{};
        state.struct_size = sizeof(state);
        state.api_version = DAYZ_VR_DEBUG_API_VERSION;

        const auto probe = dayz::runtime_probe::GetDebugSnapshot();
        state.hooks_active = probe.hooksActive;
        state.window_focused = probe.windowFocused;
        state.gui_cursor_mode = probe.guiCursorMode;
        state.gui_quad_visible = probe.guiQuadVisible;
        strncpy_s(state.build_profile, probe.buildProfile, _TRUNCATE);
        state.present_count = probe.presentCount;
        state.stereo_apply_count = probe.stereoApplyCount;
        state.pending_mouse_x = probe.pendingMouseX;
        state.pending_mouse_y = probe.pendingMouseY;
        state.aim_yaw_error = probe.aimYawError;
        state.aim_pitch_error = probe.aimPitchError;
        state.aim_yaw_gain = probe.aimYawGain;
        state.aim_pitch_gain = probe.aimPitchGain;
        state.rendered_eye = dayz::stereo_state::RenderedEye();

        const auto host = OpenXrHost::Instance().GetDebugSnapshot();
        state.openxr_initialized = host.initialized;
        state.session_running = host.sessionRunning;
        state.session_state = host.sessionState;
        state.host_fps = host.fps;
        state.hmd_valid = host.hmdValid;
        CopyPose(host.hmdPose, state.hmd_position, state.hmd_orientation);
        const auto& q = host.hmdPose.orientation;
        // Same yaw/pitch/roll convention as the "pose q=" log line in openxr_host.cpp.
        state.hmd_pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, 2.0f * (q.w * q.x - q.z * q.y))));
        state.hmd_yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        state.hmd_roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
        for (std::size_t hand = 0; hand < 2; ++hand)
            FillHand(host.grip[hand], host.aim[hand], state.hands[hand]);

        const auto eyes = dayz::stereo_state::GetEyePositions();
        state.eyes_valid = eyes.valid;
        state.eye_left_x = eyes.leftX;
        state.eye_right_x = eyes.rightX;
        const auto directions = dayz::stereo_state::GetCameraDirections();
        state.camera_directions_valid = directions.valid;
        state.native_camera_direction[0] = directions.nativeX;
        state.native_camera_direction[1] = directions.nativeY;
        state.native_camera_direction[2] = directions.nativeZ;
        state.render_camera_direction[0] = directions.renderX;
        state.render_camera_direction[1] = directions.renderY;
        state.render_camera_direction[2] = directions.renderZ;

        // Honour the caller's struct size so an older plugin keeps working.
        const std::size_t bytes = out->struct_size != 0 && out->struct_size < sizeof(state)
            ? out->struct_size : sizeof(state);
        std::memcpy(out, &state, bytes);
    }

    int GetTunable(void*, const char* name, double* out)
    {
        double value{};
        if (!out || !dayz::runtime_probe::GetTunable(name, value))
            return -1;
        *out = value;
        return 0;
    }

    int SetTunable(void*, const char* name, double value)
    {
        const int result = dayz::runtime_probe::SetTunable(name, value);
        if (result == 0)
        {
            char message[160]{};
            std::snprintf(message, sizeof(message), "Debug plugin set %s=%g", name, value);
            logging::Info(message);
        }
        return result;
    }

    struct ListContext
    {
        char* buffer;
        size_t capacity;
        size_t needed;
    };

    void AppendTunable(void* context, const char* name, double value)
    {
        auto& list = *static_cast<ListContext*>(context);
        char line[160]{};
        const int length = std::snprintf(line, sizeof(line), "%s=%g\n", name, value);
        if (length <= 0)
            return;
        const auto count = static_cast<size_t>(length);
        if (list.needed < list.capacity)
        {
            const size_t room = list.capacity - list.needed - 1;
            const size_t copy = count < room ? count : room;
            std::memcpy(list.buffer + list.needed, line, copy);
            list.buffer[list.needed + copy] = '\0';
        }
        list.needed += count;
    }

    size_t ListTunables(void*, char* buffer, size_t capacity)
    {
        if (buffer && capacity)
            buffer[0] = '\0';
        ListContext list{buffer, buffer ? capacity : 0, 0};
        dayz::runtime_probe::ForEachTunable(AppendTunable, &list);
        return list.needed;
    }

    int RunCommand(void*, const char* name)
    {
        if (name && _stricmp(name, "dump_eyes") == 0)
        {
            const bool written = OpenXrHost::Instance().DumpEyeCaptures();
            logging::Info(written ? "Debug plugin dumped eye captures" : "Debug plugin eye dump failed");
            return written ? 0 : -2;
        }
        if (name && _stricmp(name, "haptic") == 0)
            return OpenXrHost::Instance().TestHaptic() ? 0 : -2;
        if (name && _stricmp(name, "recenter") == 0)
        {
            dayz::runtime_probe::RecenterHmd();
            logging::Info("Debug plugin requested HMD recenter");
            return 0;
        }
        // "action <UAName> <value>": force an engine input action (test aid for the
        // direct input hooks; value > 0.5 also holds the digital state, 0 releases).
        if (name && _strnicmp(name, "action ", 7) == 0)
        {
            if (!dayz::input_hooks::Active())
                return -2;
            const std::string rest(name + 7);
            const auto space = rest.find(' ');
            if (space == std::string::npos || space == 0)
                return -1;
            const std::string action = rest.substr(0, space);
            char* end{};
            const double value = std::strtod(rest.c_str() + space + 1, &end);
            if (!end || *end != '\0')
                return -1;
            if (value == 0.0)
                dayz::input_hooks::ClearAction(action);
            else
                dayz::input_hooks::SetAction(action, static_cast<float>(value), value > 0.5);
            logging::Info("Debug plugin forced action " + action + " = " + rest.substr(space + 1));
            return 0;
        }
        return -1;
    }

    void Log(void*, const char* message)
    {
        if (message)
            logging::Info(message);
    }

    void StartOnce() noexcept
    {
        const std::wstring directory = ExecutableDirectory();
        const std::wstring ini = directory + L"dayz_openxr.ini";
        if (!ReadBoolean(ini, L"enabled", false))
            return;
        wchar_t pluginName[260]{};
        GetPrivateProfileStringW(L"debug", L"plugin", L"dayz_openxr_debug.dll", pluginName,
            static_cast<DWORD>(std::size(pluginName)), ini.c_str());
        const std::wstring pluginPath = directory + pluginName;
        g_plugin = LoadLibraryW(pluginPath.c_str());
        if (!g_plugin)
        {
            logging::Error("Debug plugin enabled in dayz_openxr.ini but the DLL could not be loaded");
            return;
        }
        // GetProcAddress returns FARPROC; going through void* is the portable way to
        // recover the real exported signature without a function-type-mismatch warning.
        const auto start = reinterpret_cast<DayzVrDebugStartFn>(reinterpret_cast<void*>(
            GetProcAddress(g_plugin, "DayzVrDebugStart")));
        if (!start)
        {
            logging::Error("Debug plugin does not export DayzVrDebugStart");
            return;
        }
        g_host.struct_size = sizeof(g_host);
        g_host.api_version = DAYZ_VR_DEBUG_API_VERSION;
        g_host.port = static_cast<std::uint16_t>(
            GetPrivateProfileIntW(L"debug", L"port", 48621, ini.c_str()));
        g_host.context = nullptr;
        g_host.get_state = GetState;
        g_host.get_tunable = GetTunable;
        g_host.set_tunable = SetTunable;
        g_host.list_tunables = ListTunables;
        g_host.run_command = RunCommand;
        g_host.log = Log;
        if (start(&g_host) != 0)
        {
            logging::Error("Debug plugin failed to start");
            return;
        }
        logging::Info("Debug plugin started");
    }
}

namespace dayz::debug_bridge
{
    void Start() noexcept
    {
        std::call_once(g_once, StartOnce);
    }
}
