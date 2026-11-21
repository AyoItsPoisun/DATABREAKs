
#pragma once

// DATABREAKs Engine 029 - host InputSource that replays a script.
//
// Stands in for a pad/button/encoder scanner on the host: it returns the
// events of a caller-owned array whose sampleTime is before the current
// "now" (advance() moves now forward, e.g. by one audio block), in order.
// No allocation.

#include "../input.h"

#include <cstddef>
#include <cstdint>

namespace databreaks {

class ScriptedInputSource final : public InputSource {
public:
    // `events` must stay alive and be sorted by sampleTime.
    ScriptedInputSource(const InputEvent* events, std::size_t count) : events_(events), count_(count) {}

    // Events with sampleTime < now become available.
    void advanceTo(std::int64_t now) { now_ = now; }

    std::size_t poll(InputEvent* out, std::size_t max) override {
        std::size_t n = 0;
        while (n < max && next_ < count_ && events_[next_].sampleTime < now_) out[n++] = events_[next_++];
        return n;
    }

    bool finished() const { return next_ == count_; }

private:
    const InputEvent* events_;
    std::size_t count_;
    std::size_t next_ = 0;
    std::int64_t now_ = 0;
};

} // namespace databreaks
