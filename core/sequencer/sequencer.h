
#pragma once

// DATABREAKs Engine 025 - pattern sequencer (host-side C++17).
//
// Plays one Pattern against a MusicalClock and turns it into events at
// exact sample offsets inside each rendered block. AudioPipeline calls
// process() once per mixSources() block, on the audio thread, and delivers
// the events through the instruments' scheduled-event paths
// (DrumEngine::scheduleTrigger, AudioPipeline::scheduleNote*), which apply
// them at those exact frames. Results therefore do not depend on the block
// size (tested for 1..4096).
//
// Per block [s0, s0 + n):
//   1. queued transport commands (postTransport) are applied, in order;
//   2. after stop()/reset(): note-offs for every sounding sequenced note at
//      offset 0;
//   3. while playing, every tick t with sampleOfTick(t) in [s0, s0 + n) is
//      covered:
//        drum step k of loop L on pad p fires at tick
//          L x lengthTicks + k x stepTicks + microTicks;
//        note i of loop L starts at L x lengthTicks + startTick and ends
//          lengthTicks later (also in later loops or after the pattern
//          wraps);
//   4. events are sorted by (sample offset, kind, target, pad/note): at the
//      same sample, note-offs come first (so a note can be re-struck), then
//      drum triggers in pad order 0..26, then note-ons by target
//      (mono, FM, poly) and note number;
//   5. the clock advances by n samples.
//
// Probability (0..100 %): decided by a hash of (seed, loop, lane, step),
// independent of block size and evaluation order. 0 never fires, 100
// always fires. A skipped note-on also skips its note-off.
//
// Capacity per block: MaxEventsPerBlock drum triggers + note-ons (extra
// ones are dropped and counted) plus up to MaxActiveNotes note-offs, which
// are never dropped. At most MaxActiveNotes sequenced notes can sound at
// once; a note-on beyond that is dropped and counted.
//
// Threading:
//   * process(), configuration (pattern(), clock(), setSeed) and the direct
//     transport calls (start/stop/reset) are AUDIO THREAD ONLY, or while
//     audio is not running;
//   * postTransport() may be called from ONE other thread (SPSC queue,
//     TransportCapacity commands, applied at the start of the next block;
//     a full queue rejects and counts the command).
// No allocation, locks or I/O anywhere.
//
// Host-side implementation; not validated on FM-1 hardware.

#include "musical_clock.h"
#include "pattern.h"
#include "../audio/spsc_ring.h"
#include "../audio/synth_event.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

struct SequencerEvent {
    enum class Kind : std::uint8_t { NoteOff = 0, Drum = 1, NoteOn = 2 };
    std::size_t offset = 0;                 // frame within the block
    Kind kind = Kind::Drum;
    SynthTarget target = SynthTarget::Mono; // notes only
    int value = 0;                          // pad (drums) or note number
    int velocity = 0;
};

enum class TransportCommandType : std::uint8_t { Start, Stop, Reset, SetTempo };

struct TransportCommand {
    TransportCommandType type = TransportCommandType::Start;
    std::int64_t milliBpm = 120000;  // SetTempo only
};

class Sequencer {
public:
    static constexpr std::size_t MaxEventsPerBlock = 1024;
    static constexpr std::size_t MaxActiveNotes = 256;
    static constexpr std::size_t TransportCapacity = 64;  // usable: 63

    explicit Sequencer(int sampleRate = 48000) : clock_(sampleRate) {}

    MusicalClock& clock() { return clock_; }
    const MusicalClock& clock() const { return clock_; }
    Pattern& pattern() { return pattern_; }
    const Pattern& pattern() const { return pattern_; }

    // Probability seed (default 1). Same seed + same pattern = same result.
    void setSeed(std::uint64_t seed) { seed_ = seed; }
    std::uint64_t seed() const { return seed_; }

    // ---- Transport (audio thread) ------------------------------------------

    void start() { clock_.start(); }

    // Stops; sounding sequenced notes get note-offs at the next block.
    void stop() {
        clock_.stop();
        flushNotes_ = true;
    }

