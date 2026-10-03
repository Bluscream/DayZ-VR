#include "dayz_input_hooks.hpp"

#include "dayz_build_checks.hpp"
#include "input_actions.hpp"
#include "logging.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <span>
#include <sstream>
#include <string>

#include <windows.h>

#include <MinHook.h>

namespace dayz::input_hooks
{
    namespace
    {
        // 1.29.163709 (docs/research/input.md). The Input interface is the object at
        // Input+0x28; its getters take (self, id|record, checkFocus).
        constexpr std::uintptr_t kValueByIdRva = 0x005F5CF0;
        constexpr std::uintptr_t kValueByRecordRva = 0x005F5D50;
        constexpr std::uintptr_t kPressByIdRva = 0x005F5A90;
        constexpr std::uintptr_t kPressByRecordRva = 0x005F5B00;
        constexpr std::uintptr_t kReleaseByIdRva = 0x005F5BC0;
        constexpr std::uintptr_t kReleaseByRecordRva = 0x005F5C30;
        constexpr std::uintptr_t kHoldByIdRva = 0x005F5960;
        constexpr std::uintptr_t kHoldByRecordRva = 0x005F59D0;
        constexpr std::uintptr_t kHoldBeginByIdRva = 0x005F5460;
        constexpr std::uintptr_t kHoldBeginByRecordRva = 0x005F54D0;
        constexpr std::uintptr_t kAxisPairRva = 0x005F5E10;
        constexpr std::uintptr_t kRegistryGetterRva = 0x00534AE0;
        // Input system object pointer (DAT_14100A360) and its game-focus counter: while
        // the counter is above zero a menu, the inventory or the death screen owns the
        // input and HasGameFocus (0x5F6900) is false for every gameplay action.
        constexpr std::uintptr_t kInputObjectPointerRva = 0x0100A360;
        constexpr std::ptrdiff_t kGameFocusCounterOffset = 0x183D4;
        constexpr std::uintptr_t kLookupByNameRva = 0x00534540;
        constexpr std::uintptr_t kPlayerInputUpdateRva = 0x004F5D40;
        // Record layout.
        constexpr std::ptrdiff_t kRecordIndexOffset = 0x74;   // -0xFFFF marks the default record
        constexpr std::ptrdiff_t kRecordIdOffset = 0x7C;      // index into registry+0x98
        constexpr std::ptrdiff_t kRegistryRecordsOffset = 0x98;
        constexpr std::ptrdiff_t kRegistryRecordCountOffset = 0xA4;
        constexpr int kMissingRecordIndex = -0xFFFF;

        using ValueByIdFn = float(__fastcall*)(void* self, unsigned id, bool checkFocus);
        using ValueByRecordFn = float(__fastcall*)(void* self, void* record, bool checkFocus);
        using FlagByIdFn = std::uint64_t(__fastcall*)(void* self, unsigned id, bool checkFocus);
        using FlagByRecordFn = std::uint64_t(__fastcall*)(void* self, void* record, bool checkFocus);
        using AxisPairFn = float(__fastcall*)(void* self, int mode, unsigned idA, unsigned idB, bool checkFocus);
        using RegistryGetterFn = void*(__fastcall*)();
        using LookupByNameFn = void*(__fastcall*)(void* registry, const char* name);
        using PlayerInputUpdateFn = void(__fastcall*)(void* self, void* player, float dt);

        ValueByIdFn g_valueById{};
        ValueByRecordFn g_valueByRecord{};
        FlagByIdFn g_pressById{};
        FlagByRecordFn g_pressByRecord{};
        FlagByIdFn g_releaseById{};
        FlagByRecordFn g_releaseByRecord{};
        FlagByIdFn g_holdById{};
        FlagByRecordFn g_holdByRecord{};
        FlagByIdFn g_holdBeginById{};
        FlagByRecordFn g_holdBeginByRecord{};
        AxisPairFn g_axisPair{};
        PlayerInputUpdateFn g_playerInputUpdate{};
        RegistryGetterFn g_registry{};
        LookupByNameFn g_lookupByName{};

