#pragma once

// Motion-controlled melee: turns the right controller's grip motion into swing
// events. Pure and host-testable; the OpenXR host maps events to DayZ's attack key.
//
// A swing starts when the smoothed hand speed crosses light_speed. The peak speed is
// then tracked until the hand slows again (or a short classification window ends),
// and one event fires: Heavy at or above heavy_speed, Light otherwise. The detector
// then stays disarmed until the hand slows below rearm_speed and cooldown_seconds
// have passed, so one physical swing never fires twice.
namespace dayz::melee
{
    struct Config
    {
        float lightSpeed{1.6f};      // m/s, start of a swing
        float heavySpeed{3.2f};      // m/s peak that makes it a heavy attack
        float cooldownSeconds{0.5f}; // minimum time between events
        float rearmSpeed{0.6f};      // hand must slow below this before the next swing
    };

    enum class Swing { None, Light, Heavy };

    class SwingDetector
    {
    public:
        // position in metres in any fixed space; dt seconds since the previous sample.
        // The first sample only primes the detector.
        Swing Update(float x, float y, float z, float dt, const Config& config) noexcept;
        void Reset() noexcept;
        float Speed() const noexcept { return speed_; }

    private:
        bool primed_{};
        float x_{}, y_{}, z_{};
        float speed_{};
        bool inSwing_{};
        float peak_{};
        float swingSeconds_{};
        bool armed_{true};
        float sinceEvent_{1000.0f};
    };
}
