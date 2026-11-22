
#pragma once

// DATABREAKs Engine 028 - pattern bank (host-side C++17).
//
// Stores many patterns COMPACTLY. One full working pattern lives in the
// Sequencer (Pattern + RecordedTrack + QuantizeSettings, the Engine 025-027
// objects, ~310 KB); multiplying that by the number of patterns would cost
// about 5 MB for 16 patterns. The bank instead keeps every pattern in
// shared, fixed-capacity pools:
//   * drum steps: only steps that differ from DrumStep{} (sparse), 8 B each;
//   * notes: packed, 16 B each;
//   * recorded events: packed, 56 B each (every RecordedEvent field, exact;
//     captureLoop is checked to fit 32 bits);
//   * per slot: shape, quantize settings, pool ranges and the recording's
//     pass table (undo/redo state).
// A pattern can use any share of the pools; the pools limit the total.
//
// Organisation: Banks x PatternsPerBank slots. A pattern ID is stable and
// equals bank x PatternsPerBank + slot (0 .. Capacity - 1). A slot is Empty
// or Used. Empty slots read as an empty default pattern (4/4, 1 bar, 16th
// steps, no data, quantize off).
//
// Every operation validates its arguments and either succeeds completely
// or changes nothing:
//   * store(): returns false (and counts it) when the pools cannot hold
//     the pattern; the previous contents of the slot are kept;
//   * duplicate(): returns -1 when no slot is empty or the pools are full.
// Pool space is compacted on every store/clear (bounded O(pool size)
// moves), so free space is never fragmented.
//
// Threading: the Sequencer owns its bank on the audio thread. A standalone
// bank (e.g. one being loaded from a file) may be used by any single
// thread. No allocation, no locks.
//
// Zero-initialised storage (Engine 031): a default-constructed bank is
// ALL ZERO BYTES. Records default to zero (store() writes every field
// before a record is used, and records past a pool's used count are never
// read), and each slot's metadata is stored so that zero bytes decode to
// the empty default slot (see SlotStore). A static bank is therefore placed
// in .bss, not in initialised data: on a microcontroller it costs no flash
// image and no start-up copy. Behaviour and the file format are unchanged
// (tests/audio/engine_031.cpp compares against Engine 030 fingerprints).

#include "pattern.h"
#include "quantize.h"
#include "recorded_track.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace databreaks {

// Profile (Engine 032) selects the working-state types a slot is stored
// from and loaded into (BasicPattern<Profile>, BasicRecordedTrack<Profile>).
// The pools and slot counts stay explicit; the stored records and the DBPB
// format are the same for every profile.
template <std::size_t Banks, std::size_t PatternsPerBank, std::size_t StepCapacity,
          std::size_t NoteCapacity, std::size_t EventCapacity, typename Profile = HostProfile>
class PatternBank {
public:
    using ProfileType = Profile;
    using PatternType = BasicPattern<Profile>;
    using TrackType = BasicRecordedTrack<Profile>;
    static_assert(Banks >= 1 && PatternsPerBank >= 1, "need at least one slot");
    static constexpr std::size_t BankCount = Banks;
    static constexpr std::size_t PerBank = PatternsPerBank;
    static constexpr std::size_t Capacity = Banks * PatternsPerBank;
    static constexpr std::size_t StepPool = StepCapacity;
    static constexpr std::size_t NotePool = NoteCapacity;
    static constexpr std::size_t EventPool = EventCapacity;
    static_assert(Capacity <= 65535, "pattern ids are 16-bit in the file format");

    // Packed storage records (explicit fields; see pattern_io.h for the
    // on-disk format, which is separate and endian-independent). Every
    // field defaults to zero so that the pools are zero-initialised; the
    // bank writes every field of a record before it is used.
    struct StepRecord {
        std::uint8_t pad = 0;
        std::uint8_t step = 0;
        std::uint8_t enabled = 0;
        std::uint8_t velocity = 0;
        std::uint8_t probability = 0;
        std::int16_t microTicks = 0;
    };
    struct NoteRecord {
        std::uint8_t enabled = 0;
        SynthTarget target = SynthTarget{};
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint8_t probability = 0;
        std::int32_t startTick = 0;
        std::int32_t lengthTicks = 0;
    };
    struct EventRecord {  // 56 bytes
        RecordedEvent::Kind kind = RecordedEvent::Kind{};
        SynthTarget target = SynthTarget{};
        std::uint8_t value = 0;
        std::uint8_t velocity = 0;
        std::uint8_t pending = 0;
        std::int32_t startFine = 0;   // < loop length <= 1,966,080 ticks x 1024
        std::int32_t lengthFine = 0;  // <= 61,440 ticks x 1024
        std::int32_t captureLoop = 0;  // loop index (checked by packable())
        std::uint32_t captureEpoch = 0;
        std::uint32_t createdPass = 0;
        std::uint32_t erasedPass = 0;
        std::int64_t captureSample = 0;
        std::int64_t captureFine = 0;
        std::uint64_t sequence = 0;
    };