        std::atomic_bool g_active{};
        std::atomic_bool g_directAim{};
        // [input] mouse_look=false: the aim axis pairs report only the VR rate; the
        // engine's own mouse/stick aim deltas never reach the camera.
        std::atomic_bool g_mouseLook{true};
        std::uintptr_t g_moduleBase{};
        std::atomic_uint64_t g_frames{};
        std::atomic_uint64_t g_overrides{};
        std::atomic<float> g_lastDt{};
        // Aim: the host accumulates radians, the game frame consumes them once.
        std::atomic<float> g_pendingYaw{};
        std::atomic<float> g_pendingPitch{};
        float g_frameYawRate{};    // radians per second for the current game frame
        float g_framePitchRate{};
        float g_aimYawSign{1.0f};
        float g_aimPitchSign{1.0f};
        int g_aimRightId{-1}, g_aimLeftId{-1}, g_aimDownId{-1}, g_aimUpId{-1};
        std::atomic_uint32_t g_unresolved{};
        std::atomic_uint32_t g_resolved{};
        std::mutex g_resolveMutex;

        bool ReadBoolean(const wchar_t* iniPath, const wchar_t* key, bool fallback) noexcept
        {
            wchar_t value[16]{};
            GetPrivateProfileStringW(L"input", key, fallback ? L"true" : L"false", value,
                static_cast<DWORD>(std::size(value)), iniPath);
            return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
                _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
        }

        float ReadFloat(const wchar_t* iniPath, const wchar_t* key, float fallback) noexcept
        {
            wchar_t value[32]{};
            GetPrivateProfileStringW(L"input", key, L"", value, static_cast<DWORD>(std::size(value)), iniPath);
            if (!value[0])
                return fallback;
            wchar_t* end{};
            const double parsed = wcstod(value, &end);
            return end && *end == L'\0' ? static_cast<float>(parsed) : fallback;
        }

        float AtomicAdd(std::atomic<float>& target, float delta) noexcept
        {
            float current = target.load(std::memory_order_relaxed);
            while (!target.compare_exchange_weak(current, current + delta, std::memory_order_relaxed))
            {
            }
            return current + delta;
        }

        // The engine's checkFocus flag exists so keyboard state is ignored while another
        // window owns the keyboard. VR controller input is deliberate regardless of which
        // desktop window is in front, so overrides never consult the focus check; the GUI
        // case is handled by the host (actions are cleared while a menu is open).
        bool FocusAllows(void*, bool) noexcept
        {
            return true;
        }

        // Resolves every pending action name to its registry record and id. Runs on the
        // game thread at the frame boundary, so the registry exists and is quiescent.
        void ResolvePending() noexcept
        {
            std::lock_guard<std::mutex> lock(g_resolveMutex);
            void* registry = g_registry ? g_registry() : nullptr;
            if (!registry)
                return;
            auto& table = input_actions::Global();
            table.ForEachUnresolved([&](std::string_view name) {
                char buffer[input_actions::Table::kNameLength]{};
                std::memcpy(buffer, name.data(), (std::min)(name.size(), sizeof(buffer) - 1));
                void* record = g_lookupByName(registry, buffer);
                if (!record || *reinterpret_cast<const int*>(static_cast<char*>(record) + kRecordIndexOffset) == kMissingRecordIndex)
                {
                    g_unresolved.fetch_add(1, std::memory_order_relaxed);
                    logging::Error(std::string("Direct input: unknown action \"") + buffer + "\" (ignored)");
                    // Attach a dead handle so the name is not looked up every frame.
                    table.SetHandle(name, 1, -1);
                    return;
                }
                const int id = *reinterpret_cast<const int*>(static_cast<char*>(record) + kRecordIdOffset);
                const auto count = *reinterpret_cast<const unsigned*>(static_cast<char*>(registry) + kRegistryRecordCountOffset);
                void** records = *reinterpret_cast<void***>(static_cast<char*>(registry) + kRegistryRecordsOffset);
                const bool idMatches = id >= 0 && static_cast<unsigned>(id) < count && records && records[id] == record;
                table.SetHandle(name, reinterpret_cast<std::uintptr_t>(record), idMatches ? id : -1);
                g_resolved.fetch_add(1, std::memory_order_relaxed);
                std::ostringstream message;
                message << "Direct input: " << buffer << " -> record " << record << " id " << id
                        << (idMatches ? "" : " (id not in registry array, record path only)");
                logging::Info(message.str());
            });
        }

        int ResolveAimId(const char* name) noexcept
        {
            void* registry = g_registry ? g_registry() : nullptr;
            if (!registry)
                return -1;
            void* record = g_lookupByName(registry, name);
            if (!record || *reinterpret_cast<const int*>(static_cast<char*>(record) + kRecordIndexOffset) == kMissingRecordIndex)
                return -1;
            return *reinterpret_cast<const int*>(static_cast<char*>(record) + kRecordIdOffset);
        }

