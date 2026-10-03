#include "melee_swing.hpp"

#include <algorithm>
#include <cmath>

namespace dayz::melee
{
    namespace
    {
        constexpr float kMinDt = 0.001f;
        constexpr float kMaxDt = 0.1f;
        constexpr float kClassifyWindowSeconds = 0.12f;  // longest wait for the peak
        constexpr float kPeakDropFraction = 0.8f;        // speed fell off the peak: classify
    }

    void SwingDetector::Reset() noexcept
    {
        *this = SwingDetector{};
    }

    Swing SwingDetector::Update(float x, float y, float z, float dt, const Config& config) noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(dt))
            return Swing::None;
        if (!primed_)
        {
            primed_ = true;
            x_ = x; y_ = y; z_ = z;
            return Swing::None;
        }
        dt = (std::clamp)(dt, kMinDt, kMaxDt);
        const float dx = x - x_, dy = y - y_, dz = z - z_;
        x_ = x; y_ = y; z_ = z;
        const float raw = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
        speed_ = 0.5f * speed_ + 0.5f * raw;  // two-sample smoothing against tracking jitter
        sinceEvent_ += dt;
        if (!armed_ && speed_ < config.rearmSpeed)
            armed_ = true;
        if (inSwing_)
        {
            peak_ = (std::max)(peak_, speed_);
            swingSeconds_ += dt;
            if (speed_ < peak_ * kPeakDropFraction || swingSeconds_ >= kClassifyWindowSeconds)
            {
                inSwing_ = false;
                armed_ = false;
                sinceEvent_ = 0.0f;
                return peak_ >= config.heavySpeed ? Swing::Heavy : Swing::Light;
            }
            return Swing::None;
        }
        if (armed_ && sinceEvent_ >= config.cooldownSeconds && speed_ >= config.lightSpeed)
        {
            inSwing_ = true;
            peak_ = speed_;
            swingSeconds_ = 0.0f;
        }
        return Swing::None;
    }
}
