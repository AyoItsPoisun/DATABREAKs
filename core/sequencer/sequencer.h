
#pragma once

// DATABREAKs Engine 025/026/027 - pattern sequencer, performance recorder
// and non-destructive quantization (host-side C++17).
//
// Plays one Pattern (Engine 025) and one RecordedTrack (Engine 026)
// against a MusicalClock and turns them into events at exact sample offsets
// inside each rendered block. AudioPipeline calls process() once per
// mixSources() block, on the audio thread, and delivers the events through
// the instruments' scheduled-event paths (DrumEngine::scheduleTrigger,
// AudioPipeline::scheduleNote*), which apply them at those exact frames.
// Results therefore do not depend on the block size (tested for 1..4096).
//
// Per block [s0, s0 + n):
//   1. queued transport commands (postTransport), then queued sequencer
//      commands (postCommand: recorder control, pattern edits), then queued
//      live input (postLive) are taken in, in order;
//   2. after stop()/reset(): note-offs for every sounding sequenced note at
//      offset 0;
//   3. while playing:
//      a. live inputs due in this block are processed in time order; the
//         recorder opens/closes passes and erases (Replace) as its head
//         passes, and records inputs inside the record region (Engine 026);
//      b. every tick t with sampleOfTick(t) in [s0, s0 + n) is covered:
//           drum step k of loop L on pad p fires at tick
//             L x lengthTicks + k x stepTicks + microTicks;
//           note i of loop L starts at L x lengthTicks + startTick and ends
//             lengthTicks later;
//      c. recorded events (visible ones) fire at occurrence k at
//         k x lengthFine + q, where q is the played start: the raw startFine,
//         or its quantized position (Engine 027, quantize.h; q in
//         [0, lengthFine], q == lengthFine is the next loop's start). The
//         occurrence k == the capture loop (same timeline epoch) is skipped:
//         the live input already sounded there;
//      d. live inputs sound at their own samples;
//   4. events are sorted by (sample offset, kind, target, pad/note, order):
//      at the same sample note-offs come first (so a note can be re-struck),
//      then drum triggers in pad order 0..26, then note-ons by target
//      (mono, FM, poly) and note number; `order` keeps pattern events, then
//      recorded events in capture order, then live inputs in arrival order;
//   5. the clock advances by n samples.
//
// Probability (Engine 025): a hash of (seed, loop, lane, step). Recorded
// events have no probability (they always play).
//
// Capacity per block: MaxEventsPerBlock drum triggers + note-ons from the
// pattern and the recorded track (extra ones are dropped and counted);
// note-offs and live inputs use reserved space and are never dropped here.
// At most MaxActiveNotes sequenced/recorded notes sound at once.
//
// Threading (ownership model, Engine 026):
//   * The audio thread OWNS the Pattern, the RecordedTrack and the
//     recorder state. Only process() and the direct calls below touch them.
//   * Another thread never mutates them: it sends commands. postTransport()
//     (start/stop/reset/tempo), postCommand() (arm, disarm, modes, punch,
//     undo, redo, clear, quantize settings, pattern edits) and postLive() (live input) each
//     use their own SPSC queue: ONE producer thread per queue (the same
//     thread may feed all three). They are applied at the start of the next
//     block. A full queue rejects the item and counts it.
//   * Direct calls (pattern(), clock(), recorder(), arm(), ...) are AUDIO
//     THREAD ONLY, or while audio is not running.
// No allocation, locks or I/O anywhere.
//
// Host-side implementation; not validated on FM-1 hardware.

#include "arrangement.h"
#include "musical_clock.h"
#include "pattern.h"
#include "pattern_bank.h"
#include "recorder.h"
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
    std::uint64_t order = 0;                // final tie-break (Engine 026)
};

enum class TransportCommandType : std::uint8_t { Start, Stop, Reset, SetTempo };

// When a pattern switch requested while playing takes effect (Engine 028):
// at the END of the current beat / bar / loop of the CURRENT pattern
// (counted from that pattern's start), or at the next tick (Immediate).
// While stopped every switch is immediate.
enum class SwitchTiming : std::uint8_t { Immediate, NextBeat, NextBar, NextLoop };

struct TransportCommand {
    TransportCommandType type = TransportCommandType::Start;
    std::int64_t milliBpm = 120000;  // SetTempo only
};

// Capacities (Engine 032): BasicSequencer<Profile> owns the profile's
// working Pattern, Recorder (and RecordedTrack), pattern bank, per-block
// event buffer, active-note table and quantize cache. `Sequencer` is the
// host engine: BasicSequencer<HostProfile>. Behaviour is identical for every
// profile; only the capacities (and so the limits that drop and count)
// differ.
template <typename Profile>
class BasicSequencer {
public:
    using ProfileType = Profile;
    using Pattern = BasicPattern<Profile>;
    using RecordedTrack = BasicRecordedTrack<Profile>;
    using Recorder = BasicRecorder<Profile>;
    using Bank = ProfilePatternBank<Profile>;
    using PatternData = BasicPatternData<Profile>;
    using SequencerCommand = BasicSequencerCommand<Profile>;

    static constexpr std::size_t MaxEventsPerBlock = Profile::MaxEventsPerBlock;
    static constexpr std::size_t MaxActiveNotes = Profile::MaxActiveNotes;
    static constexpr std::size_t TransportCapacity = 64;  // usable: 63
    static constexpr std::size_t CommandCapacity = 64;    // usable: 63
    static constexpr std::size_t LiveCapacity = 256;      // usable: 255
    static constexpr std::int64_t Fine = MusicalClock::FinePerTick;

    static constexpr std::size_t MaxTransitionsPerBlock = 64;

    // Entries of the per-block event buffer (see EventBufferSize below);
    // read by the resource model (core/target/resource_model.h, Engine 031).
    static constexpr std::size_t eventBufferCapacity() { return EventBufferSize; }

    explicit BasicSequencer(int sampleRate = 48000) : clock_(sampleRate) {
        bank_.store(0, pattern_, recorder_.track_, quantize_);  // pattern 0 is active
    }