    // Shape of an empty slot (with TimeSignature{}: 4/4, one bar of 16ths).
    static constexpr int DefaultBars = 1;
    static constexpr std::int64_t DefaultStepTicks = TicksPerQuarter / 4;

    struct SlotInfo {
        bool used = false;
        TimeSignature timeSignature{};
        int bars = DefaultBars;
        std::int64_t stepTicks = DefaultStepTicks;
        QuantizeSettings quantize{};
        std::size_t steps = 0;
        std::size_t notes = 0;
        std::size_t events = 0;
        std::size_t passes = 0;
    };

    // ---- IDs ---------------------------------------------------------------

    static int idOf(std::size_t bank, std::size_t slot) {
        return (bank < Banks && slot < PatternsPerBank)
                   ? static_cast<int>(bank * PatternsPerBank + slot)
                   : -1;
    }
    static bool validId(int id) { return id >= 0 && static_cast<std::size_t>(id) < Capacity; }
    static std::size_t bankOf(int id) { return static_cast<std::size_t>(id) / PatternsPerBank; }
    static std::size_t slotOf(int id) { return static_cast<std::size_t>(id) % PatternsPerBank; }

    // ---- Queries -------------------------------------------------------------

    bool used(int id) const { return validId(id) && slots_[static_cast<std::size_t>(id)].used; }

    SlotInfo info(int id) const {
        SlotInfo r;
        if (!validId(id)) return r;
        const SlotStore& s = slots_[static_cast<std::size_t>(id)];
        r.used = s.used;
        r.timeSignature = s.timeSignature();
        r.bars = s.bars();
        r.stepTicks = s.stepTicks();
        r.quantize = s.quantize();
        r.steps = s.stepCount;
        r.notes = s.noteCount;
        r.events = s.eventCount;
        r.passes = s.passCount;
        return r;
    }

