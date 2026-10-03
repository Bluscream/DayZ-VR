#pragma once

#include <Unknwn.h>
#include <dxgi.h>

namespace loader
{
    // Reads dayz_pluginloader.ini, loads plugins/*.dll and starts them. Idempotent; returns
    // true when at least one plugin started, which is what gates the DXGI detours.
    bool Initialize() noexcept;
    bool AnyPluginStarted() noexcept;
    // Backbuffer size requested by a plugin (0 = none).
    bool BackbufferOverride(unsigned& width, unsigned& height) noexcept;
    // Event fan-out to every started plugin (present thread).
    void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept;
    void OnPresent(IDXGISwapChain* swapChain, unsigned flags) noexcept;
    void OnResizeBuffers(IDXGISwapChain* swapChain, unsigned width, unsigned height) noexcept;
}