    MusicalClock& clock() { return clock_; }
    const MusicalClock& clock() const { return clock_; }
    Pattern& pattern() { return pattern_; }
    const Pattern& pattern() const { return pattern_; }
    Recorder& recorder() { return recorder_; }
    const Recorder& recorder() const { return recorder_; }

    // Probability seed (default 1). Same seed + same pattern = same result.
    void setSeed(std::uint64_t seed) { seed_ = seed; }
    std::uint64_t seed() const { return seed_; }

    // ---- Transport (audio thread) ------------------------------------------

    void start() { clock_.start(); }

    // Stops; sounding sequenced notes get note-offs at the next block. An
    // open recording pass is closed and held recorded notes end here.
    void stop() {
        finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
        clock_.stop();
        flushNotes_ = true;
    }

    // Back to tick 0 (keeps play state); sounding notes are released, an
    // open recording pass is closed, and a new timeline epoch begins.
    // Engine 028: the active pattern restarts at tick 0; a pending manual
    // switch is cancelled; a running arrangement restarts at entry 0.
    void reset() {
        finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
        clock_.reset();
        origin_ = 0;
        ++epoch_;
        if (recorder_.armed_) {
            recorder_.armFine_ = 0;
        }
        flushNotes_ = true;
        pending_ = Pending{};
        if (songActive_) beginSong();
    }

    bool playing() const { return clock_.playing(); }
    std::uint32_t timelineEpoch() const { return epoch_; }

    // ---- Recorder control (audio thread) -------------------------------------

    // Arms recording from the current position (the start of the next
    // block); with a punch window, recording happens only inside it.
    void arm() {
        recorder_.armed_ = true;
        recorder_.armFine_ = std::max<std::int64_t>(0, currentFine() - origin_ * Fine);
        recorder_.openPass_ = 0;
    }

    // Stops recording: closes the open pass; held notes end here.
    void disarm() {
        finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
        recorder_.armed_ = false;
    }

    // Undo the most recent recording pass (closing it first if it is
    // open). Returns false if there is nothing to undo.
    bool undo() {
        recorder_.openPass_ = 0;
        const std::uint32_t id = recorder_.track_.undo();
        if (id == 0) return false;
        // Notes of the undone pass that are still held are forgotten.
        std::size_t out = 0;
        for (std::size_t i = 0; i < recorder_.heldCount_; ++i) {
            if (recorder_.held_[i].pass != id) recorder_.held_[out++] = recorder_.held_[i];
        }
        recorder_.heldCount_ = out;
        return true;
    }

    bool redo() { return recorder_.track_.redo() != 0; }

    // Removes all recorded material and undo history.
    void clearRecording() {
        recorder_.track_.clear();
        recorder_.heldCount_ = 0;
        recorder_.openPass_ = 0;
    }

    // ---- Quantization (Engine 027; audio thread, or via postCommand) ---------

    // Non-destructive: only playback changes; the recorded track is never
    // modified. Invalid settings are rejected (false) and change nothing.
    bool setQuantize(const QuantizeSettings& settings) {
        if (!settings.valid()) return false;
        if (settings != quantize_) {
            quantize_ = settings;
            quantCacheValid_ = false;
        }
        return true;
    }
    const QuantizeSettings& quantizeSettings() const { return quantize_; }

    // Played start (loop-relative, in [0, loop length]) and length of
    // recorded event i under the current settings and pattern length.
    std::int64_t playedStartFine(std::size_t i) {
        const RecordedEvent& e = recorder_.track_.at(i);
        if (!quantize_.active()) return e.startFine;
        refreshQuantCache(pattern_.lengthTicks() * Fine);
        return e.startFine + quantStart_[i];
    }
    std::int64_t playedLengthFine(std::size_t i) {
        const RecordedEvent& e = recorder_.track_.at(i);
        if (!quantize_.active()) return e.lengthFine;
        refreshQuantCache(pattern_.lengthTicks() * Fine);
        return e.lengthFine + quantLength_[i];
    }

    // Times the derived cache was recomputed (diagnostics / benchmark).
    std::uint64_t quantizeRefreshCount() const { return quantRefreshes_; }
    // Invalid commands rejected by the command queue (SetQuantize, SetPunch).
    std::uint64_t invalidCommandCount() const { return invalidCommands_; }

    // ---- Pattern bank and switching (Engine 028; audio thread, or via
    //      postCommand) -----------------------------------------------------
    //
    // The working pattern (pattern(), recorder().track(), quantizeSettings())
    // IS the active bank slot; the bank's copy of that slot is refreshed
    // whenever the sequencer switches away from it (or commitActivePattern()).

    const Bank& bank() const { return bank_; }
    const Arrangement& arrangement() const { return arrangement_; }
    int activePattern() const { return activePattern_; }
    std::int64_t patternOriginTick() const { return origin_; }
    int pendingPattern() const {
        return pending_.active && pending_.action == Pending::Switch ? pending_.target : -1;
    }
    std::int64_t pendingTransitionTick() const { return pending_.active ? pending_.tick : -1; }

    // Requests a switch to pattern `id`. Stopped: immediate (the pattern
    // starts at the current position). Playing: at the end of the current
    // beat / bar / loop of the current pattern (the unit containing the next
    // undelivered tick, so a unit that is about to start still plays
    // completely), sample-exact; Immediate = the next undelivered tick. Several requests: the LAST one wins (it
    // replaces any pending switch). Requesting the active pattern cancels a
    // pending switch and does not restart the pattern. A manual request
    // stops song mode. Returns false for an invalid id (or a rejected
    // immediate switch, see bankFullCount()).
    bool requestPatternSwitch(int id, SwitchTiming timing = SwitchTiming::NextLoop) {
        if (!Bank::validId(id)) {
            ++invalidCommands_;
            return false;
        }
        songActive_ = false;
        if (id == activePattern_) {
            pending_ = Pending{};
            return true;
        }
        if (!clock_.playing()) {
            pending_ = Pending{};
            return switchTo(id, clock_.firstTickAtOrAfter(clock_.samplePosition()));
        }
        pending_ = Pending{true, Pending::Switch, id, boundaryTick(timing), false, 0};
        return true;
    }

