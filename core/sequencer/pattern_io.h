
#pragma once

// DATABREAKs Engine 028 - pattern bank persistence (host-side C++17).
//
// A versioned, explicit, little-endian byte format for a pattern bank, its
// arrangement and the active pattern id. No C++ struct is ever written as
// raw memory: every field is encoded one by one with a fixed width, so the
// format does not depend on the compiler, padding or host byte order.
//
// NOT REAL-TIME: save() writes into a std::vector and load() allocates
// temporary storage. Call them outside the audio callback, on a bank that
// no other thread is using (for the Sequencer's bank: commit the active
// pattern and read bank() while audio is stopped, or work on a copy).
//
// ---- Format "DBPB" version 1 ------------------------------------------------
//
//   Header (16 bytes)
//     u8[4]  magic "DBPB"
//     u16    version = 1
//     u16    flags = 0 (any other value: unsupported)
//     u32    bank capacity of the writer (informational)
//     u32    record count
//   Records, each:  u8[4] tag, u32 payload length, payload
//     "PATN"  one stored pattern (one per used slot); 34 fixed bytes, then
//             the variable parts
//       u16 id, u8 numerator, u8 denominator, u8 bars, u8 reserved (0),
//       u32 stepTicks,
//       u8 grid (0 whole .. 6 sixty-fourth), u8 strength, u8 swing,
//       u8 targets, u32 drumLanes, u8 quantizeNoteOff, u8[3] reserved (0),
//       u32 nextPassId, u16 steps, u16 notes, u16 passes, u16 events,
//       steps  x 7:  u8 pad, u8 step, u8 enabled, u8 velocity,
//                    u8 probability, i16 microTicks
//       notes  x 13: u8 enabled, u8 target (0 mono, 1 FM, 2 poly), u8 note,
//                    u8 velocity, u8 probability, i32 startTick,
//                    i32 lengthTicks
//       passes x 5:  u32 id, u8 active
//       events x 65: u8 kind (0 drum, 1 note), u8 target, u8 value,
//                    u8 velocity, u8 pending, i64 startFine, i64 lengthFine,
//                    i64 captureSample, i64 captureFine, i64 captureLoop,
//                    u32 captureEpoch, u64 sequence, u32 createdPass,
//                    u32 erasedPass
//     "ARNG"  the arrangement (exactly one)
//       u8 end (0 stop, 1 loop, 2 hold last), u8 reserved (0), u16 entries,
//       entries x 4: u16 pattern id, u16 repeats
//     "ACTV"  the active pattern (exactly one): u16 id
//   Trailer: u32 CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF) of
//   every preceding byte.
//
// Steps are stored sparsely (only steps that differ from DrumStep{}); notes,
// recorded events and the pass table (undo/redo state) exactly. Bank
// statistics and recorder settings are not part of the format.
//
// load() validates everything before touching the destination: magic,
// version, flags, checksum, record framing (exact lengths, known tags,
// record count), every field range, uniqueness of ids and steps, recording
// consistency (RecordedTrack::restore rules) and bank capacity. On any
// failure the destination bank and arrangement are unchanged.