        // Marks the game frame: resolves names, latches the table and converts the
        // accumulated aim delta into this frame's rate (radians per second).
        void __fastcall HookedPlayerInputUpdate(void* self, void* player, float dt)
        {
            if (g_active.load(std::memory_order_relaxed))
            {
                g_frames.fetch_add(1, std::memory_order_relaxed);
                if (dt > 0.0f && std::isfinite(dt))
                    g_lastDt.store(dt, std::memory_order_relaxed);
                ResolvePending();
                input_actions::Global().Latch();
                const float yaw = g_pendingYaw.exchange(0.0f, std::memory_order_relaxed);
                const float pitch = g_pendingPitch.exchange(0.0f, std::memory_order_relaxed);
                const float seconds = g_lastDt.load(std::memory_order_relaxed);
                // The aim delta is consumed whenever the hooks are active: stick turns
                // always arrive this way, head aim only with [input] direct_aim.
                if (seconds > 0.0f)
                {
                    g_frameYawRate = yaw * g_aimYawSign / seconds;
                    g_framePitchRate = pitch * g_aimPitchSign / seconds;
                }
                else
                {
                    g_frameYawRate = 0.0f;
                    g_framePitchRate = 0.0f;
                }
                if (g_aimRightId < 0)
                {
                    g_aimRightId = ResolveAimId("UAAimRight");
                    g_aimLeftId = ResolveAimId("UAAimLeft");
                    g_aimDownId = ResolveAimId("UAAimDown");
                    g_aimUpId = ResolveAimId("UAAimUp");
                    std::ostringstream message;
                    message << "Direct aim: ids right/left/down/up " << g_aimRightId << '/' << g_aimLeftId
                            << '/' << g_aimDownId << '/' << g_aimUpId;
                    logging::Info(message.str());
                }
            }
            g_playerInputUpdate(self, player, dt);
        }

        bool Lookup(unsigned id, input_actions::Latched& latched) noexcept
        {
            return g_active.load(std::memory_order_relaxed) &&
                input_actions::Global().GetById(static_cast<int>(id), latched);
        }

        bool Lookup(void* record, input_actions::Latched& latched) noexcept
        {
            return g_active.load(std::memory_order_relaxed) &&
                input_actions::Global().GetByHandle(reinterpret_cast<std::uintptr_t>(record), latched);
        }

        template<typename Key>
        float CombinedValue(float engine, void* self, Key key, bool checkFocus) noexcept
        {
            input_actions::Latched latched;
            if (!Lookup(key, latched) || !FocusAllows(self, checkFocus))
                return engine;
            g_overrides.fetch_add(1, std::memory_order_relaxed);
            return (std::max)(engine, latched.value);
        }

        template<typename Key>
        std::uint64_t CombinedFlag(std::uint64_t engine, void* self, Key key, bool checkFocus,
            bool input_actions::Latched::*flag) noexcept
        {
            input_actions::Latched latched;
            if (!Lookup(key, latched) || !FocusAllows(self, checkFocus))
                return engine;
            if (!(latched.*flag))
                return engine;
            g_overrides.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }

