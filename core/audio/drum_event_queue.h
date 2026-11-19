
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

#include "spsc_ring.h"

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

// The ring itself is the generic SpscRing (spsc_ring.h); this alias keeps
// the Engine 020 name and interface.
template <std::size_t Capacity = 256>
using DrumEventQueue = SpscRing<DrumEvent, Capacity>;

} // namespace databreaks
