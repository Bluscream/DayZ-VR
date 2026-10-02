#pragma once

#include <array>
#include <cstdint>

namespace dayz::builds
{
    struct BuildProfile
    {
        const char* name;
        std::uint32_t peTimestamp;
        std::uint32_t imageSize;
        std::uintptr_t prepareViewRva;
        std::uintptr_t executeViewRva;
        std::uintptr_t finalizeViewRva;
        std::uintptr_t projectionDispatchRva;
        std::uintptr_t hudLayoutRva;
        std::uintptr_t guiInputMessageRva;
        std::uintptr_t guiScaleRva;
        std::uintptr_t engineSingletonRva;
        std::uintptr_t inventoryPreviewPrepareCallerRva;
        std::uintptr_t dynamicBlurRva;
        std::uintptr_t dynamicBlurParameterIndexRva;
        std::uintptr_t profileFovRva;
        std::uintptr_t cameraManagerRva;
        std::uintptr_t getActiveCameraStateRva;
        std::uintptr_t cameraFovUpdateRva;
        std::uintptr_t drawIndexedReturnRva{0x002600DD};
        std::uintptr_t drawReturnRva{0x002601C2};
        std::uintptr_t drawSecondReturnRva{0x0026038E};
        // Per-frame world render (prepare mode 1 + execute/finalize + post passes).
        // 0 disables [stereo] stereo_mode=double for the build.
        std::uintptr_t worldRenderRva{};
    };

    constexpr std::array kBuildProfiles{
        // 1.29.163709: verified against the installed executable and native call sites.
        BuildProfile{"DayZ_x64 1.29.163709", 0x6A72FC58u, 0x04406000u,
            0x0044F5A0, 0x004507A0, 0x004508B0, 0x00952000, 0x008A2280,
            0x00350B60, 0x0426F5CC, 0x042626D0, 0x005C4CA9, 0x0022EF70,
            0x00FED8F8, 0x01007E00, 0x01007CE0, 0x004B6BE0, 0x004B7AD0,
            0x0025F4DD, 0x0025F5C2, 0x0025F78E, 0x008E7650},
        BuildProfile{"DayZ_x64", 0x6A47B9AAu, 0x04407000u, 0x004501A0, 0x004513A0,
            0x004514B0, 0x00952B30, 0x008A2DB0, 0x00351760, 0x0427063C,
            0x04263740, 0x005C5899, 0x0022FB70, 0x00FEE968, 0x01008E70,
            0x01008D50, 0x004B77D0, 0x004B86C0},
        BuildProfile{"DayZDiag_x64", 0x6A47BAF9u, 0x049E7000u, 0x0048D8A0, 0x0048EB30,
            0x0048EC40, 0x00B30D10, 0x00A7B7B0, 0x00389410, 0x04823F4C,
            0x04815740, 0, 0, 0, 0, 0, 0, 0},
    };

}
