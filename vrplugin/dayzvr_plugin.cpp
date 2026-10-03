// DayZ VR as a plugin of the generic loader (include/dayz_plugin_api.h). This file is the
// only glue: it forwards the loader's swap-chain and present events to the OpenXR
// host and the runtime probe, asks the loader for the backbuffer override that
// dayz_openxr.ini [stereo] override_game_resolution requests, and registers the two
// hotkeys (toggle VR mode, recenter). Everything else lives in vr_common.
#include "dayz_plugin_api.h"

#include "dayz_runtime_probe.hpp"
#include "debug_bridge.hpp"
#include "logging.hpp"
#include "openxr_host.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>

namespace
{
    constexpr char kName[] = "dayzvr";
    constexpr char kVersion[] = "1.29-dev";

    const DayzPluginHost* g_host{};
    std::atomic_bool g_initializationAttempted{};
    int g_toggleVrHotkey{-1};
    int g_recenterHotkey{-1};
    bool g_hudOverrideBeforeFlat{};

    bool ReadBoolean(const wchar_t* section, const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", value,
            static_cast<DWORD>(std::size(value)), g_host->config_path);
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    void RequestResolutionOverride() noexcept
    {
        if (!ReadBoolean(L"stereo", L"override_game_resolution", false))
        {
            logging::Info("Resolution config enabled=0");
            return;
        }
        const UINT width = static_cast<UINT>(GetPrivateProfileIntW(L"stereo", L"render_width", 1600, g_host->config_path));
        const UINT height = static_cast<UINT>(GetPrivateProfileIntW(L"stereo", L"render_height", 1600, g_host->config_path));
        std::ostringstream message;
        message << "Resolution config enabled=1 size=" << width << 'x' << height;
        logging::Info(message.str());
        if (width < 640 || height < 640 || width > 8192 || height > 8192)
        {
            logging::Error("override_game_resolution ignored: size out of range");
            return;
        }
        g_host->request_backbuffer_size(g_host->context, width, height);
        std::ostringstream applied;
        applied << "DayZ game resolution override active: " << width << 'x' << height;
        logging::Info(applied.str());
    }

    void OnSwapChainCreated(void*, IDXGISwapChain* swapChain)
    {
        if (!swapChain)
            return;
        Microsoft::WRL::ComPtr<ID3D11Device> gameDevice;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&gameDevice))))
        {
            dayz::runtime_probe::AttachD3DDevice(gameDevice.Get());
            dayz::runtime_probe::Initialize();
        }
    }

    void OnPresent(void*, IDXGISwapChain* swapChain, uint32_t)
    {
        auto& host = OpenXrHost::Instance();
        host.AttachGameSwapChain(swapChain);
        if (!g_initializationAttempted.exchange(true))
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
            {
                dayz::runtime_probe::AttachD3DDevice(device.Get());
                host.InitializeWithDevice(device.Get());
            }
        }
        // The debug plugin is useful with OpenXR disabled too (hooks-only runs), so
        // it starts as soon as the probe has had its chance to install.
        dayz::debug_bridge::Start();
        if (host.IsInitialized())
        {
            dayz::runtime_probe::Initialize();
            dayz::runtime_probe::BeforePresent(swapChain);
            host.Tick();
            dayz::runtime_probe::OnPresent();
        }
    }

    void OnHotkey(void*, int id, const char* action)
    {
        if (id == g_toggleVrHotkey)
        {
            double enabled{1.0};
            dayz::runtime_probe::GetTunable("stereo.vr_enabled", enabled);
            const bool next = enabled == 0.0;
            // The HUD safe-area scale only makes sense for the headset; off, the desktop
            // window shows the stock HUD layout again. The square backbuffer cannot
            // change live, so the window stays at the override size until restart.
            if (!next)
            {
                double hudOverride{};
                dayz::runtime_probe::GetTunable("stereo.override_hud_scale", hudOverride);
                g_hudOverrideBeforeFlat = hudOverride != 0.0;
                dayz::runtime_probe::SetTunable("stereo.override_hud_scale", 0.0);
            }
            else if (g_hudOverrideBeforeFlat)
                dayz::runtime_probe::SetTunable("stereo.override_hud_scale", 1.0);
            dayz::runtime_probe::SetTunable("stereo.vr_enabled", next ? 1.0 : 0.0);
            logging::Info(next ? "VR mode on (hotkey)" : "VR mode off (hotkey): flat image, HMD ignored, HUD override paused");
            return;
        }
        if (id == g_recenterHotkey)
        {
            dayz::runtime_probe::RecenterHmd();
            logging::Info("HMD recentered (hotkey)");
            return;
        }
        logging::Error(std::string("unknown hotkey ") + (action ? action : ""));
    }

    int RegisterHotkey(const char* action, const char* title, const char* defaultKey) noexcept
    {
        DayzHotkeyDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.action = action;
        desc.title = title;
        desc.default_key = defaultKey;
        const int id = g_host->hotkey_register(g_host->context, &desc);
        if (id < 0)
            logging::Error(std::string("hotkey registration failed: ") + action);
        return id;
    }
}

extern "C" __declspec(dllexport) int DayzPluginDescribe(DayzPluginInfo* info)
{
    if (!info || info->struct_size < sizeof(DayzPluginInfo))
        return -1;
    info->api_version = DAYZ_PLUGIN_API_VERSION;
    strncpy_s(info->name, kName, _TRUNCATE);
    strncpy_s(info->version, kVersion, _TRUNCATE);
    strncpy_s(info->description, "OpenXR stereo rendering for DayZ (dxgi proxy, runtime probe)", _TRUNCATE);
    wcsncpy_s(info->config_file, L"dayz_openxr.ini", _TRUNCATE);
    return 0;
}

extern "C" __declspec(dllexport) int DayzPluginStart(const DayzPluginHost* host, DayzPluginCallbacks* callbacks)
{
    if (!host || !callbacks || host->struct_size < sizeof(DayzPluginHost) ||
        host->api_version != DAYZ_PLUGIN_API_VERSION || callbacks->struct_size < sizeof(DayzPluginCallbacks))
        return -1;
    g_host = host;
    logging::Initialize();
    if (!ReadBoolean(L"hooks", L"enabled", false))
    {
        logging::Info("DXGI hooks disabled ([hooks] enabled=false); the VR plugin stays inactive");
        host->log(host->context, 0, "[hooks] enabled=false in dayz_openxr.ini; not starting");
        return 1;
    }
    logging::Info("DXGI hooks enabled");
    RequestResolutionOverride();
    g_toggleVrHotkey = RegisterHotkey("toggle_vr", "Toggle VR mode (flat image when off)", "F12");
    g_recenterHotkey = RegisterHotkey("recenter", "Recenter the HMD", "F11");
    callbacks->context = nullptr;
    callbacks->on_swapchain_created = OnSwapChainCreated;
    callbacks->on_present = OnPresent;
    callbacks->on_resize_buffers = nullptr;
    callbacks->on_hotkey = OnHotkey;
    callbacks->on_setting_changed = nullptr;
    host->log(host->context, 0, "started");
    return 0;
}

extern "C" __declspec(dllexport) void DayzPluginStop(void)
{
    OpenXrHost::Instance().Shutdown();
}