    void cancelPatternSwitch() {
        if (pending_.active && !pending_.song) pending_ = Pending{};
    }

    // Saves the working pattern into its bank slot (e.g. before saving to
    // a file). False if the pools cannot hold it.
    bool commitActivePattern() {
        return bank_.store(activePattern_, pattern_, recorder_.track_, quantize_);
    }

    // Removes steps, notes, recording and undo history; keeps the shape and
    // quantize settings. The active pattern is cleared in place.
    bool clearPattern(int id) {
        if (!Bank::validId(id)) return false;
        if (id == activePattern_) {
            finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
            pattern_.clear();
            recorder_.track_.clear();
        }
        return bank_.clearContents(id);
    }

    // dst := src, including recording, undo history, quantize and shape.
    bool copyPattern(int src, int dst) {
        if (!Bank::validId(src) || !Bank::validId(dst)) return false;
        if (src == dst) return true;
        if (src == activePattern_ && !commitActivePattern()) return false;
        if (!bank_.copy(src, dst)) return false;
        if (dst == activePattern_) reloadActive();
        return true;
    }

    // Copies src into the lowest empty slot; returns the new id or -1.
    int duplicatePattern(int src) {
        if (!Bank::validId(src)) return -1;
        if (src == activePattern_ && !commitActivePattern()) return -1;
        return lastDuplicate_ = bank_.duplicate(src);
    }
    int lastDuplicateId() const { return lastDuplicate_; }

    bool setPatternLength(int id, TimeSignature ts, int bars, std::int64_t stepTicks) {
        if (!Bank::validId(id)) return false;
        if (id == activePattern_) return pattern_.configure(ts, bars, stepTicks);
        return bank_.setShape(id, ts, bars, stepTicks);
    }

    // Replaces a pattern's whole contents. Audio thread (a copy of ~310 KB
    // when `id` is active); other threads use ReplacePattern with a
    // PatternData.
    bool replacePattern(int id, const Pattern& pattern, const RecordedTrack& track,
                        const QuantizeSettings& quantize) {
        if (!Bank::validId(id) || !quantize.valid()) return false;
        if (id != activePattern_) return bank_.store(id, pattern, track, quantize);
        finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
        pattern_ = pattern;
        recorder_.track_ = track;
        quantize_ = quantize;
        quantCacheValid_ = false;
        return true;
    }

    // ---- Arrangement (Engine 028) ---------------------------------------------
    // Editing is rejected while song mode runs.

    bool arrangementClear() {
        if (songActive_) return false;
        arrangement_.clear();
        return true;
    }
    bool arrangementAppend(int id, int repeats) {
        if (songActive_ || !Bank::validId(id)) return false;
        return arrangement_.append(id, repeats);
    }
    bool arrangementSetEnd(ArrangementEnd end) {
        if (songActive_) return false;
        arrangement_.setEnd(end);
        return true;
    }

    // Validates the arrangement and starts song mode: stopped -> entry 0
    // now; playing -> entry 0 at the current pattern's next loop boundary.
    bool startArrangement() {
        const bool ok = arrangement_.validate([this](int id) {
            return id == activePattern_ || bank_.used(id);
        });
        if (!ok) {
            ++invalidCommands_;
            return false;
        }
        songActive_ = true;
        songFinished_ = false;
        if (!clock_.playing()) {
            beginSong();
        } else {
            pending_ = Pending{true, Pending::Switch, arrangement_.at(0).pattern,
                               boundaryTick(SwitchTiming::NextLoop), true, 0};
        }
        return songActive_;
    }

    void stopArrangement() {
        songActive_ = false;
        if (pending_.active && pending_.song) pending_ = Pending{};
    }

    bool songActive() const { return songActive_; }
    bool songFinished() const { return songFinished_; }
    std::size_t songEntry() const { return songEntry_; }

    // Replaces bank, arrangement and active pattern (e.g. after loading a
    // file). Only while stopped; false (unchanged) if playing, the active id
    // is invalid or the arrangement does not validate against the bank.
    bool loadBankState(const Bank& bank, const Arrangement& arrangement,
                       int active) {
        if (clock_.playing() || !Bank::validId(active)) return false;
        if (!arrangement.empty() &&
            !arrangement.validate([&bank](int id) { return bank.used(id); })) {
            return false;
        }
        finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
        bank_ = bank;
        arrangement_ = arrangement;
        activePattern_ = active;
        bank_.load(active, pattern_, recorder_.track_, quantize_);
        if (!bank_.used(active)) bank_.store(active, pattern_, recorder_.track_, quantize_);
        origin_ = clock_.firstTickAtOrAfter(clock_.samplePosition());
        ++epoch_;
        quantCacheValid_ = false;
        pending_ = Pending{};
        songActive_ = false;
        songFinished_ = false;
        return true;
    }

    std::uint64_t patternSwitchCount() const { return switches_; }
    std::uint64_t rejectedSwitchCount() const { return rejectedSwitches_; }
    std::uint64_t bankFullCount() const { return bank_.fullCount(); }

    // Live input on the audio thread (same rules as postLive).
    bool liveEvent(const LiveEvent& event) { return recorder_.queueLive(event); }

    // ---- Cross-thread entry points (one producer thread per queue) -----------

    bool postTransport(const TransportCommand& command) {
        return transport_.push(command);
    }
    bool postCommand(const SequencerCommand& command) { return commands_.push(command); }
    bool postLive(const LiveEvent& event) { return live_.push(event); }

    std::uint32_t droppedTransportCount() const { return transport_.droppedCount(); }
    std::uint32_t droppedCommandCount() const { return commands_.droppedCount(); }
    std::uint32_t droppedLiveQueueCount() const { return live_.droppedCount(); }

    // ---- Audio thread ----------------------------------------------------------