#include "arrangement.h"
#include "pattern_bank.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace databreaks {
namespace pattern_io {

constexpr std::uint16_t FormatVersion = 1;

enum class LoadStatus : std::uint8_t {
    Ok,
    TooShort,            // smaller than header + trailer
    BadMagic,
    UnsupportedVersion,  // version != 1 or non-zero flags
    BadChecksum,
    Malformed,           // framing: lengths, tags, counts, duplicates
    OutOfRange,          // a field outside its valid range
    Inconsistent,        // valid fields that do not fit together
    CapacityExceeded     // valid data that this bank cannot hold
};

inline const char* toString(LoadStatus s) {
    switch (s) {
    case LoadStatus::Ok: return "ok";
    case LoadStatus::TooShort: return "too short";
    case LoadStatus::BadMagic: return "bad magic";
    case LoadStatus::UnsupportedVersion: return "unsupported version";
    case LoadStatus::BadChecksum: return "bad checksum";
    case LoadStatus::Malformed: return "malformed";
    case LoadStatus::OutOfRange: return "out of range";
    case LoadStatus::Inconsistent: return "inconsistent";
    case LoadStatus::CapacityExceeded: return "capacity exceeded";
    }
    return "?";
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

namespace detail {

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint32_t v) { out_.push_back(static_cast<std::uint8_t>(v & 0xFFu)); }
    void u16(std::uint32_t v) { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) { u16(v & 0xFFFFu); u16(v >> 16); }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void i16(std::int64_t v) { u16(static_cast<std::uint32_t>(static_cast<std::uint16_t>(v))); }
    void i32(std::int64_t v) { u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(v))); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void tag(const char* t) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(t[i])); }
    std::size_t size() const { return out_.size(); }
    void patchU32(std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out_[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }

private:
    std::vector<std::uint8_t>& out_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}
    bool ok() const { return ok_; }
    std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
    const std::uint8_t* position() const { return p_; }
    std::uint32_t u8() {
        if (p_ >= end_) {
            ok_ = false;
            return 0;
        }
        return *p_++;
    }
    std::uint32_t u16() { const std::uint32_t a = u8(); return a | (u8() << 8); }
    std::uint32_t u32() { const std::uint32_t a = u16(); return a | (u16() << 16); }
    std::uint64_t u64() {
        const std::uint64_t a = u32();
        return a | (static_cast<std::uint64_t>(u32()) << 32);
    }
    std::int64_t i16() { return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16())); }
    std::int64_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    bool tagIs(const char* t) const {
        if (remaining() < 4) return false;
        for (int i = 0; i < 4; ++i) {
            if (p_[i] != static_cast<std::uint8_t>(t[i])) return false;
        }
        return true;
    }
    void skip(std::size_t n) {
        if (n > remaining()) {
            ok_ = false;
            p_ = end_;
        } else {
            p_ += n;
        }
    }

private:
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

inline std::uint32_t encodeTarget(SynthTarget t) {
    switch (t) {
    case SynthTarget::Mono: return 0;
    case SynthTarget::Fm: return 1;
    case SynthTarget::Poly: return 2;
    }
    return 0;
}

inline bool decodeTarget(std::uint32_t v, SynthTarget& t) {
    switch (v) {
    case 0: t = SynthTarget::Mono; return true;
    case 1: t = SynthTarget::Fm; return true;
    case 2: t = SynthTarget::Poly; return true;
    default: return false;
    }
}

inline std::uint32_t encodeGrid(QuantizeGrid g) {
    switch (g) {
    case QuantizeGrid::Whole: return 0;
    case QuantizeGrid::Half: return 1;
    case QuantizeGrid::Quarter: return 2;
    case QuantizeGrid::Eighth: return 3;
    case QuantizeGrid::Sixteenth: return 4;
    case QuantizeGrid::ThirtySecond: return 5;
    case QuantizeGrid::SixtyFourth: return 6;
    }
    return 4;
}

inline bool decodeGrid(std::uint32_t v, QuantizeGrid& g) {
    static const QuantizeGrid grids[] = {QuantizeGrid::Whole,     QuantizeGrid::Half,
                                         QuantizeGrid::Quarter,   QuantizeGrid::Eighth,
                                         QuantizeGrid::Sixteenth, QuantizeGrid::ThirtySecond,
                                         QuantizeGrid::SixtyFourth};
    if (v > 6) return false;
    g = grids[v];
    return true;
}

}  // namespace detail

