// Stable C ABI between the DayZ VR proxy (dxgi.dll) and an optional debug plugin.
//
// The proxy never links against the plugin. When [debug] enabled=true in
// dayz_openxr.ini and dayz_openxr_debug.dll exists beside DayZ_x64.exe, the proxy
// loads it and calls DayzVrDebugStart with a DayzVrDebugHost whose callbacks read
// live mod state and change tunables. The plugin decides how to expose them (the
// shipped one runs a localhost TCP line protocol). Everything here is plain C so
// either side can be rebuilt independently as long as DAYZ_VR_DEBUG_API_VERSION
// matches.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAYZ_VR_DEBUG_API_VERSION 2u

typedef struct DayzVrDebugHand
{
    uint32_t grip_valid;
    uint32_t aim_valid;
    float grip_position[3];
    float grip_orientation[4]; /* x, y, z, w */
    float aim_position[3];
    float aim_orientation[4];
} DayzVrDebugHand;

typedef struct DayzVrDebugState
{
    uint32_t struct_size;
    uint32_t api_version;

    /* Mod lifecycle */
    uint32_t hooks_active;        /* runtime probe hooks installed on this build */
    uint32_t openxr_initialized;
    uint32_t session_running;
    int32_t session_state;        /* XrSessionState, 5 = focused */
    uint32_t window_focused;      /* GetForegroundWindow() is the game window */
    uint32_t gui_cursor_mode;     /* inventory/menu owns the mouse */
    uint32_t gui_quad_visible;
    char build_profile[64];

    /* Frame pacing */
    uint64_t present_count;       /* DXGI presents since hooks became active */
    uint64_t stereo_apply_count;  /* alternate-eye camera writes */
    uint32_t rendered_eye;        /* 0 left, 1 right */
    double host_fps;              /* OpenXR submit rate, sampled every 120 frames */

    /* Head */
    uint32_t hmd_valid;
    float hmd_orientation[4];     /* x, y, z, w in OpenXR local space */
    float hmd_position[3];        /* metres */
    float hmd_yaw;                /* radians, derived from hmd_orientation */
    float hmd_pitch;
    float hmd_roll;

    /* Camera bookkeeping inside the runtime probe */
    uint32_t eyes_valid;
    float eye_left_x;
    float eye_right_x;
    uint32_t camera_directions_valid;
    float native_camera_direction[3];
    float render_camera_direction[3];
    double pending_mouse_x;       /* queued synthetic mouse counts from head yaw */
    double pending_mouse_y;
    /* Closed-loop head aim: remaining camera error (radians) and the learned
       signed mouse counts per radian. Zero when the loop is off. */
    float aim_yaw_error;
    float aim_pitch_error;
    float aim_yaw_gain;
    float aim_pitch_gain;

    DayzVrDebugHand hands[2];     /* 0 left, 1 right */

    /* Direct action input (engine getter hooks, dayz_input_hooks.cpp) */
    uint32_t direct_input_active;
    uint32_t direct_input_resolved;   /* actions bound to an engine record */
    uint32_t direct_input_unresolved; /* names the registry does not know */
    uint64_t direct_input_frames;     /* player input controller updates seen */
    uint64_t direct_input_overrides;  /* getter calls answered with a VR value */
    float direct_input_frame_seconds; /* dt of the last game frame */
} DayzVrDebugState;

typedef struct DayzVrDebugHost
{
    uint32_t struct_size;
    uint32_t api_version;
    uint16_t port;                /* [debug] port from the ini; plugin may ignore it */
    void* context;

    void (*get_state)(void* context, DayzVrDebugState* out);
    /* Returns 0 on success, -1 for an unknown name. */
    int (*get_tunable)(void* context, const char* name, double* out);
    /* Returns 0 on success, -1 for an unknown name, -2 for a rejected value. */
    int (*set_tunable)(void* context, const char* name, double value);
    /* Writes "name=value\n" lines. Returns the length needed, which may exceed
       `capacity`, in which case the buffer holds a truncated prefix. */
    size_t (*list_tunables)(void* context, char* buffer, size_t capacity);
    /* Returns 0 on success, -1 for an unknown command. */
    int (*run_command)(void* context, const char* name);
    void (*log)(void* context, const char* message);
} DayzVrDebugHost;

/* Exported by the plugin. Start returns 0 on success. */
typedef int (*DayzVrDebugStartFn)(const DayzVrDebugHost* host);
typedef void (*DayzVrDebugStopFn)(void);

#ifdef __cplusplus
}
#endif
