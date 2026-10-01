#include "comfort.hpp"

#include "stereo_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <iterator>

#include <windows.h>

namespace dayz::comfort
{
    namespace
    {
        bool g_enabled{};
        float g_strength{0.8f};
        float g_radius{0.6f};
        float g_fadeSeconds{0.2f};
        float g_current{};

        bool ReadBoolean(const wchar_t* ini, const wchar_t* key, bool fallback) noexcept
        {
            wchar_t value[16]{};
            GetPrivateProfileStringW(L"comfort", key, fallback ? L"true" : L"false", value,
                static_cast<DWORD>(std::size(value)), ini);
            return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
                _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
        }

        float ReadFloat(const wchar_t* ini, const wchar_t* key, float fallback) noexcept
        {
            wchar_t fallbackText[32]{};
            swprintf_s(fallbackText, L"%.4f", fallback);
            wchar_t value[32]{};
            GetPrivateProfileStringW(L"comfort", key, fallbackText, value,
                static_cast<DWORD>(std::size(value)), ini);
            wchar_t* end{};
            const float parsed = std::wcstof(value, &end);
            return end != value ? parsed : fallback;
        }
    }

    void Initialize(const wchar_t* iniPath) noexcept
    {
        g_enabled = ReadBoolean(iniPath, L"vignette", false);
        g_strength = (std::clamp)(ReadFloat(iniPath, L"vignette_strength", 0.8f), 0.0f, 1.0f);
        g_radius = (std::clamp)(ReadFloat(iniPath, L"vignette_radius", 0.6f), 0.05f, 1.0f);
        g_fadeSeconds = (std::clamp)(ReadFloat(iniPath, L"vignette_fade_seconds", 0.2f),
            0.0f, 5.0f);
        g_current = 0.0f;
        dayz::stereo_state::SetComfortVignette(0.0f, g_radius);
    }

    void Update(bool moving, bool turning, float deltaSeconds) noexcept
    {
        if (!g_enabled)
            return;
        const float target = (moving || turning) ? g_strength : 0.0f;
        if (g_fadeSeconds <= 0.0f)
            g_current = target;
        else
        {
            const float step = (std::clamp)(deltaSeconds, 0.0f, 0.1f) / g_fadeSeconds * g_strength;
            g_current = target > g_current ? (std::min)(g_current + step, target)
                                           : (std::max)(g_current - step, target);
        }
        dayz::stereo_state::SetComfortVignette(g_current, g_radius);
    }
}
