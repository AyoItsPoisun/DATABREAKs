
#pragma once

// DATABREAKs Engine 029 - physical input boundary (target abstraction layer).
//
// Pads, buttons and encoders become TARGET-NEUTRAL InputEvents. A target
// adapter produces them (pad-matrix scan, interrupt, USB/MIDI bridge - none
// of which is documented for the FM-1 yet) through an InputSource; the core
// never sees hardware. The InputRouter turns them into what the existing
// engine already understands:
//
//   PadDown (id 0..26, value = velocity 1..127)
//       -> LiveEvent DrumHit for the sequencer (sounds, and is recorded
//          when armed), at the event's sampleTime;
//   PadUp       -> counted only (drum pads are one-shot in the current
//                  engine; nothing else is invented);
//   ButtonDown / ButtonUp (id = generic button index)
//   EncoderDelta (id = generic encoder index, value = signed detents)
//       -> a ControlHandler, if one is installed. NO mapping of FM-1
//          buttons or encoders to functions is assumed here: their number,
//          labels and wiring are unknown.
//
// Timestamps: sampleTime is the absolute audio-clock sample the input
// belongs to (Engine 026 LiveEvent rule; late inputs sound at the next block
// and are not recorded). Producing it is the adapter's job; no latency
// compensation is applied anywhere.
//
// Real-time: no allocation, no locks. QueueInputSource carries events from
// ONE producer (e.g. a scan interrupt) to ONE consumer through the existing
// lock-free SpscRing.

#include "../core/audio/spsc_ring.h"
#include "../core/sequencer/recorder.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

enum class InputEventType : std::uint8_t { PadDown, PadUp, ButtonDown, ButtonUp, EncoderDelta };

struct InputEvent {
    InputEventType type = InputEventType::PadDown;
    std::uint16_t id = 0;       // pad 0..26, or a generic button / encoder index
    std::int16_t value = 0;     // velocity 1..127 (PadDown), detents (EncoderDelta)
    std::int64_t sampleTime = 0;
};

class InputSource {
public:
    virtual ~InputSource() = default;
    // Copies up to `max` pending events into `out`; returns how many.
    virtual std::size_t poll(InputEvent* out, std::size_t max) = 0;
};

class ControlHandler {
public:
    virtual ~ControlHandler() = default;
    virtual void onButton(std::uint16_t id, bool down, std::int64_t sampleTime) = 0;
    virtual void onEncoder(std::uint16_t id, int delta, std::int64_t sampleTime) = 0;
};

// Lock-free single-producer / single-consumer input queue (e.g. a pad-scan
// interrupt pushes, the audio or control thread polls).
template <std::size_t Capacity>
class QueueInputSource final : public InputSource {
public:
    bool push(const InputEvent& e) { return ring_.push(e); }  // producer only
    std::size_t poll(InputEvent* out, std::size_t max) override {  // consumer only
        std::size_t n = 0;
        while (n < max && ring_.pop(out[n])) ++n;
        return n;
    }
    std::uint32_t droppedCount() const { return ring_.droppedCount(); }

private:
    SpscRing<InputEvent, Capacity> ring_;
};

struct InputStats {
    std::uint64_t padHits = 0;
    std::uint64_t padReleases = 0;
    std::uint64_t controls = 0;      // buttons + encoders delivered to the handler
    std::uint64_t unhandled = 0;     // buttons/encoders with no handler installed
    std::uint64_t invalid = 0;       // bad pad id, velocity or event type
    std::uint64_t rejected = 0;      // the live sink refused (queue full)
};

// Routes InputEvents. `LiveSink` is any callable bool(const LiveEvent&):
// e.g. [&](const LiveEvent& e) { return sequencer.liveEvent(e); } on the
// audio thread, or sequencer.postLive(e) from a control thread.
template <typename LiveSink>
class InputRouter {
public:
    static constexpr std::size_t PollBatch = 32;

    explicit InputRouter(LiveSink sink, ControlHandler* handler = nullptr)
        : sink_(sink), handler_(handler) {}

    void setControlHandler(ControlHandler* handler) { handler_ = handler; }

    // Returns true if the event was valid and delivered.
    bool dispatch(const InputEvent& e) {
        switch (e.type) {
        case InputEventType::PadDown: {
            if (e.id >= 27 || e.value < 1 || e.value > 127 || e.sampleTime < 0) {
                ++stats_.invalid;
                return false;
            }
            LiveEvent live;
            live.type = LiveEventType::DrumHit;
            live.value = e.id;
            live.velocity = e.value;
            live.sampleTime = e.sampleTime;
            if (!sink_(live)) {
                ++stats_.rejected;
                return false;
            }
            ++stats_.padHits;
            return true;
        }
        case InputEventType::PadUp:
            if (e.id >= 27) {
                ++stats_.invalid;
                return false;
            }
            ++stats_.padReleases;
            return true;
        case InputEventType::ButtonDown:
        case InputEventType::ButtonUp:
            if (handler_ == nullptr) {
                ++stats_.unhandled;
                return false;
            }
            handler_->onButton(e.id, e.type == InputEventType::ButtonDown, e.sampleTime);
            ++stats_.controls;
            return true;
        case InputEventType::EncoderDelta:
            if (handler_ == nullptr) {
                ++stats_.unhandled;
                return false;
            }
            handler_->onEncoder(e.id, e.value, e.sampleTime);
            ++stats_.controls;
            return true;
        }
        ++stats_.invalid;
        return false;
    }

    // Polls `source` (in batches of PollBatch, at most maxEvents) and
    // dispatches everything; returns the number of events polled.
    std::size_t pump(InputSource& source, std::size_t maxEvents = 256) {
        std::size_t total = 0;
        while (total < maxEvents) {
            const std::size_t want = maxEvents - total < PollBatch ? maxEvents - total : PollBatch;
            const std::size_t n = source.poll(batch_.data(), want);
            for (std::size_t i = 0; i < n; ++i) dispatch(batch_[i]);
            total += n;
            if (n < want) break;
        }
        return total;
    }

    const InputStats& stats() const { return stats_; }

private:
    LiveSink sink_;
    ControlHandler* handler_;
    InputStats stats_{};
    std::array<InputEvent, PollBatch> batch_{};
};

} // namespace databreaks
