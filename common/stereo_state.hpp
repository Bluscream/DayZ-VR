#pragma once

#include <cstdint>

namespace dayz::stereo_state
{
    struct EyePositions
    {
        float leftX{};
        float rightX{};
        bool valid{};
    };

    struct HmdOrientation
    {
        float x{};
        float y{};
        float z{};
        float w{1.0f};
        bool valid{};
    };
    struct HmdPosition
    {
        float x{};
        float y{};
        float z{};
        bool valid{};
    };
    struct CameraDirections
    {
        float nativeX{};
        float nativeY{};
        float nativeZ{-1.0f};
        float renderX{};
        float renderY{};
        float renderZ{-1.0f};
        bool valid{};
    };

    enum class FitMode : unsigned { Contain, Stretch, Cover };
    struct Presentation
    {
        FitMode fitMode{FitMode::Contain};
        float scaleX{1.0f};
        float scaleY{1.0f};
    };

    void UpdateEyePositions(float leftX, float leftY, float leftZ,
        float rightX, float rightY, float rightZ) noexcept;
    EyePositions GetEyePositions() noexcept;
    void UpdateHmdOrientation(float x, float y, float z, float w) noexcept;
    HmdOrientation GetHmdOrientation() noexcept;
    // Right controller aim-pose orientation in the same space as the HMD;
    // valid=false when the controller is not tracked.
    void UpdateAimOrientation(float x, float y, float z, float w, bool valid) noexcept;
    HmdOrientation GetAimOrientation() noexcept;
    void UpdateHmdPosition(float x, float y, float z) noexcept;
    HmdPosition GetHmdPosition() noexcept;
    void UpdateCameraDirections(float nativeX, float nativeY, float nativeZ,
        float renderX, float renderY, float renderZ) noexcept;
    CameraDirections GetCameraDirections() noexcept;
    // Reject stale poses after tracking/session loss. Each getter returns a
    // complete tuple; no reader can see partially invalidated pose components.
    void InvalidateTracking() noexcept;

    // Per-frame records: the game thread freezes one HMD sample per game frame when it
    // applies the pose to the camera (BeginGameFrame) and the render thread, at Present,
    // looks the record up for the frame it is presenting (PresentedRecord with the
    // pipeline lag in frames). The eye and the exact view poses used to render that
    // frame travel with the record, so the capture is filed under the right eye and the
    // compositor receives the pose the image was rendered with.
    struct ViewPose
    {
        float qx{}, qy{}, qz{}, qw{1.0f};
        float px{}, py{}, pz{};
        float fovLeft{}, fovRight{}, fovUp{}, fovDown{};
    };
    struct FrameRecord
    {
        std::uint64_t index{};
        unsigned eye{};
        HmdOrientation orientation{};
        HmdPosition position{};
        ViewPose views[2]{};
        bool valid{};
    };
    // Host side, together with the orientation/position of the same sample.
    void UpdateHmdViews(const ViewPose& left, const ViewPose& right) noexcept;
    // Game thread, once per game frame: freezes the latest sample, assigns the eye
    // (alternating when alternate is true) and files the record. Returns its index.
    std::uint64_t BeginGameFrame(bool alternate) noexcept;
    // The sample frozen by the last BeginGameFrame (valid=false before the first one or
    // after InvalidateTracking); the camera code uses it so both refreshes of a frame
    // see the same pose.
    FrameRecord CurrentFrame() noexcept;
    // Render thread at Present: the record of the frame being presented, i.e. the one
    // filed `lag` BeginGameFrame calls before the latest. False when unknown.
    bool PresentedRecord(unsigned lag, FrameRecord& out) noexcept;
    std::uint64_t LatestFrameIndex() noexcept;
    // Pipeline lag in frames between a game frame's BeginGameFrame and its Present, and
    // whether the host submits the recorded view poses with the eye images.
    void SetFrameLag(unsigned lag) noexcept;
    unsigned FrameLag() noexcept;
    void SetSubmitRenderedPose(bool enabled) noexcept;
    bool SubmitRenderedPose() noexcept;
    unsigned RenderedEye() noexcept;
    void AdvanceEye() noexcept;
    void SetRenderedEye(unsigned eye) noexcept;
    // Mid-frame eye capture used by stereo_mode=double. DayZ submits D3D work
    // from its own render thread, so the capture runs inside that command
    // stream: the probe calls this from the ClearRenderTargetView hook with the
    // resource about to be cleared, and the host copies it into the eye's
    // capture if it is the backbuffer. Returns true when captured.
    using EyeCaptureCallback = bool (*)(unsigned eye, void* d3dResource);
    void SetEyeCaptureCallback(EyeCaptureCallback callback) noexcept;
    bool CaptureEyeIfBackBuffer(unsigned eye, void* d3dResource) noexcept;
    // Backbuffer dimensions published by the frame source (0 until known).
    void SetBackBufferSize(unsigned width, unsigned height) noexcept;
    void GetBackBufferSize(unsigned& width, unsigned& height) noexcept;
    void SetImageShift(float shift) noexcept;
    float ImageShift() noexcept;
    void SetPresentation(FitMode fitMode, float scaleX, float scaleY) noexcept;
    Presentation GetPresentation() noexcept;
    // Comfort vignette published by dayz::comfort; strength 0 draws nothing.
    struct ComfortVignette { float strength{}; float radius{0.6f}; };
    void SetComfortVignette(float strength, float radius) noexcept;
    ComfortVignette GetComfortVignette() noexcept;
}
