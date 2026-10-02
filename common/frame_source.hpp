#pragma once

#include <string>

#include <d3d11.h>
#include <openxr/openxr.h>

#include <cstdint>

struct EyeRenderInfo
{
    std::uint32_t eyeIndex{};
    XrPosef pose{};
    XrFovf fov{};
    ID3D11Texture2D* target{};
    ID3D11RenderTargetView* rtv{};
    std::uint32_t width{};
    std::uint32_t height{};
};

class IFrameSource
{
public:
    virtual ~IFrameSource() = default;
    virtual void PrepareFrame(std::uint32_t sourceEye = 0) noexcept { (void)sourceEye; }
    // Mid-frame variant for stereo_mode=double, called from DayZ's render thread
    // right before it clears `resource`: copies the backbuffer into the eye's
    // capture when that clear marks the start of the next pass.
    virtual bool CaptureIfBackBuffer(std::uint32_t sourceEye, void* resource) noexcept
    {
        (void)sourceEye; (void)resource; return false;
    }
    virtual bool HasGameData() const noexcept = 0;
    virtual void RenderEye(const EyeRenderInfo& eye) noexcept = 0;
    virtual bool DumpCaptures(const std::wstring&) noexcept { return false; }
};
