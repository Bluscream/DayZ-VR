#pragma once

#include <cstdint>

// Workarounds for DayZ engine bugs that the VR proxy exposes. None of this is part
// of the stereo/OpenXR core: the hooks in dayz_runtime_probe.cpp only forward the
// call they already intercept, and every workaround can be switched off from the
// [patches] section of dayz_openxr.ini.
namespace dayz::patches
{
    // Reads [patches] from the given dayz_openxr.ini path. Call once before the
    // render hooks are enabled; later calls are ignored.
    void Initialize(const wchar_t* iniPath) noexcept;

    // DayZ 1.29.163709 reads the render context's prepared-view pointer
    // (RenderContext+0xA10, set by the view setup and cleared by finalize) without a
    // null check inside executeView. After the game window was dragged the engine was
    // seen executing a view whose pointer was already cleared and crashed at
    // DayZ+0x1DDE4B. Returns true when the executeView call has to be skipped
    // because that pointer is null; the engine ignores executeView's return value at
    // both of its call sites, so skipping only drops that one view for one frame.
    bool SkipExecuteWithoutPreparedView(const void* context, std::uint8_t mode,
        std::uintptr_t callerRva) noexcept;

    // Logs the workaround counters; intended for the crash-context dump.
    void DumpCounters() noexcept;
}
