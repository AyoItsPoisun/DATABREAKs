
#pragma once

// DATABREAKs Engine 026 - recorded performance storage (host-side C++17).
//
// A RecordedTrack holds the musical events captured by the Recorder (not
// audio): drum hits and synth notes, with their raw, unquantised timing.
// The Sequencer plays it back alongside the Pattern, through the same
// SequencerEvent kinds (Drum, NoteOn, NoteOff).
//
// Timing (see musical_clock.h): positions are FINE TICKS, 1/1024 of a tick
// (1/983040 of a quarter note). startFine is loop-relative, in
// [0, loopLengthFine); the absolute capture sample and fine position are
// kept as well. Nothing is quantised: nearestStep() and timingErrorFine()
// give a later quantiser (Engine 027) the grid position and the signed
// error without changing the stored data.
//
// Passes and undo: every event records the pass that created it and, if a
// Replace pass erased it, that pass. Nothing is deleted while a pass can
// still be undone:
//   visible(e) = !pending && created by an active (or permanent) pass &&
//                not erased by an active pass.
// Undo marks the newest active pass "undone": its events disappear and the
// events it erased reappear, which restores the exact previous state.
// Redo re-activates the oldest undone pass. Opening a new pass discards the
// undone ones (physically). At most MaxPasses passes are kept; opening one
// more makes the oldest permanent (no longer undoable).
//
// All storage is fixed (std::array); nothing allocates. Removal compacts
// the array and keeps the relative order of the remaining events.
//
// Threading: owned by the audio thread (the Sequencer); other threads use
// the Sequencer's command queue.

#include "../audio/synth_event.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

struct RecordedEvent {
    enum class Kind : std::uint8_t { Drum, Note };

    Kind kind = Kind::Drum;
    SynthTarget target = SynthTarget::Mono;  // notes only
    int value = 0;                 // pad 0..26 or MIDI note 0..127
    int velocity = 0;              // 1..127
    std::int64_t startFine = 0;    // loop-relative fine position
    std::int64_t lengthFine = 0;   // notes (0 for drums / while pending)
    std::int64_t captureSample = 0;  // absolute input timestamp (samples)
    std::int64_t captureFine = 0;    // absolute fine position at capture
    std::int64_t captureLoop = 0;    // loop index at capture
    std::uint32_t captureEpoch = 0;  // timeline epoch (Sequencer resets)
    std::uint64_t sequence = 0;      // capture order (ties)
    std::uint32_t createdPass = 0;   // 0 = permanent
    std::uint32_t erasedPass = 0;    // 0 = not erased
    bool pending = false;            // note still held (length unknown)

    // Nearest grid step for a step size in ticks (rounding half up), and
    // the signed error in fine ticks (positive = played late).
    std::int64_t nearestStep(std::int64_t stepTicks) const {
        const std::int64_t stepFine = stepTicks * 1024;
        return (startFine + stepFine / 2) / stepFine;
    }
    std::int64_t timingErrorFine(std::int64_t stepTicks) const {
        return startFine - nearestStep(stepTicks) * stepTicks * 1024;
    }

    bool sameContent(const RecordedEvent& o) const {
        return kind == o.kind && target == o.target && value == o.value &&
               velocity == o.velocity && startFine == o.startFine &&
               lengthFine == o.lengthFine && captureSample == o.captureSample &&
               captureFine == o.captureFine && captureLoop == o.captureLoop &&
               captureEpoch == o.captureEpoch &&
               sequence == o.sequence && pending == o.pending;
    }
};

// Bit mask of what a pass records / replaces.
enum RecordTargets : unsigned {
    RecordDrums = 1u << 0,
    RecordMono = 1u << 1,
    RecordFm = 1u << 2,
    RecordPoly = 1u << 3,
    RecordAll = 0xFu
};

inline unsigned targetBit(const RecordedEvent& e) {
    if (e.kind == RecordedEvent::Kind::Drum) return RecordDrums;
    switch (e.target) {
    case SynthTarget::Mono: return RecordMono;
    case SynthTarget::Fm: return RecordFm;
    case SynthTarget::Poly: return RecordPoly;
    }
    return 0;
}

class RecordedTrack {
public:
    static constexpr std::size_t MaxEvents = 2048;
    static constexpr std::size_t MaxEventsPerPass = 512;
    static constexpr std::size_t MaxPasses = 16;  // undo depth

    // ---- Passes ------------------------------------------------------------

    // Discards undone passes, makes the oldest permanent if MaxPasses are
    // kept, and returns the new pass id (> 0).
    std::uint32_t openPass() {
        purgeUndone();
        if (passCount_ == MaxPasses) {
            flattenOldest();
        }
        const std::uint32_t id = nextPassId_++;
        passes_[passCount_++] = {id, true, 0};
        markDirty();
        return id;
    }

    // Undo the newest active pass. Returns its id, or 0 if none.
    std::uint32_t undo() {
        for (std::size_t i = passCount_; i > 0; --i) {
            if (passes_[i - 1].active) {
                passes_[i - 1].active = false;
                markDirty();
                return passes_[i - 1].id;
            }
        }
        return 0;
    }

