// Engine 031 - observable-state fingerprint of a PatternBank (test helper).
// Hashes everything the bank's public API can show: every slot's info,
// pass table (all MaxPasses entries), records, and the full working state
// that load() produces for every slot.
#pragma once

#include "../../core/sequencer/pattern_bank.h"
#include "../../core/sequencer/pattern_io.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace e031 {

using namespace databreaks;

struct Fnv {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    void u(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void i(std::int64_t v) { u(static_cast<std::uint64_t>(v)); }
};

inline void hashQuantize(Fnv& f, const QuantizeSettings& q) {
    f.u(static_cast<std::uint64_t>(q.grid));
    f.i(q.strength);
    f.i(q.swing);
    f.u(q.targets);
    f.u(q.drumLanes);
    f.u(q.quantizeNoteOff ? 1 : 0);
}

inline void hashEvent(Fnv& f, const RecordedEvent& e) {
    f.u(static_cast<std::uint64_t>(e.kind));
    f.u(static_cast<std::uint64_t>(e.target));
    f.i(e.value);
    f.i(e.velocity);
    f.i(e.startFine);
    f.i(e.lengthFine);
    f.i(e.captureSample);
    f.i(e.captureFine);
    f.i(e.captureLoop);
    f.u(e.captureEpoch);
    f.u(e.sequence);
    f.u(e.createdPass);
    f.u(e.erasedPass);
    f.u(e.pending ? 1 : 0);
}

inline void hashWorking(Fnv& f, const Pattern& p, const RecordedTrack& t, const QuantizeSettings& q) {
    f.i(p.timeSignature().numerator);
    f.i(p.timeSignature().denominator);
    f.i(p.bars());
    f.i(p.stepTicks());
    f.i(p.lengthTicks());
    f.u(p.stepCount());
    for (std::size_t pad = 0; pad < Pattern::Lanes; ++pad) {
        for (std::size_t k = 0; k < Pattern::MaxSteps; ++k) {
            const DrumStep& d = *p.step(pad, k);
            f.u(d.enabled ? 1 : 0);
            f.i(d.velocity);
            f.i(d.probability);
            f.i(d.microTicks);
        }
    }
    f.u(p.noteCount());
    for (std::size_t n = 0; n < p.noteCount(); ++n) {
        const NoteEvent& e = *p.note(n);
        f.u(e.enabled ? 1 : 0);
        f.u(static_cast<std::uint64_t>(e.target));
        f.i(e.note);
        f.i(e.velocity);
        f.i(e.probability);
        f.i(e.startTick);
        f.i(e.lengthTicks);
    }
    f.u(t.size());
    for (std::size_t n = 0; n < t.size(); ++n) {
        hashEvent(f, t.at(n));
        f.u(t.visible(n) ? 1 : 0);
    }
    f.u(t.passCount());
    for (std::size_t n = 0; n < t.passCount(); ++n) {
        f.u(t.pass(n).id);
        f.u(t.pass(n).active ? 1 : 0);
    }
    f.u(t.nextPassId());
    f.u(t.undoablePassCount());
    hashQuantize(f, q);
}

// Fingerprint of everything observable through the bank's public API.
template <typename Bank>
std::uint64_t bankFingerprint(const Bank& b) {
    Fnv f;
    f.u(b.usedCount());
    f.i(b.firstEmpty());
    f.u(b.stepsUsed());
    f.u(b.notesUsed());
    f.u(b.eventsUsed());
    f.u(b.fullCount());
    auto d = std::make_unique<PatternData>();
    for (int id = 0; id < static_cast<int>(Bank::Capacity); ++id) {
        const auto s = b.info(id);
        f.u(b.used(id) ? 1 : 0);
        f.u(s.used ? 1 : 0);
        f.i(s.timeSignature.numerator);
        f.i(s.timeSignature.denominator);
        f.i(s.bars);
        f.i(s.stepTicks);
        hashQuantize(f, s.quantize);
        f.u(s.steps);
        f.u(s.notes);
        f.u(s.events);
        f.u(s.passes);
        f.u(b.nextPassId(id));
        for (std::size_t i = 0; i < RecordedTrack::MaxPasses; ++i) {  // includes unused entries
            const auto p = b.passState(id, i);
            f.u(p.id);
            f.u(p.active ? 1 : 0);
        }
        for (std::size_t i = 0; i < s.steps; ++i) {
            const auto& r = b.stepRecords(id)[i];
            f.u(r.pad);
            f.u(r.step);
            f.u(r.enabled);
            f.u(r.velocity);
            f.u(r.probability);
            f.i(r.microTicks);
        }
        for (std::size_t i = 0; i < s.notes; ++i) {
            const auto& r = b.noteRecords(id)[i];
            f.u(r.enabled);
            f.u(static_cast<std::uint64_t>(r.target));
            f.u(r.note);
            f.u(r.velocity);
            f.u(r.probability);
            f.i(r.startTick);
            f.i(r.lengthTicks);
        }
        for (std::size_t i = 0; i < s.events; ++i) hashEvent(f, Bank::unpack(b.eventRecords(id)[i]));
        f.u(b.load(id, d->pattern, d->track, d->quantize) ? 1 : 0);
        hashWorking(f, d->pattern, d->track, d->quantize);
    }
    return f.h;
}

inline std::uint64_t fnvBytes(const std::vector<std::uint8_t>& v) {
    Fnv f;
    for (std::uint8_t c : v) f.byte(c);
    return f.h;
}

// A pattern state using every kind of data: steps, notes, a recording with
// three passes (one undone) and non-default quantize settings.
inline std::unique_ptr<PatternData> richPattern(int variant) {
    auto d = std::make_unique<PatternData>();
    d->pattern.configure({7, 8}, 2 + variant, 120);
    for (std::size_t pad = 0; pad < 5; ++pad) {
        for (std::size_t k = 0; k < 20; k += 3) {
            DrumStep s;
            s.enabled = (k % 2) == 0;
            s.velocity = 30 + static_cast<int>(pad * 7 + k) % 90;
            s.probability = 50 + static_cast<int>(k);
            s.microTicks = static_cast<int>(k % 5) - 2;
            d->pattern.setStep(pad + static_cast<std::size_t>(variant), k, s);
        }
    }
    for (int i = 0; i < 6 + variant; ++i) {
        d->pattern.addNote({i % 3 != 0, static_cast<SynthTarget>(i % 3), 40 + i, 60 + i, 90, i * 77, 100 + i});
    }
    for (int pass = 0; pass < 3; ++pass) {
        const std::uint32_t id = d->track.openPass();
        for (int e = 0; e < 4; ++e) {
            RecordedEvent r;
            r.kind = (e % 2) ? RecordedEvent::Kind::Note : RecordedEvent::Kind::Drum;
            r.target = SynthTarget::Fm;
            r.value = 10 + e + variant;
            r.velocity = 70 + e;
            r.startFine = pass * 5000 + e * 1000 + variant;
            r.lengthFine = (e % 2) ? 2048 : 0;
            r.captureSample = 1000 + pass * 10 + e;
            r.captureFine = 2000 + pass;
            r.captureLoop = pass;
            r.captureEpoch = 3;
            r.sequence = static_cast<std::uint64_t>(pass * 4 + e + 1);
            d->track.add(r, id);
        }
    }
    d->track.undo();
    d->quantize.grid = QuantizeGrid::Eighth;
    d->quantize.strength = 40 + variant;
    d->quantize.swing = 10;
    d->quantize.targets = RecordDrums | RecordPoly;
    d->quantize.drumLanes = 0x15;
    d->quantize.quantizeNoteOff = true;
    return d;
}

// A dense pattern (more steps than a small bank's pool can hold).
inline std::unique_ptr<PatternData> densePattern() {
    auto d = std::make_unique<PatternData>();
    d->pattern.configure({4, 4}, 4, 240);
    for (std::size_t pad = 0; pad < Pattern::Lanes; ++pad) {
        for (std::size_t k = 0; k < 64; ++k) d->pattern.setHit(pad, k, 90);
    }
    return d;
}

struct ScriptResult {
    std::uint64_t stage[5];
    std::uint64_t dbpb;
    std::size_t dbpbSize;
};

// Exercises every bank operation, including the ones that act on a
// pristine (never written) slot, and fingerprints the bank after each stage.
template <typename Bank>
ScriptResult runScript(Bank& b) {
    ScriptResult r{};
    r.stage[0] = bankFingerprint(b);

    // Stage 1: operations on pristine slots.
    b.setShape(3, {3, 4}, 2, 120);
    QuantizeSettings q;
    q.grid = QuantizeGrid::ThirtySecond;
    q.strength = 50;
    q.swing = 20;
    q.targets = RecordDrums;
    q.drumLanes = 0x7;
    q.quantizeNoteOff = true;
    b.setQuantize(5, q);
    b.clearContents(7);
    r.stage[1] = bankFingerprint(b);

    // Stage 2: stores, copies, duplicate, erase, copying an empty slot.
    auto p0 = richPattern(0);
    auto p1 = richPattern(1);
    b.store(0, p0->pattern, p0->track, p0->quantize);
    b.store(1, p1->pattern, p1->track, p1->quantize);
    b.copy(0, 9);
    b.duplicate(1);
    b.erase(0);
    b.copy(4, 9);  // empty source: empties 9
    b.copy(1, 12);
    r.stage[2] = bankFingerprint(b);

    // Stage 3: a store that may not fit (fails on a small bank, counted).
    auto dense = densePattern();
    b.store(10, dense->pattern, dense->track, dense->quantize);
    b.clearContents(12);
    r.stage[3] = bankFingerprint(b);

    // Serialised form of the stage-3 bank.
    Arrangement arr;
    arr.append(1, 2);
    arr.append(3, 1);
    std::vector<std::uint8_t> bytes;
    pattern_io::save(b, arr, 1, bytes);
    r.dbpb = fnvBytes(bytes);
    r.dbpbSize = bytes.size();

    // Stage 4: clearAll, then one store into the last slot.
    b.clearAll();
    b.store(static_cast<int>(Bank::Capacity) - 1, p1->pattern, p1->track, p1->quantize);
    r.stage[4] = bankFingerprint(b);
    return r;
}

} // namespace e031
