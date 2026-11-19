
#pragma once

// DATABREAKs Engine 020 - fixed-capacity drum event queue (host-side C++17).
//
// Single-producer / single-consumer, lock-free ring buffer:
//   * exactly ONE thread may call push() (the "control" side, e.g. a future
//     pad-scanning task);
//   * exactly ONE thread may call pop() (the audio side, DrumEngine's
//     processBlock).
// Using more producers or consumers at once is not supported.
//
// No allocation, no locks, no blocking. When the queue is full, push()
// returns false, the event is dropped and droppedCount() increases.
// Usable capacity is Capacity - 1 events (one slot separates full from
// empty).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace databreaks {

enum class DrumEventType : std::uint8_t {
    Trigger,     // pad + velocity
    ReleasePad,  // fade out a pad's voices
    StopPad,     // stop a pad's voices immediately
    StopAll      // stop every voice immediately
};

struct DrumEvent {
    DrumEventType type = DrumEventType::Trigger;
    std::size_t pad = 0;
    int velocity = 0;
    // Frame within the next processBlock() call at which the event applies.
    std::size_t frameOffset = 0;
};

template <std::size_t Capacity = 256>
class DrumEventQueue {
public:
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two and at least 2");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "std::atomic<std::size_t> must be lock-free");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "std::atomic<std::uint32_t> must be lock-free");

    static constexpr std::size_t capacity() { return Capacity - 1; }

    // Producer thread only.
    bool push(const DrumEvent& event) {
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
    bool pop(DrumEvent& event) {
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
    std::array<DrumEvent, Capacity> buffer_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

} // namespace databreaks