    std::size_t usedCount() const {
        std::size_t n = 0;
        for (const SlotStore& s : slots_) n += s.used ? 1 : 0;
        return n;
    }
    // Lowest empty slot, or -1.
    int firstEmpty() const {
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (!slots_[i].used) return static_cast<int>(i);
        }
        return -1;
    }
    std::size_t stepsUsed() const { return stepsUsed_; }
    std::size_t notesUsed() const { return notesUsed_; }
    std::size_t eventsUsed() const { return eventsUsed_; }
    std::uint64_t fullCount() const { return full_; }

    // Pool requirements of a working pattern (what store() would need).
    static std::size_t stepsNeeded(const PatternType& p) {
        std::size_t n = 0;
        for (std::size_t pad = 0; pad < PatternType::Lanes; ++pad) {
            for (std::size_t k = 0; k < PatternType::MaxSteps; ++k) {
                n += isDefault(*p.step(pad, k)) ? 0 : 1;
            }
        }
        return n;
    }

    // ---- Store / load ------------------------------------------------------------

    // Saves a complete pattern state into slot `id` (marking it used).
    // Returns false, changes nothing and counts it if `id` is invalid, the
    // data cannot be packed, or the pools cannot hold it.
    bool store(int id, const PatternType& pattern, const TrackType& track,
               const QuantizeSettings& quantize) {
        if (!validId(id) || !quantize.valid()) return false;
        SlotStore& s = slots_[static_cast<std::size_t>(id)];
        const std::size_t steps = stepsNeeded(pattern);
        const std::size_t notes = pattern.noteCount();
        const std::size_t events = track.size();
        if (stepsUsed_ - s.stepCount + steps > StepCapacity ||
            notesUsed_ - s.noteCount + notes > NoteCapacity ||
            eventsUsed_ - s.eventCount + events > EventCapacity) {
            ++full_;
            return false;
        }
        for (std::size_t i = 0; i < events; ++i) {
            if (!packable(track.at(i))) return false;
        }
        release(static_cast<std::size_t>(id));
        s.stepOffset = stepsUsed_;
        for (std::size_t pad = 0; pad < PatternType::Lanes; ++pad) {
            for (std::size_t k = 0; k < PatternType::MaxSteps; ++k) {
                const DrumStep& d = *pattern.step(pad, k);
                if (isDefault(d)) continue;
                StepRecord r;
                r.pad = static_cast<std::uint8_t>(pad);
                r.step = static_cast<std::uint8_t>(k);
                r.enabled = d.enabled ? 1 : 0;
                r.velocity = static_cast<std::uint8_t>(d.velocity);
                r.probability = static_cast<std::uint8_t>(d.probability);
                r.microTicks = static_cast<std::int16_t>(d.microTicks);
                steps_[stepsUsed_++] = r;
            }
        }
        s.stepCount = steps;
        s.noteOffset = notesUsed_;
        for (std::size_t i = 0; i < notes; ++i) {
            const NoteEvent& n = *pattern.note(i);
            NoteRecord r;
            r.enabled = n.enabled ? 1 : 0;
            r.target = n.target;
            r.note = static_cast<std::uint8_t>(n.note);
            r.velocity = static_cast<std::uint8_t>(n.velocity);
            r.probability = static_cast<std::uint8_t>(n.probability);
            r.startTick = static_cast<std::int32_t>(n.startTick);
            r.lengthTicks = static_cast<std::int32_t>(n.lengthTicks);
            notes_[notesUsed_++] = r;
        }
        s.noteCount = notes;
        s.eventOffset = eventsUsed_;
        for (std::size_t i = 0; i < events; ++i) events_[eventsUsed_++] = pack(track.at(i));
        s.eventCount = events;
        s.passCount = track.passCount();
        for (std::size_t i = 0; i < s.passCount; ++i) s.setPass(i, track.pass(i));
        s.setNextPassId(track.nextPassId());
        s.setShape(pattern.timeSignature(), pattern.bars(), pattern.stepTicks());
        s.setQuantize(quantize);
        s.used = true;
        return true;
    }

    // Loads slot `id` into a working pattern state (an empty slot loads the
    // empty default pattern). Returns false for an invalid id.
    bool load(int id, PatternType& pattern, TrackType& track, QuantizeSettings& quantize) const {
        if (!validId(id)) return false;
        const SlotStore& s = slots_[static_cast<std::size_t>(id)];
        pattern.configure(s.timeSignature(), s.bars(), s.stepTicks());
        pattern.clear();
        for (std::size_t i = 0; i < s.stepCount; ++i) {
            const StepRecord& r = steps_[s.stepOffset + i];
            DrumStep d;
            d.enabled = r.enabled != 0;
            d.velocity = r.velocity;
            d.probability = r.probability;
            d.microTicks = r.microTicks;
            pattern.restoreStep(r.pad, r.step, d);
        }
        for (std::size_t i = 0; i < s.noteCount; ++i) {
            const NoteRecord& r = notes_[s.noteOffset + i];
            NoteEvent n;
            n.enabled = r.enabled != 0;
            n.target = r.target;
            n.note = r.note;
            n.velocity = r.velocity;
            n.probability = r.probability;
            n.startTick = r.startTick;
            n.lengthTicks = r.lengthTicks;
            pattern.restoreNote(n);
        }
        // Unpacked straight into the track (no intermediate buffer).
        const EventRecord* records = events_.data() + s.eventOffset;
        std::array<RecordedPassState, TrackType::MaxPasses> passes{};
        for (std::size_t i = 0; i < passes.size(); ++i) passes[i] = s.pass(i);
        track.restoreFrom(s.eventCount, [records](std::size_t i) { return unpack(records[i]); },
                          passes.data(), s.passCount, s.nextPassId());
        quantize = s.quantize();
        return true;
    }

    // ---- Editing -------------------------------------------------------------------

    // Empties slot `id` completely (it becomes Empty). Returns false for an
    // invalid id.
    bool erase(int id) {
        if (!validId(id)) return false;
        release(static_cast<std::size_t>(id));
        slots_[static_cast<std::size_t>(id)] = SlotStore{};
        return true;
    }

    // Removes the musical contents (steps, notes, recording and undo
    // history) and keeps the shape and quantize settings; the slot is used.
    bool clearContents(int id) {
        if (!validId(id)) return false;
        SlotStore& s = slots_[static_cast<std::size_t>(id)];
        release(static_cast<std::size_t>(id));
        s.passCount = 0;
        s.setNextPassId(1);
        s.used = true;
        return true;
    }

    // dst := src (everything: shape, steps, notes, recording, undo history,
    // quantize). Copying an empty slot empties dst. Fails (nothing changes)
    // for invalid ids or full pools; src == dst is a successful no-op.
    bool copy(int src, int dst) {
        if (!validId(src) || !validId(dst)) return false;
        if (src == dst) return true;
        const SlotStore& a = slots_[static_cast<std::size_t>(src)];
        SlotStore& b = slots_[static_cast<std::size_t>(dst)];
        if (!a.used) return erase(dst);
        if (stepsUsed_ - b.stepCount + a.stepCount > StepCapacity ||
            notesUsed_ - b.noteCount + a.noteCount > NoteCapacity ||
            eventsUsed_ - b.eventCount + a.eventCount > EventCapacity) {
            ++full_;
            return false;
        }
        release(static_cast<std::size_t>(dst));  // may move src's ranges
        const SlotStore& s = slots_[static_cast<std::size_t>(src)];
        SlotStore copySlot = s;
        copySlot.stepOffset = stepsUsed_;
        for (std::size_t i = 0; i < s.stepCount; ++i) steps_[stepsUsed_++] = steps_[s.stepOffset + i];
        copySlot.noteOffset = notesUsed_;
        for (std::size_t i = 0; i < s.noteCount; ++i) notes_[notesUsed_++] = notes_[s.noteOffset + i];
        copySlot.eventOffset = eventsUsed_;
        for (std::size_t i = 0; i < s.eventCount; ++i) events_[eventsUsed_++] = events_[s.eventOffset + i];
        slots_[static_cast<std::size_t>(dst)] = copySlot;
        return true;
    }

    // Copies src into the lowest empty slot; returns its id, or -1.
    int duplicate(int src) {
        if (!validId(src) || !slots_[static_cast<std::size_t>(src)].used) return -1;
        const int dst = firstEmpty();
        if (dst < 0) {
            ++full_;
            return -1;
        }
        return copy(src, dst) ? dst : -1;
    }

    // Changes the shape of a stored pattern (same rules as
    // Pattern::configure: data is kept). Marks the slot used.
    bool setShape(int id, TimeSignature ts, int bars, std::int64_t stepTicks) {
        if (!validId(id)) return false;
        PatternType probe;  // validation only (no allocation: a local object)
        if (!probe.configure(ts, bars, stepTicks)) return false;
        SlotStore& s = slots_[static_cast<std::size_t>(id)];
        s.setShape(ts, bars, stepTicks);
        s.used = true;
        return true;
    }

    bool setQuantize(int id, const QuantizeSettings& q) {
        if (!validId(id) || !q.valid()) return false;
        slots_[static_cast<std::size_t>(id)].setQuantize(q);
        slots_[static_cast<std::size_t>(id)].used = true;
        return true;
    }

    // Removes every pattern.
    void clearAll() {
        for (SlotStore& s : slots_) s = SlotStore{};
        stepsUsed_ = notesUsed_ = eventsUsed_ = 0;
    }

    // Raw read access for serialisation (records of one slot).
    const StepRecord* stepRecords(int id) const {
        return validId(id) ? steps_.data() + slots_[static_cast<std::size_t>(id)].stepOffset : nullptr;
    }
    const NoteRecord* noteRecords(int id) const {
        return validId(id) ? notes_.data() + slots_[static_cast<std::size_t>(id)].noteOffset : nullptr;
    }
    const EventRecord* eventRecords(int id) const {
        return validId(id) ? events_.data() + slots_[static_cast<std::size_t>(id)].eventOffset : nullptr;
    }
    RecordedPassState passState(int id, std::size_t i) const {
        return slots_[static_cast<std::size_t>(id)].pass(i);
    }
    std::uint32_t nextPassId(int id) const { return slots_[static_cast<std::size_t>(id)].nextPassId(); }

    static bool isDefault(const DrumStep& d) {
        const DrumStep z{};
        return d.enabled == z.enabled && d.velocity == z.velocity &&
               d.probability == z.probability && d.microTicks == z.microTicks;
    }

    static bool packable(const RecordedEvent& e) {
        return e.startFine >= 0 && e.startFine <= INT32_MAX && e.lengthFine >= 0 &&
               e.lengthFine <= INT32_MAX && e.captureLoop >= INT32_MIN &&
               e.captureLoop <= INT32_MAX && e.value >= 0 && e.value <= 255 &&
               e.velocity >= 0 && e.velocity <= 255;
    }

    static EventRecord pack(const RecordedEvent& e) {
        EventRecord r;
        r.kind = e.kind;
        r.target = e.target;
        r.value = static_cast<std::uint8_t>(e.value);
        r.velocity = static_cast<std::uint8_t>(e.velocity);
        r.pending = e.pending ? 1 : 0;
        r.captureEpoch = e.captureEpoch;
        r.startFine = static_cast<std::int32_t>(e.startFine);
        r.lengthFine = static_cast<std::int32_t>(e.lengthFine);
        r.captureSample = e.captureSample;
        r.captureFine = e.captureFine;
        r.captureLoop = static_cast<std::int32_t>(e.captureLoop);
        r.sequence = e.sequence;
        r.createdPass = e.createdPass;
        r.erasedPass = e.erasedPass;
        return r;
    }

    static RecordedEvent unpack(const EventRecord& r) {
        RecordedEvent e;
        e.kind = r.kind;
        e.target = r.target;
        e.value = r.value;
        e.velocity = r.velocity;
        e.pending = r.pending != 0;
        e.captureEpoch = r.captureEpoch;
        e.startFine = r.startFine;
        e.lengthFine = r.lengthFine;
        e.captureSample = r.captureSample;
        e.captureFine = r.captureFine;
        e.captureLoop = r.captureLoop;
        e.sequence = r.sequence;
        e.createdPass = r.createdPass;
        e.erasedPass = r.erasedPass;
        return e;
    }

