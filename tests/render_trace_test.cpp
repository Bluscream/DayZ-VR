#include "../common/render_trace.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    struct TaggedEvent
    {
        unsigned tag{};
        unsigned inverse{};
    };

    void Expect(bool condition, const char* reason)
    {
        if (!condition)
            throw std::runtime_error(reason);
    }

    void TestSnapshotOwnership()
    {
        dayz::trace::Buffer<TaggedEvent, 2> buffer;
        buffer.Push({1, ~1u});
        buffer.Push({2, ~2u});
        buffer.Push({3, ~3u});
        const auto previous = buffer.Take();
        Expect(previous.count == 3 && previous.entries[1].tag == 2,
            "bounded buffer lost overflow count or overwritten entries");
        buffer.Push({4, ~4u});
        Expect(previous.entries[0].tag == 1, "producer reused a consumer-owned snapshot slot");
        const auto current = buffer.Copy();
        Expect(current.count == 1 && current.entries[0].tag == 4, "reset retained old entries");
        buffer.PushUnique({4, ~4u}, [](const auto& a, const auto& b) { return a.tag == b.tag; });
        Expect(buffer.Take().count == 1, "unique insertion duplicated a completed event");
    }

    void TestConcurrentCapture()
    {
        dayz::trace::Buffer<TaggedEvent, 64> buffer;
        std::atomic<unsigned> running{2};
        const auto writer = [&](unsigned tag) {
            for (unsigned index = 0; index < 30000; ++index)
                buffer.Push({tag + index, ~(tag + index)});
            --running;
        };
        std::jthread first(writer, 1u);
        std::jthread second(writer, 100000u);
        std::size_t total{};
        while (running.load() != 0)
        {
            const auto snapshot = buffer.Take();
            total += snapshot.count;
            for (std::size_t index = 0; index < (std::min)(snapshot.count, buffer.size()); ++index)
                Expect(snapshot.entries[index].inverse == ~snapshot.entries[index].tag,
                    "snapshot exposed an incomplete record");
        }
        first.join();
        second.join();
        total += buffer.Take().count;
        Expect(total == 60000, "snapshot/reset lost or duplicated event accounting");
    }
}

int main()
{
    try
    {
        TestSnapshotOwnership();
        TestConcurrentCapture();
    }
    catch (const std::exception& error)
    {
        std::cerr << "render_trace_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "render_trace_test: all checks passed\n";
}
