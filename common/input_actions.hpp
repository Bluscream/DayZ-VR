#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

// Desired values for DayZ's named input actions ("UAMoveForward", "UAFire", ...),
// written by the VR host on its own thread and read by the engine hooks on the game
// thread (see dayz_input_hooks.cpp). The table is pure and host-testable: it knows
// nothing about the engine, only names, analogue values and the per-frame edge flags
// DayZ's getters expose (press, release, hold begin). Latch() is called once per
// engine frame by the consumer and freezes the state the getters return until the
// next frame, so a value set mid-frame cannot produce a press without its release.
namespace dayz::input_actions
{
    struct Latched
    {
        float value{};     // LocalValue: 0..1 for digital actions, analogue for axes
        bool held{};       // LocalHold
        bool press{};      // LocalPress: first frame held
        bool release{};    // LocalRelease: first frame no longer held
        bool holdBegin{};  // LocalHoldBegin: same frame as press
    };

    class Table
    {
    public:
        static constexpr std::size_t kCapacity = 64;
        static constexpr std::size_t kNameLength = 40;

        // Producer side. value is the analogue reading (0..1 for buttons), held the
        // digital state. Returns false when the name is too long or the table is full.
        bool Set(std::string_view name, float value, bool held) noexcept;
        // Releases the action: it stays for one more Latch() to deliver the release edge.
        void Clear(std::string_view name) noexcept;
        void ClearAll() noexcept;

        // Consumer side, once per engine frame.
        void Latch() noexcept;
        bool Get(std::string_view name, Latched& out) const noexcept;
        // Engine handles attached by the native layer after resolving the name.
        bool GetByHandle(std::uintptr_t handle, Latched& out) const noexcept;
        bool GetById(int id, Latched& out) const noexcept;
        bool SetHandle(std::string_view name, std::uintptr_t handle, int id) noexcept;
        // Names that have no handle yet; the callback receives the name and returns
        // true when it attached one (handle, id) through SetHandle.
        template<typename Fn>
        void ForEachUnresolved(Fn&& fn) const
        {
            char names[kCapacity][kNameLength]{};
            std::size_t count = CopyUnresolvedNames(names);
            for (std::size_t index = 0; index < count; ++index)
                fn(std::string_view(names[index]));
        }
        std::size_t Count() const noexcept;

    private:
        struct Entry
        {
            char name[kNameLength]{};
            bool active{};
            bool removing{};
            float value{};
            bool held{};
            bool previousHeld{};
            Latched latched{};
            std::uintptr_t handle{};
            int id{-1};
            bool resolved{};
        };
        Entry* Find(std::string_view name) noexcept;
        const Entry* Find(std::string_view name) const noexcept;
        std::size_t CopyUnresolvedNames(char (*names)[kNameLength]) const noexcept;

        mutable std::mutex mutex_;
        Entry entries_[kCapacity]{};
    };

    // Process-wide table used by the host and the hooks.
    Table& Global() noexcept;
}
