#include "../common/xr_swapchain_image.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    std::string calls;
    std::array<XrResult, 3> results{};

    void Expect(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    XrResult XRAPI_PTR Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index)
    {
        calls += 'A';
        *index = 2;
        return results[0];
    }
    XrResult XRAPI_PTR Wait(XrSwapchain, const XrSwapchainImageWaitInfo*)
    {
        calls += 'W';
        return results[1];
    }
    XrResult XRAPI_PTR Release(XrSwapchain, const XrSwapchainImageReleaseInfo*)
    {
        calls += 'L';
        return results[2];
    }
}

int main()
{
    using namespace dayz::xr;
    const ImageCalls api{Acquire, Wait, Release};
    const auto run = [&](bool render = true) {
        calls.clear();
        return UpdateImage(XR_NULL_HANDLE, api, [&](std::uint32_t index) {
            Expect(index == 2, "render must use the acquired image index");
            calls += 'R';
            return render;
        });
    };
    results.fill(XR_SUCCESS);
    Expect(run().Ready() && calls == "AWRL", "complete ownership cycle");
    Expect(!run(false).Ready() && calls == "AWRL", "failed drawing still releases but cannot submit");
    for (const XrResult failure : {XR_ERROR_RUNTIME_FAILURE, XR_TIMEOUT_EXPIRED})
    {
        results = {failure, XR_SUCCESS, XR_SUCCESS};
        const auto acquire = run();
        Expect(!acquire.Ready() && !acquire.MustStop() && calls == "A", "failed acquire must not draw");
        results = {XR_SUCCESS, failure, XR_SUCCESS};
        const auto wait = run();
        Expect(!wait.Ready() && wait.MustStop() && calls == "AW", "failed wait must not draw or release");
        results = {XR_SUCCESS, XR_SUCCESS, failure};
        const auto release = run();
        Expect(!release.Ready() && release.MustStop() && calls == "AWRL", "failed release must not submit");
    }
    // An earlier image success cannot validate a failed update of the next image.
    results.fill(XR_SUCCESS);
    Expect(run().Ready(), "first image ready");
    results[0] = XR_ERROR_RUNTIME_FAILURE;
    Expect(!run().Ready(), "stale image readiness must not leak across acquire failures");
    std::cout << "xr_swapchain_image_test: ownership, timeout and stale-image cases passed\n";
}
