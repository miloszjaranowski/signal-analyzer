#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include "sa/SpscRingBuffer.hpp"

using sa::SpscRingBuffer;

TEST(SpscRingBuffer, CapacityIsRoundedUpToPowerOfTwo) {
    SpscRingBuffer<int> rb(100);
    EXPECT_EQ(rb.capacity(), 128u);
}

TEST(SpscRingBuffer, PushPopPreservesOrder) {
    SpscRingBuffer<int> rb(8);
    const std::vector<int> in{1, 2, 3, 4, 5};
    EXPECT_EQ(rb.push(in), 5u);
    EXPECT_EQ(rb.size(), 5u);

    std::vector<int> out(5);
    EXPECT_EQ(rb.pop(out), 5u);
    EXPECT_EQ(out, in);
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBuffer, PushStopsWhenFull) {
    SpscRingBuffer<int> rb(4);
    const std::vector<int> in{1, 2, 3, 4, 5, 6};
    EXPECT_EQ(rb.push(in), 4u);
    EXPECT_EQ(rb.push(in), 0u);
}

TEST(SpscRingBuffer, WrapsAroundCorrectly) {
    SpscRingBuffer<int> rb(4);
    std::vector<int> out(3);
    for (int round = 0; round < 10; ++round) {
        const std::vector<int> in{round, round + 1, round + 2};
        ASSERT_EQ(rb.push(in), 3u);
        ASSERT_EQ(rb.pop(out), 3u);
        ASSERT_EQ(out, in);
    }
}

// Producer and consumer on separate threads; every value must arrive exactly once,
// in order. Run under ThreadSanitizer in CI to catch memory-ordering bugs.
TEST(SpscRingBuffer, ConcurrentProducerConsumerKeepsSequence) {
    constexpr std::uint64_t kTotal = 2'000'000;
    SpscRingBuffer<std::uint64_t> rb(1024);

    std::thread producer([&] {
        std::vector<std::uint64_t> batch(97);  // odd size exercises wrap-around
        std::uint64_t next = 0;
        while (next < kTotal) {
            const std::size_t n = std::min<std::uint64_t>(batch.size(), kTotal - next);
            std::iota(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(n), next);
            std::size_t sent = 0;
            while (sent < n) {
                sent += rb.push(std::span(batch.data() + sent, n - sent));
            }
            next += n;
        }
    });

    std::vector<std::uint64_t> buf(256);
    std::uint64_t expected = 0;
    bool ordered = true;
    while (expected < kTotal) {
        const std::size_t n = rb.pop(buf);
        for (std::size_t i = 0; i < n; ++i) {
            ordered &= (buf[i] == expected++);
        }
    }
    producer.join();

    EXPECT_TRUE(ordered);
    EXPECT_EQ(expected, kTotal);
    EXPECT_EQ(rb.size(), 0u);
}
