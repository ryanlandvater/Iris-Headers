// TypeTests_1.3 (development/MODERNIZATION.md, SafetyNet_1): today's behaviour
// of the types consumers rely on, recorded before Modernize_4 rewrites them, so
// each change there is a visible, deliberate edit to an expectation here.
//
// Not tested, because they cannot be called: IrisCore.hpp declares
// Buffer_change_strength, which nothing defines, and Buffer_write_into_buffer
// with a signature src/IrisBuffer.cpp does not define (it defines a
// (Buffer&, size_t&) overload). Both are fixed in RuntimeLibraries_4.5.
#include <IrisCore.hpp>
#include <IrisAsync.hpp>
#include <IrisMemory.hpp>
#include <IrisQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

TEST_CASE("Result: an unassigned Result fails", "[Result]") {
    const Iris::Result unassigned;
    CHECK(unassigned.failed());
    CHECK_FALSE(static_cast<bool>(unassigned));
}

TEST_CASE("Result: a warning succeeds and converts to true; a failure does not", "[Result]") {
    const Iris::Result success{Iris::IRIS_SUCCESS};
    const Iris::Result warning{Iris::IRIS_WARNING_VALIDATION, "warned"};
    const Iris::Result failure{Iris::IRIS_VALIDATION_FAILURE, "bad value: 7"};
    CHECK(success.succeeded());
    CHECK_FALSE(success.is_warning());
    CHECK(warning.succeeded());
    CHECK(warning.is_warning());
    // ResultShapeQuestion_3 removes this conversion; this line changes then.
    CHECK(static_cast<bool>(warning));
    CHECK(failure.failed());
    CHECK(failure.message == "bad value: 7");
}

TEST_CASE("Buffer: a strong copy owns its bytes; a weak wrap shares them", "[Buffer]") {
    std::array<char, 4> source{'a', 'b', 'c', 'd'};
    const Iris::Buffer strong = Iris::Copy_strong_buffer_from_data(source.data(), source.size());
    const Iris::Buffer weak = Iris::Wrap_weak_buffer_fom_data(source.data(), source.size());
    source[0] = 'z';

    void* data = nullptr;
    size_t bytes = 0;
    REQUIRE(Iris::Buffer_get_data(strong, data, bytes).succeeded());
    CHECK(bytes == 4);
    CHECK(std::memcmp(data, "abcd", 4) == 0);

    REQUIRE(Iris::Buffer_get_data(weak, data, bytes).succeeded());
    CHECK(data == source.data());
    CHECK(static_cast<const char*>(data)[0] == 'z');
}

TEST_CASE("Buffer: a strong buffer created with a size reports it empty", "[Buffer]") {
    const Iris::Buffer buffer = Iris::Create_strong_buffer(64);
    void* data = nullptr;
    size_t bytes = 1;
    REQUIRE(Iris::Buffer_get_data(buffer, data, bytes).succeeded());
    CHECK(bytes == 0);
}

// Consumers pop through an iterator from begin(), as IrisAsync.cpp does.
TEST_CASE("FIFO2 queue: first in, first out; popping an empty queue returns false", "[Queue]") {
    Iris::FIFO2::Queue<int> queue;
    int value = -1;
    CHECK_FALSE(queue.begin().pop(value));
    for (int pushed = 0; pushed < 3; ++pushed) queue.push(pushed);
    Iris::FIFO2::Iterator<int> reader = queue.begin();
    for (int expected = 0; expected < 3; ++expected) {
        REQUIRE(reader.pop(value));
        CHECK(value == expected);
    }
    CHECK_FALSE(reader.pop(value));
}

// An iterator follows the node chain as it pops, which is how IrisAsync's
// workers read their task queue: each takes begin() before any task is pushed.
TEST_CASE("FIFO2 queue: an iterator taken first sees every entry from several producers, across nodes", "[Queue]") {
    Iris::FIFO2::Queue<int> queue;
    Iris::FIFO2::Iterator<int> reader = queue.begin();
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1000;  // 4000 entries: two nodes of 2048
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&queue, producer] {
            for (int index = 0; index < kPerProducer; ++index) queue.push(producer * kPerProducer + index);
        });
    }
    for (std::thread& producer : producers) producer.join();
    std::vector<int> seen(kProducers * kPerProducer, 0);
    int value = -1;
    while (reader.pop(value)) ++seen.at(static_cast<size_t>(value));
    for (const int count : seen) REQUIRE(count == 1);
}

// Known defect, found 2026-11-12: begin() returns an iterator at the newest
// node, so an iterator taken after a backlog longer than one node (2048
// entries) never sees the earlier nodes. Pushing 4096 then reading pops only
// 2048..4095. This test states the correct behaviour and is expected to fail
// ([!shouldfail]) until IteratorFromOldestNode_4.8 fixes begin(); it then
// passes, Catch2 reports the [!shouldfail] test as failed, and the tag comes off.
TEST_CASE("FIFO2 queue: begin() after a backlog longer than one node sees every entry", "[Queue][!shouldfail]") {
    Iris::FIFO2::Queue<int> queue;
    for (int pushed = 0; pushed < 4096; ++pushed) queue.push(pushed);
    Iris::FIFO2::Iterator<int> reader = queue.begin();
    int value = -1;
    REQUIRE(reader.pop(value));
    CHECK(value == 0);
}

TEST_CASE("Async pool: every issued task runs before wait_until_complete returns", "[Async]") {
    const Iris::Async::ThreadPool pool = Iris::Async::createThreadPool(4);
    std::atomic<int> ran{0};
    for (int task = 0; task < 100; ++task) pool->issue_task([&ran] { ++ran; });
    pool->wait_until_complete();
    CHECK(ran.load() == 100);
}

// Found by CI on Windows (2026-11-12): a worker that had not reached its loop
// when wait_until_complete switched the pool to draining never ran anything,
// so the call could return with every task unrun (0 of 100 there). Many short
// rounds make the race show on any platform.
TEST_CASE("Async pool: wait_until_complete runs every task even when workers start late", "[Async]") {
    for (int round = 0; round < 300; ++round) {
        std::atomic<int> ran{0};
        const Iris::Async::ThreadPool pool = Iris::Async::createThreadPool(4);
        for (int task = 0; task < 20; ++task) pool->issue_task([&ran] { ++ran; });
        pool->wait_until_complete();
        REQUIRE(ran.load() == 20);
        REQUIRE(pool->pending_tasks() == 0);
    }
}

TEST_CASE("Async pool: a fence signals when its task has run", "[Async]") {
    const Iris::Async::ThreadPool pool = Iris::Async::createThreadPool(2);
    std::atomic<bool> ran{false};
    const Iris::Async::Fence fence = pool->issue_task_with_fence([&ran] { ran = true; });
    fence->wait_on_signal();
    CHECK(ran.load());
    CHECK(fence->complete.load());
}

TEST_CASE("MemoryArena: an anonymous arena reserves its capacity and commits writable pages", "[Memory]") {
    Iris::Memory arena;
    Iris::MemoryCreateInfo info;
    info.capacity = 1u << 20;
    REQUIRE(Iris::create_memory(info, arena).succeeded());
    REQUIRE(static_cast<bool>(arena));
    CHECK(arena.capacity() >= info.capacity);
    arena.commit(0, 4096);
    arena.base()[0] = 0x5A;
    arena.base()[4095] = 0xA5;
    CHECK(arena.base()[0] == 0x5A);
    CHECK(arena.base()[4095] == 0xA5);
}
