// DXGI factory, swap-chain, Present and ResizeBuffers detours. Generic: knows nothing
// about VR; every event is handed to the plugins through loader::On*.
#include "swapchain_hooks.hpp"
#include "present_frame.hpp"

#include "logging.hpp"
#include "plugin_loader.hpp"
#include <MinHook.h>

#include <atomic>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <wrl/client.h>

namespace
{
    static_assert(hooks::kPresentTestFlag == DXGI_PRESENT_TEST);
    using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
        DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
        const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
        IDXGISwapChain1**);
    using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
        IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
        const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
        DXGI_FORMAT, UINT);

    std::mutex g_tablesMutex;
    std::unordered_map<void*, void*> g_trampolines;
    bool g_minHookInitialized{};

    void ApplyResolution(UINT& width, UINT& height) noexcept
    {
        unsigned overrideWidth{}, overrideHeight{};
        if (loader::BackbufferOverride(overrideWidth, overrideHeight))
        {
            width = overrideWidth;
            height = overrideHeight;
        }
    }

    HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(IDXGIFactory*, IUnknown*,
        DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(IDXGIFactory2*, IUnknown*, HWND,
        const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
        IDXGISwapChain1**);
    HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(IDXGIFactory2*, IUnknown*,
        IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(IDXGIFactory2*, IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain*, UINT, UINT);
    HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1*, UINT, UINT,
        const DXGI_PRESENT_PARAMETERS*);
    HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT,
        DXGI_FORMAT, UINT);

    void InstallHooks(void* interfacePointer,
        std::initializer_list<std::pair<std::size_t, void*>> replacements) noexcept
    {
        if (!interfacePointer)
            return;

        void** table = *reinterpret_cast<void***>(interfacePointer);
        std::scoped_lock lock(g_tablesMutex);
        if (!g_minHookInitialized)
        {
            const MH_STATUS status = MH_Initialize();
            if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
            {
                logging::Error("MH_Initialize failed");
                return;
            }
            g_minHookInitialized = true;
        }

        for (const auto& [index, replacement] : replacements)
        {
            void* target = table[index];
            if (g_trampolines.contains(target))
                continue;

            void* trampoline{};
            const MH_STATUS createStatus = MH_CreateHook(target, replacement, &trampoline);
            if (createStatus != MH_OK)
            {
                logging::Error("MH_CreateHook failed");
                continue;
            }
            const MH_STATUS enableStatus = MH_EnableHook(target);
            if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
            {
                MH_RemoveHook(target);
                logging::Error("MH_EnableHook failed");
                continue;
            }
            g_trampolines.emplace(target, trampoline);
        }
    }

    template<typename Function, typename Interface>
    Function Original(Interface* self, std::size_t index) noexcept
    {
        if (!self)
            return nullptr;
        void** table = *reinterpret_cast<void***>(self);
        void* target = table[index];
        std::scoped_lock lock(g_tablesMutex);
        const auto found = g_trampolines.find(target);
        if (found == g_trampolines.end())
            return nullptr;
        return reinterpret_cast<Function>(found->second);
    }

    HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(IDXGIFactory* self, IUnknown* device,
        DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain)
    {
        const auto original = Original<CreateSwapChainFn>(self, 10);
        if (!original)
            return E_FAIL;
        DXGI_SWAP_CHAIN_DESC adjusted{};
        DXGI_SWAP_CHAIN_DESC* effective = description;
        if (description)
        {
            adjusted = *description;
            ApplyResolution(adjusted.BufferDesc.Width, adjusted.BufferDesc.Height);
            effective = &adjusted;
        }
        const HRESULT result = original(self, device, effective, swapChain);
        if (SUCCEEDED(result) && swapChain)
            hooks::OnSwapChainCreated(*swapChain);
        return result;
    }

    HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device,
        HWND window, const DXGI_SWAP_CHAIN_DESC1* description,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
        IDXGISwapChain1** swapChain)
    {
        const auto original = Original<CreateSwapChainForHwndFn>(self, 15);
        if (!original)
            return E_FAIL;
        DXGI_SWAP_CHAIN_DESC1 adjusted{};
        const DXGI_SWAP_CHAIN_DESC1* effective = description;
        if (description)
        {
            adjusted = *description;
            ApplyResolution(adjusted.Width, adjusted.Height);
            effective = &adjusted;
        }
        const HRESULT result = original(self, device, window, effective, fullscreen, output,
            swapChain);
        if (SUCCEEDED(result) && swapChain)
            hooks::OnSwapChainCreated(*swapChain);
        return result;
    }

    HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(IDXGIFactory2* self,
        IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* description,
        IDXGIOutput* output, IDXGISwapChain1** swapChain)
    {
        const auto original = Original<CreateSwapChainForCoreWindowFn>(self, 16);
        if (!original)
            return E_FAIL;
        DXGI_SWAP_CHAIN_DESC1 adjusted{};
        const DXGI_SWAP_CHAIN_DESC1* effective = description;
        if (description)
        {
            adjusted = *description;
            ApplyResolution(adjusted.Width, adjusted.Height);
            effective = &adjusted;
        }
        const HRESULT result = original(self, device, window, effective, output, swapChain);
        if (SUCCEEDED(result) && swapChain)
            hooks::OnSwapChainCreated(*swapChain);
        return result;
    }

    HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(IDXGIFactory2* self,
        IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* description, IDXGIOutput* output,
        IDXGISwapChain1** swapChain)
    {
        const auto original = Original<CreateSwapChainForCompositionFn>(self, 17);
        if (!original)
            return E_FAIL;
        DXGI_SWAP_CHAIN_DESC1 adjusted{};
        const DXGI_SWAP_CHAIN_DESC1* effective = description;
        if (description)
        {
            adjusted = *description;
            ApplyResolution(adjusted.Width, adjusted.Height);
            effective = &adjusted;
        }
        const HRESULT result = original(self, device, effective, output, swapChain);
        if (SUCCEEDED(result) && swapChain)
            hooks::OnSwapChainCreated(*swapChain);
        return result;
    }

    HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags)
    {
        const auto original = Original<PresentFn>(self, 8);
        if (!original)
            return DXGI_ERROR_INVALID_CALL;
        return hooks::TickAndPresent(flags, [self, flags] {
            loader::OnPresent(self, flags);
            static std::atomic_bool firstPresent{true};
            if (firstPresent.exchange(false))
                logging::Info("DXGI Present detour active");
        }, [&] { return original(self, syncInterval, flags); });
    }

    HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags,
        const DXGI_PRESENT_PARAMETERS* parameters)
    {
        const auto original = Original<Present1Fn>(self, 22);
        if (!original)
            return DXGI_ERROR_INVALID_CALL;
        return hooks::TickAndPresent(flags, [self, flags] { loader::OnPresent(self, flags); },
            [&] { return original(self, syncInterval, flags, parameters); });
    }

    HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* self, UINT count, UINT width,
        UINT height, DXGI_FORMAT format, UINT flags)
    {
        const auto original = Original<ResizeBuffersFn>(self, 13);
        if (!original)
            return DXGI_ERROR_INVALID_CALL;
        const UINT requestedWidth = width;
        const UINT requestedHeight = height;
        ApplyResolution(width, height);
        const HRESULT result = original(self, count, width, height, format, flags);
        std::ostringstream message;
        message << "ResizeBuffers requested=" << requestedWidth << 'x' << requestedHeight
            << " applied=" << width << 'x' << height << " count=" << count
            << " format=" << static_cast<unsigned>(format) << " flags=0x" << std::hex << flags
            << " hr=0x" << static_cast<unsigned long>(result);
        if (SUCCEEDED(result))
        {
            logging::Info(message.str());
            loader::OnResizeBuffers(self, width, height);
        }
        else
            logging::Error(message.str());
        return result;
    }
}