        float __fastcall HookedValueById(void* self, unsigned id, bool checkFocus)
        {
            return CombinedValue(g_valueById(self, id, checkFocus), self, id, checkFocus);
        }
        float __fastcall HookedValueByRecord(void* self, void* record, bool checkFocus)
        {
            return CombinedValue(g_valueByRecord(self, record, checkFocus), self, record, checkFocus);
        }
        std::uint64_t __fastcall HookedPressById(void* self, unsigned id, bool checkFocus)
        {
            return CombinedFlag(g_pressById(self, id, checkFocus), self, id, checkFocus, &input_actions::Latched::press);
        }
        std::uint64_t __fastcall HookedPressByRecord(void* self, void* record, bool checkFocus)
        {
            return CombinedFlag(g_pressByRecord(self, record, checkFocus), self, record, checkFocus, &input_actions::Latched::press);
        }
        std::uint64_t __fastcall HookedReleaseById(void* self, unsigned id, bool checkFocus)
        {
            return CombinedFlag(g_releaseById(self, id, checkFocus), self, id, checkFocus, &input_actions::Latched::release);
        }
        std::uint64_t __fastcall HookedReleaseByRecord(void* self, void* record, bool checkFocus)
        {
            return CombinedFlag(g_releaseByRecord(self, record, checkFocus), self, record, checkFocus, &input_actions::Latched::release);
        }
        std::uint64_t __fastcall HookedHoldById(void* self, unsigned id, bool checkFocus)
        {
            return CombinedFlag(g_holdById(self, id, checkFocus), self, id, checkFocus, &input_actions::Latched::held);
        }
        std::uint64_t __fastcall HookedHoldByRecord(void* self, void* record, bool checkFocus)
        {
            return CombinedFlag(g_holdByRecord(self, record, checkFocus), self, record, checkFocus, &input_actions::Latched::held);
        }
        std::uint64_t __fastcall HookedHoldBeginById(void* self, unsigned id, bool checkFocus)
        {
            return CombinedFlag(g_holdBeginById(self, id, checkFocus), self, id, checkFocus, &input_actions::Latched::holdBegin);
        }
        std::uint64_t __fastcall HookedHoldBeginByRecord(void* self, void* record, bool checkFocus)
        {
            return CombinedFlag(g_holdBeginByRecord(self, record, checkFocus), self, record, checkFocus, &input_actions::Latched::holdBegin);
        }

        // Aim: the player input controller asks for (UAAimRight, UAAimLeft) and
        // (UAAimDown, UAAimUp) and multiplies the result by dt, so the result is an
        // angular rate. The VR rate is added to whatever the mouse produced.
        float __fastcall HookedAxisPair(void* self, int mode, unsigned idA, unsigned idB, bool checkFocus)
        {
            const float engine = g_axisPair(self, mode, idA, idB, checkFocus);
            if (!g_active.load(std::memory_order_relaxed) || !FocusAllows(self, checkFocus))
                return engine;
            const int a = static_cast<int>(idA);
            const int b = static_cast<int>(idB);
            // The consumer passes the pairs in either order; the sign follows the order.
            float extra = 0.0f;
            if (g_aimRightId >= 0 && a == g_aimRightId && b == g_aimLeftId)
                extra = g_frameYawRate;
            else if (g_aimRightId >= 0 && a == g_aimLeftId && b == g_aimRightId)
                extra = -g_frameYawRate;
            else if (g_aimDownId >= 0 && a == g_aimDownId && b == g_aimUpId)
                extra = g_framePitchRate;
            else if (g_aimDownId >= 0 && a == g_aimUpId && b == g_aimDownId)
                extra = -g_framePitchRate;
            else
                return engine;
            if (extra != 0.0f)
                g_overrides.fetch_add(1, std::memory_order_relaxed);
            if (!g_mouseLook.load(std::memory_order_relaxed))
                return extra;
            return engine + extra;
        }

        template<typename Fn>
        bool Hook(std::uintptr_t base, std::uintptr_t rva, void* detour, Fn& original, const char* what) noexcept
        {
            void* target = reinterpret_cast<void*>(base + rva);
            const MH_STATUS created = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
            if (created != MH_OK || MH_EnableHook(target) != MH_OK)
            {
                logging::Error(std::string("Direct input: hook failed for ") + what);
                return false;
            }
            return true;
        }
    }

