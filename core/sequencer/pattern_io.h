
#pragma once

// DATABREAKs Engine 028 - pattern bank persistence (host-side C++17).
//
// A versioned, explicit, little-endian byte format for a pattern bank, its
// arrangement and the active pattern id. No C++ struct is ever written as
// raw memory: every field is encoded one by one with a fixed width, so the
// format does not depend on the compiler, padding or host byte order.
//
// NOT REAL-TIME. Call outside the audio callback, on a bank that no other
// thread is using (for the Sequencer's bank: commit the active pattern and
// read bank() while audio is stopped, or work on a copy).
//
// Engine 029 - storage boundary (platform/storage.h):
//   * save(bank, arrangement, active, ByteSink&) streams through a fixed
//     256-byte buffer with a running CRC: no allocation, no contiguous
//     buffer; record lengths are computed before they are written;
//   * load(ByteSource&, LoadScratch&, ...) reads by offset and decodes into
//     caller-provided scratch (no allocation inside the core), still
//     all-or-nothing;
//   * saveTo / loadFrom go through an injected StorageBackend (host file,
//     flash, ...): the core never knows the medium;
//   * the Engine 028 std::vector / pointer overloads remain as host
//     conveniences (they allocate) and give identical bytes and results.
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
#include "musical_clock.h"
#include "pattern_bank.h"
#include "../../platform/storage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>  // host-convenience overloads only (no <memory>: on hosted
                   // libstdc++ it pulls in the threading library)

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
    CapacityExceeded,    // valid data that this bank cannot hold
    ReadError,           // the storage medium failed to deliver bytes (Engine 029)
    NotFound             // StorageBackend: no object with that name (Engine 029)
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
    case LoadStatus::ReadError: return "read error";
    case LoadStatus::NotFound: return "not found";
    }
    return "?";
}

// Result of saving through a StorageBackend (Engine 029).
enum class SaveStatus : std::uint8_t {
    Ok,
    BeginFailed,   // invalid name, no space, or the backend refused
    WriteFailed,   // the sink failed; the write was aborted
    CommitFailed   // the backend could not make the new version current
};

inline const char* toString(SaveStatus s) {
    switch (s) {
    case SaveStatus::Ok: return "ok";
    case SaveStatus::BeginFailed: return "begin failed";
    case SaveStatus::WriteFailed: return "write failed";
    case SaveStatus::CommitFailed: return "commit failed";
    }
    return "?";
}

// CRC-32 (IEEE): crc32Update over a byte stream, starting from 0xFFFFFFFF;
// the final value is the state xor 0xFFFFFFFF.
inline std::uint32_t crc32Update(std::uint32_t state, const std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        state ^= data[i];
        for (int b = 0; b < 8; ++b) state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1u)));
    }
    return state;
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    return crc32Update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