private:
    // One slot's metadata, stored so that ALL-ZERO BYTES decode to the empty
    // default slot (unused; 4/4, one bar of 16ths; QuantizeSettings{}; no
    // data; next pass id 1; pass-table entries {0, active}). Fields whose
    // default is not zero are stored XOR their default: an exact, reversible
    // encoding of every value, so nothing observable changes.
    struct SlotStore {
        bool used = false;
        std::size_t stepOffset = 0, stepCount = 0;
        std::size_t noteOffset = 0, noteCount = 0;
        std::size_t eventOffset = 0, eventCount = 0;
        std::size_t passCount = 0;

        TimeSignature timeSignature() const {
            TimeSignature t;
            t.numerator = numeratorX_ ^ TimeSignature{}.numerator;
            t.denominator = denominatorX_ ^ TimeSignature{}.denominator;
            return t;
        }
        int bars() const { return barsX_ ^ DefaultBars; }
        std::int64_t stepTicks() const { return stepTicksX_ ^ DefaultStepTicks; }
        void setShape(TimeSignature ts, int bars, std::int64_t stepTicks) {
            numeratorX_ = ts.numerator ^ TimeSignature{}.numerator;
            denominatorX_ = ts.denominator ^ TimeSignature{}.denominator;
            barsX_ = bars ^ DefaultBars;
            stepTicksX_ = stepTicks ^ DefaultStepTicks;
        }

        QuantizeSettings quantize() const {
            const QuantizeSettings d{};
            QuantizeSettings q;
            q.grid = static_cast<QuantizeGrid>(gridX_ ^ static_cast<std::uint8_t>(d.grid));
            q.strength = strengthX_ ^ d.strength;
            q.swing = swingX_ ^ d.swing;
            q.targets = targetsX_ ^ d.targets;
            q.drumLanes = drumLanesX_ ^ d.drumLanes;
            q.quantizeNoteOff = noteOffX_ != d.quantizeNoteOff;
            return q;
        }
        void setQuantize(const QuantizeSettings& q) {
            const QuantizeSettings d{};
            gridX_ = static_cast<std::uint8_t>(static_cast<std::uint8_t>(q.grid) ^
                                               static_cast<std::uint8_t>(d.grid));
            strengthX_ = q.strength ^ d.strength;
            swingX_ = q.swing ^ d.swing;
            targetsX_ = q.targets ^ d.targets;
            drumLanesX_ = q.drumLanes ^ d.drumLanes;
            noteOffX_ = q.quantizeNoteOff != d.quantizeNoteOff;
        }

        std::uint32_t nextPassId() const { return nextPassIdX_ ^ 1u; }
        void setNextPassId(std::uint32_t id) { nextPassIdX_ = id ^ 1u; }

        RecordedPassState pass(std::size_t i) const {
            const RecordedPassState d{};
            RecordedPassState p;
            p.id = passIds_[i] ^ d.id;
            p.active = passActiveX_[i] != d.active;
            return p;
        }
        void setPass(std::size_t i, RecordedPassState p) {
            const RecordedPassState d{};
            passIds_[i] = p.id ^ d.id;
            passActiveX_[i] = p.active != d.active;
        }

    private:
        int numeratorX_ = 0;
        int denominatorX_ = 0;
        int barsX_ = 0;
        std::int64_t stepTicksX_ = 0;
        std::uint8_t gridX_ = 0;
        int strengthX_ = 0;
        int swingX_ = 0;
        unsigned targetsX_ = 0;
        std::uint32_t drumLanesX_ = 0;
        bool noteOffX_ = false;
        std::uint32_t nextPassIdX_ = 0;
        std::array<std::uint32_t, TrackType::MaxPasses> passIds_{};
        std::array<bool, TrackType::MaxPasses> passActiveX_{};
    };

    std::array<SlotStore, Capacity> slots_{};
    std::array<StepRecord, StepCapacity> steps_{};
    std::array<NoteRecord, NoteCapacity> notes_{};
    std::array<EventRecord, EventCapacity> events_{};
    std::size_t stepsUsed_ = 0;
    std::size_t notesUsed_ = 0;
    std::size_t eventsUsed_ = 0;
    std::uint64_t full_ = 0;

    // Frees a slot's pool ranges, compacting each pool and fixing the
    // offsets of the slots stored after it. The slot's counts become 0.
    void release(std::size_t id) {
        SlotStore& s = slots_[id];
        removeRange(steps_, stepsUsed_, s.stepOffset, s.stepCount, &SlotStore::stepOffset);
        removeRange(notes_, notesUsed_, s.noteOffset, s.noteCount, &SlotStore::noteOffset);
        removeRange(events_, eventsUsed_, s.eventOffset, s.eventCount, &SlotStore::eventOffset);
        s.stepCount = s.noteCount = s.eventCount = 0;
        s.stepOffset = s.noteOffset = s.eventOffset = 0;
    }

    template <typename Pool>
    void removeRange(Pool& pool, std::size_t& used, std::size_t offset, std::size_t count,
                     std::size_t SlotStore::*field) {
        if (count == 0) return;
        std::copy(pool.begin() + static_cast<std::ptrdiff_t>(offset + count),
                  pool.begin() + static_cast<std::ptrdiff_t>(used),
                  pool.begin() + static_cast<std::ptrdiff_t>(offset));
        used -= count;
        for (SlotStore& other : slots_) {
            if (other.*field > offset) other.*field -= count;
        }
    }
};

