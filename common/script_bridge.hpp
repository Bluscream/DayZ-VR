#pragma once

#include <string>

// Native half of the Enforce Script bridge. DayZ's script API can only reach
// files under its profile directory, so the two sides exchange small
// "key=value" text files in <profile>/dayzvr/ (see enforce/DayZVR):
//   vr.txt    written here every [bridge] interval_frames: HMD and controller
//             pose, aim lock states, closed-loop errors.
//   game.txt  written by the script mod: weapon, ammo, health level, stance, ...
// Everything is optional: without the script mod the folder simply stays
// one-directional.
namespace dayz::script_bridge
{
    struct GameState
    {
        bool valid{};
        int frame{-1};
        std::string weapon;
        int ammo{-1};
        bool chamber{};
        int healthLevel{};   // eInjuryHandlerLevels 0 pristine .. 4 ruined (client-synced)
        int bleedingBits{};  // bit mask of active bleeding sources
        float stamina{};
        bool inventoryOpen{};
        int stance{-1};
        bool raised{};
        bool inVehicle{};
        bool melee{};        // fists or a melee weapon in hands (motion melee may swing)
    };

    void Initialize(const wchar_t* iniPath) noexcept;
    // Called once per presented frame from the runtime probe.
    void Update() noexcept;
    GameState GetGameState() noexcept;
    bool Enabled() noexcept;
}