    // Generates this block's events and passes each to `sink`
    // (void(const SequencerEvent&)) in delivery order, then advances the
    // clock by `frames`.
    template <typename Sink>
    void process(std::size_t frames, Sink&& sink) {
        applyTransportCommands();
        applyCommands();
        drainLiveQueue();
        count_ = 0;
        generated_ = 0;
        orderCounter_ = 0;

        if (flushNotes_) {
            for (std::size_t i = 0; i < activeCount_; ++i) {
                push({0, SequencerEvent::Kind::NoteOff, active_[i].target,
                      active_[i].note, 0, 0}, true);
            }
            activeCount_ = 0;
            flushNotes_ = false;
        }

        std::int64_t advanceBy = static_cast<std::int64_t>(frames);
        if (frames > 0) {
            if (clock_.playing()) {
                advanceBy = generateBlock(static_cast<std::int64_t>(frames));
            } else {
                playLiveWhileStopped();
            }
        }

        std::sort(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(count_),
                  [](const SequencerEvent& a, const SequencerEvent& b) {
                      if (a.offset != b.offset) return a.offset < b.offset;
                      if (a.kind != b.kind) return a.kind < b.kind;
                      if (a.target != b.target) return a.target < b.target;
                      if (a.value != b.value) return a.value < b.value;
                      return a.order < b.order;
                  });
        for (std::size_t i = 0; i < count_; ++i) {
            sink(events_[i]);
        }
        delivered_ += count_;
        clock_.advance(advanceBy);
        if (stopAtBlockEnd_) {
            clock_.stop();
            stopAtBlockEnd_ = false;
        }
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
        std::int64_t offFine;  // absolute fine position of the note-off
    };

    static constexpr std::uint64_t RecordedOrder = 1ull << 40;
    static constexpr std::uint64_t LiveOrder = 1ull << 41;
    static constexpr std::int64_t MaxNoteLengthFine = Pattern::MaxNoteLengthTicks * Fine;

    struct Pending {
        enum Action : std::uint8_t { Switch, SongEnd, SongFinish };
        bool active = false;
        Action action = Switch;
        int target = 0;
        std::int64_t tick = 0;  // absolute tick of the transition
        bool song = false;
        std::size_t entry = 0;  // arrangement entry entered (song switches)
    };

    MusicalClock clock_;
    Pattern pattern_;
    Recorder recorder_;
    // Engine 028: bank, arrangement, switching.
    Bank bank_;
    Arrangement arrangement_;
    int activePattern_ = 0;
    std::int64_t origin_ = 0;  // absolute tick where the active pattern started
    Pending pending_{};
    bool songActive_ = false;
    bool songFinished_ = false;
    std::size_t songEntry_ = 0;
    bool stopAtBlockEnd_ = false;
    int lastDuplicate_ = -1;
    std::uint64_t switches_ = 0;
    std::uint64_t rejectedSwitches_ = 0;
    std::uint64_t seed_ = 1;
    std::uint32_t epoch_ = 0;
    SpscRing<TransportCommand, TransportCapacity> transport_;
    SpscRing<SequencerCommand, CommandCapacity> commands_;
    SpscRing<LiveEvent, LiveCapacity> live_;

    // Worst case per block: MaxEventsPerBlock triggers/note-ons; one
    // note-off per note that was sounding at the block start (<=
    // MaxActiveNotes) or started in the block (<= MaxEventsPerBlock), which
    // also covers flushes and segment ends of pattern switches (Engine 028);
    // and every pending live input.
    static constexpr std::size_t EventBufferSize =
        MaxEventsPerBlock + (MaxEventsPerBlock + MaxActiveNotes) + Recorder::MaxPendingLive;
    std::array<SequencerEvent, EventBufferSize> events_{};
    std::size_t count_ = 0;
    std::size_t generated_ = 0;  // limited events this block
    std::uint64_t orderCounter_ = 0;
    std::array<ActiveNote, MaxActiveNotes> active_{};
    std::size_t activeCount_ = 0;
    bool flushNotes_ = false;

    std::uint64_t delivered_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t skipped_ = 0;
    std::uint64_t invalidCommands_ = 0;

    // Quantization (Engine 027). The cache holds, per recorded event, the
    // difference between played and raw start/length. It is DERIVED data
    // (the raw events are never touched), recomputed from the raw events
    // whenever the settings, the track revision or the loop length change.
    // |delta| < 1.5 grid steps <= 5,898,240 fine ticks, so int32 is enough.
    QuantizeSettings quantize_{};
    std::array<std::int32_t, RecordedTrack::MaxEvents> quantStart_{};
    std::array<std::int32_t, RecordedTrack::MaxEvents> quantLength_{};
    bool quantCacheValid_ = false;
    std::uint64_t quantRevision_ = 0;
    std::int64_t quantLoopFine_ = 0;
    std::uint64_t quantRefreshes_ = 0;

    void refreshQuantCache(std::int64_t loopFine) {
        const RecordedTrack& t = recorder_.track_;
        if (quantCacheValid_ && quantRevision_ == t.revision() && quantLoopFine_ == loopFine) {
            return;
        }
        for (std::size_t i = 0; i < t.size(); ++i) {
            const RecordedEvent& e = t.at(i);
            std::int64_t start = 0;
            std::int64_t length = 0;
            quantize::apply(e, loopFine, quantize_, start, length);
            quantStart_[i] = static_cast<std::int32_t>(start - e.startFine);
            quantLength_[i] = static_cast<std::int32_t>(length - e.lengthFine);
        }
        quantCacheValid_ = true;
        quantRevision_ = t.revision();
        quantLoopFine_ = loopFine;
        ++quantRefreshes_;
    }

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

    // First fine position delivered at or after the current sample (never
    // negative: fine ticks with exact times in (-1, 0) do not exist).
    std::int64_t currentFine() const {
        return std::max<std::int64_t>(0, clock_.firstFineAtOrAfter(clock_.samplePosition()));
    }

