// Setting value validation shared by the loader and tests/key_names_test.cpp.
// Header-only and free of <Windows.h>.
#pragma once

#include "dayz_plugin_api.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>

namespace loader::settings_check
{
    inline std::string Lower(std::string_view text)
    {
        std::string lower(text);
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower;
    }

    inline bool IsBoolean(std::string_view value) noexcept
    {
        const std::string lower = Lower(value);
        return lower == "true" || lower == "false" || lower == "1" || lower == "0" ||
            lower == "yes" || lower == "no" || lower == "on" || lower == "off";
    }

    inline bool ParseNumber(std::string_view value, double& out) noexcept
    {
        if (value.empty())
            return false;
        const std::string text(value);
        char* end{};
        errno = 0;
        out = std::strtod(text.c_str(), &end);
        return errno == 0 && end != text.c_str() && *end == '\0';
    }

    inline bool InChoices(std::string_view choices, std::string_view value) noexcept
    {
        std::size_t start = 0;
        while (start <= choices.size())
        {
            std::size_t end = choices.find('|', start);
            if (end == std::string_view::npos)
                end = choices.size();
            if (choices.substr(start, end - start) == value)
                return true;
            if (end == choices.size())
                break;
            start = end + 1;
        }
        return false;
    }

    // True when value is acceptable for the descriptor.
    inline bool Accepts(const DayzSettingDesc& desc, std::string_view value) noexcept
    {
        switch (desc.type)
        {
        case DAYZ_SETTING_BOOL:
            return IsBoolean(value);
        case DAYZ_SETTING_INT:
        case DAYZ_SETTING_FLOAT:
        {
            double number{};
            if (!ParseNumber(value, number))
                return false;
            if (desc.type == DAYZ_SETTING_INT && number != static_cast<double>(static_cast<long long>(number)))
                return false;
            return !(desc.minimum < desc.maximum) || (number >= desc.minimum && number <= desc.maximum);
        }
        case DAYZ_SETTING_ENUM:
            return desc.choices && InChoices(desc.choices, value);
        case DAYZ_SETTING_STRING:
            return value.find('\n') == std::string_view::npos && value.find('\r') == std::string_view::npos;
        }
        return false;
    }

    // "section.key" -> section, key. False without a dot or with an empty half.
    inline bool SplitKey(std::string_view full, std::string& section, std::string& key)
    {
        const std::size_t dot = full.find('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 >= full.size())
            return false;
        section.assign(full.substr(0, dot));
        key.assign(full.substr(dot + 1));
        return true;
    }

    inline bool ValidDescriptor(const DayzSettingDesc& desc) noexcept
    {
        if (desc.struct_size < sizeof(DayzSettingDesc) || !desc.key || !desc.default_value)
            return false;
        std::string section, key;
        if (!SplitKey(desc.key, section, key))
            return false;
        if (desc.type == DAYZ_SETTING_ENUM && !desc.choices)
            return false;
        return Accepts(desc, desc.default_value);
    }
}
