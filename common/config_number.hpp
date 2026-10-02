#pragma once

#include <cerrno>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace dayz::config_number
{
    // INI input is a terminated wide string. Only a complete, finite number may
    // reach camera transforms or shader constants; whitespace is harmless.
    inline float ParseFloat(const wchar_t* text, float fallback) noexcept
    {
        wchar_t* end{};
        errno = 0;
        const float value = std::wcstof(text, &end);
        if (end == text || errno == ERANGE || !std::isfinite(value))
            return fallback;
        while (*end && std::iswspace(static_cast<std::wint_t>(*end)))
            ++end;
        return *end == L'\0' ? value : fallback;
    }
}
