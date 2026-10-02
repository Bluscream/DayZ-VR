#pragma once

#include <mutex>

namespace dayz::runtime_probe
{
    // A real engine projection grants one world-render replay. Internal per-eye
    // projection rebuilds must not grant another replay of a possibly stale view.
    class ProjectionReplay
    {
    public:
        void Publish(void* context, bool internalReplay) noexcept
        {
            if (internalReplay)
                return;
            std::scoped_lock lock(mutex_);
            pending_ = context;
        }

        void* Consume() noexcept
        {
            std::scoped_lock lock(mutex_);
            void* context = pending_;
            pending_ = nullptr;
            return context;
        }

    private:
        std::mutex mutex_;
        void* pending_{};
    };
}