// Serialises every used slot of `bank`, the arrangement and the active id.
template <typename Bank>
void save(const Bank& bank, const Arrangement& arrangement, int activeId,
          std::vector<std::uint8_t>& out) {
    out.clear();
    detail::Writer w(out);
    w.tag("DBPB");
    w.u16(FormatVersion);
    w.u16(0);
    w.u32(static_cast<std::uint32_t>(Bank::Capacity));
    const std::size_t countAt = w.size();
    w.u32(0);
    std::uint32_t records = 0;

    for (std::size_t slot = 0; slot < Bank::Capacity; ++slot) {
        const int id = static_cast<int>(slot);
        if (!bank.used(id)) continue;
        const auto info = bank.info(id);
        w.tag("PATN");
        const std::size_t lengthAt = w.size();
        w.u32(0);
        const std::size_t start = w.size();
        w.u16(static_cast<std::uint32_t>(id));
        w.u8(static_cast<std::uint32_t>(info.timeSignature.numerator));
        w.u8(static_cast<std::uint32_t>(info.timeSignature.denominator));
        w.u8(static_cast<std::uint32_t>(info.bars));
        w.u8(0);
        w.u32(static_cast<std::uint32_t>(info.stepTicks));
        w.u8(detail::encodeGrid(info.quantize.grid));
        w.u8(static_cast<std::uint32_t>(info.quantize.strength));
        w.u8(static_cast<std::uint32_t>(info.quantize.swing));
        w.u8(info.quantize.targets);
        w.u32(info.quantize.drumLanes);
        w.u8(info.quantize.quantizeNoteOff ? 1 : 0);
        w.u8(0);
        w.u8(0);
        w.u8(0);
        w.u32(bank.nextPassId(id));
        w.u16(static_cast<std::uint32_t>(info.steps));
        w.u16(static_cast<std::uint32_t>(info.notes));
        w.u16(static_cast<std::uint32_t>(info.passes));
        w.u16(static_cast<std::uint32_t>(info.events));
        const auto* steps = bank.stepRecords(id);
        for (std::size_t i = 0; i < info.steps; ++i) {
            w.u8(steps[i].pad);
            w.u8(steps[i].step);
            w.u8(steps[i].enabled);
            w.u8(steps[i].velocity);
            w.u8(steps[i].probability);
            w.i16(steps[i].microTicks);
        }
        const auto* notes = bank.noteRecords(id);
        for (std::size_t i = 0; i < info.notes; ++i) {
            w.u8(notes[i].enabled);
            w.u8(detail::encodeTarget(notes[i].target));
            w.u8(notes[i].note);
            w.u8(notes[i].velocity);
            w.u8(notes[i].probability);
            w.i32(notes[i].startTick);
            w.i32(notes[i].lengthTicks);
        }
        for (std::size_t i = 0; i < info.passes; ++i) {
            const RecordedTrack::PassState p = bank.passState(id, i);
            w.u32(p.id);
            w.u8(p.active ? 1 : 0);
        }
        const auto* events = bank.eventRecords(id);
        for (std::size_t i = 0; i < info.events; ++i) {
            const RecordedEvent e = Bank::unpack(events[i]);
            w.u8(e.kind == RecordedEvent::Kind::Drum ? 0 : 1);
            w.u8(detail::encodeTarget(e.target));
            w.u8(static_cast<std::uint32_t>(e.value));
            w.u8(static_cast<std::uint32_t>(e.velocity));
            w.u8(e.pending ? 1 : 0);
            w.i64(e.startFine);
            w.i64(e.lengthFine);
            w.i64(e.captureSample);
            w.i64(e.captureFine);
            w.i64(e.captureLoop);
            w.u32(e.captureEpoch);
            w.u64(e.sequence);
            w.u32(e.createdPass);
            w.u32(e.erasedPass);
        }
        w.patchU32(lengthAt, static_cast<std::uint32_t>(w.size() - start));
        ++records;
    }

    w.tag("ARNG");
    w.u32(static_cast<std::uint32_t>(4 + 4 * arrangement.size()));
    w.u8(static_cast<std::uint32_t>(arrangement.end()));
    w.u8(0);
    w.u16(static_cast<std::uint32_t>(arrangement.size()));
    for (std::size_t i = 0; i < arrangement.size(); ++i) {
        w.u16(arrangement.at(i).pattern);
        w.u16(arrangement.at(i).repeats);
    }
    ++records;

    w.tag("ACTV");
    w.u32(2);
    w.u16(static_cast<std::uint32_t>(activeId));
    ++records;

    w.patchU32(countAt, records);
    w.u32(crc32(out.data(), out.size()));
}