    // `reserved` events (note-offs, live input) use reserved space; others
    // respect MaxEventsPerBlock.
    bool push(SequencerEvent e, bool reserved) {
        if (!reserved) {
            if (generated_ == MaxEventsPerBlock) {
                ++dropped_;
                return false;
            }
            ++generated_;
        }
        if (count_ == events_.size()) {  // cannot happen (see EventBufferSize)
            ++dropped_;
            return false;
        }
        if (e.order == 0) e.order = orderCounter_++;
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

    void applyCommands() {
        SequencerCommand c;
        for (std::size_t i = 0; i < CommandCapacity && commands_.pop(c); ++i) {
            switch (c.type) {
            case SequencerCommandType::Arm: arm(); break;
            case SequencerCommandType::Disarm: disarm(); break;
            case SequencerCommandType::SetRecordMode: recorder_.setRecordMode(c.recordMode); break;
            case SequencerCommandType::SetWriteMode: recorder_.setWriteMode(c.writeMode); break;
            case SequencerCommandType::SetTargets: recorder_.setTargets(c.targets); break;
            case SequencerCommandType::SetPunch:
                if (!recorder_.setPunch(c.a, c.b)) ++invalidCommands_;
                break;
            case SequencerCommandType::SetQuantize:
                if (!setQuantize(c.quantize)) ++invalidCommands_;
                break;
            case SequencerCommandType::ClearPunch: recorder_.clearPunch(); break;
            case SequencerCommandType::Undo: undo(); break;
            case SequencerCommandType::Redo: redo(); break;
            case SequencerCommandType::ClearRecording: clearRecording(); break;
            case SequencerCommandType::SetStep:
                pattern_.setStep(static_cast<std::size_t>(c.a), static_cast<std::size_t>(c.b), c.step);
                break;
            case SequencerCommandType::ClearStep:
                pattern_.clearStep(static_cast<std::size_t>(c.a), static_cast<std::size_t>(c.b));
                break;
            case SequencerCommandType::AddNote: pattern_.addNote(c.note); break;
            case SequencerCommandType::RemoveNote:
                pattern_.removeNote(static_cast<std::size_t>(c.a));
                break;
            case SequencerCommandType::ClearPattern: pattern_.clear(); break;
            case SequencerCommandType::ConfigurePattern:
                pattern_.configure(c.timeSignature, c.bars, c.stepTicks);
                break;
            case SequencerCommandType::SwitchPattern:
                if (c.b < 0 || c.b > static_cast<std::int64_t>(SwitchTiming::NextLoop)) {
                    ++invalidCommands_;
                } else {
                    requestPatternSwitch(static_cast<int>(c.a), static_cast<SwitchTiming>(c.b));
                }
                break;
            case SequencerCommandType::CancelPatternSwitch: cancelPatternSwitch(); break;
            case SequencerCommandType::ClearBankPattern:
                if (!clearPattern(static_cast<int>(c.a))) ++invalidCommands_;
                break;
            case SequencerCommandType::CopyPattern:
                if (!copyPattern(static_cast<int>(c.a), static_cast<int>(c.b))) ++invalidCommands_;
                break;
            case SequencerCommandType::DuplicatePattern:
                if (duplicatePattern(static_cast<int>(c.a)) < 0) ++invalidCommands_;
                break;
            case SequencerCommandType::SetPatternLength:
                if (!setPatternLength(static_cast<int>(c.a), c.timeSignature, c.bars, c.stepTicks)) {
                    ++invalidCommands_;
                }
                break;
            case SequencerCommandType::ReplacePattern:
                if (c.data == nullptr) {
                    ++invalidCommands_;
                } else {
                    const bool ok = replacePattern(static_cast<int>(c.a), c.data->pattern,
                                                   c.data->track, c.data->quantize);
                    if (!ok) ++invalidCommands_;
                    c.data->status.store(ok ? 1 : -1, std::memory_order_release);
                }
                break;
            case SequencerCommandType::ArrangementClear:
                if (!arrangementClear()) ++invalidCommands_;
                break;
            case SequencerCommandType::ArrangementAppend:
                if (!arrangementAppend(static_cast<int>(c.a), static_cast<int>(c.b))) {
                    ++invalidCommands_;
                }
                break;
            case SequencerCommandType::ArrangementSetEnd:
                if (songActive_ || c.a < 0 || c.a > static_cast<std::int64_t>(ArrangementEnd::HoldLast)) {
                    ++invalidCommands_;
                } else {
                    arrangementSetEnd(static_cast<ArrangementEnd>(c.a));
                }
                break;
            case SequencerCommandType::StartArrangement: startArrangement(); break;
            case SequencerCommandType::StopArrangement: stopArrangement(); break;
            }
        }
    }

    void drainLiveQueue() {
        LiveEvent e;
        for (std::size_t i = 0; i < LiveCapacity && live_.pop(e); ++i) {
            recorder_.queueLive(e);
        }
    }

    // ---- Recording --------------------------------------------------------------

    void closePass() { recorder_.openPass_ = 0; }

    bool passOpenFor(std::int64_t loop) const {
        const Recorder& r = recorder_;
        return r.openPass_ != 0 && (r.mode_ == RecordMode::Free || r.openPassLoop_ == loop);
    }

    // Opens the pass for `loop` if needed (Loop mode: one pass per loop).
    void ensurePass(std::int64_t loop) {
        Recorder& r = recorder_;
        if (r.mode_ == RecordMode::Loop && r.openPass_ != 0 && r.openPassLoop_ != loop) {
            closePass();
        }
        if (r.openPass_ == 0) {
            r.openPass_ = r.track_.openPass();
            r.openPassLoop_ = loop;
        }
    }

    // Ends held recorded notes at absolute fine position `absFine` and
    // closes the open pass.
    void finishRecordingAt(std::int64_t absFine) {
        const std::int64_t fine = absFine - origin_ * Fine;
        Recorder& r = recorder_;
        for (std::size_t i = 0; i < r.heldCount_; ++i) {
            const std::int64_t len = std::clamp<std::int64_t>(
                fine - r.held_[i].startFine, 1, MaxNoteLengthFine);
            r.track_.finishNote(r.held_[i].sequence, len);
        }
        r.heldCount_ = 0;
        closePass();
    }

    void recordInput(const LiveEvent& e, std::int64_t fine, std::int64_t loopFine) {
        Recorder& r = recorder_;
        const std::int64_t loop = floorDiv(fine, loopFine);
        RecordedEvent rec;
        rec.value = e.value;
        rec.velocity = e.velocity;
        rec.startFine = fine - loop * loopFine;
        rec.captureSample = e.sampleTime;
        rec.captureFine = fine + origin_ * Fine;  // absolute
        rec.captureLoop = loop;
        rec.captureEpoch = epoch_;
        if (e.type == LiveEventType::DrumHit) {
            if ((r.targets_ & RecordDrums) == 0) return;
            ensurePass(loop);
            rec.kind = RecordedEvent::Kind::Drum;
            rec.sequence = ++r.sequence_;
            if (r.track_.add(rec, r.openPass_) >= 0) ++r.recorded_;
            return;
        }
        // Note-on.
        rec.kind = RecordedEvent::Kind::Note;
        rec.target = e.target;
        if ((r.targets_ & targetBit(rec)) == 0) return;
        if (r.heldCount_ == Recorder::MaxHeldNotes) {
            ++r.heldFull_;
            return;
        }
        ensurePass(loop);
        rec.pending = true;
        rec.sequence = ++r.sequence_;
        if (r.track_.add(rec, r.openPass_) < 0) return;
        ++r.recorded_;
        r.held_[r.heldCount_++] = {rec.sequence, e.target, e.value, fine, r.openPass_};
    }

    void finishInput(const LiveEvent& e, std::int64_t fine) {
        Recorder& r = recorder_;
        for (std::size_t i = 0; i < r.heldCount_; ++i) {
            if (r.held_[i].target == e.target && r.held_[i].note == e.value) {
                const std::int64_t len = std::clamp<std::int64_t>(
                    fine - r.held_[i].startFine, 1, MaxNoteLengthFine);
                r.track_.finishNote(r.held_[i].sequence, len);
                for (std::size_t k = i + 1; k < r.heldCount_; ++k) r.held_[k - 1] = r.held_[k];
                --r.heldCount_;
                return;
            }
        }
    }

    // Live inputs due in [s0, s1): recorder bookkeeping (passes, Replace
    // erasure, capture) in time order. Returns the number of due inputs.
    std::size_t recordBlock(std::int64_t s0, std::int64_t s1, std::int64_t f0,
                            std::int64_t f1, std::int64_t loopFine) {
        Recorder& r = recorder_;
        std::size_t due = 0;
        while (due < r.pendingCount_ && r.pending_[due].event.sampleTime < s1) ++due;
        // Late = before the block (not merely before this segment).
        const std::int64_t lateBefore = blockStart_;

        // Record region inside this block: [a, b).
        std::int64_t a = f0;
        std::int64_t b = f1;
        if (r.armed_) {
            a = std::max(a, r.armFine_);
            if (r.punch_) {
                a = std::max(a, (r.punchIn_ - origin_) * Fine);
                b = std::min(b, (r.punchOut_ - origin_) * Fine);
            }
            if (r.punch_ && r.openPass_ != 0 && (r.punchOut_ - origin_) * Fine <= f0) {
                closePass();  // punch-out already passed
            }
        }
        const bool recording = r.armed_ && a < b;

        std::int64_t next = a;              // next unprocessed record position
        std::int64_t pieceStart = 0;
        std::int64_t pieceEnd = 0;          // current piece [pieceStart, pieceEnd)
        auto advanceTo = [&](std::int64_t position) {
            // Process record pieces (pass handling + Replace erasure) up to
            // and including the piece that contains `position`.
            while (recording && next < b && next <= position) {
                const std::int64_t loop = floorDiv(next, loopFine);
                const std::int64_t end = std::min(b, (loop + 1) * loopFine);
                if (r.writeMode_ == WriteMode::Replace) {
                    const std::int64_t from = next - loop * loopFine;
                    const std::int64_t to = end - loop * loopFine;
                    // A pass is opened only when it has something to do.
                    if (passOpenFor(loop) ||
                        r.track_.countErasable(from, to, r.targets_) > 0) {
                        ensurePass(loop);
                        r.track_.erase(from, to, r.targets_, r.openPass_);
                    }
                }
                pieceStart = next;
                pieceEnd = end;
                next = end;
            }
        };

        for (std::size_t i = 0; i < due; ++i) {
            const LiveEvent& e = r.pending_[i].event;
            if (e.sampleTime < lateBefore) {
                // Late: sounds at offset 0 and is never recorded; a late
                // note-off still ends a held recorded note (at f0).
                ++r.late_;
                if (e.type == LiveEventType::NoteOff) finishInput(e, localFineAtSample(s0));
                continue;
            }
            const std::int64_t fine = localFineAtSample(e.sampleTime);  // in [f0, f1)
            advanceTo(fine);
            const bool inside = recording && fine >= pieceStart && fine < pieceEnd;
            if (e.type == LiveEventType::NoteOff) {
                finishInput(e, fine);
            } else if (inside) {
                recordInput(e, fine, loopFine);
            }
        }
        advanceTo(b);  // remaining pieces of this block

        if (recording && r.punch_ && (r.punchOut_ - origin_) * Fine <= f1) {
            closePass();  // punch-out inside this block (exclusive end)
        }
        return due;
    }

    void pushLive(const LiveEvent& e, std::size_t offset, std::uint64_t arrival) {
        SequencerEvent ev;
        ev.offset = offset;
        ev.value = e.value;
        ev.target = e.target;
        ev.velocity = e.velocity;
        ev.order = LiveOrder + arrival;
        switch (e.type) {
        case LiveEventType::DrumHit:
            ev.kind = SequencerEvent::Kind::Drum;
            ev.target = SynthTarget::Mono;
            break;
        case LiveEventType::NoteOn:
            ev.kind = SequencerEvent::Kind::NoteOn;
            break;
        case LiveEventType::NoteOff:
            ev.kind = SequencerEvent::Kind::NoteOff;
            ev.velocity = 0;
            break;
        }
        push(ev, true);
    }

    void removePending(std::size_t count) {
        Recorder& r = recorder_;
        for (std::size_t i = count; i < r.pendingCount_; ++i) r.pending_[i - count] = r.pending_[i];
        r.pendingCount_ -= count;
    }

    void playLiveWhileStopped() {
        Recorder& r = recorder_;
        for (std::size_t i = 0; i < r.pendingCount_; ++i) {
            pushLive(r.pending_[i].event, 0, r.pending_[i].arrival);
        }
        removePending(r.pendingCount_);
    }

    // ---- Playback ---------------------------------------------------------------

    std::size_t offsetOfFine(std::int64_t fine, std::int64_t s0) const {
        const std::int64_t s = clock_.sampleOfFine(fine) - s0;
        return static_cast<std::size_t>(s < 0 ? 0 : s);
    }

    // ---- Engine 028: switching helpers ------------------------------------------

    std::int64_t blockStart_ = 0;

    std::int64_t localFineAtSample(std::int64_t sample) const {
        return clock_.fineAtSample(sample) - origin_ * Fine;
    }

    // End of the beat / bar / loop (of the current pattern, counted from its
    // start) that contains the next undelivered tick: the unit that is
    // playing - or about to start - always completes. Immediate: the next
    // undelivered tick.
    std::int64_t boundaryTick(SwitchTiming timing) const {
        const std::int64_t next = clock_.firstTickAtOrAfter(clock_.samplePosition());
        std::int64_t unit = 1;
        switch (timing) {
        case SwitchTiming::Immediate: return next;
        case SwitchTiming::NextBeat: unit = pattern_.timeSignature().ticksPerBeat(); break;
        case SwitchTiming::NextBar: unit = pattern_.timeSignature().ticksPerBar(); break;
        case SwitchTiming::NextLoop: unit = pattern_.lengthTicks(); break;
        }
        const std::int64_t local = std::max<std::int64_t>(0, next - origin_);
        return origin_ + (floorDiv(local, unit) + 1) * unit;
    }

    // Makes pattern `id` the active one, starting at absolute tick `tick`.
    // Held recorded notes end there and the open pass closes (recording
    // stays armed and continues in the new pattern). The working state is
    // saved into its slot first; if the bank cannot hold it the switch is
    // rejected and nothing changes. Sounding notes keep their note-offs.
    bool switchTo(int id, std::int64_t tick) {
        if (id == activePattern_) return true;
        finishRecordingAt(tick * Fine);
        if (!bank_.store(activePattern_, pattern_, recorder_.track_, quantize_)) {
            ++rejectedSwitches_;
            return false;
        }
        bank_.load(id, pattern_, recorder_.track_, quantize_);
        if (!bank_.used(id)) bank_.store(id, pattern_, recorder_.track_, quantize_);
        activePattern_ = id;
        origin_ = tick;
        ++epoch_;
        quantCacheValid_ = false;
        if (recorder_.armed_) recorder_.armFine_ = 0;
        ++switches_;
        return true;
    }

    // Reloads the working state from the active slot (after it was replaced
    // in the bank).
    void reloadActive() {
        finishRecordingAt(clock_.fineAtSample(clock_.samplePosition()));
        bank_.load(activePattern_, pattern_, recorder_.track_, quantize_);
        quantCacheValid_ = false;
    }

    // Song mode: enter entry 0 now (stopped, or after reset).
    void beginSong() {
        const std::int64_t tick = clock_.firstTickAtOrAfter(clock_.samplePosition());
        enterEntry(0, tick);
    }

    void enterEntry(std::size_t entry, std::int64_t tick) {
        if (!switchTo(arrangement_.at(entry).pattern, tick)) {
            songActive_ = false;
            pending_ = Pending{};
            return;
        }
        songEntry_ = entry;
        // A same-pattern entry continues the pattern: count its repeats
        // from this boundary.
        const std::int64_t end = tick + arrangement_.at(entry).repeats * pattern_.lengthTicks();
        if (entry + 1 < arrangement_.size()) {
            pending_ = Pending{true, Pending::Switch, arrangement_.at(entry + 1).pattern, end,
                               true, entry + 1};
            return;
        }
        switch (arrangement_.end()) {
        case ArrangementEnd::Stop:
            pending_ = Pending{true, Pending::SongEnd, 0, end, true, entry};
            break;
        case ArrangementEnd::Loop:
            pending_ = Pending{true, Pending::Switch, arrangement_.at(0).pattern, end, true, 0};
            break;
        case ArrangementEnd::HoldLast:
            pending_ = Pending{true, Pending::SongFinish, 0, end, true, entry};
            break;
        }
    }

    // Executes the pending transition at absolute tick `tick` (inside the
    // current block). Returns false if generation must stop (song end).
    bool executePending(std::int64_t tick, std::int64_t blockStart) {
        const Pending p = pending_;
        pending_ = Pending{};
        if (p.action == Pending::Switch) {
            if (p.song) {
                if (songActive_) enterEntry(p.entry, tick);
            } else {
                switchTo(p.target, tick);
            }
            return true;
        }
        songActive_ = false;
        songFinished_ = true;
        if (p.action == Pending::SongFinish) return true;  // HoldLast: keep looping
        // Stop: release everything at the end boundary; the transport
        // stops exactly there.
        finishRecordingAt(tick * Fine);
        const std::size_t offset = offsetOfFine(tick * Fine, blockStart);
        for (std::size_t i = 0; i < activeCount_; ++i) {
            push({offset, SequencerEvent::Kind::NoteOff, active_[i].target, active_[i].note, 0, 0},
                 true);
        }
        activeCount_ = 0;
        stopAtBlockEnd_ = true;
        return false;
    }

    // Live input due before `sEnd` sounds at its own sample (not recorded).
    void playLiveRange(std::int64_t sEnd, std::int64_t blockStart) {
        Recorder& r = recorder_;
        std::size_t due = 0;
        while (due < r.pendingCount_ && r.pending_[due].event.sampleTime < sEnd) ++due;
        for (std::size_t i = 0; i < due; ++i) {
            const std::int64_t offset = r.pending_[i].event.sampleTime - blockStart;
            pushLive(r.pending_[i].event, static_cast<std::size_t>(offset < 0 ? 0 : offset),
                     r.pending_[i].arrival);
        }
        removePending(due);
    }

    // Generates one block, split at every pending transition inside it, so
    // switches happen on their exact sample for any block size. Returns the
    // number of samples the clock should advance (less than `frames` only
    // when an arrangement stopped inside the block).
    std::int64_t generateBlock(std::int64_t frames) {
        const std::int64_t s0 = clock_.samplePosition();
        const std::int64_t s1 = s0 + frames;
        blockStart_ = s0;
        std::int64_t segS = s0;
        std::int64_t segT = clock_.firstTickAtOrAfter(s0);
        std::int64_t segF = std::max<std::int64_t>(0, clock_.firstFineAtOrAfter(s0));
        const std::int64_t endT = clock_.firstTickAtOrAfter(s1);
        const std::int64_t endF = clock_.firstFineAtOrAfter(s1);
        for (std::size_t n = 0; n < MaxTransitionsPerBlock && pending_.active; ++n) {
            if (pending_.tick >= endT) break;  // delivered in a later block
            const std::int64_t t = std::max(pending_.tick, segT);
            const std::int64_t sT = clock_.sampleOfTick(t);
            generateRange(segS, sT, segT, t, segF, t * Fine, s0);
            segS = sT;
            segT = t;
            segF = t * Fine;
            if (!executePending(t, s0)) {
                playLiveRange(s1, s0);
                return sT - s0;
            }
        }
        generateRange(segS, s1, segT, endT, segF, endF, s0);
        return frames;
    }

    // Events for samples [sA, sB), ticks [tA, tB) and fine positions
    // [fA, fB) (all absolute) of the active pattern, whose tick 0 is
    // origin_. Offsets are relative to blockStart.
    void generateRange(std::int64_t sA, std::int64_t sB, std::int64_t tA, std::int64_t tB,
                       std::int64_t fA, std::int64_t fB, std::int64_t blockStart) {
        if (sB <= sA && tB <= tA && fB <= fA) return;
        const std::int64_t originFine = origin_ * Fine;
        // Local (pattern-relative) positions; nothing exists before tick 0.
        const std::int64_t tFirst = std::max<std::int64_t>(0, tA - origin_);
        const std::int64_t tEnd = tB - origin_;
        const std::int64_t f0 = std::max<std::int64_t>(0, fA - originFine);
        const std::int64_t f1 = fB - originFine;

        const std::int64_t length = pattern_.lengthTicks();
        const std::int64_t loopFine = length * Fine;
        const std::int64_t stepTicks = pattern_.stepTicks();
        const std::int64_t steps = static_cast<std::int64_t>(pattern_.stepCount());
        const std::int64_t maxMicro = stepTicks / 2;

        // a. Recorder (before playback, so Replace erasure takes effect now).
        const std::size_t due = recordBlock(sA, sB, f0, f1, loopFine);

        if (tEnd > tFirst) {
            // b. Pattern drum steps.
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
                        push({offsetOfFine((tick + origin_) * Fine, blockStart),
                              SequencerEvent::Kind::Drum, SynthTarget::Mono,
                              static_cast<int>(pad), st.velocity, 0},
                             false);
                    }
                }
            }

