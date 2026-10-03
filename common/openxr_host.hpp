#pragma once

#include "dayz_runtime_probe.hpp"
#include "frame_source.hpp"
#include "xr_structure.hpp"
#include "xr_swapchain_image.hpp"

#include <atomic>
#include <d3d11.h>
#include <dxgi1_6.h>
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>

#include "stereo_state.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <wrl/client.h>

// Rendering-only host (see parked/README.md): controller input, the ammo quad, the
// axis/ray layers, haptics, melee, stance and steering live in parked/ and are not
// compiled. What remains is the OpenXR session, the two eye swapchains fed from the
// game's backbuffer captures, the GUI quad and the pose/frame-record bookkeeping.
class OpenXrHost
{
public:
    static OpenXrHost& Instance() noexcept;
    // Debug: request the captured eye images be written as BMP beside
    // DayZ_x64.exe. Executed on the render thread at the next frame because
    // the immediate context must not be used from the debug plugin thread.
    bool DumpEyeCaptures() noexcept;

    bool InitializeWithDevice(ID3D11Device* device) noexcept;
    void AttachGameSwapChain(IDXGISwapChain* swapChain) noexcept;
    bool InitializeStandalone() noexcept;
    void Tick() noexcept;
    void Shutdown() noexcept;
    bool IsInitialized() const noexcept { return initialized_; }
    bool IsSessionRunning() const noexcept { return sessionRunning_; }
    bool ShouldExit() const noexcept { return shouldExit_; }

    // Copy of the last rendered frame's tracking data for the debug plugin. Kept
    // behind its own mutex so a reader never blocks on xrWaitFrame.
    struct DebugSnapshot
    {
        bool initialized{};
        bool sessionRunning{};
        int sessionState{};
        double fps{};
        bool hmdValid{};
        XrPosef hmdPose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    };
    DebugSnapshot GetDebugSnapshot() const noexcept;

private:
    struct EyeSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs;
    };

    struct GuiSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs;
    };

    bool CreateInstanceAndSystem();
    bool CreateCompatibleDevice();
    bool ValidateDevice(ID3D11Device* device);
    bool FinishInitialization(ID3D11Device* device);
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchains();
    bool CreateGuiSwapchain(const std::vector<std::int64_t>& formats);

    void PollEvents();
    void RenderFrame();
    void AnchorGuiQuad(const XrPosef& headPose) noexcept;
    bool Check(XrResult result, const char* operation) const noexcept;
    bool CheckImageUpdate(const dayz::xr::ImageUpdate& update, const char* operation) noexcept;

    mutable std::mutex mutex_;
    mutable std::mutex debugMutex_;
    DebugSnapshot debugSnapshot_{};
    double lastFps_{};
    struct FrameTiming
    {
        double waitFrame{}, imageWork{}, endFrame{}, total{};
        unsigned frames{};
    } timing_{};
    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId systemId_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace localSpace_{XR_NULL_HANDLE};
    XrSpace viewSpace_{XR_NULL_HANDLE};
    std::atomic<XrSessionState> sessionState_{XR_SESSION_STATE_UNKNOWN};
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> gameSwapChain_;
    std::unique_ptr<IFrameSource> debugFrameSource_;
    std::unique_ptr<IFrameSource> gameFrameSource_;
    std::atomic<bool> eyeDumpRequested_{false};
    std::array<EyeSwapchain, 2> eyeSwapchains_{};
    GuiSwapchain guiSwapchain_{};
    std::array<XrView, 2> views_{{(MakeXr<XrView>(XR_TYPE_VIEW)), (MakeXr<XrView>(XR_TYPE_VIEW))}};
    XrPosef guiQuadPose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.25f}};
    float guiQuadWidthMeters_{1.4f};
    float guiQuadDistance_{1.25f};
    float guiQuadVerticalOffset_{-0.1f};
    bool guiQuadEnabled_{true};
    bool guiQuadAnchored_{};
    bool guiQuadWasVisible_{};
    bool guiQuadHasImage_{};
    // View poses the current capture of each eye was rendered with (frame records).
    struct CapturedView { dayz::stereo_state::FrameRecord record{}; bool valid{}; };
    std::array<CapturedView, 2> capturedViews_{};
    std::atomic<bool> initialized_{};
    std::atomic<bool> sessionRunning_{};
    std::atomic<bool> shouldExit_{};
};
