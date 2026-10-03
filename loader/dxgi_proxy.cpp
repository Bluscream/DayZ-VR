// dxgi.dll proxy: forwards every export to the system DXGI and, when the plugin loader
// started at least one plugin, attaches the swap-chain detours to the created factory.
#include "dxgi_proxy.hpp"
#include "swapchain_hooks.hpp"
#include "logging.hpp"
#include "plugin_loader.hpp"
#include <dxgi1_6.h>
#include <mutex>
#include <string>

namespace
{
    HMODULE g_systemDxgi{};
    std::once_flag g_once;

    void LoadSystemDxgi() noexcept
    {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        std::wstring path(systemDirectory, length);
        path += L"\\dxgi.dll";
        g_systemDxgi = LoadLibraryW(path.c_str());

        HMODULE proxyModule{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadSystemDxgi), &proxyModule);
        if (g_systemDxgi == proxyModule)
            g_systemDxgi = nullptr;
    }

    // Plugins start on the first factory creation, before the game creates its swap chain,
    // so a plugin can still request a backbuffer size.
    bool HooksEnabled() noexcept
    {
        static std::once_flag logged;
        const bool enabled = loader::Initialize();
        std::call_once(logged, [enabled] {
            logging::Info(enabled ? "DXGI hooks enabled (plugins started)" : "DXGI hooks disabled (no plugin started)");
        });
        return enabled;
    }
}

namespace proxy
{
    bool EnsureSystemDxgiLoaded() noexcept { std::call_once(g_once, LoadSystemDxgi); return g_systemDxgi != nullptr; }
    FARPROC Resolve(const char* name) noexcept { return EnsureSystemDxgiLoaded() ? GetProcAddress(g_systemDxgi, name) : nullptr; }
}

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("CreateDXGIFactory"));
    if (!original) return E_NOINTERFACE;
    const HRESULT result = original(riid, factory);
    if (SUCCEEDED(result) && factory && *factory && HooksEnabled())
        hooks::AttachToFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("CreateDXGIFactory1"));
    if (!original) return E_NOINTERFACE;
    const HRESULT result = original(riid, factory);
    if (SUCCEEDED(result) && factory && *factory && HooksEnabled())
        hooks::AttachToFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("CreateDXGIFactory2"));
    if (!original) return E_NOINTERFACE;
    const HRESULT result = original(flags, riid, factory);
    if (SUCCEEDED(result) && factory && *factory && HooksEnabled())
        hooks::AttachToFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    using Fn = HRESULT(WINAPI*)();
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("DXGIDeclareAdapterRemovalSupport"));
    return original ? original() : E_NOINTERFACE;
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** value)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto original = reinterpret_cast<Fn>(proxy::Resolve("DXGIGetDebugInterface1"));
    return original ? original(flags, riid, value) : E_NOINTERFACE;
}