    // Redo the oldest undone pass. Returns its id, or 0 if none.
    std::uint32_t redo() {
        for (std::size_t i = 0; i < passCount_; ++i) {
            if (!passes_[i].active) {
                passes_[i].active = true;
                markDirty();
                return passes_[i].id;
            }
        }
        return 0;
    }

    std::size_t undoablePassCount() const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < passCount_; ++i) n += passes_[i].active ? 1 : 0;
        return n;
    }
    std::size_t redoablePassCount() const { return passCount_ - undoablePassCount(); }

    bool passActive(std::uint32_t id) const {
        if (id == 0) return true;
        for (std::size_t i = 0; i < passCount_; ++i) {
            if (passes_[i].id == id) return passes_[i].active;
        }
        return false;  // purged (undone and discarded)
    }

    // ---- Events ------------------------------------------------------------

    // Adds an event to `pass`. Returns its index, or -1 if the track or the
    // pass is full (nothing is written; the overflow is counted).
    int add(const RecordedEvent& event, std::uint32_t pass) {
        PassInfo* info = find(pass);
        if (count_ == MaxEvents) {
            ++trackFull_;
            return -1;
        }
        if (info != nullptr && info->events == MaxEventsPerPass) {
            ++passFull_;
            return -1;
        }
        events_[count_] = event;
        events_[count_].createdPass = pass;
        events_[count_].erasedPass = 0;
        if (info != nullptr) ++info->events;
        markDirty();
        return static_cast<int>(count_++);
    }

    // Replace: marks visible events of the masked kinds whose startFine is
    // in [fromFine, toFine) as erased by `pass` (events of `pass` itself
    // are kept). Returns how many were erased.
    std::size_t erase(std::int64_t fromFine, std::int64_t toFine, unsigned mask,
                      std::uint32_t pass) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            RecordedEvent& e = events_[i];
            if (e.createdPass == pass || !visible(i)) continue;
            if ((targetBit(e) & mask) == 0) continue;
            if (e.startFine < fromFine || e.startFine >= toFine) continue;
            e.erasedPass = pass;
            ++n;
        }
        if (n > 0) markDirty();
        return n;
    }

    // Number of visible events of the masked kinds with startFine in
    // [fromFine, toFine) (what a new Replace pass would erase there).
    std::size_t countErasable(std::int64_t fromFine, std::int64_t toFine,
                              unsigned mask) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const RecordedEvent& e = events_[i];
            if (!visible(i) || (targetBit(e) & mask) == 0) continue;
            if (e.startFine >= fromFine && e.startFine < toFine) ++n;
        }
        return n;
    }

    // Finishes a held note. Returns false if `sequence` is not found.
    bool finishNote(std::uint64_t sequence, std::int64_t lengthFine) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (events_[i].sequence == sequence && events_[i].pending) {
                events_[i].lengthFine = lengthFine;
                events_[i].pending = false;
                markDirty();
                return true;
            }
        }
        return false;
    }

    // Removes every event and pass.
    void clear() {
        count_ = 0;
        passCount_ = 0;
        markDirty();
    }

    std::size_t size() const { return count_; }
    const RecordedEvent& at(std::size_t i) const { return events_[i]; }

    bool visible(std::size_t i) const {
        if (dirty_) refresh();
        return visible_[i];
    }

    std::size_t visibleCount() const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) n += visible(i) ? 1 : 0;
        return n;
    }

    // ---- Exact save / restore (Engine 028) ---------------------------------

    struct PassState {
        std::uint32_t id = 0;
        bool active = true;
    };

    std::size_t passCount() const { return passCount_; }
    PassState pass(std::size_t i) const { return {passes_[i].id, passes_[i].active}; }
    std::uint32_t nextPassId() const { return nextPassId_; }

    // Replaces the whole contents (events, passes, pass-id counter) with the
    // given data, after validating it; returns false and changes nothing if
    // it is inconsistent: too many events/passes, pass ids zero, duplicated
    // or >= nextPassId, events referring to unknown passes, more than
    // MaxEventsPerPass events in one pass, an invalid event field. Statistics
    // counters are not part of the contents and are kept.
    bool restore(const RecordedEvent* events, std::size_t eventCount,
                 const PassState* passes, std::size_t passCount, std::uint32_t nextPassId) {
        return restoreFrom(eventCount, [events](std::size_t i) { return events[i]; }, passes,
                           passCount, nextPassId);
    }

    // Same, reading event i through get(i) (twice: validate, then copy), so
    // packed storage can be restored without an intermediate buffer.
    template <typename Get>
    bool restoreFrom(std::size_t eventCount, Get get, const PassState* passes,
                     std::size_t passCount, std::uint32_t nextPassId) {
        if (eventCount > MaxEvents || passCount > MaxPasses || nextPassId == 0) return false;
        std::array<std::size_t, MaxPasses> perPass{};
        for (std::size_t i = 0; i < passCount; ++i) {
            if (passes[i].id == 0 || passes[i].id >= nextPassId) return false;
            for (std::size_t k = 0; k < i; ++k) {
                if (passes[k].id == passes[i].id) return false;
            }
        }
        auto indexOf = [&](std::uint32_t id) -> int {
            for (std::size_t k = 0; k < passCount; ++k) {
                if (passes[k].id == id) return static_cast<int>(k);
            }
            return -1;
        };
        for (std::size_t i = 0; i < eventCount; ++i) {
            const RecordedEvent e = get(i);
            if (!validEvent(e)) return false;
            if (e.createdPass != 0) {
                const int k = indexOf(e.createdPass);
                if (k < 0 || ++perPass[static_cast<std::size_t>(k)] > MaxEventsPerPass) return false;
            }
            if (e.erasedPass != 0 && indexOf(e.erasedPass) < 0) return false;
        }
        for (std::size_t i = 0; i < eventCount; ++i) events_[i] = get(i);
        count_ = eventCount;
        for (std::size_t i = 0; i < passCount; ++i) {
            passes_[i] = {passes[i].id, passes[i].active, perPass[i]};
        }
        passCount_ = passCount;
        nextPassId_ = nextPassId;
        markDirty();
        return true;
    }

    // Field ranges of a stored event (used by restore and by loaders).
    static bool validEvent(const RecordedEvent& e) {
        if (e.kind == RecordedEvent::Kind::Drum) {
            if (e.value < 0 || e.value > 26 || e.pending || e.lengthFine != 0) return false;
        } else if (e.kind == RecordedEvent::Kind::Note) {
            if (e.value < 0 || e.value > 127) return false;
            if (e.target != SynthTarget::Mono && e.target != SynthTarget::Fm &&
                e.target != SynthTarget::Poly) {
                return false;
            }
            if (e.lengthFine < 0 || (!e.pending && e.lengthFine < 1)) return false;
        } else {
            return false;
        }
        return e.velocity >= 1 && e.velocity <= 127 && e.startFine >= 0 && e.captureSample >= 0;
    }

    // Increases on every change to the stored events or pass states
    // (Engine 027: lets derived caches know when to refresh).
    std::uint64_t revision() const { return revision_; }

    // Statistics.
    std::uint64_t trackFullCount() const { return trackFull_; }
    std::uint64_t passFullCount() const { return passFull_; }
    std::uint64_t flattenedPassCount() const { return flattened_; }

