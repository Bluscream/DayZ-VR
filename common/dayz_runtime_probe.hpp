#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11RenderTargetView;
struct IDXGISwapChain;
struct HWND__;

namespace dayz::runtime_probe
{
    // Installs observation-only hooks for the one DayZ build described by output/output_p1_dr.
    // No engine argument or object is modified. A failed identity/signature check leaves stock
    // execution untouched.
    bool Initialize() noexcept;
    void AttachD3DDevice(ID3D11Device* device) noexcept;
    void BeforePresent(IDXGISwapChain* swapChain) noexcept;
    void OnPresent() noexcept;
    bool IsGuiQuadVisible() noexcept;
    void SetGuiVirtualCursorNormalized(float u, float v) noexcept;
    bool RenderGuiQuad(ID3D11RenderTargetView* target, std::uint32_t width,
        std::uint32_t height) noexcept;
    bool IsActive() noexcept;
    // The real foreground window even while [hooks] keep_focus makes the engine see
    // the game window as always active. Use this for anything that injects input.
    HWND__* RealForegroundWindow() noexcept;

    // Debug-plugin surface. These touch the same plain globals the render thread
    // reads; a torn read is impossible for the 4-byte values involved, but values
    // may be observed one frame late. Debug use only.
    struct DebugSnapshot
    {
        bool hooksActive{};
        bool windowFocused{};
        bool guiCursorMode{};
        bool guiQuadVisible{};
        const char* buildProfile{""};
        std::uint64_t presentCount{};
        std::uint64_t stereoApplyCount{};
        double pendingMouseX{};
        double pendingMouseY{};
        float aimYawError{};
        float aimPitchError{};
        float aimYawGain{};
        float aimPitchGain{};
    };
    DebugSnapshot GetDebugSnapshot() noexcept;

    // HUD content rectangle (fractions of the backbuffer) the proxy wrote into DayZ's
    // renderer: GUI widget coordinates 0..1 span this rectangle, not the full frame.
    struct HudContentRect
    {
        float left{};
        float top{};
        float width{1.0f};
        float height{1.0f};
        bool valid{};
    };
    HudContentRect GetHudContentRect() noexcept;
    // Tunable names are "section.key" as in dayz_openxr.ini. Booleans use 0/1.
    bool GetTunable(const char* name, double& value) noexcept;
    // Returns 0 on success, -1 for an unknown name, -2 for a rejected value.
    int SetTunable(const char* name, double value) noexcept;
    // Invokes `visit(name, value)` for every tunable.
    void ForEachTunable(void (*visit)(void* context, const char* name, double value),
        void* context) noexcept;
    // Forget the captured HMD yaw/position centre so the next frame recaptures it.
    void RecenterHmd() noexcept;
    // True while [stereo] hmd_aim_closed_loop drives DayZ's mouse camera.
    bool ClosedLoopAimActive() noexcept;
    // Rotates the closed-loop yaw target (radians, positive = left) for stick or
    // snap turning; the loop then turns the game camera to match.
    void AddAimYawOffset(float radians) noexcept;
}
