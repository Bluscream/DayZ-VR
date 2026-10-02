#include "script_bridge.hpp"

#include "dayz_runtime_probe.hpp"
#include "logging.hpp"
#include "stereo_state.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>

#include <windows.h>
#include <shlobj.h>

namespace dayz::script_bridge
{
    namespace
    {
        bool g_enabled{};
        unsigned g_intervalFrames{6};
        std::wstring g_directory;
        std::wstring g_vrPath;
        std::wstring g_vrTempPath;
        std::wstring g_gamePath;
        std::uint64_t g_frame{};
        std::mutex g_stateMutex;
        GameState g_state;
        int g_lastLoggedAmmo{-2};
        std::string g_lastLoggedWeapon{"\x01"};

        bool ReadBoolean(const wchar_t* ini, const wchar_t* key, bool fallback) noexcept
        {
            wchar_t value[16]{};
            GetPrivateProfileStringW(L"bridge", key, fallback ? L"true" : L"false", value,
                static_cast<DWORD>(std::size(value)), ini);
            return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
                _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
        }

        // DayZ's profile directory ($profile:): -profiles=<dir> on the command line,
        // else %LOCALAPPDATA%\DayZ (verified 1.29: the script mod's files landed there).
        std::wstring ProfileDirectory() noexcept
        {
            const wchar_t* commandLine = GetCommandLineW();
            if (const wchar_t* flag = commandLine ? wcsstr(commandLine, L"-profiles=") : nullptr)
            {
                flag += wcslen(L"-profiles=");
                std::wstring value;
                bool quoted = *flag == L'"';
                if (quoted)
                    ++flag;
                while (*flag && (quoted ? *flag != L'"' : *flag != L' '))
                    value += *flag++;
                if (!value.empty())
                    return value;
            }
            wchar_t localAppData[MAX_PATH]{};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData)))
                return std::wstring(localAppData) + L"\\DayZ";
            return L".";
        }

        std::string Narrow(const std::wstring& text)
        {
            return std::string(text.begin(), text.end());
        }

        void WriteVr() noexcept
        {
            const dayz::stereo_state::HmdOrientation hmd = dayz::stereo_state::GetHmdOrientation();
            const dayz::stereo_state::HmdPosition position = dayz::stereo_state::GetHmdPosition();
            const dayz::stereo_state::HmdOrientation aim = dayz::stereo_state::GetAimOrientation();
            const auto yawOf = [](const dayz::stereo_state::HmdOrientation& q) {
                return std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            };
            const auto pitchOf = [](const dayz::stereo_state::HmdOrientation& q) {
                const float s = 2.0f * (q.w * q.x - q.z * q.y);
                return std::asin(s < -1.0f ? -1.0f : s > 1.0f ? 1.0f : s);
            };
            const auto rollOf = [](const dayz::stereo_state::HmdOrientation& q) {
                return std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
            };
            const dayz::runtime_probe::DebugSnapshot probe = dayz::runtime_probe::GetDebugSnapshot();
            char text[768]{};
            sprintf_s(text,
                "frame=%llu\nhmd_valid=%d\nhmd_yaw=%.5f\nhmd_pitch=%.5f\nhmd_roll=%.5f\n"
                "hmd_x=%.4f\nhmd_y=%.4f\nhmd_z=%.4f\naim_valid=%d\naim_yaw=%.5f\naim_pitch=%.5f\n"
                "aim_yaw_error=%.5f\naim_pitch_error=%.5f\ngui_cursor=%d\n",
                static_cast<unsigned long long>(g_frame), hmd.valid ? 1 : 0,
                hmd.valid ? yawOf(hmd) : 0.0f, hmd.valid ? pitchOf(hmd) : 0.0f, hmd.valid ? rollOf(hmd) : 0.0f,
                position.x, position.y, position.z, aim.valid ? 1 : 0,
                aim.valid ? yawOf(aim) : 0.0f, aim.valid ? pitchOf(aim) : 0.0f,
                probe.aimYawError, probe.aimPitchError, probe.guiCursorMode ? 1 : 0);
            FILE* file{};
            if (_wfopen_s(&file, g_vrTempPath.c_str(), L"wb") != 0 || !file)
                return;
            fwrite(text, 1, strlen(text), file);
            fclose(file);
            // Atomic replace so the script never reads a half-written file.
            MoveFileExW(g_vrTempPath.c_str(), g_vrPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        void ReadGame() noexcept
        {
            FILE* file{};
            if (_wfopen_s(&file, g_gamePath.c_str(), L"rb") != 0 || !file)
                return;
            char buffer[2048]{};
            const std::size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
            fclose(file);
            buffer[length] = '\0';
            GameState state;
            state.valid = true;
            char* cursor = buffer;
            while (*cursor)
            {
                char* end = strpbrk(cursor, "\r\n");
                if (end)
                    *end = '\0';
                if (char* equals = strchr(cursor, '='))
                {
                    *equals = '\0';
                    const char* key = cursor;
                    const char* value = equals + 1;
                    if (!strcmp(key, "frame")) state.frame = atoi(value);
                    else if (!strcmp(key, "weapon")) state.weapon = value;
                    else if (!strcmp(key, "ammo")) state.ammo = atoi(value);
                    else if (!strcmp(key, "chamber")) state.chamber = atoi(value) != 0;
                    else if (!strcmp(key, "health_level")) state.healthLevel = atoi(value);
                    else if (!strcmp(key, "bleeding")) state.bleedingBits = atoi(value);
                    else if (!strcmp(key, "stamina")) state.stamina = static_cast<float>(atof(value));
                    else if (!strcmp(key, "inventory_open")) state.inventoryOpen = atoi(value) != 0;
                    else if (!strcmp(key, "stance")) state.stance = atoi(value);
                    else if (!strcmp(key, "raised")) state.raised = atoi(value) != 0;
                    else if (!strcmp(key, "in_vehicle")) state.inVehicle = atoi(value) != 0;
                }
                if (!end)
                    break;
                cursor = end + 1;
            }
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_state = state;
            }
            if (state.ammo != g_lastLoggedAmmo || state.weapon != g_lastLoggedWeapon)
            {
                g_lastLoggedAmmo = state.ammo;
                g_lastLoggedWeapon = state.weapon;
                logging::Info("Script bridge: weapon='" + state.weapon + "' ammo=" + std::to_string(state.ammo) +
                    " health_level=" + std::to_string(state.healthLevel) + " bleeding=" + std::to_string(state.bleedingBits) + " stance=" + std::to_string(state.stance));
            }
        }
    }

    void Initialize(const wchar_t* iniPath) noexcept
    {
        g_enabled = ReadBoolean(iniPath, L"enabled", true);
        wchar_t interval[16]{};
        GetPrivateProfileStringW(L"bridge", L"interval_frames", L"6", interval,
            static_cast<DWORD>(std::size(interval)), iniPath);
        const int parsed = _wtoi(interval);
        g_intervalFrames = parsed >= 1 && parsed <= 600 ? static_cast<unsigned>(parsed) : 6;
        if (!g_enabled)
        {
            logging::Info("Script bridge disabled");
            return;
        }
        g_directory = ProfileDirectory() + L"\\dayzvr";
        CreateDirectoryW(g_directory.c_str(), nullptr);
        g_vrPath = g_directory + L"\\vr.txt";
        g_vrTempPath = g_directory + L"\\vr.tmp";
        g_gamePath = g_directory + L"\\game.txt";
        logging::Info("Script bridge: exchanging files in " + Narrow(g_directory) + " every " +
            std::to_string(g_intervalFrames) + " frames");
    }

    void Update() noexcept
    {
        if (!g_enabled)
            return;
        if (++g_frame % g_intervalFrames != 0)
            return;
        WriteVr();
        ReadGame();
    }

    GameState GetGameState() noexcept
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_state;
    }

    bool Enabled() noexcept
    {
        return g_enabled;
    }
}
