#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dayz::trace
{
    enum class EventKind : std::uint8_t { Projection, Prepare, Execute, Finalize };
    struct Event
    {
        EventKind kind{};
        std::uint8_t mode{};
        std::uint32_t thread{};
        void* context{};
        void* camera{};
        void* descriptor{};
        std::uintptr_t arenaCursor{};
        std::uintptr_t callerRva{};
    };

    enum class ApiKind : std::uint8_t
    {
        Targets, Viewport, ClearColor, ClearDepth, Copy, Resolve, DrawIndexed, Draw
    };
    struct ApiEvent
    {
        ApiKind kind{};
        std::uint32_t thread{};
        void* object0{};
        void* object1{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t format{};
        std::uint32_t samples{};
        std::uint32_t targetWidth{};
        std::uint32_t targetHeight{};
        std::uint32_t elementCount{};
        std::uintptr_t callerRva{};
    };
    struct DrawStateEvent
    {
        ApiKind kind{};
        std::uint32_t thread{};
        void* pixelShader{};
        void* vertexShader{};
        void* blendState{};
        void* depthState{};
        std::uint32_t targetWidth{};
        std::uint32_t targetHeight{};
        std::uint32_t targetFormat{};
        std::uint32_t elementCount{};
        std::uintptr_t callerRva{};
        std::uintptr_t parentCallerRva{};
        float viewportX{};
        float viewportY{};
        float viewportWidth{};
        float viewportHeight{};
        bool depthEnabled{};
        bool depthWrite{};
    };

    // Only completed local records enter the buffer. Snapshot/reset share the
    // same lock, so no producer can reuse slots while the consumer logs them.
    template<typename T, std::size_t Capacity>
    class Buffer
    {
    public:
        struct Snapshot
        {
            std::array<T, Capacity> entries{};
            std::size_t count{};
        };

        void Push(const T& event) noexcept
        {
            std::scoped_lock lock(mutex_);
            Append(event);
        }

        template<typename Equal>
        void PushUnique(const T& event, Equal equal) noexcept
        {
            std::scoped_lock lock(mutex_);
            for (std::size_t index = 0; index < (std::min)(state_.count, Capacity); ++index)
                if (equal(state_.entries[index], event))
                    return;
            Append(event);
        }

        Snapshot Copy() const noexcept
        {
            std::scoped_lock lock(mutex_);
            return state_;
        }

        Snapshot Take() noexcept
        {
            std::scoped_lock lock(mutex_);
            const Snapshot result = state_;
            state_.count = 0;
            return result;
        }

        static constexpr std::size_t size() noexcept { return Capacity; }

    private:
        void Append(const T& event) noexcept
        {
            if (state_.count < Capacity)
                state_.entries[state_.count] = event;
            ++state_.count;
        }

        mutable std::mutex mutex_;
        Snapshot state_;
    };
}
