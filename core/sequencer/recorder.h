
#pragma once

// DATABREAKs Engine 026 - performance recorder state (host-side C++17).
//
// The Recorder captures live performance events (pad hits, synth notes)
// into a RecordedTrack. It records MUSICAL EVENTS, never audio. Its timing
// logic runs inside Sequencer::process() on the audio thread; this header
// holds the settings, state, input types and limits. See sequencer.h for
// the per-block algorithm.
//
// Live input (LiveEvent) carries the absolute clock sample at which the
// input happened (`sampleTime`, the documented input timestamp). It is
// played through the normal instruments at that sample and, while
// recording, stored at the fine position of that sample:
//   f = clock.fineAtSample(sampleTime), sampleOfFine(f) == sampleTime
// so a recording replays on exactly the captured sample (same tempo).
// No latency compensation is applied anywhere; see the progress report.
//
// Modes:
//   * Free record: one pass from arm (or punch-in) until disarm (or
//     punch-out, transport stop/reset); it may span several loops.
//   * Loop record: a new pass at every loop wrap (each loop is one undo
//     step).
//   * Overdub: new events are added; nothing is erased.
//   * Replace: as the record head passes, existing visible events of the
//     armed kinds in the covered loop region are erased (by this pass,
//     undoably); events outside the covered region are untouched.
//   A pass is opened lazily, when it first records or erases something,
//   so passing loops without playing creates no empty undo steps.
//   * Punch: recording happens only for positions in
//     [punchIn, punchOut) (absolute ticks); playback continues throughout.
// Region convention everywhere: start INCLUSIVE, end EXCLUSIVE. An event
// exactly on the loop end is the start of the next loop (loop-relative 0).
//
// Limits (fixed storage, no allocation):
//   RecordedTrack::MaxEvents 2048, MaxEventsPerPass 512, MaxPasses 16
//   (undo depth); MaxHeldNotes 64 notes being recorded at once;
//   MaxPendingLive 512 live inputs waiting for their block. Overflow drops
//   the excess input from the recording (it is still played if it fits)
//   and counts it; recording continues; stored data is never touched.

#include "pattern.h"
#include "recorded_track.h"
#include "../audio/synth_event.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

enum class LiveEventType : std::uint8_t { DrumHit, NoteOn, NoteOff };

struct LiveEvent {
    LiveEventType type = LiveEventType::DrumHit;
    SynthTarget target = SynthTarget::Mono;  // notes only
    int value = 0;                           // pad 0..26 or note 0..127
    int velocity = 100;                      // 1..127 (hits and note-ons)
    std::int64_t sampleTime = 0;             // absolute clock sample
};

enum class RecordMode : std::uint8_t { Free, Loop };
enum class WriteMode : std::uint8_t { Overdub, Replace };

// Commands for the audio thread (Sequencer::postCommand, one producer).
enum class SequencerCommandType : std::uint8_t {
    Arm, Disarm, SetRecordMode, SetWriteMode, SetTargets, SetPunch, ClearPunch,
    Undo, Redo, ClearRecording,
    SetStep, ClearStep, AddNote, RemoveNote, ClearPattern, ConfigurePattern
};

struct SequencerCommand {
    SequencerCommandType type = SequencerCommandType::Arm;
    RecordMode recordMode = RecordMode::Free;
    WriteMode writeMode = WriteMode::Overdub;
    unsigned targets = RecordAll;
    std::int64_t a = 0;   // punch-in tick / pad / note index
    std::int64_t b = 0;   // punch-out tick / step
    DrumStep step{};
    NoteEvent note{};
    TimeSignature timeSignature{};
    int bars = 1;
    std::int64_t stepTicks = 240;
};

class Recorder {
public:
    static constexpr std::size_t MaxHeldNotes = 64;
    static constexpr std::size_t MaxPendingLive = 512;

    RecordedTrack& track() { return track_; }
    const RecordedTrack& track() const { return track_; }

    // ---- Settings (audio thread; or via Sequencer::postCommand) -------------

    void setRecordMode(RecordMode mode) { mode_ = mode; }
    void setWriteMode(WriteMode mode) { writeMode_ = mode; }
    void setTargets(unsigned mask) { targets_ = mask & RecordAll; }

