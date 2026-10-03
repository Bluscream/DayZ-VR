#include "hotkeys.hpp"

#include "logging.hpp"

#include <Windows.h>

#include <iterator>
#include <mutex>
#include <vector>

namespace
{
    struct Entry
    {
        std::size_t pluginIndex;
        std::string action;      // "<plugin>.<action>"
        std::string title;
        loader::keys::Binding binding;
        bool wasDown;
    };

    std::mutex g_mutex;
    std::vector<Entry> g_entries;

    bool ValidIdentifier(const char* text) noexcept
    {
        if (!text || !*text)
            return false;
        for (const char* c = text; *c; ++c)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
                return false;
        return true;
    }

    bool KeyDown(int key) noexcept
    {
        // Bit 0 reports a press since the previous query, so a tap shorter than one
        // frame is still seen by the per-frame poll.
        return (GetAsyncKeyState(key) & 0x8001) != 0;
    }

    bool BindingDown(const loader::keys::Binding& binding) noexcept
    {
        return binding.Enabled() && KeyDown(binding.key) &&
            KeyDown(loader::keys::kControl) == binding.control &&
            KeyDown(loader::keys::kMenu) == binding.alt &&
            KeyDown(loader::keys::kShift) == binding.shift;
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t c : text)
            out += c < 0x80 ? static_cast<char>(c) : '?';
        return out;
    }
}

namespace loader::hotkeys
{
    int Register(std::size_t pluginIndex, const std::string& pluginName, const char* action,
        const char* title, const char* defaultKey, const wchar_t* loaderIni) noexcept
    {
        if (!ValidIdentifier(action))
        {
            logging::Error("hotkey_register: action name must be [a-z0-9_] (" + pluginName + ")");
            return -1;
        }
        const std::string full = pluginName + "." + action;
        std::scoped_lock lock(g_mutex);
        for (const Entry& entry : g_entries)
            if (entry.action == full)
            {
                logging::Error("hotkey_register: duplicate " + full);
                return -1;
            }
        const std::wstring wideKey(full.begin(), full.end());
        std::wstring wideDefault;
        for (const char* c = defaultKey ? defaultKey : ""; *c; ++c)
            wideDefault += static_cast<wchar_t>(*c);
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"hotkeys", wideKey.c_str(), wideDefault.c_str(), value,
            static_cast<DWORD>(std::size(value)), loaderIni);
        keys::Binding binding{};
        const std::string text = Narrow(value);
        if (!keys::ParseBinding(text, binding))
        {
            logging::Error("hotkey " + full + " ignored: cannot parse '" + text + "'");
            if (!keys::ParseBinding(defaultKey ? defaultKey : "", binding))
                binding = {};
        }
        g_entries.push_back({pluginIndex, full, title ? title : "", binding, false});
        logging::Info("hotkey " + full + " = " + keys::Describe(binding) +
            (title && *title ? std::string(" (") + title + ")" : std::string()));
        return static_cast<int>(g_entries.size() - 1);
    }

    void Poll(void (*dispatch)(std::size_t pluginIndex, int id, const char* action)) noexcept
    {
        // Copy the bindings so a registration from a plugin callback cannot deadlock.
        std::vector<std::pair<int, bool>> presses;
        {
            std::scoped_lock lock(g_mutex);
            for (std::size_t index = 0; index < g_entries.size(); ++index)
            {
                Entry& entry = g_entries[index];
                const bool down = BindingDown(entry.binding);
                if (down && !entry.wasDown)
                    presses.emplace_back(static_cast<int>(index), true);
                entry.wasDown = down;
            }
        }
        for (const auto& [id, pressed] : presses)
        {
            std::size_t pluginIndex{};
            std::string action;
            {
                std::scoped_lock lock(g_mutex);
                pluginIndex = g_entries[static_cast<std::size_t>(id)].pluginIndex;
                action = g_entries[static_cast<std::size_t>(id)].action;
            }
            logging::Info("hotkey pressed: " + action);
            dispatch(pluginIndex, id, action.c_str());
        }
    }

    std::size_t Count() noexcept
    {
        std::scoped_lock lock(g_mutex);
        return g_entries.size();
    }
}
