#pragma once

// Comfort vignette: darkens the periphery of each eye while the player moves or
// turns with the stick, the standard VR-mod answer to artificial locomotion
// sickness. Kept out of the OpenXR host; it only publishes a strength that the
// frame-source shader reads.
//
// [comfort] in dayz_openxr.ini: vignette (bool), vignette_strength (0..1 edge
// darkness), vignette_radius (0..1 clear centre radius), vignette_fade_seconds.
namespace dayz::comfort
{
    void Initialize(const wchar_t* iniPath) noexcept;
    // Called once per frame by the host with the current locomotion state.
    void Update(bool moving, bool turning, float deltaSeconds) noexcept;
}
