
#pragma once

// DATABREAKs - fixed-capacity single-producer / single-consumer ring buffer
// (host-side C++17). Introduced in Engine 020 as DrumEventQueue; made
// generic in Engine 022 so synth note events can use the same tested code.
//
//   * exactly ONE thread may call push() (the producer);
//   * exactly ONE thread may call pop() (the consumer, e.g. the audio thread).
// Using more producers or consumers at once is not supported.
//
// No allocation, no locks, no blocking. When the ring is full, push()
// returns false, the item is dropped and droppedCount() increases.
// Usable capacity is Capacity - 1 items (one slot separates full from
// empty). T must be trivially copyable in spirit (copied by assignment).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace databreaks {

template <typename T, std::size_t Capacity>
class SpscRing {
public:
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two and at least 2");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "std::atomic<std::size_t> must be lock-free");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "std::atomic<std::uint32_t> must be lock-free");

    static constexpr std::size_t capacity() { return Capacity - 1; }

    // Producer thread only.
    bool push(const T& event) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & (Capacity - 1);

        if (next == head_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buffer_[tail] = event;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool pop(T& event) {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        event = buffer_[head];
        head_.store((head + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    // Approximate when called while the other side is active.
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // Events rejected because the queue was full (wraps after 2^32).
    std::uint32_t droppedCount() const {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::array<T, Capacity> buffer_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

} // namespace databreaks
