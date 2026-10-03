#include "plugin_loader.hpp"

#include "dayz_plugin_api.h"
#include "hotkeys.hpp"
#include "logging.hpp"
#include "settings.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    struct Plugin
    {
        std::size_t index{};
        HMODULE module{};
        std::wstring path;
        std::string name;
        std::wstring configPath;
        DayzPluginInfo info{};
        DayzPluginHost host{};
        DayzPluginCallbacks callbacks{};
        DayzPluginStopFn stop{};
        bool started{};
        bool inStart{};
    };

    std::once_flag g_once;
    std::wstring g_gameDir;
    std::wstring g_pluginsDir;
    std::wstring g_loaderIni;
    std::vector<std::unique_ptr<Plugin>> g_plugins;
    std::atomic<bool> g_anyStarted{};
    std::atomic<unsigned> g_overrideWidth{};
    std::atomic<unsigned> g_overrideHeight{};

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t c : text)
            out += c < 0x80 ? static_cast<char>(c) : '?';
        return out;
    }

    std::wstring ExecutableDirectory()
    {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path))
            return L"";
        std::wstring directory(path, length);
        const auto separator = directory.find_last_of(L"\\/");
        return separator == std::wstring::npos ? L"" : directory.substr(0, separator + 1);
    }

    bool ReadBoolean(const wchar_t* section, const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", value,
            static_cast<DWORD>(std::size(value)), g_loaderIni.c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    bool ValidPluginName(const char* text) noexcept
    {
        if (!text || !*text)
            return false;
        for (const char* c = text; *c; ++c)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
                return false;
        return true;
    }

    Plugin* Owner(void* context) noexcept { return static_cast<Plugin*>(context); }

    // ---- host callbacks (context = Plugin*) ---------------------------------------------
    void HostLog(void* context, int level, const char* message)
    {
        const Plugin* plugin = Owner(context);
        if (!plugin || !message)
            return;
        const std::string line = "[" + plugin->name + "] " + message;
        if (level == 0)
            logging::Info(line);
        else
            logging::Error(line);
    }

    void HostRequestBackbufferSize(void* context, uint32_t width, uint32_t height)
    {
        const Plugin* plugin = Owner(context);
        if (!plugin)
            return;
        if (!plugin->inStart)
        {
            logging::Error("[" + plugin->name + "] request_backbuffer_size is only honoured during DayzPluginStart");
            return;
        }
        if ((width == 0) != (height == 0) || (width && (width < 640 || height < 640 || width > 8192 || height > 8192)))
        {
            logging::Error("[" + plugin->name + "] request_backbuffer_size rejected: " +
                std::to_string(width) + "x" + std::to_string(height));
            return;
        }
        g_overrideWidth.store(width, std::memory_order_relaxed);
        g_overrideHeight.store(height, std::memory_order_relaxed);
        logging::Info("[" + plugin->name + "] backbuffer override " + (width ? std::to_string(width) + "x" + std::to_string(height) : std::string("cleared")));
    }

    int HostSettingRegister(void* context, const DayzSettingDesc* desc)
    {
        const Plugin* plugin = Owner(context);
        return plugin ? loader::settings::Register(plugin->index, plugin->configPath, desc) : -1;
    }

    int HostSettingGet(void* context, const char* key, char* buffer, size_t capacity)
    {
        const Plugin* plugin = Owner(context);
        return plugin ? loader::settings::Get(plugin->index, key, buffer, capacity) : -1;
    }

    void NotifySettingChanged(std::size_t pluginIndex, const char* key, const char* value)
    {
        if (pluginIndex >= g_plugins.size())
            return;
        const Plugin& plugin = *g_plugins[pluginIndex];
        if (plugin.started && plugin.callbacks.on_setting_changed)
            plugin.callbacks.on_setting_changed(plugin.callbacks.context, key, value);
    }

    int HostSettingSet(void* context, const char* key, const char* value)
    {
        const Plugin* plugin = Owner(context);
        return plugin ? loader::settings::Set(plugin->index, key, value, NotifySettingChanged) : -1;
    }

    int HostHotkeyRegister(void* context, const DayzHotkeyDesc* desc)
    {
        const Plugin* plugin = Owner(context);
        if (!plugin || !desc || desc->struct_size < sizeof(DayzHotkeyDesc))
            return -1;
        return loader::hotkeys::Register(plugin->index, plugin->name, desc->action, desc->title,
            desc->default_key, g_loaderIni.c_str());
    }

    void DispatchHotkey(std::size_t pluginIndex, int id, const char* action)
    {
        if (pluginIndex >= g_plugins.size())
            return;
        const Plugin& plugin = *g_plugins[pluginIndex];
        if (plugin.started && plugin.callbacks.on_hotkey)
            plugin.callbacks.on_hotkey(plugin.callbacks.context, id, action);
    }

    // ---- discovery and lifecycle ----------------------------------------------------
    template<typename Fn>
    Fn Export(HMODULE module, const char* name) noexcept
    {
        // GetProcAddress returns FARPROC; void* is the portable way back to the real type.
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    }

    std::vector<std::wstring> FindPluginDlls()
    {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW data{};
        const std::wstring pattern = g_pluginsDir + L"*.dll";
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return files;
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                files.emplace_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        std::sort(files.begin(), files.end());
        return files;
    }

    void StartPlugin(const std::wstring& fileName)
    {
        auto plugin = std::make_unique<Plugin>();
        plugin->index = g_plugins.size();
        plugin->path = g_pluginsDir + fileName;
        const std::string shortName = Narrow(fileName);
        plugin->module = LoadLibraryExW(plugin->path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!plugin->module)
        {
            logging::Error("plugin " + shortName + " could not be loaded (error " +
                std::to_string(GetLastError()) + "); a missing dependency DLL is the usual cause");
            return;
        }
        const auto describe = Export<DayzPluginDescribeFn>(plugin->module, "DayzPluginDescribe");
        const auto start = Export<DayzPluginStartFn>(plugin->module, "DayzPluginStart");
        plugin->stop = Export<DayzPluginStopFn>(plugin->module, "DayzPluginStop");
        if (!describe || !start)
        {
            logging::Error("plugin " + shortName + " does not export DayzPluginDescribe/DayzPluginStart; skipped");
            FreeLibrary(plugin->module);
            return;
        }
        plugin->info.struct_size = sizeof(plugin->info);
        if (describe(&plugin->info) != 0 || plugin->info.api_version != DAYZ_PLUGIN_API_VERSION ||
            !ValidPluginName(plugin->info.name))
        {
            logging::Error("plugin " + shortName + " describe failed, API version mismatch (" +
                std::to_string(plugin->info.api_version) + " vs " + std::to_string(DAYZ_PLUGIN_API_VERSION) +
                ") or invalid name; skipped");
            FreeLibrary(plugin->module);
            return;
        }
        plugin->name = plugin->info.name;
        for (const auto& other : g_plugins)
            if (other->name == plugin->name)
            {
                logging::Error("plugin " + shortName + " duplicates the name " + plugin->name + "; skipped");
                FreeLibrary(plugin->module);
                return;
            }
        std::wstring configFile(plugin->info.config_file);
        if (configFile.empty())
            configFile = std::wstring(plugin->name.begin(), plugin->name.end()) + L".ini";
        plugin->configPath = g_gameDir + configFile;

        plugin->host.struct_size = sizeof(plugin->host);
        plugin->host.api_version = DAYZ_PLUGIN_API_VERSION;
        plugin->host.context = plugin.get();
        plugin->host.game_dir = g_gameDir.c_str();
        plugin->host.plugins_dir = g_pluginsDir.c_str();
        plugin->host.config_path = plugin->configPath.c_str();
        plugin->host.log = HostLog;
        plugin->host.request_backbuffer_size = HostRequestBackbufferSize;
        plugin->host.setting_register = HostSettingRegister;
        plugin->host.setting_get = HostSettingGet;
        plugin->host.setting_set = HostSettingSet;
        plugin->host.hotkey_register = HostHotkeyRegister;
        plugin->callbacks.struct_size = sizeof(plugin->callbacks);

        Plugin* raw = plugin.get();
        g_plugins.push_back(std::move(plugin));   // index must be valid during Start
        raw->inStart = true;
        const int result = start(&raw->host, &raw->callbacks);
        raw->inStart = false;
        if (result != 0)
        {
            logging::Info("plugin " + raw->name + " " + raw->info.version + " did not start (returned " +
                std::to_string(result) + "); unloaded");
            FreeLibrary(raw->module);
            raw->module = nullptr;
            return;
        }
        raw->started = true;
        g_anyStarted.store(true, std::memory_order_release);
        logging::Info("plugin " + raw->name + " " + raw->info.version + " started from " + shortName +
            " (config " + Narrow(configFile) + ")" +
            (raw->info.description[0] ? std::string(": ") + raw->info.description : std::string()));
    }

    void InitializeOnce() noexcept
    {
        logging::Initialize(L"dayz_pluginloader.log");
        g_gameDir = ExecutableDirectory();
        g_loaderIni = g_gameDir + L"dayz_pluginloader.ini";
        if (!ReadBoolean(L"loader", L"enabled", true))
        {
            logging::Info("plugin loader disabled in dayz_pluginloader.ini; DXGI passes through");
            return;
        }
        wchar_t modsDir[260]{};
        GetPrivateProfileStringW(L"loader", L"plugins_dir", L"plugins", modsDir,
            static_cast<DWORD>(std::size(modsDir)), g_loaderIni.c_str());
        g_pluginsDir = g_gameDir + modsDir;
        if (!g_pluginsDir.empty() && g_pluginsDir.back() != L'\\' && g_pluginsDir.back() != L'/')
            g_pluginsDir += L'\\';
        const std::vector<std::wstring> files = FindPluginDlls();
        logging::Info("plugin loader: " + std::to_string(files.size()) + " candidate(s) in " + Narrow(g_pluginsDir));
        for (const std::wstring& file : files)
            StartPlugin(file);
        logging::Info("plugin loader: " + std::to_string(std::count_if(g_plugins.begin(), g_plugins.end(),
            [](const auto& plugin) { return plugin->started; })) + " plugin(s) started, " +
            std::to_string(loader::settings::Count()) + " setting(s), " +
            std::to_string(loader::hotkeys::Count()) + " hotkey(s)");
    }

    template<typename Fn>
    void ForEachStarted(Fn fn) noexcept
    {
        for (const auto& plugin : g_plugins)
            if (plugin->started)
                fn(*plugin);
    }
}

