#pragma once

#include "dayz_runtime_probe.hpp"
#include "frame_source.hpp"
#include "melee_swing.hpp"
#include "shot_detector.hpp"
#include "physical_stance.hpp"
#include "vehicle_steering.hpp"
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
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <wrl/client.h>

class OpenXrHost
{
public:
    static OpenXrHost& Instance() noexcept;
    // Debug: request the captured eye images be written as BMP beside
    // DayZ_x64.exe. Executed on the render thread at the next frame because
    // the immediate context must not be used from the debug plugin thread.
    // Protocol command "haptic": test pulse on the right controller.
    bool TestHaptic() noexcept;
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
        std::array<XrSpaceLocation, 2> grip{};
        std::array<XrSpaceLocation, 2> aim{};
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

    struct AxisSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::vector<XrSwapchainImageD3D11KHR> images;
    };

    // Controller-anchored ammo display ([hud] ammo_quad): seven-segment bitmap uploaded
    // only when the shown text changes, submitted as one quad layer at the right grip.
    struct AmmoSwapchain
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::vector<XrSwapchainImageD3D11KHR> images;
        unsigned width{};
        unsigned height{};
        std::string text;
        std::uint32_t colour{};
        bool hasImage{};
    };

    bool CreateInstanceAndSystem();
    bool CreateCompatibleDevice();
    bool ValidateDevice(ID3D11Device* device);
    bool FinishInitialization(ID3D11Device* device);
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchains();
    bool CreateGuiSwapchain(const std::vector<std::int64_t>& formats);
    bool CreateAmmoSwapchain(const std::vector<std::int64_t>& formats);
    // Builds the ammo quad for this frame; returns false when nothing should be shown.
    bool PrepareAmmoLayer(XrCompositionLayerQuad& layer) noexcept;
    bool CreateControllerActions();
    bool CreateAxisSwapchain(const std::vector<std::int64_t>& formats);
    void SyncControllerInput(XrTime displayTime, bool guiVisible, bool injectInput);
    void ReleaseInjectedInput() noexcept;  // held keys/mouse buttons only; poses stay
    bool UpdateMotionMelee(XrTime displayTime, float dt, bool guiVisible) noexcept;
    void PublishVehicleSteering() noexcept;
    void UpdatePhysicalStance(XrTime displayTime) noexcept;
    void UpdateFireHaptics() noexcept;
    // Pulses one controller (0 left, 1 right). seconds/amplitude clamped; false when
    // haptics are unavailable (no actions, session not focused, runtime error).
    bool PulseHaptic(int hand, float seconds, float amplitude) noexcept;

    void ReleaseControllerKeys() noexcept; // ReleaseInjectedInput + forget controller poses
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
    AxisSwapchain axisSwapchain_{};
    XrActionSet actionSet_{XR_NULL_HANDLE};
    XrAction gripPoseAction_{XR_NULL_HANDLE};
    XrAction aimPoseAction_{XR_NULL_HANDLE};
    XrAction triggerAction_{XR_NULL_HANDLE};
    XrAction grabAction_{XR_NULL_HANDLE};
    // Optional instance extensions the runtime exposed and we enabled (controller
    // profile extensions); interaction profiles are suggested only for these.
    std::vector<std::string> enabledOptionalExtensions_;
    XrAction xButtonAction_{XR_NULL_HANDLE};
    XrAction yButtonAction_{XR_NULL_HANDLE};
    XrAction aButtonAction_{XR_NULL_HANDLE};
    XrAction bButtonAction_{XR_NULL_HANDLE};
    XrAction thumbstickAction_{XR_NULL_HANDLE};
    XrAction thumbstickClickAction_{XR_NULL_HANDLE};
    XrAction hapticAction_{XR_NULL_HANDLE};
    std::array<XrPath, 2> handPaths_{{XR_NULL_PATH, XR_NULL_PATH}};
    std::array<XrSpace, 2> gripSpaces_{{XR_NULL_HANDLE, XR_NULL_HANDLE}};
    std::array<XrSpace, 2> aimSpaces_{{XR_NULL_HANDLE, XR_NULL_HANDLE}};
    std::array<XrSpaceLocation, 2> gripLocations_{{
        (MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION)), (MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION))}};
    std::array<XrSpaceLocation, 2> aimLocations_{{
        (MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION)), (MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION))}};
    std::array<XrView, 2> views_{{(MakeXr<XrView>(XR_TYPE_VIEW)), (MakeXr<XrView>(XR_TYPE_VIEW))}};
    XrPosef guiQuadPose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.25f}};
    float guiQuadWidthMeters_{1.4f};
    float guiQuadDistance_{1.25f};
    float guiQuadVerticalOffset_{-0.1f};
    bool guiQuadEnabled_{true};
    bool guiQuadAnchored_{};
    bool guiQuadWasVisible_{};
    bool guiQuadHasImage_{};
    bool controllerInputEnabled_{true};
    bool controllerAxesEnabled_{true};
    bool ammoQuadEnabled_{true};  // swapchain exists (ini, needs restart)
    // Live tunables (hud.ammo_quad*), registered with the runtime probe so the debug
    // plugin can change them while DayZ runs. Written by the debug thread, read per frame.
    std::atomic<float> ammoQuadVisible_{1.0f};
    std::atomic<float> ammoQuadWidthMeters_{0.07f};
    std::atomic<float> ammoQuadOffsetX_{0.0f};
    std::atomic<float> ammoQuadOffsetY_{0.04f};
    std::atomic<float> ammoQuadOffsetZ_{-0.02f};
    std::atomic<float> ammoQuadTiltDegrees_{40.0f};
    unsigned ammoQuadPixelHeight_{48};
    // Motion melee ([melee]): right grip swings become attack presses while fists or
    // a melee weapon are in hands. Live tunables like the ammo quad.
    std::atomic<float> meleeMotionSwing_{0.0f};
    std::atomic<float> meleeLightSpeed_{1.6f};
    std::atomic<float> meleeHeavySpeed_{3.2f};
    std::atomic<float> meleeCooldownSeconds_{0.5f};
    std::atomic<float> meleeHeavyHoldSeconds_{0.45f};
    dayz::melee::SwingDetector meleeSwing_;
    XrTime meleeReleaseTime_{};  // attack key held until this display time (0 = not held)
    // Two-hand steering wheel ([vehicle]), published to the script bridge.
    std::atomic<float> vehicleSteering_{1.0f};
    std::atomic<float> vehicleWheelMaxDegrees_{90.0f};
    std::atomic<float> vehicleDeadzone_{0.05f};
    std::atomic<float> vehicleInvert_{0.0f};
    std::atomic<float> vehicleRequireGrip_{1.0f};  // both squeeze > 0.5 to hold the wheel
    // Physical crouch/prone from head height ([stance]); standing height is captured
    // at the first tracked frame and again after every recenter.
    std::atomic<float> stancePhysical_{0.0f};
    std::atomic<float> stanceCrouchDrop_{0.35f};
    std::atomic<float> stanceProneDrop_{0.85f};
    std::atomic<float> stanceHysteresis_{0.08f};
    float standingHeight_{};
    bool haveStandingHeight_{};
    unsigned recenterGenerationSeen_{};
    XrTime stanceKeyReleaseTime_{};
    WORD stanceKey_{};
    XrTime stanceNextChangeTime_{};
    // Haptics ([haptics]): a pulse per fired round detected from the bridge ammo count.
    std::atomic<float> hapticsFire_{1.0f};
    std::atomic<float> hapticsFireSeconds_{0.08f};
    std::atomic<float> hapticsFireAmplitude_{0.8f};
    dayz::shot::Detector shotDetector_;
    unsigned hapticPulses_{};
    std::array<dayz::runtime_probe::ExternalTunable, 23> hostTunables_{};
    AmmoSwapchain ammoSwapchain_{};
    bool guiRayEnabled_{true};
    float guiRayLength_{2.0f};
    float guiRayThickness_{0.004f};
    bool guiRayValid_{};
    float currentGuiRayLength_{2.0f};
    bool directionRaysEnabled_{true};
    float directionRayLength_{3.0f};
    float directionRayThickness_{0.006f};
    float controllerTurnScale_{18.0f};
    float controllerTurnRate_{90.0f};
    float controllerSnapTurn_{0.0f};
    bool snapTurnArmed_{true};
    XrTime lastTurnTime_{};
    float controllerDeadzone_{0.3f};
    std::array<bool, 4> movementKeys_{};
    bool leftMouseDown_{};
    bool rightMouseDown_{};
    bool xKeyDown_{};
    bool recenterOnStickClick_{true};
    bool leftStickClickDown_{};
    bool yKeyDown_{};
    bool escapeKeyDown_{};
    bool tabKeyDown_{};
    bool rightGrabDown_{};
    bool aButtonDown_{};
    bool bButtonDown_{};
    bool hotbarPreviousDown_{};
    bool hotbarNextDown_{};
    WORD hotbarPreviousKey_{};
    WORD hotbarNextKey_{};
    unsigned hotbarSlot_{1};
    std::atomic<bool> initialized_{};
    std::atomic<bool> sessionRunning_{};
    std::atomic<bool> shouldExit_{};
};
