
#pragma once

// DATABREAKs Engine 025 - pattern model (host-side C++17).
//
// A Pattern holds two different kinds of musical data:
//   * DRUM STEPS: a grid of 27 lanes (one per FM-1 pad, index 0..26) x up to
//     MaxSteps steps. A step is a position on the grid; it may trigger its
//     pad once, with a velocity, a probability and a microtiming offset.
//   * NOTE EVENTS: a list of up to MaxNotes synth notes. A note is a MIDI
//     style event (destination, note number, velocity, start tick, length,
//     probability), placed at any tick, not tied to the step grid; several
//     notes can start on the same tick.
//
// Length: `bars` (1..MaxBars) of the pattern's time signature. The step
// grid divides the pattern into steps of `stepTicks` ticks (60..3840, i.e.
// a 1/64 note to a whole note); lengthTicks must be a whole multiple of
// stepTicks and give at most MaxSteps steps. The pattern loops every
// lengthTicks ticks.
//
// All storage is fixed (std::array); nothing allocates. Invalid indices or
// values are rejected (the call returns false / -1 and changes nothing).
//
// Threading: edit on the audio thread, or while the sequencer is stopped
// (other threads use Sequencer::postCommand, Engine 026).

#include "musical_time.h"
#include "../audio/synth_event.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

struct DrumStep {
    bool enabled = false;
    int velocity = 100;       // 1..127
    int probability = 100;    // 0..100 percent
    int microTicks = 0;       // -stepTicks/2 .. +stepTicks/2
};

struct NoteEvent {
    bool enabled = true;
    SynthTarget target = SynthTarget::Poly;
    int note = 60;                 // 0..127
    int velocity = 100;            // 1..127
    int probability = 100;         // 0..100 percent
    std::int64_t startTick = 0;    // 0 .. lengthTicks - 1
    std::int64_t lengthTicks = 240;  // 1 .. MaxNoteLengthTicks
};

class Pattern {
public:
    static constexpr std::size_t Lanes = 27;
    static constexpr std::size_t MaxSteps = 256;
    static constexpr int MaxBars = 16;
    static constexpr std::size_t MaxNotes = 512;
    static constexpr std::int64_t MinStepTicks = TicksPerQuarter / 16;  // 60
    static constexpr std::int64_t MaxStepTicks = TicksPerQuarter * 4;   // 3840
    static constexpr std::int64_t MaxNoteLengthTicks = TicksPerQuarter * 4 * 16;

    Pattern() { configure(TimeSignature{}, 1, TicksPerQuarter / 4); }

    // ---- Shape ---------------------------------------------------------------

    // Returns false (and changes nothing) for an invalid combination. Step
    // and note data are kept; steps beyond the new step count and notes
    // starting beyond the new length are simply not played.
    bool configure(TimeSignature ts, int bars, std::int64_t stepTicks) {
        if (!ts.valid() || bars < 1 || bars > MaxBars ||
            stepTicks < MinStepTicks || stepTicks > MaxStepTicks) {
            return false;
        }
        const std::int64_t length = ts.ticksPerBar() * bars;
        if (length % stepTicks != 0 ||
            length / stepTicks > static_cast<std::int64_t>(MaxSteps)) {
            return false;
        }
        timeSignature_ = ts;
        bars_ = bars;
        stepTicks_ = stepTicks;
        lengthTicks_ = length;
        stepCount_ = static_cast<std::size_t>(length / stepTicks);
        return true;
    }

    const TimeSignature& timeSignature() const { return timeSignature_; }
    int bars() const { return bars_; }
    std::int64_t stepTicks() const { return stepTicks_; }
    std::int64_t lengthTicks() const { return lengthTicks_; }
    std::size_t stepCount() const { return stepCount_; }

    // ---- Drum steps -------------------------------------------------------------

    bool setStep(std::size_t pad, std::size_t step, const DrumStep& value) {
        if (pad >= Lanes || step >= stepCount_ || !validStep(value)) {
            return false;
        }
        steps_[pad][step] = value;
        return true;
    }

    // Convenience: enable a step with a velocity (probability 100, on grid).
    bool setHit(std::size_t pad, std::size_t step, int velocity = 100) {
        DrumStep s;
        s.enabled = true;
        s.velocity = velocity;
        return setStep(pad, step, s);
    }