    // Punch window in absolute ticks, [in, out). Returns false (unchanged)
    // unless 0 <= in < out.
    bool setPunch(std::int64_t inTick, std::int64_t outTick) {
        if (inTick < 0 || outTick <= inTick) return false;
        punch_ = true;
        punchIn_ = inTick;
        punchOut_ = outTick;
        return true;
    }
    void clearPunch() { punch_ = false; }

    RecordMode recordMode() const { return mode_; }
    WriteMode writeMode() const { return writeMode_; }
    unsigned targets() const { return targets_; }
    bool punchEnabled() const { return punch_; }
    std::int64_t punchInTick() const { return punchIn_; }
    std::int64_t punchOutTick() const { return punchOut_; }

    bool armed() const { return armed_; }
    std::uint32_t openPass() const { return openPass_; }
    std::size_t heldNoteCount() const { return heldCount_; }
    std::size_t pendingLiveCount() const { return pendingCount_; }

    // ---- Statistics ------------------------------------------------------------

    std::uint64_t recordedEventCount() const { return recorded_; }
    std::uint64_t heldFullCount() const { return heldFull_; }
    std::uint64_t pendingFullCount() const { return pendingFull_; }
    std::uint64_t lateLiveCount() const { return late_; }
    std::uint64_t invalidLiveCount() const { return invalid_; }

private:
    friend class Sequencer;

    struct Held {
        std::uint64_t sequence = 0;
        SynthTarget target = SynthTarget::Mono;
        int note = 0;
        std::int64_t startFine = 0;  // absolute
        std::uint32_t pass = 0;
    };

    struct Pending {
        LiveEvent event;
        std::uint64_t arrival = 0;
    };

    RecordedTrack track_;
    RecordMode mode_ = RecordMode::Free;
    WriteMode writeMode_ = WriteMode::Overdub;
    unsigned targets_ = RecordAll;
    bool punch_ = false;
    std::int64_t punchIn_ = 0;
    std::int64_t punchOut_ = 0;

    bool armed_ = false;
    std::int64_t armFine_ = 0;       // recording starts at this fine position
    std::uint32_t openPass_ = 0;     // 0 = no pass open
    std::int64_t openPassLoop_ = 0;  // loop index of the open pass (Loop mode)

    std::array<Held, MaxHeldNotes> held_{};
    std::size_t heldCount_ = 0;

    std::array<Pending, MaxPendingLive> pending_{};
    std::size_t pendingCount_ = 0;
    std::uint64_t arrivals_ = 0;
    std::uint64_t sequence_ = 0;

    std::uint64_t recorded_ = 0;
    std::uint64_t heldFull_ = 0;
    std::uint64_t pendingFull_ = 0;
    std::uint64_t late_ = 0;
    std::uint64_t invalid_ = 0;

    static bool validLive(const LiveEvent& e) {
        if (e.sampleTime < 0) return false;
        switch (e.type) {
        case LiveEventType::DrumHit:
            return e.value >= 0 && e.value < static_cast<int>(Pattern::Lanes) &&
                   e.velocity >= 1 && e.velocity <= 127;
        case LiveEventType::NoteOn:
            return e.value >= 0 && e.value <= 127 && e.velocity >= 1 &&
                   e.velocity <= 127 && validTarget(e.target);
        case LiveEventType::NoteOff:
            return e.value >= 0 && e.value <= 127 && validTarget(e.target);
        }
        return false;
    }

    static bool validTarget(SynthTarget t) {
        return t == SynthTarget::Mono || t == SynthTarget::Fm || t == SynthTarget::Poly;
    }

    // Inserted in (sampleTime, arrival) order.
    bool queueLive(const LiveEvent& e) {
        if (!validLive(e)) {
            ++invalid_;
            return false;
        }
        if (pendingCount_ == MaxPendingLive) {
            ++pendingFull_;
            return false;
        }
        std::size_t i = pendingCount_;
        while (i > 0 && pending_[i - 1].event.sampleTime > e.sampleTime) {
            pending_[i] = pending_[i - 1];
            --i;
        }
        pending_[i] = {e, arrivals_++};
        ++pendingCount_;
        return true;
    }
};

} // namespace databreaks
