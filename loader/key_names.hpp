// Hotkey binding grammar shared by the loader and its host-side test: "f12",
// "ctrl+shift+r", "numpad5", "0x7b", "pageup". Case-insensitive, modifiers in any
// order. Header-only and free of <Windows.h> so tests/key_names_test.cpp compiles on
// the Linux host; the virtual-key values are the documented Win32 constants.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace loader::keys
{
    inline constexpr std::uint16_t kF1 = 0x70;
    inline constexpr std::uint16_t kNumpad0 = 0x60;
    inline constexpr std::uint16_t kControl = 0x11;
    inline constexpr std::uint16_t kMenu = 0x12;
    inline constexpr std::uint16_t kShift = 0x10;

    struct Binding
    {
        std::uint16_t key{};
        bool control{};
        bool alt{};
        bool shift{};
        bool Enabled() const noexcept { return key != 0; }
    };

    struct NamedKey
    {
        std::string_view name;
        std::uint16_t code;
    };

    inline constexpr NamedKey kNamedKeys[]{
        {"home", 0x24}, {"end", 0x23}, {"insert", 0x2D}, {"delete", 0x2E},
        {"pageup", 0x21}, {"pagedown", 0x22}, {"pause", 0x13}, {"scrolllock", 0x91},
        {"backspace", 0x08}, {"tab", 0x09}, {"space", 0x20}, {"enter", 0x0D},
        {"escape", 0x1B}, {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27},
        // The key left of "1" on a German QWERTZ layout (^ / °); layout-dependent.
        {"caret", 0xDC}, {"grave", 0xC0},
    };

    inline std::string Lower(std::string_view text)
    {
        std::string lower(text);
        for (char& character : lower)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return lower;
    }

    inline bool ParseKeyName(std::string_view rawName, std::uint16_t& code) noexcept
    {
        const std::string name = Lower(rawName);
        if (name.empty())
            return false;
        if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9')))
        {
            code = static_cast<std::uint16_t>(std::toupper(static_cast<unsigned char>(name[0])));
            return true;
        }
        if (name[0] == 'f' && name.size() <= 3)
        {
            const int number = std::atoi(name.c_str() + 1);
            if (number >= 1 && number <= 24)
            {
                code = static_cast<std::uint16_t>(kF1 + number - 1);
                return true;
            }
        }
        if (name.rfind("numpad", 0) == 0 && name.size() == 7 && name[6] >= '0' && name[6] <= '9')
        {
            code = static_cast<std::uint16_t>(kNumpad0 + (name[6] - '0'));
            return true;
        }
        if (name.rfind("0x", 0) == 0)
        {
            char* end{};
            const unsigned long value = std::strtoul(name.c_str(), &end, 16);
            if (end != name.c_str() && *end == '\0' && value > 0 && value < 0x100)
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

    // "ctrl+shift+f12" -> Binding. An empty or "none" text yields a disabled binding
    // and returns true; unknown names return false.
    inline bool ParseBinding(std::string_view text, Binding& binding) noexcept
    {
        binding = {};
        const std::string lower = Lower(text);
        if (lower.empty() || lower == "none" || lower == "off")
            return true;
        std::size_t start = 0;
        while (start <= lower.size())
        {
            std::size_t end = lower.find('+', start);
            if (end == std::string::npos)
                end = lower.size();
            std::string_view part(lower.data() + start, end - start);
            while (!part.empty() && part.front() == ' ')
                part.remove_prefix(1);
            while (!part.empty() && part.back() == ' ')
                part.remove_suffix(1);
            if (part == "ctrl" || part == "control")
                binding.control = true;
            else if (part == "alt")
                binding.alt = true;
            else if (part == "shift")
                binding.shift = true;
            else if (!ParseKeyName(part, binding.key))
                return false;
            if (end == lower.size())
                break;
            start = end + 1;
        }
        return binding.key != 0;
    }

    inline std::string Describe(const Binding& binding)
    {
        if (!binding.Enabled())
            return "none";
        std::string text;
        if (binding.control) text += "ctrl+";
        if (binding.alt) text += "alt+";
        if (binding.shift) text += "shift+";
        if (binding.key >= kF1 && binding.key < kF1 + 24)
            return text + "f" + std::to_string(binding.key - kF1 + 1);
        if (binding.key >= kNumpad0 && binding.key < kNumpad0 + 10)
            return text + "numpad" + std::to_string(binding.key - kNumpad0);
        if ((binding.key >= 'A' && binding.key <= 'Z') || (binding.key >= '0' && binding.key <= '9'))
            return text + static_cast<char>(std::tolower(binding.key));
        for (const NamedKey& key : kNamedKeys)
            if (key.code == binding.key)
                return text + std::string(key.name);
        char hex[8]{};
        std::snprintf(hex, sizeof(hex), "0x%02x", binding.key);
        return text + hex;
    }
}