    void Initialize(const wchar_t* iniPath, std::uintptr_t moduleBase, std::size_t imageSize) noexcept
    {
        static std::atomic_bool attempted{};
        if (attempted.exchange(true))
            return;
        if (!ReadBoolean(iniPath, L"direct_actions", true))
        {
            logging::Info("Direct input disabled ([input] direct_actions=false); using SendInput");
            return;
        }
        if (!moduleBase || !dayz::builds::ValidateInputBuild({reinterpret_cast<const std::uint8_t*>(moduleBase), imageSize}))
        {
            logging::Error("Direct input: executable does not match the 1.29.163709 input hooks; using SendInput");
            return;
        }
        g_directAim.store(ReadBoolean(iniPath, L"direct_aim", false));
        g_mouseLook.store(ReadBoolean(iniPath, L"mouse_look", true));
        g_aimYawSign = ReadFloat(iniPath, L"aim_yaw_sign", 1.0f) < 0.0f ? -1.0f : 1.0f;
        g_aimPitchSign = ReadFloat(iniPath, L"aim_pitch_sign", 1.0f) < 0.0f ? -1.0f : 1.0f;
        g_moduleBase = moduleBase;
        g_registry = reinterpret_cast<RegistryGetterFn>(moduleBase + kRegistryGetterRva);
        g_lookupByName = reinterpret_cast<LookupByNameFn>(moduleBase + kLookupByNameRva);
        const bool ok =
            Hook(moduleBase, kValueByIdRva, reinterpret_cast<void*>(HookedValueById), g_valueById, "value by id") &&
            Hook(moduleBase, kValueByRecordRva, reinterpret_cast<void*>(HookedValueByRecord), g_valueByRecord, "value by record") &&
            Hook(moduleBase, kPressByIdRva, reinterpret_cast<void*>(HookedPressById), g_pressById, "press by id") &&
            Hook(moduleBase, kPressByRecordRva, reinterpret_cast<void*>(HookedPressByRecord), g_pressByRecord, "press by record") &&
            Hook(moduleBase, kReleaseByIdRva, reinterpret_cast<void*>(HookedReleaseById), g_releaseById, "release by id") &&
            Hook(moduleBase, kReleaseByRecordRva, reinterpret_cast<void*>(HookedReleaseByRecord), g_releaseByRecord, "release by record") &&
            Hook(moduleBase, kHoldByIdRva, reinterpret_cast<void*>(HookedHoldById), g_holdById, "hold by id") &&
            Hook(moduleBase, kHoldByRecordRva, reinterpret_cast<void*>(HookedHoldByRecord), g_holdByRecord, "hold by record") &&
            Hook(moduleBase, kHoldBeginByIdRva, reinterpret_cast<void*>(HookedHoldBeginById), g_holdBeginById, "hold begin by id") &&
            Hook(moduleBase, kHoldBeginByRecordRva, reinterpret_cast<void*>(HookedHoldBeginByRecord), g_holdBeginByRecord, "hold begin by record") &&
            Hook(moduleBase, kAxisPairRva, reinterpret_cast<void*>(HookedAxisPair), g_axisPair, "axis pair") &&
            Hook(moduleBase, kPlayerInputUpdateRva, reinterpret_cast<void*>(HookedPlayerInputUpdate), g_playerInputUpdate, "player input update");
        if (!ok)
        {
            logging::Error("Direct input: not all hooks installed; staying off (SendInput fallback)");
            return;
        }
        g_active.store(true);
        std::ostringstream message;
        message << "Direct input active: engine action getters hooked (direct_aim="
                << (g_directAim.load() ? "on" : "off") << ", mouse_look="
                << (g_mouseLook.load() ? "on" : "off") << ", aim signs " << g_aimYawSign << '/' << g_aimPitchSign << ')';
        logging::Info(message.str());
    }

    bool Active() noexcept
    {
        return g_active.load(std::memory_order_relaxed);
    }

    bool MenuOwnsInput() noexcept
    {
        if (!g_active.load(std::memory_order_relaxed) || !g_moduleBase)
            return false;
        const auto* input = *reinterpret_cast<const char* const*>(g_moduleBase + kInputObjectPointerRva);
        if (!input)
            return false;
        return *reinterpret_cast<const int*>(input + kGameFocusCounterOffset) > 0;
    }

    bool DirectAimEnabled() noexcept
    {
        return g_active.load(std::memory_order_relaxed) && g_directAim.load(std::memory_order_relaxed);
    }

    void SetAction(std::string_view name, float value, bool held) noexcept
    {
        if (!input_actions::Global().Set(name, value, held))
            logging::Error(std::string("Direct input: cannot set action ") + std::string(name));
    }

    void ClearAction(std::string_view name) noexcept
    {
        input_actions::Global().Clear(name);
    }

    void ClearAllActions() noexcept
    {
        input_actions::Global().ClearAll();
    }

    void AddAimDelta(float yawRadians, float pitchRadians) noexcept
    {
        if (std::isfinite(yawRadians))
            AtomicAdd(g_pendingYaw, yawRadians);
        if (std::isfinite(pitchRadians))
            AtomicAdd(g_pendingPitch, pitchRadians);
    }

    Stats GetStats() noexcept
    {
        Stats stats;
        stats.frames = g_frames.load(std::memory_order_relaxed);
        stats.overrides = g_overrides.load(std::memory_order_relaxed);
        stats.resolved = g_resolved.load(std::memory_order_relaxed);
        stats.unresolved = g_unresolved.load(std::memory_order_relaxed);
        stats.lastFrameSeconds = g_lastDt.load(std::memory_order_relaxed);
        return stats;
    }
}