    // Back to tick 0 (keeps play state); sounding notes are released.
    void reset() {
        clock_.reset();
        flushNotes_ = true;
    }

    bool playing() const { return clock_.playing(); }

    // ---- Transport (one producer thread) -------------------------------------

    bool postTransport(const TransportCommand& command) {
        return transport_.push(command);
    }

    std::uint32_t droppedTransportCount() const { return transport_.droppedCount(); }

    // ---- Audio thread ----------------------------------------------------------

    // Generates this block's events and passes each to `sink`
    // (void(const SequencerEvent&)) in delivery order, then advances the
    // clock by `frames`.
    template <typename Sink>
    void process(std::size_t frames, Sink&& sink) {
        applyTransportCommands();
        count_ = 0;
        generated_ = 0;

        if (flushNotes_) {
            for (std::size_t i = 0; i < activeCount_; ++i) {
                push({0, SequencerEvent::Kind::NoteOff, active_[i].target,
                      active_[i].note, 0}, true);
            }
            activeCount_ = 0;
            flushNotes_ = false;
        }

        if (clock_.playing() && frames > 0) {
            generate(static_cast<std::int64_t>(frames));
        }

        std::sort(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(count_),
                  [](const SequencerEvent& a, const SequencerEvent& b) {
                      if (a.offset != b.offset) return a.offset < b.offset;
                      if (a.kind != b.kind) return a.kind < b.kind;
                      if (a.target != b.target) return a.target < b.target;
                      return a.value < b.value;
                  });
        for (std::size_t i = 0; i < count_; ++i) {
            sink(events_[i]);
        }
        delivered_ += count_;
        clock_.advance(static_cast<std::int64_t>(frames));
    }

    // ---- Statistics (audio thread) ------------------------------------------------

    std::uint64_t deliveredEventCount() const { return delivered_; }
    std::uint64_t droppedEventCount() const { return dropped_; }
    std::uint64_t skippedByProbabilityCount() const { return skipped_; }
    std::size_t activeNoteCount() const { return activeCount_; }

    // Deterministic probability decision (exposed for tests).
    bool chance(std::int64_t loop, std::uint64_t lane, std::uint64_t index,
                int probability) const {
        if (probability >= 100) return true;
        if (probability <= 0) return false;
        const std::uint64_t key = mix(static_cast<std::uint64_t>(loop) * 0x9E3779B97F4A7C15ull ^
                                      mix(lane * 0xBF58476D1CE4E5B9ull + index));
        return mix(seed_ ^ key) % 100 < static_cast<std::uint64_t>(probability);
    }

private:
    struct ActiveNote {
        SynthTarget target;
        int note;
        std::int64_t offTick;
    };

    MusicalClock clock_;
    Pattern pattern_;
    std::uint64_t seed_ = 1;
    SpscRing<TransportCommand, TransportCapacity> transport_;

    // Worst case per block: MaxActiveNotes flushed note-offs (after stop or
    // reset), MaxEventsPerBlock triggers/note-ons, and MaxActiveNotes
    // note-offs for notes that end inside the block.
    std::array<SequencerEvent, MaxEventsPerBlock + 2 * MaxActiveNotes> events_{};
    std::size_t count_ = 0;
    std::size_t generated_ = 0;  // drum + note-on events this block
    std::array<ActiveNote, MaxActiveNotes> active_{};
    std::size_t activeCount_ = 0;
    bool flushNotes_ = false;

    std::uint64_t delivered_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t skipped_ = 0;

    static std::uint64_t mix(std::uint64_t x) {  // splitmix64 finaliser
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return x;
    }

