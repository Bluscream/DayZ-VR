#pragma once

#include <cstdint>

namespace dayz::crash_report
{
    // Callback that logs mod-side context (recent render events, HMD state) when a
    // fatal exception is observed. Must only call logging functions.
    using ContextDumpFn = void (*)() noexcept;

    // Installs a vectored exception handler that writes fatal exceptions (access
    // violation, illegal instruction, stack overflow, ...) to the mod log as
    // DayZ+RVA / module+offset frames before the game's own crash reporter runs.
    // Exceptions raised from inside this module (the probe's guarded reads) are
    // ignored. Harmless if called more than once.
    void Install(std::uintptr_t gameModuleBase, std::uint32_t gameImageSize,
        ContextDumpFn contextDump) noexcept;
}
