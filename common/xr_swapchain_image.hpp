#pragma once

#include "xr_structure.hpp"

#include <cstdint>
#include <openxr/openxr.h>

namespace dayz::xr
{
    struct ImageCalls
    {
        PFN_xrAcquireSwapchainImage acquire;
        PFN_xrWaitSwapchainImage wait;
        PFN_xrReleaseSwapchainImage release;
    };

    enum class ImageFailure { None, Acquire, Wait, Render, Release };

    struct ImageUpdate
    {
        ImageFailure failure{ImageFailure::None};
        XrResult result{XR_SUCCESS};
        bool Ready() const noexcept { return failure == ImageFailure::None; }
        bool MustStop() const noexcept
        {
            // After a failed wait/release the acquired image has unresolved
            // ownership. Stop this session instead of issuing another acquire.
            return failure == ImageFailure::Wait || failure == ImageFailure::Release;
        }
    };

    template<class Render>
    ImageUpdate UpdateImage(XrSwapchain swapchain, const ImageCalls& calls, Render render)
    {
        std::uint32_t index{};
        const auto acquire = MakeXr<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
        XrResult result = calls.acquire(swapchain, &acquire, &index);
        if (result != XR_SUCCESS)
            return {ImageFailure::Acquire, result};
        auto wait = MakeXr<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        wait.timeout = XR_INFINITE_DURATION;
        result = calls.wait(swapchain, &wait);
        // XR_TIMEOUT_EXPIRED is nonnegative, but does not grant image ownership.
        if (result != XR_SUCCESS)
            return {ImageFailure::Wait, result};
        const bool rendered = render(index);
        const auto release = MakeXr<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        result = calls.release(swapchain, &release);
        if (result != XR_SUCCESS)
            return {ImageFailure::Release, result};
        return {rendered ? ImageFailure::None : ImageFailure::Render, XR_SUCCESS};
    }
}
