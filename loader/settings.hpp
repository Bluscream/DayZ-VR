#pragma once

#include "dayz_plugin_api.h"

#include <cstddef>
#include <string>

namespace loader::settings
{
    // Values live in the owning plugin's config file ("section.key"). Register returns 0
    // or -1 (bad descriptor, duplicate key). Get copies the ini value or the default
    // and returns its length, -1 for an unknown key. Set validates against the
    // descriptor, writes the ini and returns 0; -1 unknown key, -2 rejected value.
    // The plugin's descriptor strings must stay valid for the process lifetime.
    int Register(std::size_t pluginIndex, const std::wstring& configPath,
        const DayzSettingDesc* desc) noexcept;
    int Get(std::size_t pluginIndex, const char* key, char* buffer, std::size_t capacity) noexcept;
    int Set(std::size_t pluginIndex, const char* key, const char* value,
        void (*notify)(std::size_t pluginIndex, const char* key, const char* value)) noexcept;
    std::size_t Count() noexcept;
    // Validation only (no ini), exposed for the host-side test through settings_check.hpp.
}