namespace loader
{
    bool Initialize() noexcept
    {
        std::call_once(g_once, InitializeOnce);
        return AnyPluginStarted();
    }

    bool AnyPluginStarted() noexcept { return g_anyStarted.load(std::memory_order_acquire); }

    bool BackbufferOverride(unsigned& width, unsigned& height) noexcept
    {
        const unsigned w = g_overrideWidth.load(std::memory_order_relaxed);
        const unsigned h = g_overrideHeight.load(std::memory_order_relaxed);
        if (!w || !h)
            return false;
        width = w;
        height = h;
        return true;
    }

    void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept
    {
        ForEachStarted([&](const Plugin& plugin) {
            if (plugin.callbacks.on_swapchain_created)
                plugin.callbacks.on_swapchain_created(plugin.callbacks.context, swapChain);
        });
    }

    void OnPresent(IDXGISwapChain* swapChain, unsigned flags) noexcept
    {
        hotkeys::Poll(DispatchHotkey);
        ForEachStarted([&](const Plugin& plugin) {
            if (plugin.callbacks.on_present)
                plugin.callbacks.on_present(plugin.callbacks.context, swapChain, flags);
        });
    }

    void OnResizeBuffers(IDXGISwapChain* swapChain, unsigned width, unsigned height) noexcept
    {
        ForEachStarted([&](const Plugin& plugin) {
            if (plugin.callbacks.on_resize_buffers)
                plugin.callbacks.on_resize_buffers(plugin.callbacks.context, swapChain, width, height);
        });
    }
}