private:
    struct PassInfo {
        std::uint32_t id = 0;
        bool active = true;
        std::size_t events = 0;
    };

    std::array<RecordedEvent, MaxEvents> events_{};
    std::size_t count_ = 0;
    std::array<PassInfo, MaxPasses> passes_{};
    std::size_t passCount_ = 0;
    std::uint32_t nextPassId_ = 1;

    mutable std::array<bool, MaxEvents> visible_{};
    mutable bool dirty_ = true;
    std::uint64_t revision_ = 0;  // Engine 027: changes on every mutation

    void markDirty() {
        dirty_ = true;
        ++revision_;
    }

    std::uint64_t trackFull_ = 0;
    std::uint64_t passFull_ = 0;
    std::uint64_t flattened_ = 0;

    PassInfo* find(std::uint32_t id) {
        for (std::size_t i = 0; i < passCount_; ++i) {
            if (passes_[i].id == id) return &passes_[i];
        }
        return nullptr;
    }

    void refresh() const {
        for (std::size_t i = 0; i < count_; ++i) {
            const RecordedEvent& e = events_[i];
            visible_[i] = !e.pending && passActive(e.createdPass) &&
                          !(e.erasedPass != 0 && passActive(e.erasedPass));
        }
        dirty_ = false;
    }

    // Keeps events for which keep(e) is true, in order; returns removed.
    template <typename Keep>
    void compact(Keep keep) {
        std::size_t out = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (keep(events_[i])) events_[out++] = events_[i];
        }
        count_ = out;
        markDirty();
    }

    void purgeUndone() {
        std::size_t out = 0;
        for (std::size_t i = 0; i < passCount_; ++i) {
            if (passes_[i].active) {
                passes_[out++] = passes_[i];
                continue;
            }
            const std::uint32_t id = passes_[i].id;
            compact([id](const RecordedEvent& e) { return e.createdPass != id; });
            for (std::size_t k = 0; k < count_; ++k) {
                if (events_[k].erasedPass == id) events_[k].erasedPass = 0;
            }
        }
        passCount_ = out;
        markDirty();
    }

    void flattenOldest() {
        const std::uint32_t id = passes_[0].id;
        compact([id](const RecordedEvent& e) { return e.erasedPass != id; });
        for (std::size_t k = 0; k < count_; ++k) {
            if (events_[k].createdPass == id) events_[k].createdPass = 0;
        }
        for (std::size_t i = 1; i < passCount_; ++i) passes_[i - 1] = passes_[i];
        --passCount_;
        ++flattened_;
        markDirty();
    }
};

} // namespace databreaks
