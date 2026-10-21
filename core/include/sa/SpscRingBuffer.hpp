#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <span>
#include <vector>

namespace sa {

// Lock-free single-producer / single-consumer ring buffer.
//
// - Exactly one thread may call push(), exactly one (other) thread may call pop().
// - head_ and tail_ are monotonically increasing counters; the slot index is
//   counter & mask_. Unsigned wrap-around keeps (head_ - tail_) correct.
// - The producer publishes data with a release store on head_, the consumer
//   acquires it; symmetrically for tail_. No locks, no allocation after construction.
// - Counters live on separate cache lines to avoid false sharing.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t minCapacity)
        : capacity_(std::bit_ceil(std::max<std::size_t>(minCapacity, 2))),
          mask_(capacity_ - 1),
          buffer_(capacity_) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer side. Returns how many items were written (less than
    // items.size() when the buffer is full; the caller decides what to do).
    std::size_t push(std::span<const T> items) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t freeSlots = capacity_ - (head - tail);
        const std::size_t n = std::min(freeSlots, items.size());
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[(head + i) & mask_] = items[i];
        }
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Returns how many items were read into `out`.
    std::size_t pop(std::span<T> out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t n = std::min(head - tail, out.size());
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = buffer_[(tail + i) & mask_];
        }
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Approximate when called concurrently; exact when the buffer is idle.
    std::size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const { return capacity_; }

private:
    static constexpr std::size_t kCacheLine = 64;

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> buffer_;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by consumer
};

}  // namespace sa
