#include "dayz_hotkeys.hpp"

#include "dayz_runtime_probe.hpp"
#include "logging.hpp"

#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>

#include <windows.h>

namespace dayz::hotkeys
{
    namespace
    {
        struct NamedKey
        {
            const wchar_t* name;
            std::uint16_t code;
        };

        constexpr NamedKey kNamedKeys[]{
            {L"home", VK_HOME}, {L"end", VK_END}, {L"insert", VK_INSERT},
            {L"delete", VK_DELETE}, {L"pageup", VK_PRIOR}, {L"pagedown", VK_NEXT},
            {L"pause", VK_PAUSE}, {L"scrolllock", VK_SCROLL}, {L"backspace", VK_BACK},
            {L"tab", VK_TAB}, {L"space", VK_SPACE}, {L"enter", VK_RETURN},
            {L"escape", VK_ESCAPE}, {L"up", VK_UP}, {L"down", VK_DOWN},
            {L"left", VK_LEFT}, {L"right", VK_RIGHT},
        };

        Binding g_recenter{};
        bool g_recenterWasDown{};

        std::wstring Lower(const wchar_t* begin, const wchar_t* end)
        {
            std::wstring text(begin, end);
            for (wchar_t& character : text)
                character = static_cast<wchar_t>(std::towlower(character));
            return text;
        }

        bool ParseKeyName(const std::wstring& name, std::uint16_t& code) noexcept
        {
            if (name.empty())
                return false;
            if (name.size() == 1 && ((name[0] >= L'a' && name[0] <= L'z') ||
                (name[0] >= L'0' && name[0] <= L'9')))
            {
                code = static_cast<std::uint16_t>(std::towupper(name[0]));
                return true;
            }
            if (name[0] == L'f' && name.size() <= 3)
            {
                const int number = _wtoi(name.c_str() + 1);
                if (number >= 1 && number <= 24)
                {
                    code = static_cast<std::uint16_t>(VK_F1 + number - 1);
                    return true;
                }
            }
            if (name.rfind(L"numpad", 0) == 0 && name.size() == 7 &&
                name[6] >= L'0' && name[6] <= L'9')
            {
                code = static_cast<std::uint16_t>(VK_NUMPAD0 + (name[6] - L'0'));
                return true;
            }
            if (name.rfind(L"0x", 0) == 0)
            {
                wchar_t* end{};
                const unsigned long value = std::wcstoul(name.c_str(), &end, 16);
                if (end != name.c_str() && *end == L'\0' && value > 0 && value < 0x100)
                {
                    code = static_cast<std::uint16_t>(value);
                    return true;
                }
            }
            for (const NamedKey& key : kNamedKeys)
                if (name == key.name)
                {
                    code = key.code;
                    return true;
                }
            return false;
        }

        bool KeyDown(int key) noexcept
        {
            return (GetAsyncKeyState(key) & 0x8000) != 0;
        }

        bool BindingDown(const Binding& binding) noexcept
        {
            return binding.Enabled() && KeyDown(binding.key) &&
                KeyDown(VK_CONTROL) == binding.control && KeyDown(VK_MENU) == binding.alt &&
                KeyDown(VK_SHIFT) == binding.shift;
        }

        Binding ReadBinding(const wchar_t* iniPath, const wchar_t* key,
            const wchar_t* fallback) noexcept
        {
            wchar_t value[64]{};
            GetPrivateProfileStringW(L"hotkeys", key, fallback, value,
                static_cast<DWORD>(std::size(value)), iniPath);
            Binding binding{};
            if (!ParseBinding(value, binding))
            {
                std::wstring text(value);
                logging::Error("Hotkey " + std::string(key, key + wcslen(key)) +
                    " ignored: cannot parse '" + std::string(text.begin(), text.end()) + "'");
            }
            return binding;
        }

        bool GameWindowForeground() noexcept
        {
            const HWND window = dayz::runtime_probe::RealForegroundWindow();
            DWORD processId{};
            return window && GetWindowThreadProcessId(window, &processId) &&
                processId == GetCurrentProcessId();
        }
    }

    bool ParseBinding(const wchar_t* text, Binding& binding) noexcept
    {
        binding = {};
        if (!text)
            return true;
        Binding parsed{};
        const wchar_t* cursor = text;
        while (*cursor)
        {
            while (*cursor == L' ' || *cursor == L'\t')
                ++cursor;
            const wchar_t* start = cursor;
            while (*cursor && *cursor != L'+')
                ++cursor;
            const wchar_t* stop = cursor;
            while (stop > start && (stop[-1] == L' ' || stop[-1] == L'\t'))
                --stop;
            const std::wstring token = Lower(start, stop);
            if (*cursor == L'+')
                ++cursor;
            if (token.empty())
                continue;
            if (token == L"ctrl" || token == L"control")
                parsed.control = true;
            else if (token == L"alt")
                parsed.alt = true;
            else if (token == L"shift")
                parsed.shift = true;
            else if (parsed.key == 0 && ParseKeyName(token, parsed.key))
                continue;
            else
                return false;
        }
        if (parsed.key == 0 && (parsed.control || parsed.alt || parsed.shift))
            return false;
        binding = parsed;
        return true;
    }

    void Initialize(const wchar_t* iniPath) noexcept
    {
        g_recenter = ReadBinding(iniPath, L"recenter", L"");
        g_recenterWasDown = false;
        if (g_recenter.Enabled())
            logging::Info("Hotkeys: recenter bound to virtual key 0x" +
                [] {
                    char text[8]{};
                    sprintf_s(text, "%02X", g_recenter.key);
                    return std::string(text);
                }() +
                (g_recenter.control ? " +ctrl" : "") + (g_recenter.alt ? " +alt" : "") +
                (g_recenter.shift ? " +shift" : ""));
        else
            logging::Info("Hotkeys: recenter unbound");
    }

    void Poll() noexcept
    {
        if (!g_recenter.Enabled())
            return;
        const bool down = GameWindowForeground() && BindingDown(g_recenter);
        if (down && !g_recenterWasDown)
        {
            dayz::runtime_probe::RecenterHmd();
            logging::Info("Recenter hotkey pressed");
        }
        g_recenterWasDown = down;
    }
}
