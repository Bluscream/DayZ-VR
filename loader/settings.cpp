#include "settings.hpp"

#include "logging.hpp"
#include "settings_check.hpp"

#include <Windows.h>

#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

namespace
{
    struct Entry
    {
        std::size_t pluginIndex;
        std::wstring configPath;
        std::string key;
        std::wstring section;
        std::wstring name;
        DayzSettingDesc desc;   // strings point into the plugin; it keeps them alive
    };

    std::mutex g_mutex;
    std::vector<Entry> g_entries;

    std::wstring Widen(std::string_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (char c : text)
            out += static_cast<wchar_t>(static_cast<unsigned char>(c));
        return out;
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t c : text)
            out += c < 0x80 ? static_cast<char>(c) : '?';
        return out;
    }

    Entry* Find(std::size_t pluginIndex, const char* key) noexcept
    {
        if (!key)
            return nullptr;
        for (Entry& entry : g_entries)
            if (entry.pluginIndex == pluginIndex && _stricmp(entry.key.c_str(), key) == 0)
                return &entry;
        return nullptr;
    }
}

namespace loader::settings
{
    int Register(std::size_t pluginIndex, const std::wstring& configPath,
        const DayzSettingDesc* desc) noexcept
    {
        if (!desc || !settings_check::ValidDescriptor(*desc))
        {
            logging::Error(std::string("setting_register: bad descriptor") +
                (desc && desc->key ? std::string(" for ") + desc->key : std::string()));
            return -1;
        }
        std::string section, name;
        settings_check::SplitKey(desc->key, section, name);
        std::scoped_lock lock(g_mutex);
        if (Find(pluginIndex, desc->key))
        {
            logging::Error(std::string("setting_register: duplicate ") + desc->key);
            return -1;
        }
        g_entries.push_back({pluginIndex, configPath, desc->key, Widen(section), Widen(name), *desc});
        return 0;
    }

    int Get(std::size_t pluginIndex, const char* key, char* buffer, std::size_t capacity) noexcept
    {
        std::scoped_lock lock(g_mutex);
        const Entry* entry = Find(pluginIndex, key);
        if (!entry)
            return -1;
        wchar_t value[512]{};
        GetPrivateProfileStringW(entry->section.c_str(), entry->name.c_str(),
            Widen(entry->desc.default_value).c_str(), value, static_cast<DWORD>(std::size(value)),
            entry->configPath.c_str());
        const std::string text = Narrow(value);
        if (buffer && capacity)
        {
            const std::size_t copy = text.size() < capacity - 1 ? text.size() : capacity - 1;
            std::memcpy(buffer, text.data(), copy);
            buffer[copy] = '\0';
        }
        return static_cast<int>(text.size());
    }

    int Set(std::size_t pluginIndex, const char* key, const char* value,
        void (*notify)(std::size_t pluginIndex, const char* key, const char* value)) noexcept
    {
        if (!value)
            return -2;
        std::wstring section, name, configPath;
        std::string fullKey;
        {
            std::scoped_lock lock(g_mutex);
            const Entry* entry = Find(pluginIndex, key);
            if (!entry)
                return -1;
            if (!settings_check::Accepts(entry->desc, value))
                return -2;
            section = entry->section;
            name = entry->name;
            configPath = entry->configPath;
            fullKey = entry->key;
        }
        if (!WritePrivateProfileStringW(section.c_str(), name.c_str(), Widen(value).c_str(),
                configPath.c_str()))
        {
            logging::Error("setting_set: cannot write " + fullKey);
            return -2;
        }
        logging::Info("setting " + fullKey + " = " + value);
        if (notify)
            notify(pluginIndex, fullKey.c_str(), value);
        return 0;
    }

    std::size_t Count() noexcept
    {
        std::scoped_lock lock(g_mutex);
        return g_entries.size();
    }
}
