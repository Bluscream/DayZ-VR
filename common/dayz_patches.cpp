#include "dayz_patches.hpp"

#include "logging.hpp"

#include <atomic>
#include <cstddef>
#include <iterator>
#include <sstream>

#include <windows.h>

namespace dayz::patches
{
    namespace
    {
        // RenderContext field that holds the view object prepared for the current
        // execute. DayZ+0x1C4440 stores it, the finalize path (DayZ+0x1C57A2) resets
        // it to null, and DayZ+0x1B8F06 dereferences it unconditionally.
        constexpr std::ptrdiff_t kPreparedViewOffset = 0xA10;
        // Every skip is logged until this many have been seen, then every 100th, so a
        // persistent condition cannot flood the log at frame rate.
        constexpr std::uint64_t kVerboseSkipLimit = 10;
        constexpr std::uint64_t kSkipLogInterval = 100;

        std::atomic<bool> g_initialized{false};
        bool g_guardPreparedView{true};
        std::atomic<std::uint64_t> g_preparedViewSkips{0};

        bool ReadBoolean(const wchar_t* iniPath, const wchar_t* key, bool fallback) noexcept
        {
            wchar_t value[16]{};
            GetPrivateProfileStringW(L"patches", key, fallback ? L"true" : L"false", value,
                static_cast<DWORD>(std::size(value)), iniPath);
            return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
                _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
        }

        // The context comes from the engine's own call, so it is readable; SEH only
        // covers a context torn down on another thread.
        const void* ReadPreparedView(const void* context) noexcept
        {
            __try
            {
                return *reinterpret_cast<const void* const*>(
                    reinterpret_cast<std::uintptr_t>(context) + kPreparedViewOffset);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }
    }

    void Initialize(const wchar_t* iniPath) noexcept
    {
        if (g_initialized.exchange(true))
            return;
        g_guardPreparedView = ReadBoolean(iniPath, L"guard_execute_without_prepared_view", true);
        std::ostringstream message;
        message << "Engine patches: guard_execute_without_prepared_view="
                << (g_guardPreparedView ? "on" : "off");
        logging::Info(message.str());
    }

    bool SkipExecuteWithoutPreparedView(const void* context, std::uint8_t mode,
        std::uintptr_t callerRva) noexcept
    {
        if (!g_guardPreparedView || !context || ReadPreparedView(context))
            return false;
        const std::uint64_t skips = g_preparedViewSkips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skips <= kVerboseSkipLimit || skips % kSkipLogInterval == 0)
        {
            std::ostringstream message;
            message << "Skipped executeView without a prepared view (would crash at DayZ+0x1DDE4B):"
                    << " ctx=" << context << " mode=" << static_cast<unsigned>(mode)
                    << " tid=" << GetCurrentThreadId() << " caller=DayZ+0x" << std::hex << callerRva
                    << std::dec << " total=" << skips;
            logging::Error(message.str());
        }
        return true;
    }

    void DumpCounters() noexcept
    {
        std::ostringstream message;
        message << "Engine patch counters: execute_without_prepared_view_skips="
                << g_preparedViewSkips.load(std::memory_order_relaxed)
                << " guard=" << (g_guardPreparedView ? "on" : "off");
        logging::Error(message.str());
    }
}