    bool clearStep(std::size_t pad, std::size_t step) {
        if (pad >= Lanes || step >= MaxSteps) {
            return false;
        }
        steps_[pad][step] = DrumStep{};
        return true;
    }

    // nullptr for an invalid index (any of the MaxSteps slots can be read).
    const DrumStep* step(std::size_t pad, std::size_t step) const {
        return (pad < Lanes && step < MaxSteps) ? &steps_[pad][step] : nullptr;
    }

    bool clearPad(std::size_t pad) {
        if (pad >= Lanes) {
            return false;
        }
        steps_[pad].fill(DrumStep{});
        return true;
    }

    void clearDrums() {
        for (auto& lane : steps_) {
            lane.fill(DrumStep{});
        }
    }

    // ---- Notes ----------------------------------------------------------------------

    // Returns the note's index, or -1 if the note is invalid or the list is
    // full.
    int addNote(const NoteEvent& note) {
        if (noteCount_ == MaxNotes || !validNote(note)) {
            return -1;
        }
        notes_[noteCount_] = note;
        return static_cast<int>(noteCount_++);
    }

    bool setNote(std::size_t index, const NoteEvent& note) {
        if (index >= noteCount_ || !validNote(note)) {
            return false;
        }
        notes_[index] = note;
        return true;
    }

    // Removes a note; later notes move down one index (order is kept).
    bool removeNote(std::size_t index) {
        if (index >= noteCount_) {
            return false;
        }
        for (std::size_t i = index; i + 1 < noteCount_; ++i) {
            notes_[i] = notes_[i + 1];
        }
        --noteCount_;
        return true;
    }

    const NoteEvent* note(std::size_t index) const {
        return index < noteCount_ ? &notes_[index] : nullptr;
    }

    std::size_t noteCount() const { return noteCount_; }
    void clearNotes() { noteCount_ = 0; }

    // Clears steps and notes (keeps the shape).
    void clear() {
        clearDrums();
        clearNotes();
    }

    // ---- Exact restore (Engine 028: pattern bank / persistence) --------------------
    //
    // Unlike setStep/addNote these only check bounds and capacity, not the
    // current shape, because a pattern may legitimately hold steps beyond
    // its step count or notes beyond its length (configure() keeps them).
    // Used to restore a pattern exactly as it was saved.

    bool restoreStep(std::size_t pad, std::size_t step, const DrumStep& value) {
        if (pad >= Lanes || step >= MaxSteps) {
            return false;
        }
        steps_[pad][step] = value;
        return true;
    }

    bool restoreNote(const NoteEvent& note) {
        if (noteCount_ == MaxNotes) {
            return false;
        }
        notes_[noteCount_++] = note;
        return true;
    }

    // ---- Validation ---------------------------------------------------------------

    bool validStep(const DrumStep& s) const {
        return s.velocity >= 1 && s.velocity <= 127 &&
               s.probability >= 0 && s.probability <= 100 &&
               s.microTicks >= -stepTicks_ / 2 && s.microTicks <= stepTicks_ / 2;
    }

    bool validNote(const NoteEvent& n) const {
        const bool targetOk = n.target == SynthTarget::Mono ||
                              n.target == SynthTarget::Fm ||
                              n.target == SynthTarget::Poly;
        return targetOk && n.note >= 0 && n.note <= 127 &&
               n.velocity >= 1 && n.velocity <= 127 &&
               n.probability >= 0 && n.probability <= 100 &&
               n.startTick >= 0 && n.startTick < lengthTicks_ &&
               n.lengthTicks >= 1 && n.lengthTicks <= MaxNoteLengthTicks;
    }

private:
    TimeSignature timeSignature_{};
    int bars_ = 1;
    std::int64_t stepTicks_ = 240;
    std::int64_t lengthTicks_ = 3840;
    std::size_t stepCount_ = 16;

    std::array<std::array<DrumStep, MaxSteps>, Lanes> steps_{};
    std::array<NoteEvent, MaxNotes> notes_{};
    std::size_t noteCount_ = 0;
};

} // namespace databreaks
