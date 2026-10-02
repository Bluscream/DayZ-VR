#include "stereo_state.hpp"

#include <atomic>
#include <mutex>

namespace
{
    // Pose tuples are read by camera, render and debug threads. A single lock
    // publishes complete values and makes tracking invalidation indivisible.
    std::mutex g_poseMutex;
    dayz::stereo_state::EyePositions g_positions{};
    dayz::stereo_state::HmdOrientation g_orientation{};
    dayz::stereo_state::HmdPosition g_hmdPosition{};
    dayz::stereo_state::CameraDirections g_cameraDirections{};
    dayz::stereo_state::HmdOrientation g_aimOrientation{};
    std::atomic_uint g_eye{};
    std::atomic<float> g_imageShift{};
    std::atomic_uint g_fitMode{static_cast<unsigned>(dayz::stereo_state::FitMode::Contain)};
    std::atomic<float> g_scaleX{1.0f};
    std::atomic<float> g_scaleY{1.0f};
}

namespace dayz::stereo_state
{
    void UpdateEyePositions(float leftX, float, float,
        float rightX, float, float) noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        g_positions = {leftX, rightX, true};
    }

    EyePositions GetEyePositions() noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        return g_positions;
    }

    void UpdateHmdOrientation(float x, float y, float z, float w) noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        g_orientation = {x, y, z, w, true};
    }

    HmdOrientation GetHmdOrientation() noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        return g_orientation;
    }

    void UpdateHmdPosition(float x, float y, float z) noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        g_hmdPosition = {x, y, z, true};
    }

    HmdPosition GetHmdPosition() noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        return g_hmdPosition;
    }

    void UpdateCameraDirections(float nativeX, float nativeY, float nativeZ,
        float renderX, float renderY, float renderZ) noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        g_cameraDirections = {nativeX, nativeY, nativeZ, renderX, renderY, renderZ, true};
    }

    CameraDirections GetCameraDirections() noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        return g_cameraDirections;
    }

    void InvalidateTracking() noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        g_positions = {};
        g_orientation = {};
        g_hmdPosition = {};
        g_cameraDirections = {};
        g_aimOrientation = {};
    }

    unsigned RenderedEye() noexcept
    {
        return g_eye.load(std::memory_order_relaxed) & 1u;
    }

    void AdvanceEye() noexcept
    {
        g_eye.fetch_xor(1u, std::memory_order_relaxed);
    }

    void SetImageShift(float shift) noexcept
    {
        g_imageShift.store(shift, std::memory_order_relaxed);
    }

    float ImageShift() noexcept
    {
        return g_imageShift.load(std::memory_order_relaxed);
    }

    void SetPresentation(FitMode fitMode, float scaleX, float scaleY) noexcept
    {
        g_fitMode.store(static_cast<unsigned>(fitMode), std::memory_order_relaxed);
        g_scaleX.store(scaleX, std::memory_order_relaxed);
        g_scaleY.store(scaleY, std::memory_order_relaxed);
    }

    Presentation GetPresentation() noexcept
    {
        Presentation result{};
        result.fitMode = static_cast<FitMode>(g_fitMode.load(std::memory_order_relaxed));
        result.scaleX = g_scaleX.load(std::memory_order_relaxed);
        result.scaleY = g_scaleY.load(std::memory_order_relaxed);
        return result;
    }

    namespace
    {
        std::atomic<float> g_vignetteStrength{0.0f};
        std::atomic<float> g_vignetteRadius{0.6f};
    }

    void SetComfortVignette(float strength, float radius) noexcept
    {
        g_vignetteStrength.store(strength, std::memory_order_relaxed);
        g_vignetteRadius.store(radius, std::memory_order_relaxed);
    }

    ComfortVignette GetComfortVignette() noexcept
    {
        ComfortVignette result{};
        result.strength = g_vignetteStrength.load(std::memory_order_relaxed);
        result.radius = g_vignetteRadius.load(std::memory_order_relaxed);
        return result;
    }

    void UpdateAimOrientation(float x, float y, float z, float w, bool valid) noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        g_aimOrientation = {x, y, z, w, valid};
    }

    HmdOrientation GetAimOrientation() noexcept
    {
        std::scoped_lock lock(g_poseMutex);
        return g_aimOrientation;
    }

    namespace
    {
        std::atomic<EyeCaptureCallback> g_eyeCapture{nullptr};
    }

    void SetRenderedEye(unsigned eye) noexcept
    {
        g_eye.store(eye & 1u, std::memory_order_relaxed);
    }

    void SetEyeCaptureCallback(EyeCaptureCallback callback) noexcept
    {
        g_eyeCapture.store(callback, std::memory_order_release);
    }

    bool CaptureEyeIfBackBuffer(unsigned eye, void* d3dResource) noexcept
    {
        const EyeCaptureCallback callback = g_eyeCapture.load(std::memory_order_acquire);
        return callback && callback(eye & 1u, d3dResource);
    }

    namespace
    {
        std::atomic<unsigned> g_backWidth{0};
        std::atomic<unsigned> g_backHeight{0};
    }

    void SetBackBufferSize(unsigned width, unsigned height) noexcept
    {
        g_backWidth.store(width, std::memory_order_relaxed);
        g_backHeight.store(height, std::memory_order_relaxed);
    }

    void GetBackBufferSize(unsigned& width, unsigned& height) noexcept
    {
        width = g_backWidth.load(std::memory_order_relaxed);
        height = g_backHeight.load(std::memory_order_relaxed);
    }
}