namespace detail {

// Buffered sequential writer into a ByteSink, with a running CRC. Fixed
// 256-byte buffer; no allocation.
class StreamWriter {
public:
    explicit StreamWriter(ByteSink& sink) : sink_(sink) {}
    bool ok() const { return ok_; }
    std::size_t written() const { return written_; }
    void u8(std::uint32_t v) {
        buffer_[used_++] = static_cast<std::uint8_t>(v & 0xFFu);
        ++written_;
        if (used_ == buffer_.size()) flush();
    }
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
    // Writes the CRC of everything written so far (not itself included).
    void crcTrailer() {
        flush();
        const std::uint32_t crc = crc_ ^ 0xFFFFFFFFu;
        u32(crc);
        flushRaw();
    }
    void flush() {
        crc_ = crc32Update(crc_, buffer_.data(), used_);
        flushRaw();
    }

private:
    void flushRaw() {
        if (used_ > 0 && ok_) ok_ = sink_.write(buffer_.data(), used_);
        used_ = 0;
    }
    ByteSink& sink_;
    std::array<std::uint8_t, 256> buffer_{};
    std::size_t used_ = 0;
    std::size_t written_ = 0;
    std::uint32_t crc_ = 0xFFFFFFFFu;
    bool ok_ = true;
};

// Bounds-checked reader of the window [begin, end) of a ByteSource, with a
// small read cache. Reading past the window clears ok(); a failing medium
// sets *ioError (reported as ReadError rather than as a format error).
class SourceReader {
public:
    SourceReader(const ByteSource& src, std::size_t begin, std::size_t end, bool* ioError)
        : src_(src), pos_(begin), end_(end), ioError_(ioError) {}
    bool ok() const { return ok_; }
    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return end_ - pos_; }
    std::uint32_t u8() {
        if (pos_ >= end_) {
            ok_ = false;
            return 0;
        }
        if (pos_ < cacheStart_ || pos_ >= cacheStart_ + cacheSize_) {
            const std::size_t n = std::min<std::size_t>(cache_.size(), end_ - pos_);
            if (!src_.read(pos_, cache_.data(), n)) {
                ok_ = false;
                if (ioError_ != nullptr) *ioError_ = true;
                pos_ = end_;
                return 0;
            }
            cacheStart_ = pos_;
            cacheSize_ = n;
        }
        return cache_[pos_++ - cacheStart_];
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
    // Compares the next 4 bytes with `t` without consuming them.
    bool tagIs(const char* t) {
        if (remaining() < 4) return false;
        const std::size_t at = pos_;
        bool same = true;
        for (int i = 0; i < 4; ++i) same = (u8() == static_cast<std::uint8_t>(t[i])) && same;
        pos_ = at;
        return same && ok_;
    }
    void skip(std::size_t n) {
        if (n > remaining()) {
            ok_ = false;
            pos_ = end_;
        } else {
            pos_ += n;
        }
    }

private:
    const ByteSource& src_;
    std::size_t pos_;
    std::size_t end_;
    bool* ioError_;
    bool ok_ = true;
    std::array<std::uint8_t, 64> cache_{};
    std::size_t cacheStart_ = 0;
    std::size_t cacheSize_ = 0;
};

// A ByteSink appending to a std::vector (host convenience; allocates).
class VectorSink final : public ByteSink {
public:
    explicit VectorSink(std::vector<std::uint8_t>& out) : out_(out) {}
    bool write(const std::uint8_t* data, std::size_t n) override {
        out_.insert(out_.end(), data, data + n);
        return true;
    }

private:
    std::vector<std::uint8_t>& out_;
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

constexpr std::size_t HeaderSize = 16;
constexpr std::size_t PatnFixed = 34;
constexpr std::size_t StepBytes = 7;
constexpr std::size_t NoteBytes = 13;
constexpr std::size_t PassBytes = 5;
constexpr std::size_t EventBytes = 65;
constexpr std::int64_t MaxPatternTicks = 32 * 3840 * Pattern::MaxBars;

// DBPB v1 per-pattern limits. They are the host engine's capacities
// (BasicPattern/BasicRecordedTrack<HostProfile>) and are part of the format:
// a file beyond them is OutOfRange. A bank built for a smaller profile
// (Engine 032) rejects a VALID file that exceeds its own working capacities
// with CapacityExceeded instead; for the host profile the two coincide.
using FormatPattern = BasicPattern<HostProfile>;
using FormatTrack = BasicRecordedTrack<HostProfile>;

// Decodes one 65-byte event; false if a field is out of range.
inline bool decodeEvent(SourceReader& p, RecordedEvent& e) {
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
    if (kind > 1 || pending > 1 || !decodeTarget(target, e.target)) return false;
    e.kind = kind == 0 ? RecordedEvent::Kind::Drum : RecordedEvent::Kind::Note;
    e.pending = pending == 1;
    return true;
}

}  // namespace detail

// Exact number of bytes save() produces for this bank and arrangement
// (e.g. to check the space of a storage medium before writing).
template <typename Bank>
std::size_t encodedSize(const Bank& bank, const Arrangement& arrangement) {
    std::size_t size = detail::HeaderSize;
    for (std::size_t slot = 0; slot < Bank::Capacity; ++slot) {
        const int id = static_cast<int>(slot);
        if (!bank.used(id)) continue;
        const auto info = bank.info(id);
        size += 8 + detail::PatnFixed + detail::StepBytes * info.steps +
                detail::NoteBytes * info.notes + detail::PassBytes * info.passes +
                detail::EventBytes * info.events;
    }
    size += 8 + 4 + 4 * arrangement.size();  // ARNG
    size += 8 + 2;                            // ACTV
    return size + 4;                          // CRC
}

// Serialises every used slot of `bank`, the arrangement and the active id
// into `sink`, streaming (fixed 256-byte buffer, no allocation; record
// lengths are computed before they are written). Returns false if the sink
// failed (what it received is then incomplete).
template <typename Bank>
bool save(const Bank& bank, const Arrangement& arrangement, int activeId, ByteSink& sink) {
    detail::StreamWriter w(sink);
    w.tag("DBPB");
    w.u16(FormatVersion);
    w.u16(0);
    w.u32(static_cast<std::uint32_t>(Bank::Capacity));
    w.u32(static_cast<std::uint32_t>(bank.usedCount() + 2));  // PATN records + ARNG + ACTV

    for (std::size_t slot = 0; slot < Bank::Capacity; ++slot) {
        const int id = static_cast<int>(slot);
        if (!bank.used(id)) continue;
        const auto info = bank.info(id);
        w.tag("PATN");
        w.u32(static_cast<std::uint32_t>(detail::PatnFixed + detail::StepBytes * info.steps +
                                         detail::NoteBytes * info.notes +
                                         detail::PassBytes * info.passes +
                                         detail::EventBytes * info.events));
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
            const RecordedPassState p = bank.passState(id, i);
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

    w.tag("ACTV");
    w.u32(2);
    w.u16(static_cast<std::uint32_t>(activeId));

    w.crcTrailer();
    return w.ok();
}

// Host convenience (Engine 028 API): serialises into a std::vector.
// Allocates. Produces exactly the bytes of the streaming save().
template <typename Bank>
void save(const Bank& bank, const Arrangement& arrangement, int activeId,
          std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(encodedSize(bank, arrangement));
    detail::VectorSink sink(out);
    save(bank, arrangement, activeId, sink);
}

// Working memory for load(): a staged bank and one pattern state
// (~510 KB with DefaultPatternBank). The caller provides it - static
// storage on a target, the heap on a host - so load() itself never
// allocates. Its contents after a load are unspecified.
template <typename Bank>
struct LoadScratch {
    Bank staged;
    typename Bank::PatternType pattern;
    typename Bank::TrackType track;
};

// Validates and loads from `source` into `bank`, `arrangement` and
// `activeId` (all unchanged unless the result is Ok). The file is decoded
// into scratch.staged and copied to `bank` only when everything is valid:
// all-or-nothing. Reads by offset; no allocation; not real-time.
template <typename Bank>
LoadStatus load(const ByteSource& source, LoadScratch<Bank>& scratch, Bank& bank,
                Arrangement& arrangement, int& activeId) {
    using detail::SourceReader;
    const std::size_t size = source.size();
    if (size < detail::HeaderSize + 4) return LoadStatus::TooShort;
    bool ioError = false;
    auto fail = [&ioError](LoadStatus s) { return ioError ? LoadStatus::ReadError : s; };

    SourceReader header(source, 0, size, &ioError);
    if (!header.tagIs("DBPB")) return fail(LoadStatus::BadMagic);
    header.skip(4);
    const std::uint32_t version = header.u16();
    const std::uint32_t flags = header.u16();
    if (ioError) return LoadStatus::ReadError;
    if (version != FormatVersion || flags != 0) return LoadStatus::UnsupportedVersion;
    header.u32();  // writer capacity (informational)
    const std::uint32_t recordCount = header.u32();
    const std::size_t bodyEnd = size - 4;
    {
        std::array<std::uint8_t, 256> chunk{};
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t at = 0; at < bodyEnd; at += chunk.size()) {
            const std::size_t n = std::min(chunk.size(), bodyEnd - at);
            if (!source.read(at, chunk.data(), n)) return LoadStatus::ReadError;
            crc = crc32Update(crc, chunk.data(), n);
        }
        SourceReader trailer(source, bodyEnd, size, &ioError);
        const std::uint32_t stored = trailer.u32();
        if (ioError) return LoadStatus::ReadError;
        if (stored != (crc ^ 0xFFFFFFFFu)) return LoadStatus::BadChecksum;
    }

    Bank& staged = scratch.staged;
    staged.clearAll();
    Arrangement stagedArrangement;
    int stagedActive = -1;
    bool haveArrangement = false;
    using Pattern = typename Bank::PatternType;  // this bank's working capacities
    using Track = typename Bank::TrackType;
    using FormatPattern = detail::FormatPattern;  // the file format's limits
    using FormatTrack = detail::FormatTrack;
    std::array<RecordedPassState, FormatTrack::MaxPasses> passes{};
    std::array<bool, Bank::Capacity> seen{};
    constexpr std::size_t StepSlots = FormatPattern::Lanes * FormatPattern::MaxSteps;
    std::array<std::uint64_t, (StepSlots + 63) / 64> stepSeen{};

    SourceReader r(source, detail::HeaderSize, bodyEnd, &ioError);
    for (std::uint32_t rec = 0; rec < recordCount; ++rec) {
        if (r.remaining() < 8) return fail(LoadStatus::Malformed);
        const bool patn = r.tagIs("PATN");
        const bool arng = r.tagIs("ARNG");
        const bool actv = r.tagIs("ACTV");
        if (!patn && !arng && !actv) return fail(LoadStatus::Malformed);
        r.skip(4);
        const std::uint32_t length = r.u32();
        if (ioError) return LoadStatus::ReadError;
        if (length > r.remaining()) return LoadStatus::Malformed;
        SourceReader p(source, r.position(), r.position() + length, &ioError);
        r.skip(length);

        if (actv) {
            if (stagedActive >= 0 || length != 2) return LoadStatus::Malformed;
            stagedActive = static_cast<int>(p.u16());
            if (ioError) return LoadStatus::ReadError;
            continue;
        }
        if (arng) {
            if (haveArrangement || length < 4) return LoadStatus::Malformed;
            const std::uint32_t end = p.u8();
            const std::uint32_t reserved = p.u8();
            const std::uint32_t count = p.u16();
            if (ioError) return LoadStatus::ReadError;
            if (length != 4 + 4 * count) return LoadStatus::Malformed;
            if (reserved != 0) return LoadStatus::Malformed;
            if (count > Arrangement::MaxEntries || !stagedArrangement.setEnd(static_cast<int>(end))) {
                return LoadStatus::OutOfRange;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const int pattern = static_cast<int>(p.u16());
                const int repeats = static_cast<int>(p.u16());
                if (ioError) return LoadStatus::ReadError;
                if (!Bank::validId(pattern) || !stagedArrangement.append(pattern, repeats)) {
                    return LoadStatus::OutOfRange;
                }
            }
            haveArrangement = true;
            continue;
        }

        // ---- PATN ----
        if (length < detail::PatnFixed) return LoadStatus::Malformed;
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
        const std::uint32_t reserved1a = p.u8();
        const std::uint32_t reserved1b = p.u8();
        const std::uint32_t reserved1c = p.u8();
        const std::uint32_t nextPassId = p.u32();
        const std::size_t stepCount = p.u16();
        const std::size_t noteCount = p.u16();
        const std::size_t passCount = p.u16();
        const std::size_t eventCount = p.u16();
        if (ioError) return LoadStatus::ReadError;
        if (reserved0 != 0 || (reserved1a | reserved1b | reserved1c) != 0) return LoadStatus::Malformed;
        if (length != detail::PatnFixed + detail::StepBytes * stepCount +
                          detail::NoteBytes * noteCount + detail::PassBytes * passCount +
                          detail::EventBytes * eventCount) {
            return LoadStatus::Malformed;
        }
        if (!Bank::validId(id)) return LoadStatus::OutOfRange;
        if (seen[static_cast<std::size_t>(id)]) return LoadStatus::Malformed;
        seen[static_cast<std::size_t>(id)] = true;
        if (!detail::decodeGrid(grid, q.grid) || noteOff > 1) return LoadStatus::OutOfRange;
        q.quantizeNoteOff = noteOff == 1;
        if (!q.valid()) return LoadStatus::OutOfRange;
        if (stepCount > StepSlots || noteCount > FormatPattern::MaxNotes ||
            passCount > FormatTrack::MaxPasses || eventCount > FormatTrack::MaxEvents) {
            return LoadStatus::OutOfRange;
        }
        if (!FormatPattern::validShape({numerator, denominator}, bars, stepTicks)) {
            return LoadStatus::OutOfRange;
        }
        // Valid DBPB, but beyond this bank's working capacities (only
        // possible for a profile smaller than the host's).
        if (noteCount > Pattern::MaxNotes || passCount > Track::MaxPasses ||
            eventCount > Track::MaxEvents) {
            return LoadStatus::CapacityExceeded;
        }

        Pattern& pattern = scratch.pattern;
        pattern = Pattern{};
        if (!pattern.configure({numerator, denominator}, bars, stepTicks)) return LoadStatus::CapacityExceeded;
        stepSeen.fill(0);
        for (std::size_t i = 0; i < stepCount; ++i) {
            const std::size_t pad = p.u8();
            const std::size_t step = p.u8();
            const std::uint32_t enabled = p.u8();
            DrumStep d;
            d.velocity = static_cast<int>(p.u8());
            d.probability = static_cast<int>(p.u8());
            d.microTicks = static_cast<int>(p.i16());
            if (ioError) return LoadStatus::ReadError;
            if (pad >= FormatPattern::Lanes || step >= FormatPattern::MaxSteps || enabled > 1 ||
                d.velocity < 1 || d.velocity > 127 || d.probability > 100 ||
                d.microTicks < -FormatPattern::MaxStepTicks / 2 ||
                d.microTicks > FormatPattern::MaxStepTicks / 2) {
                return LoadStatus::OutOfRange;
            }
            d.enabled = enabled == 1;
            const std::size_t slot = pad * FormatPattern::MaxSteps + step;
            if ((stepSeen[slot / 64] >> (slot % 64)) & 1u) return LoadStatus::Inconsistent;
            stepSeen[slot / 64] |= std::uint64_t{1} << (slot % 64);
            if (step >= Pattern::MaxSteps) return LoadStatus::CapacityExceeded;
            pattern.restoreStep(pad, step, d);
        }
        for (std::size_t i = 0; i < noteCount; ++i) {
            NoteEvent n;
            const std::uint32_t enabled = p.u8();
            const std::uint32_t target = p.u8();
            n.note = static_cast<int>(p.u8());
            n.velocity = static_cast<int>(p.u8());
            n.probability = static_cast<int>(p.u8());
            n.startTick = p.i32();
            n.lengthTicks = p.i32();
            if (ioError) return LoadStatus::ReadError;
            if (enabled > 1 || !detail::decodeTarget(target, n.target) || n.note > 127 ||
                n.velocity < 1 || n.velocity > 127 || n.probability > 100 || n.startTick < 0 ||
                n.startTick >= detail::MaxPatternTicks || n.lengthTicks < 1 ||
                n.lengthTicks > FormatPattern::MaxNoteLengthTicks) {
                return LoadStatus::OutOfRange;
            }
            n.enabled = enabled == 1;
            pattern.restoreNote(n);
        }
        for (std::size_t i = 0; i < passCount; ++i) {
            passes[i].id = p.u32();
            const std::uint32_t active = p.u8();
            if (ioError) return LoadStatus::ReadError;
            if (active > 1) return LoadStatus::OutOfRange;
            passes[i].active = active == 1;
        }
        // Events: validate every field first, reading in order; then
        // RecordedTrack::restoreFrom re-reads them by offset (no buffer).
        const std::size_t eventsAt = p.position();
        for (std::size_t i = 0; i < eventCount; ++i) {
            RecordedEvent e;
            const bool fieldsOk = detail::decodeEvent(p, e);
            if (ioError) return LoadStatus::ReadError;
            if (!fieldsOk || !Track::validEvent(e) || !Bank::packable(e) ||
                e.startFine >= detail::MaxPatternTicks * MusicalClock::FinePerTick) {
                return LoadStatus::OutOfRange;
            }
        }
        if (!p.ok() || p.remaining() != 0) return fail(LoadStatus::Malformed);
        auto eventAt = [&source, eventsAt, &ioError](std::size_t i) {
            SourceReader e(source, eventsAt + detail::EventBytes * i,
                           eventsAt + detail::EventBytes * (i + 1), &ioError);
            RecordedEvent out;
            if (!detail::decodeEvent(e, out) || !e.ok()) out.velocity = 0;  // makes restore fail
            return out;
        };
        if (!scratch.track.restoreFrom(eventCount, eventAt, passes.data(), passCount, nextPassId)) {
            return fail(LoadStatus::Inconsistent);
        }
        if (!staged.store(id, pattern, scratch.track, q)) return LoadStatus::CapacityExceeded;
    }
    if (!r.ok() || r.remaining() != 0) return fail(LoadStatus::Malformed);
    if (!haveArrangement || stagedActive < 0) return LoadStatus::Malformed;
    if (!Bank::validId(stagedActive)) return LoadStatus::OutOfRange;
    if (!staged.used(stagedActive)) return LoadStatus::Inconsistent;
    if (!stagedArrangement.empty() &&
        !stagedArrangement.validate([&staged](int id) { return staged.used(id); })) {
        return LoadStatus::Inconsistent;
    }
    bank = staged;
    arrangement = stagedArrangement;
    activeId = stagedActive;
    return LoadStatus::Ok;
}

// Host convenience (Engine 028 API): loads from memory, allocating the
// scratch on the heap. Same validation and results.
template <typename Bank>
LoadStatus load(const std::uint8_t* data, std::size_t size, Bank& bank,
                Arrangement& arrangement, int& activeId) {
    if (data == nullptr || size < detail::HeaderSize + 4) return LoadStatus::TooShort;
    const MemoryByteSource source(data, size);
    struct Owner {  // heap scratch, freed on every return path
        LoadScratch<Bank>* p = new LoadScratch<Bank>();
        ~Owner() { delete p; }
    } scratch;
    return load(source, *scratch.p, bank, arrangement, activeId);
}

// ---- Through an injected StorageBackend (Engine 029) -------------------------------

// Saves into object `name`: the previous version is replaced atomically on
// success and kept on any failure.
template <typename Bank>
SaveStatus saveTo(StorageBackend& storage, const char* name, const Bank& bank,
                  const Arrangement& arrangement, int activeId) {
    ByteSink* sink = storage.beginWrite(name);
    if (sink == nullptr) return SaveStatus::BeginFailed;
    if (!save(bank, arrangement, activeId, *sink)) {
        storage.abortWrite();
        return SaveStatus::WriteFailed;
    }
    return storage.commitWrite() ? SaveStatus::Ok : SaveStatus::CommitFailed;
}

template <typename Bank>
LoadStatus loadFrom(StorageBackend& storage, const char* name, LoadScratch<Bank>& scratch,
                    Bank& bank, Arrangement& arrangement, int& activeId) {
    const ByteSource* source = storage.openRead(name);
    if (source == nullptr) return LoadStatus::NotFound;
    const LoadStatus status = load(*source, scratch, bank, arrangement, activeId);
    storage.closeRead();
    return status;
}

}  // namespace pattern_io
}  // namespace databreaks
