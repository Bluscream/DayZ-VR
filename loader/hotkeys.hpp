#pragma once

#include "key_names.hpp"

#include <cstddef>
#include <string>

namespace loader::hotkeys
{
    // Binding source: dayz_pluginloader.ini [hotkeys] <plugin>.<action>, else defaultKey.
    // Returns the hotkey id or -1 (bad action name, duplicate, unparsable binding).
    int Register(std::size_t pluginIndex, const std::string& pluginName, const char* action,
        const char* title, const char* defaultKey, const wchar_t* loaderIni) noexcept;
    // Edge-triggered poll; calls dispatch(pluginIndex, id, action) for each new press.
    // Runs on the present thread.
    void Poll(void (*dispatch)(std::size_t pluginIndex, int id, const char* action)) noexcept;
    std::size_t Count() noexcept;
}
