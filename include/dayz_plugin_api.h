// Stable C ABI between the DayZ plugin loader (dxgi.dll) and the plugins it hosts
// (plugins/*.dll beside DayZ_x64.exe).
//
// A plugin exports three functions:
//   int  DayzPluginDescribe(DayzPluginInfo* info);                      identity, config file
//   int  DayzPluginStart(const DayzPluginHost* host, DayzPluginCallbacks* callbacks);
//   void DayzPluginStop(void);
// Describe and Start return 0 on success; any other value makes the loader skip the
// plugin (a plugin that is disabled in its own config returns non-zero from Start). The
// loader never links against a plugin; both sides can be rebuilt independently as long
// as DAYZ_PLUGIN_API_VERSION matches. Every struct starts with struct_size so fields can
// be appended later without breaking older plugins.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAYZ_PLUGIN_API_VERSION 1u

typedef struct DayzPluginInfo
{
    uint32_t struct_size;
    uint32_t api_version;       /* DAYZ_PLUGIN_API_VERSION the plugin was built against */
    char name[64];              /* identifier, [a-z0-9_]: ini keys, log prefix, console */
    char version[32];
    char description[128];
    wchar_t config_file[260];   /* ini beside the game exe holding this plugin's settings;
                                   empty = "<name>.ini" */
} DayzPluginInfo;

typedef enum DayzSettingType
{
    DAYZ_SETTING_BOOL = 0,
    DAYZ_SETTING_INT = 1,
    DAYZ_SETTING_FLOAT = 2,
    DAYZ_SETTING_ENUM = 3,
    DAYZ_SETTING_STRING = 4
} DayzSettingType;

enum
{
    DAYZ_SETTING_LIVE = 1u,     /* a change takes effect at once */
    DAYZ_SETTING_RESTART = 2u   /* read once at start; a change needs a game restart */
};

typedef struct DayzSettingDesc
{
    uint32_t struct_size;
    const char* key;            /* "section.key" inside the plugin's config file */
    const char* title;
    const char* description;
    DayzSettingType type;
    uint32_t flags;             /* DAYZ_SETTING_LIVE | DAYZ_SETTING_RESTART */
    const char* default_value;  /* textual, exactly as written to the ini */
    double minimum;             /* int/float */
    double maximum;
    const char* choices;        /* enum: "first|second|third" */
} DayzSettingDesc;

typedef struct DayzHotkeyDesc
{
    uint32_t struct_size;
    const char* action;         /* [a-z0-9_]; bound as "<plugin>.<action>" */
    const char* title;
    const char* default_key;    /* "F12", "ctrl+shift+r", "numpad5", "0x7B"; "" = unbound */
} DayzHotkeyDesc;

struct IDXGISwapChain;

typedef struct DayzPluginHost
{
    uint32_t struct_size;
    uint32_t api_version;
    void* context;              /* passed back as the first argument of every call */
    const wchar_t* game_dir;    /* directory of the game exe, trailing separator */
    const wchar_t* plugins_dir;    /* directory the plugin was loaded from, trailing separator */
    const wchar_t* config_path; /* this plugin's config file, absolute */

    void (*log)(void* context, int level, const char* message);  /* 0 info, 1 error */

    /* Backbuffer size applied to swap-chain creation and ResizeBuffers for the game
       window. 0x0 clears the override. Only honoured when called from DayzPluginStart. */
    void (*request_backbuffer_size)(void* context, uint32_t width, uint32_t height);

    /* Settings. register: 0, or -1 for a bad descriptor or duplicate key. get copies
       the current textual value (ini value, else the default) and returns its length
       or -1 for an unknown key. set validates against the descriptor, writes the ini,
       notifies the owning plugin and returns 0, -1 unknown key, -2 rejected value. */
    int (*setting_register)(void* context, const DayzSettingDesc* desc);
    int (*setting_get)(void* context, const char* key, char* buffer, size_t capacity);
    int (*setting_set)(void* context, const char* key, const char* value);

    /* Hotkeys. Returns the id handed to on_hotkey, or -1. The binding comes from
       dayz_pluginloader.ini [hotkeys] <plugin>.<action>, falling back to default_key. */
    int (*hotkey_register)(void* context, const DayzHotkeyDesc* desc);
} DayzPluginHost;

typedef struct DayzPluginCallbacks
{
    uint32_t struct_size;
    void* context;              /* the plugin's own; passed back as the first argument */
    /* The game created a swap chain (device and window are reachable from it). */
    void (*on_swapchain_created)(void* context, struct IDXGISwapChain* swap_chain);
    /* Before every real Present (DXGI_PRESENT_TEST calls are not forwarded). */
    void (*on_present)(void* context, struct IDXGISwapChain* swap_chain, uint32_t flags);
    /* After a successful ResizeBuffers with the size that was applied. */
    void (*on_resize_buffers)(void* context, struct IDXGISwapChain* swap_chain,
        uint32_t width, uint32_t height);
    /* A registered hotkey was pressed (edge-triggered, present thread). */
    void (*on_hotkey)(void* context, int hotkey_id, const char* action);
    /* A registered setting was changed through the host (not by the plugin itself). */
    void (*on_setting_changed)(void* context, const char* key, const char* value);
} DayzPluginCallbacks;

typedef int (*DayzPluginDescribeFn)(DayzPluginInfo* info);
typedef int (*DayzPluginStartFn)(const DayzPluginHost* host, DayzPluginCallbacks* callbacks);
typedef void (*DayzPluginStopFn)(void);

#ifdef __cplusplus
}
#endif
