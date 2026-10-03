#pragma once

#include <cstdint>

// Keyboard hotkeys the proxy polls once per presented frame. Kept outside the
// runtime probe so the core render path only sees a single Poll() call.
//
// Keys are configured in the [hotkeys] ini section as a modifier chain plus a
// key name, for example "F9", "ctrl+alt+home" or "shift+numpad5". Key names:
// F1..F24, A..Z, 0..9, numpad0..numpad9, home, end, insert, delete, pageup,
// pagedown, pause, scrolllock, backspace, tab, space, enter, escape, or a raw
// virtual-key code as 0x.. hex. An empty value disables the hotkey.
//
// toggle1..toggle8 flip a boolean tunable: "F10 stereo.lock_pitch" (key, space,
// tunable name as listed by the debug plugin).
namespace dayz::hotkeys
{
    struct Binding
    {
        std::uint16_t key{};
        bool control{};
        bool alt{};
        bool shift{};
        bool Enabled() const noexcept { return key != 0; }
    };

    // Parses "ctrl+alt+F9" style text. Returns false (and an empty binding) on
    // unknown names so a typo never silently binds a different key.
    bool ParseBinding(const wchar_t* text, Binding& binding) noexcept;

    void Initialize(const wchar_t* iniPath) noexcept;

    // Edge-triggered poll; only reacts while DayZ's window is the real foreground
    // window, so typing in another app never recenters the view.
    void Poll() noexcept;
}
