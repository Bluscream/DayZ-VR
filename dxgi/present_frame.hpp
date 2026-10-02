#pragma once

namespace hooks
{
    // DXGI_PRESENT_TEST is an occlusion query. It must reach DXGI without
    // starting an OpenXR frame or advancing the mod's game-frame state.
    inline constexpr unsigned kPresentTestFlag = 0x00000001u;

    template<typename Tick, typename Present>
    auto TickAndPresent(unsigned flags, Tick tick, Present present)
    {
        if ((flags & kPresentTestFlag) == 0)
            tick();
        return present();
    }
}
