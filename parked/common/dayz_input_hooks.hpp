#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Direct action input: DayZ's gameplay code reads named input actions
// ("UAMoveForward", "UAFire", ...) through the Input interface getters (value, press,
// release, hold, hold begin by id or by record) and the axis-pair getter used for
// aiming. These hooks let the VR host supply its own values for any action, which the
// engine then treats exactly like a key or a stick: no SendInput, no window focus
// requirement, analogue movement, one code path for keyboard and VR. The engine's own
// reading still counts (values take the maximum, flags are OR-ed), so the keyboard
// keeps working next to the controllers. Addresses are 1.29.163709
// (docs/research/input.md) and guarded by dayz_build_checks.hpp; when they do not
// match, the hooks stay off and Active() is false so the host falls back to SendInput.
namespace dayz::input_hooks
{
    // Reads [input] from the ini, validates the build and installs the hooks. Needs
    // MinHook initialized. moduleBase/imageSize describe the loaded DayZ_x64.exe.
    void Initialize(const wchar_t* iniPath, std::uintptr_t moduleBase, std::size_t imageSize) noexcept;
    // True when the hooks are installed and enabled.
    bool Active() noexcept;
    // True when head/stick aiming goes through the axis-pair getter instead of mouse counts.
    bool DirectAimEnabled() noexcept;
    // True while the engine's game-focus counter says a menu, the inventory or the death
    // screen owns the input (HasGameFocus false): gameplay actions are ignored there and
    // the GUI needs real mouse clicks.
    bool MenuOwnsInput() noexcept;

    // Producer side (any thread). Values are latched once per game frame.
    void SetAction(std::string_view name, float value, bool held) noexcept;
    void ClearAction(std::string_view name) noexcept;
    void ClearAllActions() noexcept;
    // Accumulates an aim change (radians, yaw positive = right, pitch positive = up)
    // that the next game frame consumes through the axis-pair getter.
    void AddAimDelta(float yawRadians, float pitchRadians) noexcept;

    struct Stats
    {
        std::uint64_t frames{};        // game frames seen (player input controller updates)
        std::uint64_t overrides{};     // getter calls answered with a VR value
        std::uint32_t resolved{};      // actions with an engine record attached
        std::uint32_t unresolved{};    // actions the registry does not know
        float lastFrameSeconds{};      // dt of the last game frame
    };
    Stats GetStats() noexcept;
}