namespace hooks
{
    void AttachToFactory(IUnknown* unknown) noexcept
    {
        if (!unknown)
            return;

        Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        unknown->QueryInterface(IID_PPV_ARGS(&factory2));
        unknown->QueryInterface(IID_PPV_ARGS(&factory));

        if (factory2)
        {
            InstallHooks(factory2.Get(), {
                {10, reinterpret_cast<void*>(HookedCreateSwapChain)},
                {15, reinterpret_cast<void*>(HookedCreateSwapChainForHwnd)},
                {16, reinterpret_cast<void*>(HookedCreateSwapChainForCoreWindow)},
                {17, reinterpret_cast<void*>(HookedCreateSwapChainForComposition)}});
        }
        if (factory && factory.Get() != factory2.Get())
        {
            InstallHooks(factory.Get(), {
                {10, reinterpret_cast<void*>(HookedCreateSwapChain)}});
        }
    }

    void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept
    {
        if (!swapChain)
            return;

        logging::Info("DXGI swap chain captured; installing MinHook detours");
        static std::atomic_bool overrideLogged{};
        unsigned overrideWidth{}, overrideHeight{};
        if (loader::BackbufferOverride(overrideWidth, overrideHeight) && !overrideLogged.exchange(true))
        {
            std::ostringstream message;
            message << "backbuffer override active: " << overrideWidth << 'x' << overrideHeight;
            logging::Info(message.str());
        }

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1));
        if (swapChain1)
        {
            InstallHooks(swapChain1.Get(), {
                {8, reinterpret_cast<void*>(HookedPresent)},
                {13, reinterpret_cast<void*>(HookedResizeBuffers)},
                {22, reinterpret_cast<void*>(HookedPresent1)}});
        }
        if (!swapChain1 || swapChain != swapChain1.Get())
        {
            InstallHooks(swapChain, {
                {8, reinterpret_cast<void*>(HookedPresent)},
                {13, reinterpret_cast<void*>(HookedResizeBuffers)}});
        }
        loader::OnSwapChainCreated(swapChain);
    }
}