// A complete pattern state prepared by another thread for
// SequencerCommandType::ReplacePattern (or loaded from a file). The sender
// keeps it alive until `status` becomes non-zero (1 = applied, -1 =
// rejected); the audio thread only reads it while status is 0. About
// 310 KB for the host profile: allocate it outside the audio thread.
template <typename Profile>
struct BasicPatternData {
    BasicPattern<Profile> pattern;
    BasicRecordedTrack<Profile> track;
    QuantizeSettings quantize{};
    std::atomic<int> status{0};
};
using PatternData = BasicPatternData<HostProfile>;

// The Sequencer's bank: 2 banks x 8 patterns = 16 patterns, sharing 8,192
// drum steps (more than one fully dense 27 x 256 pattern), 1,024 notes and
// 2,048 recorded events (one full Engine 026 track). See the progress report
// for the memory reasoning.
using DefaultPatternBank = PatternBank<2, 8, 8192, 1024, 2048>;

// The bank a target profile's Sequencer owns (Engine 032): slots and pools
// from the profile. ProfilePatternBank<HostProfile> is DefaultPatternBank.
template <typename P>
using ProfilePatternBank = PatternBank<P::PatternBanks, P::PatternsPerBank, P::BankStepPool,
                                       P::BankNotePool, P::BankEventPool, P>;

} // namespace databreaks