// Validates and loads `data` into `bank`, `arrangement` and `activeId`
// (all unchanged unless the result is Ok). Allocates; not real-time.
template <typename Bank>
LoadStatus load(const std::uint8_t* data, std::size_t size, Bank& bank,
                Arrangement& arrangement, int& activeId) {
    constexpr std::size_t HeaderSize = 16;
    if (data == nullptr || size < HeaderSize + 4) return LoadStatus::TooShort;
    detail::Reader header(data, size);
    if (!header.tagIs("DBPB")) return LoadStatus::BadMagic;
    header.skip(4);
    const std::uint32_t version = header.u16();
    const std::uint32_t flags = header.u16();
    if (version != FormatVersion || flags != 0) return LoadStatus::UnsupportedVersion;
    header.u32();  // writer capacity (informational)
    const std::uint32_t recordCount = header.u32();
    const std::size_t bodyEnd = size - 4;
    detail::Reader trailer(data + bodyEnd, 4);
    if (trailer.u32() != crc32(data, bodyEnd)) return LoadStatus::BadChecksum;

    auto staged = std::make_unique<Bank>();
    auto work = std::make_unique<PatternData>();
    Arrangement stagedArrangement;
    int stagedActive = -1;
    bool haveArrangement = false;
    std::vector<RecordedEvent> events;
    std::vector<RecordedTrack::PassState> passes;
    std::vector<bool> seen(Bank::Capacity, false);

    detail::Reader r(data + HeaderSize, bodyEnd - HeaderSize);
    for (std::uint32_t rec = 0; rec < recordCount; ++rec) {
        if (r.remaining() < 8) return LoadStatus::Malformed;
        const bool patn = r.tagIs("PATN");
        const bool arng = r.tagIs("ARNG");
        const bool actv = r.tagIs("ACTV");
        if (!patn && !arng && !actv) return LoadStatus::Malformed;
        r.skip(4);
        const std::uint32_t length = r.u32();
        if (length > r.remaining()) return LoadStatus::Malformed;
        detail::Reader p(r.position(), length);
        r.skip(length);

        if (actv) {
            if (stagedActive >= 0 || length != 2) return LoadStatus::Malformed;
            stagedActive = static_cast<int>(p.u16());
            continue;
        }
        if (arng) {
            if (haveArrangement || length < 4) return LoadStatus::Malformed;
            const std::uint32_t end = p.u8();
            const std::uint32_t reserved = p.u8();
            const std::uint32_t count = p.u16();
            if (length != 4 + 4 * count) return LoadStatus::Malformed;
            if (reserved != 0) return LoadStatus::Malformed;
            if (count > Arrangement::MaxEntries || !stagedArrangement.setEnd(static_cast<int>(end))) {
                return LoadStatus::OutOfRange;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const int pattern = static_cast<int>(p.u16());
                const int repeats = static_cast<int>(p.u16());
                if (!Bank::validId(pattern) || !stagedArrangement.append(pattern, repeats)) {
                    return LoadStatus::OutOfRange;
                }
            }
            haveArrangement = true;
            continue;
        }

        // ---- PATN ----
        if (length < 34) return LoadStatus::Malformed;
        const int id = static_cast<int>(p.u16());
        const int numerator = static_cast<int>(p.u8());
        const int denominator = static_cast<int>(p.u8());
        const int bars = static_cast<int>(p.u8());
        const std::uint32_t reserved0 = p.u8();
        const std::int64_t stepTicks = p.u32();
        QuantizeSettings q;
        const std::uint32_t grid = p.u8();
        q.strength = static_cast<int>(p.u8());
        q.swing = static_cast<int>(p.u8());
        q.targets = p.u8();
        q.drumLanes = p.u32();
        const std::uint32_t noteOff = p.u8();
        const std::uint32_t reserved1 = p.u8() | p.u8() | p.u8();
        const std::uint32_t nextPassId = p.u32();
        const std::size_t stepCount = p.u16();
        const std::size_t noteCount = p.u16();
        const std::size_t passCount = p.u16();
        const std::size_t eventCount = p.u16();
        if (reserved0 != 0 || reserved1 != 0) return LoadStatus::Malformed;
        if (length != 34 + 7 * stepCount + 13 * noteCount + 5 * passCount + 65 * eventCount) {
            return LoadStatus::Malformed;
        }
        if (!Bank::validId(id)) return LoadStatus::OutOfRange;
        if (seen[static_cast<std::size_t>(id)]) return LoadStatus::Malformed;
        seen[static_cast<std::size_t>(id)] = true;
        if (!detail::decodeGrid(grid, q.grid) || noteOff > 1) return LoadStatus::OutOfRange;
        q.quantizeNoteOff = noteOff == 1;
        if (!q.valid()) return LoadStatus::OutOfRange;
        if (stepCount > Pattern::Lanes * Pattern::MaxSteps || noteCount > Pattern::MaxNotes ||
            passCount > RecordedTrack::MaxPasses || eventCount > RecordedTrack::MaxEvents) {
            return LoadStatus::OutOfRange;
        }

        Pattern& pattern = work->pattern;
        pattern = Pattern{};
        if (!pattern.configure({numerator, denominator}, bars, stepTicks)) return LoadStatus::OutOfRange;
        std::vector<bool> stepSeen(Pattern::Lanes * Pattern::MaxSteps, false);
        for (std::size_t i = 0; i < stepCount; ++i) {
            const std::size_t pad = p.u8();
            const std::size_t step = p.u8();
            const std::uint32_t enabled = p.u8();
            DrumStep d;
            d.velocity = static_cast<int>(p.u8());
            d.probability = static_cast<int>(p.u8());
            d.microTicks = static_cast<int>(p.i16());
            if (pad >= Pattern::Lanes || step >= Pattern::MaxSteps || enabled > 1 ||
                d.velocity < 1 || d.velocity > 127 || d.probability > 100 ||
                d.microTicks < -Pattern::MaxStepTicks / 2 || d.microTicks > Pattern::MaxStepTicks / 2) {
                return LoadStatus::OutOfRange;
            }
            d.enabled = enabled == 1;
            if (stepSeen[pad * Pattern::MaxSteps + step]) return LoadStatus::Inconsistent;
            stepSeen[pad * Pattern::MaxSteps + step] = true;
            pattern.restoreStep(pad, step, d);
        }
        constexpr std::int64_t MaxPatternTicks = 32 * 3840 * Pattern::MaxBars;
        for (std::size_t i = 0; i < noteCount; ++i) {
            NoteEvent n;
            const std::uint32_t enabled = p.u8();
            const std::uint32_t target = p.u8();
            n.note = static_cast<int>(p.u8());
            n.velocity = static_cast<int>(p.u8());
            n.probability = static_cast<int>(p.u8());
            n.startTick = p.i32();
            n.lengthTicks = p.i32();
            if (enabled > 1 || !detail::decodeTarget(target, n.target) || n.note > 127 ||
                n.velocity < 1 || n.velocity > 127 || n.probability > 100 || n.startTick < 0 ||
                n.startTick >= MaxPatternTicks || n.lengthTicks < 1 ||
                n.lengthTicks > Pattern::MaxNoteLengthTicks) {
                return LoadStatus::OutOfRange;
            }
            n.enabled = enabled == 1;
            pattern.restoreNote(n);
        }
        passes.assign(passCount, RecordedTrack::PassState{});
        for (std::size_t i = 0; i < passCount; ++i) {
            passes[i].id = p.u32();
            const std::uint32_t active = p.u8();
            if (active > 1) return LoadStatus::OutOfRange;
            passes[i].active = active == 1;
        }
        events.assign(eventCount, RecordedEvent{});
        for (std::size_t i = 0; i < eventCount; ++i) {
            RecordedEvent& e = events[i];
            const std::uint32_t kind = p.u8();
            const std::uint32_t target = p.u8();
            e.value = static_cast<int>(p.u8());
            e.velocity = static_cast<int>(p.u8());
            const std::uint32_t pending = p.u8();
            e.startFine = p.i64();
            e.lengthFine = p.i64();
            e.captureSample = p.i64();
            e.captureFine = p.i64();
            e.captureLoop = p.i64();
            e.captureEpoch = p.u32();
            e.sequence = p.u64();
            e.createdPass = p.u32();
            e.erasedPass = p.u32();
            if (kind > 1 || pending > 1 || !detail::decodeTarget(target, e.target)) {
                return LoadStatus::OutOfRange;
            }
            e.kind = kind == 0 ? RecordedEvent::Kind::Drum : RecordedEvent::Kind::Note;
            e.pending = pending == 1;
            if (!RecordedTrack::validEvent(e) || !Bank::packable(e) ||
                e.startFine >= MaxPatternTicks * MusicalClock::FinePerTick) {
                return LoadStatus::OutOfRange;
            }
        }
        if (!p.ok() || p.remaining() != 0) return LoadStatus::Malformed;
        if (!work->track.restore(events.data(), eventCount, passes.data(), passCount, nextPassId)) {
            return LoadStatus::Inconsistent;
        }
        if (!staged->store(id, pattern, work->track, q)) return LoadStatus::CapacityExceeded;
    }
    if (!r.ok() || r.remaining() != 0) return LoadStatus::Malformed;
    if (!haveArrangement || stagedActive < 0) return LoadStatus::Malformed;
    if (!Bank::validId(stagedActive)) return LoadStatus::OutOfRange;
    if (!staged->used(stagedActive)) return LoadStatus::Inconsistent;
    if (!stagedArrangement.empty() &&
        !stagedArrangement.validate([&staged](int id) { return staged->used(id); })) {
        return LoadStatus::Inconsistent;
    }
    bank = *staged;
    arrangement = stagedArrangement;
    activeId = stagedActive;
    return LoadStatus::Ok;
}

}  // namespace pattern_io
}  // namespace databreaks