    static std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
        std::int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }

    static std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
        return -floorDiv(-a, b);
    }

    // `offs` events use the reserved space; others respect the limit.
    bool push(const SequencerEvent& e, bool noteOff) {
        if (!noteOff) {
            if (generated_ == MaxEventsPerBlock) {
                ++dropped_;
                return false;
            }
            ++generated_;
        }
        events_[count_++] = e;
        return true;
    }

    void applyTransportCommands() {
        TransportCommand c;
        for (std::size_t i = 0; i < TransportCapacity && transport_.pop(c); ++i) {
            switch (c.type) {
            case TransportCommandType::Start: start(); break;
            case TransportCommandType::Stop: stop(); break;
            case TransportCommandType::Reset: reset(); break;
            case TransportCommandType::SetTempo: clock_.setTempoMilli(c.milliBpm); break;
            }
        }
    }

    std::size_t offsetOf(std::int64_t tick, std::int64_t s0) const {
        const std::int64_t s = clock_.sampleOfTick(tick) - s0;
        return static_cast<std::size_t>(s < 0 ? 0 : s);
    }

    void generate(std::int64_t frames) {
        const std::int64_t s0 = clock_.samplePosition();
        const std::int64_t tFirst = clock_.firstTickAtOrAfter(s0);
        const std::int64_t tEnd = clock_.firstTickAtOrAfter(s0 + frames);
        // Ticks in [tFirst, tEnd) are delivered inside this block.

        const std::int64_t length = pattern_.lengthTicks();
        const std::int64_t stepTicks = pattern_.stepTicks();
        const std::int64_t steps = static_cast<std::int64_t>(pattern_.stepCount());
        const std::int64_t maxMicro = stepTicks / 2;

        if (tEnd > tFirst) {
            // Drum steps.
            const std::int64_t loopFirst = floorDiv(tFirst - maxMicro, length);
            const std::int64_t loopLast = floorDiv(tEnd - 1 + maxMicro, length);
            for (std::int64_t loop = loopFirst; loop <= loopLast; ++loop) {
                const std::int64_t base = loop * length;
                const std::int64_t kMin = std::max<std::int64_t>(
                    0, ceilDiv(tFirst - maxMicro - base, stepTicks));
                const std::int64_t kMax = std::min<std::int64_t>(
                    steps - 1, floorDiv(tEnd - 1 + maxMicro - base, stepTicks));
                for (std::int64_t k = kMin; k <= kMax; ++k) {
                    for (std::size_t pad = 0; pad < Pattern::Lanes; ++pad) {
                        const DrumStep& st =
                            *pattern_.step(pad, static_cast<std::size_t>(k));
                        if (!st.enabled) continue;
                        const std::int64_t tick = base + k * stepTicks + st.microTicks;
                        if (tick < tFirst || tick >= tEnd || tick < 0) continue;
                        if (!chance(loop, pad, static_cast<std::uint64_t>(k),
                                    st.probability)) {
                            ++skipped_;
                            continue;
                        }
                        push({offsetOf(tick, s0), SequencerEvent::Kind::Drum,
                              SynthTarget::Mono, static_cast<int>(pad), st.velocity},
                             false);
                    }
                }
            }

            // Note-ons.
            const std::int64_t nLoopFirst = floorDiv(tFirst, length);
            const std::int64_t nLoopLast = floorDiv(tEnd - 1, length);
            for (std::int64_t loop = nLoopFirst; loop <= nLoopLast; ++loop) {
                for (std::size_t i = 0; i < pattern_.noteCount(); ++i) {
                    const NoteEvent& n = *pattern_.note(i);
                    if (!n.enabled) continue;
                    const std::int64_t tick = loop * length + n.startTick;
                    if (tick < tFirst || tick >= tEnd) continue;
                    if (!chance(loop, 1000 + i, 0, n.probability)) {
                        ++skipped_;
                        continue;
                    }
                    if (activeCount_ == MaxActiveNotes) {
                        ++dropped_;
                        continue;
                    }
                    if (!push({offsetOf(tick, s0), SequencerEvent::Kind::NoteOn,
                               n.target, n.note, n.velocity}, false)) {
                        continue;
                    }
                    active_[activeCount_++] = {n.target, n.note, tick + n.lengthTicks};
                }
            }
        }

        // Note-offs due in this block (including notes started above).
        std::size_t i = 0;
        while (i < activeCount_) {
            if (active_[i].offTick < tEnd) {
                push({offsetOf(active_[i].offTick, s0), SequencerEvent::Kind::NoteOff,
                      active_[i].target, active_[i].note, 0}, true);
                active_[i] = active_[--activeCount_];
            } else {
                ++i;
            }
        }
    }
};

} // namespace databreaks