            // b. Pattern note-ons.
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
                    if (!push({offsetOfFine((tick + origin_) * Fine, blockStart),
                               SequencerEvent::Kind::NoteOn, n.target, n.note, n.velocity, 0},
                              false)) {
                        continue;
                    }
                    active_[activeCount_++] = {n.target, n.note,
                                               (tick + origin_ + n.lengthTicks) * Fine};
                }
            }
        }

        // c. Recorded events.
        const RecordedTrack& track = recorder_.track_;
        const bool quant = quantize_.active();
        if (quant) refreshQuantCache(loopFine);
        if (f1 > f0 && track.size() > 0) {
            // A quantized start can reach loopFine (next loop's position 0),
            // so with quantization the previous loop's occurrences are
            // examined too.
            const std::int64_t rLoopFirst = floorDiv(f0, loopFine) - (quant ? 1 : 0);
            const std::int64_t rLoopLast = floorDiv(f1 - 1, loopFine);
            for (std::int64_t loop = rLoopFirst; loop <= rLoopLast; ++loop) {
                for (std::size_t i = 0; i < track.size(); ++i) {
                    if (!track.visible(i)) continue;
                    const RecordedEvent& e = track.at(i);
                    if (e.startFine >= loopFine) continue;
                    if (loop == e.captureLoop && e.captureEpoch == epoch_) continue;
                    const std::int64_t fine =
                        loop * loopFine + e.startFine + (quant ? quantStart_[i] : 0);
                    if (fine < f0 || fine >= f1) continue;
                    const std::uint64_t order = RecordedOrder + e.sequence;
                    if (e.kind == RecordedEvent::Kind::Drum) {
                        push({offsetOfFine(fine + originFine, blockStart),
                              SequencerEvent::Kind::Drum, SynthTarget::Mono, e.value,
                              e.velocity, order},
                             false);
                        continue;
                    }
                    if (activeCount_ == MaxActiveNotes) {
                        ++dropped_;
                        continue;
                    }
                    if (!push({offsetOfFine(fine + originFine, blockStart),
                               SequencerEvent::Kind::NoteOn, e.target, e.value, e.velocity,
                               order},
                              false)) {
                        continue;
                    }
                    active_[activeCount_++] = {
                        e.target, e.value,
                        fine + originFine + e.lengthFine + (quant ? quantLength_[i] : 0)};
                }
            }
        }

        // d. Live inputs sound at their own samples (late ones at offset 0).
        for (std::size_t i = 0; i < due; ++i) {
            const LiveEvent& e = recorder_.pending_[i].event;
            const std::int64_t offset = e.sampleTime - blockStart;
            pushLive(e, static_cast<std::size_t>(offset < 0 ? 0 : offset),
                     recorder_.pending_[i].arrival);
        }
        removePending(due);

        // Note-offs due in this range (absolute; including notes started
        // above and notes of a pattern switched away from).
        std::size_t i = 0;
        while (i < activeCount_) {
            if (active_[i].offFine < fB) {
                push({offsetOfFine(active_[i].offFine, blockStart),
                      SequencerEvent::Kind::NoteOff, active_[i].target, active_[i].note, 0, 0},
                     true);
                active_[i] = active_[--activeCount_];
            } else {
                ++i;
            }
        }
    }
};

// The host engine's sequencer.
using Sequencer = BasicSequencer<HostProfile>;

} // namespace databreaks
