// DATABREAKs Engine 029 - target portability boundary tests.
//
// Host-side assertion tests for core/sequencer/exact_int128.h (portable
// exact arithmetic, compared with the native __int128 and through the real
// MusicalClock), the storage boundary (platform/storage.h, streaming
// pattern_io, MemoryStorage / FileStorage host backends), the audio I/O
// boundary (platform/audio_io.h, OfflineAudioDevice) and the input boundary
// (platform/input.h, ScriptedInputSource, QueueInputSource).
// Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"
#include "../../core/sequencer/pattern_io.h"
#include "../../platform/audio_io.h"
#include "../../platform/input.h"
#include "../../platform/host/file_storage.h"
#include "../../platform/host/memory_storage.h"
#include "../../platform/host/offline_audio_device.h"
#include "../../platform/host/scripted_input.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
std::atomic<std::size_t> g_allocations{0};
}

// Counting replacements; noinline so GCC -O3 does not pair the inlined
// malloc/free with new/delete (-Wmismatched-new-delete false positive).
__attribute__((noinline)) void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
__attribute__((noinline)) void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

using namespace databreaks;

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const std::string& description) {
    if (condition) {
        ++passed;
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++failed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

using Bank = DefaultPatternBank;
using P = PortableInt128;

#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 Native;
__extension__ typedef unsigned __int128 NativeU;

P fromNative(Native v) {
    const NativeU u = static_cast<NativeU>(v);
    return P::fromWords(static_cast<std::uint64_t>(u >> 64), static_cast<std::uint64_t>(u));
}
bool same(P p, Native n) {
    const P q = fromNative(n);
    return p.high() == q.high() && p.low() == q.low();
}
#endif

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {  // splitmix64
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

// ---------------------------------------------------------------------------
void int128Tests() {
    std::cout << "\n-- Portable exact 128-bit integer --\n";
    static_assert(P(5) + P(7) == P(12), "constexpr add");
    static_assert(P(-9) / P(4) == P(-2) && P(-9) % P(4) == P(-1), "truncating division");
    static_assert(P(9) / P(-4) == P(-2) && P(9) % P(-4) == P(1), "truncating division");
    static_assert((P(1) * P(INT64_MAX) * P(4)).high() == 1, "carry into the high word");
    check(true, "N0 constexpr: +, /, % (truncation toward zero, remainder sign), carries");
#if defined(__SIZEOF_INT128__)
    {
        // Edge values and random values of every magnitude; all ops vs native.
        std::vector<Native> values = {0, 1, -1, 2, -2, 3, 7, -7, 1000, -1000,
                                      INT64_MAX, INT64_MIN, static_cast<Native>(INT64_MAX) + 1,
                                      static_cast<Native>(INT64_MIN) - 1,
                                      static_cast<Native>(1) << 64, -(static_cast<Native>(1) << 64),
                                      (static_cast<Native>(1) << 100) + 12345,
                                      static_cast<Native>((static_cast<NativeU>(1) << 127) - 1),  // max
                                      -static_cast<Native>((static_cast<NativeU>(1) << 127) - 1)};
        Rng rng{2029};
        for (int i = 0; i < 4000; ++i) {
            const int bits = static_cast<int>(rng.next() % 126) + 1;
            NativeU u = (static_cast<NativeU>(rng.next()) << 64) | rng.next();
            u &= (static_cast<NativeU>(1) << bits) - 1;
            Native v = static_cast<Native>(u);
            if (rng.next() & 1) v = -v;
            values.push_back(v);
        }
        std::size_t cases = 0;
        bool ok = true;
        for (std::size_t i = 0; i < values.size() && ok; ++i) {
            for (std::size_t j = 0; j < 60 && ok; ++j) {
                const Native a = values[i];
                const Native b = values[(i * 7919 + j * 104729) % values.size()];
                const P pa = fromNative(a);
                const P pb = fromNative(b);
                // Wrapping ops are computed in unsigned native to avoid UB.
                const Native sum = static_cast<Native>(static_cast<NativeU>(a) + static_cast<NativeU>(b));
                const Native dif = static_cast<Native>(static_cast<NativeU>(a) - static_cast<NativeU>(b));
                const Native prod = static_cast<Native>(static_cast<NativeU>(a) * static_cast<NativeU>(b));
                ok = same(pa + pb, sum) && same(pa - pb, dif) && same(pa * pb, prod) &&
                     (pa < pb) == (a < b) && (pa == pb) == (a == b) && (pa <= pb) == (a <= b) &&
                     static_cast<std::int64_t>(pa) == static_cast<std::int64_t>(a);
                const Native minimum = -static_cast<Native>((static_cast<NativeU>(1) << 127) - 1) - 1;
                if (b != 0 && !(a == minimum && b == -1)) {
                    ok = ok && same(pa / pb, a / b) && same(pa % pb, a % b);
                }
                ++cases;
            }
        }
        check(ok, "N1 +, -, *, /, %, comparisons and int64 truncation equal the native __int128 for " +
                      std::to_string(cases) + " operand pairs (edges, all magnitudes, both signs)");
    }
    {
        // The real clock with both integer types: identical everywhere,
        // including positions whose intermediate values exceed 64 bits.
        bool ok = true;
        std::size_t cases = 0;
        const int rates[] = {8000, 44100, 48000, 96000, 192000};
        const std::int64_t tempos[] = {20000, 97531, 120000, 123456, 300000};
        for (int fs : rates) {
            for (std::int64_t milli : tempos) {
                BasicMusicalClock<Native> a(fs);
                BasicMusicalClock<P> b(fs);
                a.setTempoMilli(milli);
                b.setTempoMilli(milli);
                a.start();
                b.start();
                Rng rng{static_cast<std::uint64_t>(fs) * 31 + static_cast<std::uint64_t>(milli)};
                for (int step = 0; step < 400 && ok; ++step) {
                    // Advance by up to ~2^34 samples; ~13 days of audio overall.
                    const std::int64_t adv = static_cast<std::int64_t>(rng.next() % (1ull << 34));
                    a.advance(adv);
                    b.advance(adv);
                    if (step % 50 == 7) {  // tempo changes keep the segments aligned
                        const std::int64_t t = 20000 + static_cast<std::int64_t>(rng.next() % 280001);
                        a.setTempoMilli(t);
                        b.setTempoMilli(t);
                    }
                    const std::int64_t pos = a.samplePosition();
                    const std::int64_t tick = a.tickPosition() + static_cast<std::int64_t>(rng.next() % 100000);
                    const std::int64_t fine = tick * 1024 + static_cast<std::int64_t>(rng.next() % 1024);
                    ok = b.samplePosition() == pos && b.tickPosition() == a.tickPosition() &&
                         b.sampleOfTick(tick) == a.sampleOfTick(tick) &&
                         b.firstTickAtOrAfter(pos + 17) == a.firstTickAtOrAfter(pos + 17) &&
                         b.sampleOfFine(fine) == a.sampleOfFine(fine) &&
                         b.firstFineAtOrAfter(pos + 3) == a.firstFineAtOrAfter(pos + 3) &&
                         b.fineAtSample(pos + 99) == a.fineAtSample(pos + 99) &&
                         b.samplesPerTick() == a.samplesPerTick();
                    ++cases;
                }
            }
        }
        check(ok, "N2 MusicalClock on the portable integer equals the native clock: 5 rates x 5 "
                  "tempos, random advances up to 2^34 samples, tempo changes (" +
                      std::to_string(cases) + " states, 7 queries each)");
    }
#else
    check(true, "N1 (native __int128 not available on this host: comparison skipped)");
    check(true, "N2 (native __int128 not available on this host: comparison skipped)");
#endif
#if defined(DATABREAKS_PORTABLE_INT128) || !defined(__SIZEOF_INT128__)
    const bool expectNative = false;
#else
    const bool expectNative = true;
#endif
    check(ExactInt128IsNative == expectNative &&
              std::is_class<ExactInt128>::value == !expectNative &&
              std::is_same<MusicalClock, BasicMusicalClock<ExactInt128>>::value,
          std::string("N3 The clock uses the ") + (ExactInt128IsNative ? "native" : "portable") +
              " integer, as configured (-DDATABREAKS_PORTABLE_INT128 forces the portable one)");
}

// ---------------------------------------------------------------------------
// The deterministic bank whose Engine 028 serialisation is pinned below.
// The golden values were produced by the Engine 028 (d0790c0) serializer:
// the file size, the CRC-32 stored in its trailer (the CRC of the body),
// and an independent FNV-1a-64 hash of every byte. (The CRC-32 of a whole
// file INCLUDING its correct trailer is the constant residue 0x2144DF1C for
// every file, so it must not be used as a golden value.)
constexpr std::size_t kGoldenSize = 5705;
constexpr std::uint32_t kGoldenCrc = 0x48a0ecfau;
constexpr std::uint64_t kGoldenFnv = 0xbe055ea1726fdac6ull;

std::uint64_t fnv1a64(const std::uint8_t* d, std::size_t n) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= d[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::uint32_t bodyCrc(const std::uint8_t* d, std::size_t n) { return pattern_io::crc32(d, n - 4); }

void fixCrc(std::vector<std::uint8_t>& d) {
    const std::uint32_t c = bodyCrc(d.data(), d.size());
    for (int i = 0; i < 4; ++i) d[d.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(c >> (8 * i));
}

void goldenBank(Bank& bank, Arrangement& arr) {
    bank.clearAll();
    auto d = std::make_unique<PatternData>();
    for (int id : {0, 3, 7, 12, 15}) {
        Pattern& p = d->pattern;
        p = Pattern{};
        p.configure({id % 2 ? 3 : 4, 4}, 1 + id % 3, id % 2 ? 120 : 240);
        for (int k = 0; k < 12 + id; ++k) {
            DrumStep s;
            s.enabled = k % 3 != 0;
            s.velocity = 1 + (k * 37 + id) % 127;
            s.probability = (k * 13) % 101;
            s.microTicks = (k % 7) * 9 - 27;
            p.setStep(static_cast<std::size_t>((k * 5 + id) % 27), static_cast<std::size_t>(k) % p.stepCount(), s);
        }
        for (int n = 0; n < 3 + id % 4; ++n) {
            p.addNote({n % 2 == 0, static_cast<SynthTarget>(n % 3), 30 + n * 7 + id, 1 + (n * 41) % 127,
                       (n * 29) % 101, n * 60, 1 + n * 333});
        }
        RecordedTrack& t = d->track;
        t.clear();
        for (int pass = 0; pass < 1 + id % 3; ++pass) {
            const std::uint32_t pid = t.openPass();
            for (int e = 0; e < 4 + id; ++e) {
                RecordedEvent r;
                r.kind = e % 2 ? RecordedEvent::Kind::Note : RecordedEvent::Kind::Drum;
                r.target = static_cast<SynthTarget>(e % 3);
                r.value = e % 2 ? 40 + e : e % 27;
                r.velocity = 1 + (e * 19) % 127;
                r.startFine = e * 123457 + pass;
                r.lengthFine = e % 2 ? 1000 + e : 0;
                r.captureSample = 5000000000LL + e;
                r.captureFine = -7 - e;
                r.captureLoop = 3 - pass;
                r.captureEpoch = 0xABCD0000u + static_cast<std::uint32_t>(e);
                r.sequence = 0x1234567890ULL + static_cast<std::uint64_t>(e) * 3;
                t.add(r, pid);
            }
            if (pass == 1) t.erase(0, 300000, RecordAll, pid);
        }
        if (id % 3 == 2) t.undo();
        d->quantize.grid = static_cast<QuantizeGrid>(id % 7);
        d->quantize.strength = (id * 17) % 101;
        d->quantize.swing = (id * 23) % 101;
        d->quantize.targets = 1 + static_cast<unsigned>(id) % 15;
        d->quantize.drumLanes = 0x5A5A5A5u & ((1u << 27) - 1);
        d->quantize.quantizeNoteOff = id % 2;
        bank.store(id, d->pattern, d->track, d->quantize);
    }
    arr.clear();
    arr.append(3, 2);
    arr.append(0, 999);
    arr.append(15, 1);
    arr.setEnd(ArrangementEnd::HoldLast);
}

// A ByteSink collecting into a fixed array (no allocation).
struct ArraySink final : ByteSink {
    std::array<std::uint8_t, 8192> data{};
    std::size_t size = 0;
    bool write(const std::uint8_t* d, std::size_t n) override {
        if (n > data.size() - size) return false;
        std::copy(d, d + n, data.begin() + static_cast<std::ptrdiff_t>(size));
        size += n;
        return true;
    }
};

bool sameBank(const Bank& a, const Bank& b) {
    for (int id = 0; id < static_cast<int>(Bank::Capacity); ++id) {
        if (a.used(id) != b.used(id)) return false;
        const auto x = a.info(id);
        const auto y = b.info(id);
        if (x.steps != y.steps || x.notes != y.notes || x.events != y.events ||
            x.passes != y.passes || x.quantize != y.quantize || x.bars != y.bars ||
            x.stepTicks != y.stepTicks || a.nextPassId(id) != b.nextPassId(id)) {
            return false;
        }
    }
    std::vector<std::uint8_t> s1;
    std::vector<std::uint8_t> s2;
    Arrangement empty;
    pattern_io::save(a, empty, 0, s1);
    pattern_io::save(b, empty, 0, s2);
    return s1 == s2;
}

void storageTests() {
    std::cout << "\n-- Storage boundary --\n";
    static Bank bank;
    Arrangement arr;
    goldenBank(bank, arr);
    std::vector<std::uint8_t> bytes;
    pattern_io::save(bank, arr, 7, bytes);
    check(bytes.size() == kGoldenSize && bodyCrc(bytes.data(), bytes.size()) == kGoldenCrc &&
              fnv1a64(bytes.data(), bytes.size()) == kGoldenFnv &&
              pattern_io::encodedSize(bank, arr) == kGoldenSize,
          "S1 DBPB output is byte-for-byte the Engine 028 serializer's (5,705 bytes, body CRC "
          "0x48a0ecfa, FNV-1a-64 0xbe055ea1726fdac6); encodedSize() is exact");
    {
        static ArraySink sink;
        sink.size = 0;
        const std::size_t a0 = g_allocations.load();
        const bool ok = pattern_io::save(bank, arr, 7, sink);
        const std::size_t allocs = g_allocations.load() - a0;
        check(ok && allocs == 0 && sink.size == bytes.size() &&
                  std::equal(bytes.begin(), bytes.end(), sink.data.begin()),
              "S2 Streaming save into a ByteSink: identical bytes, 0 allocations, no contiguous "
              "buffer");
    }
    {
        // Load from a ByteSource into caller-provided static scratch: 0 allocations.
        static pattern_io::LoadScratch<Bank> scratch;
        static Bank dest;
        Arrangement a2;
        int active = -1;
        const MemoryByteSource src(bytes.data(), bytes.size());
        const std::size_t a0 = g_allocations.load();
        const auto st = pattern_io::load(src, scratch, dest, a2, active);
        const std::size_t allocs = g_allocations.load() - a0;
        check(st == pattern_io::LoadStatus::Ok && allocs == 0 && active == 7 && a2 == arr &&
                  sameBank(dest, bank),
              "S3 Load from a ByteSource with caller scratch: exact, 0 allocations inside the core");
    }
    {
        // Through MemoryStorage (a flash-like backend), with atomic replace.
        static MemoryStorage<8192, 2> store;
        static pattern_io::LoadScratch<Bank> scratch;
        static Bank dest;
        Arrangement a2;
        int active = -1;
        const auto s1 = pattern_io::saveTo(store, "bank", bank, arr, 7);
        const auto l1 = pattern_io::loadFrom(store, "bank", scratch, dest, a2, active);
        const bool roundTrip = s1 == pattern_io::SaveStatus::Ok && l1 == pattern_io::LoadStatus::Ok &&
                               sameBank(dest, bank) && store.objectSize("bank") == kGoldenSize;
        // A failing write keeps the previous version. The replacement (an
        // empty bank) is a ~40-byte file, so the fault is placed inside it.
        static Bank other;
        Arrangement otherArr;
        other.clearAll();
        store.failWriteAfter(20);
        const auto s2 = pattern_io::saveTo(store, "bank", other, otherArr, 0);
        store.clearFaults();
        const bool keptAfterWriteFail = s2 == pattern_io::SaveStatus::WriteFailed &&
                                        store.objectSize("bank") == kGoldenSize &&
                                                        bodyCrc(store.objectData("bank"), kGoldenSize) == kGoldenCrc;
        // A failing commit keeps the previous version too.
        store.failNextCommit();
        const auto s3 = pattern_io::saveTo(store, "bank", other, otherArr, 0);
        const bool keptAfterCommitFail = s3 == pattern_io::SaveStatus::CommitFailed &&
                                         store.objectSize("bank") == kGoldenSize;
        const auto missing = pattern_io::loadFrom(store, "nothing", scratch, dest, a2, active);
        const bool badName = pattern_io::saveTo(store, "Bad Name", bank, arr, 7) ==
                                 pattern_io::SaveStatus::BeginFailed &&
                             !StorageBackend::validName("") && !StorageBackend::validName("a/b") &&
                             !StorageBackend::validName("0123456789012345678901234567890x") &&
                             StorageBackend::validName("bank-1.dbpb");
        check(roundTrip && keptAfterWriteFail && keptAfterCommitFail &&
                  missing == pattern_io::LoadStatus::NotFound && badName,
              "S4 StorageBackend (MemoryStorage): round trip; a failed write or commit keeps the "
              "previous version intact; NotFound; invalid names rejected");
    }
    {
        // Read failures are reported as ReadError and never change the destination.
        static MemoryStorage<8192, 2> store;
        static pattern_io::LoadScratch<Bank> scratch;
        static Bank dest;
        dest.clearAll();
        Arrangement a2;
        a2.append(1, 1);
        int active = 3;
        pattern_io::saveTo(store, "bank", bank, arr, 7);
        bool ok = true;
        for (std::size_t from : {std::size_t(0), std::size_t(10), std::size_t(17), std::size_t(500),
                                 std::size_t(3000), kGoldenSize - 4, kGoldenSize - 1}) {
            store.failReadsFrom(from);
            const auto st = pattern_io::loadFrom(store, "bank", scratch, dest, a2, active);
            ok = ok && st == pattern_io::LoadStatus::ReadError && dest.usedCount() == 0 &&
                 a2.size() == 1 && active == 3;
        }
        store.clearFaults();
        check(ok, "S5 Medium read failures at 7 offsets -> ReadError (not a format error); the "
                  "destination is unchanged");
    }
    {
        // The ByteSource load path rejects every truncation and every
        // single-byte corruption, like the Engine 028 tests require.
        static pattern_io::LoadScratch<Bank> scratch;
        static Bank dest;
        dest.clearAll();
        Arrangement a2;
        int active = -1;
        bool ok = true;
        for (std::size_t n = 0; n < bytes.size() && ok; ++n) {
            const MemoryByteSource src(bytes.data(), n);
            ok = pattern_io::load(src, scratch, dest, a2, active) != pattern_io::LoadStatus::Ok &&
                 dest.usedCount() == 0;
        }
        std::vector<std::uint8_t> m = bytes;
        for (std::size_t i = 0; i < m.size() && ok; i += 3) {
            m[i] ^= 0x41;
            const MemoryByteSource src(m.data(), m.size());
            ok = pattern_io::load(src, scratch, dest, a2, active) != pattern_io::LoadStatus::Ok &&
                 dest.usedCount() == 0;
            m[i] ^= 0x41;
        }
        check(ok, "S6 ByteSource load: every truncation and every third byte flipped are rejected; "
                  "nothing loaded");
    }
    {
        // Exact status for corrupted recorded-event fields (checksum fixed so
        // the field validation itself is reached): OutOfRange, not a later
        // consistency failure.
        static pattern_io::LoadScratch<Bank> scratch;
        static Bank dest;
        dest.clearAll();
        Arrangement a2;
        int active = -1;
        const auto info = bank.info(0);  // slot 0 is the first PATN record
        const std::size_t ev0 = 16 + 8 + 34 + 7 * info.steps + 13 * info.notes + 5 * info.passes;
        bool ok = info.events > 0;
        const std::pair<std::size_t, std::uint8_t> patches[] = {
            {3, 0},     // velocity 0
            {3, 200},   // velocity 200
            {0, 5},     // kind 5
            {1, 9},     // target 9
            {4, 2}};    // pending 2
        for (const auto& patch : patches) {
            std::vector<std::uint8_t> m = bytes;
            m[ev0 + patch.first] = patch.second;
            fixCrc(m);
            const MemoryByteSource src(m.data(), m.size());
            ok = ok && pattern_io::load(src, scratch, dest, a2, active) == pattern_io::LoadStatus::OutOfRange &&
                 dest.usedCount() == 0;
        }
        check(ok, "S8 Corrupted recorded-event fields (velocity, kind, target, pending) are rejected "
                  "as OutOfRange by the field validation");
    }
    {
        // FileStorage: a real file through the same interface.
        char tmpl[] = "/tmp/databreaks029XXXXXX";
        const char* dir = mkdtemp(tmpl);
        bool ok = dir != nullptr;
        if (ok) {
            FileStorage files(dir);
            static pattern_io::LoadScratch<Bank> scratch;
            static Bank dest;
            Arrangement a2;
            int active = -1;
            ok = pattern_io::saveTo(files, "bank.dbpb", bank, arr, 7) == pattern_io::SaveStatus::Ok &&
                 pattern_io::loadFrom(files, "bank.dbpb", scratch, dest, a2, active) ==
                     pattern_io::LoadStatus::Ok &&
                 sameBank(dest, bank) && active == 7 && a2 == arr;
            // An aborted write keeps the committed file.
            ByteSink* sink = files.beginWrite("bank.dbpb");
            const std::uint8_t junk[3] = {1, 2, 3};
            ok = ok && sink != nullptr && sink->write(junk, 3);
            files.abortWrite();
            ok = ok && pattern_io::loadFrom(files, "bank.dbpb", scratch, dest, a2, active) ==
                           pattern_io::LoadStatus::Ok &&
                 pattern_io::loadFrom(files, "missing", scratch, dest, a2, active) ==
                     pattern_io::LoadStatus::NotFound;
            const std::string base = std::string(dir) + "/bank.dbpb";
            std::remove(base.c_str());
            std::remove((base + ".tmp").c_str());
            ok = ok && std::remove(dir) == 0;  // the directory is empty: nothing leaked
        }
        check(ok, "S7 FileStorage (host): save/load through files, aborted writes keep the "
                  "committed file, temporary files are removed");
    }
}

// ---------------------------------------------------------------------------
using Pipeline = AudioPipeline<16>;

std::vector<StereoFrame> kickSample() {
    std::vector<StereoFrame> s(4800);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double v = 0.6 * std::sin(static_cast<double>(i) * 0.03) * (1.0 - static_cast<double>(i) / 4800.0);
        s[i] = {v, v * 0.9};
    }
    return s;
}

void setupPipeline(Pipeline& p) {
    static const std::vector<StereoFrame> kick = kickSample();
    p.drums().assignSample(0, &kick);
    p.drums().assignSample(5, &kick);
    p.setMasterLevel(0.9);
    Sequencer& s = p.sequencer();
    s.clock().setTempo(123.0);
    s.pattern().configure({4, 4}, 1, 240);
    for (int k = 0; k < 16; k += 3) s.pattern().setHit(0, static_cast<std::size_t>(k), 100 + k);
    s.pattern().setHit(5, 7, 127);
    s.pattern().addNote({true, SynthTarget::Poly, 60, 90, 100, 480, 900});
    s.pattern().addNote({true, SynthTarget::Mono, 36, 110, 100, 0, 300});
    s.start();
}

struct CollectSink final : BlockSink {
    std::vector<std::uint8_t> bytes;
    void consume(const void* data, std::size_t frames, SampleFormat format) override {
        const auto* b = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), b, b + frames * 2 * bytesPerSample(format));
    }
};

void audioTests() {
    std::cout << "\n-- Audio I/O boundary --\n";
    constexpr std::size_t kFrames = 48000;
    // Reference: render directly with 512-frame blocks and convert like the
    // render demos' WAV writers (lround(clamp * 32767)).
    std::vector<std::int16_t> reference;
    {
        auto p = std::make_unique<Pipeline>();
        setupPipeline(*p);
        std::vector<StereoFrame> buf(512);
        for (std::size_t pos = 0; pos < kFrames; pos += 512) {
            std::fill(buf.begin(), buf.end(), StereoFrame{});
            const std::size_t n = std::min<std::size_t>(512, kFrames - pos);
            p->render(buf.data(), n);
            for (std::size_t i = 0; i < n; ++i) {
                reference.push_back(static_cast<std::int16_t>(std::lround(clampAudio(buf[i].left) * 32767.0)));
                reference.push_back(static_cast<std::int16_t>(std::lround(clampAudio(buf[i].right) * 32767.0)));
            }
        }
    }
    bool ok = true;
    double peak = 0.0;
    for (std::int16_t v : reference) peak = std::max(peak, std::abs(static_cast<double>(v)));
    std::string bad;
    for (std::size_t block : {std::size_t(1), std::size_t(7), std::size_t(64), std::size_t(256),
                              std::size_t(1000), std::size_t(4096)}) {
        auto p = std::make_unique<Pipeline>();
        setupPipeline(*p);
        PipelineRenderer<Pipeline> renderer(*p);
        AudioStreamConfig cfg;
        cfg.blockFrames = block;
        cfg.format = SampleFormat::Int16;
        auto device = std::make_unique<OfflineAudioDevice<>>(cfg);
        CollectSink sink;
        const bool started = device->start(renderer);
        device->run(kFrames, &sink);
        const bool same = started && sink.bytes.size() == reference.size() * 2 &&
                          std::equal(reference.begin(), reference.end(),
                                     reinterpret_cast<const std::int16_t*>(sink.bytes.data()));
        if (!same) bad += " " + std::to_string(block);
        ok = ok && same;
    }
    check(ok && peak > 1000.0,
          "A1 Int16 stream through PipelineRenderer + OfflineAudioDevice equals the direct render "
          "converted like the demo WAV writers, for device blocks 1/7/64/256/1000/4096" + bad);
    {
        const StereoFrame in[4] = {{0.0, -0.0}, {1.5, -1.5}, {0.25, -0.25}, {1.0, -1.0}};
        std::int16_t i16[8];
        std::int32_t i32[8];
        float f32[8];
        audio_convert::convert(in, 4, i16, 0, SampleFormat::Int16);
        audio_convert::convert(in, 4, i32, 0, SampleFormat::Int32);
        audio_convert::convert(in, 4, f32, 0, SampleFormat::Float32);
        check(i16[0] == 0 && i16[2] == 32767 && i16[3] == -32767 && i16[4] == 8192 && i16[5] == -8192 &&
                  i32[2] == 2147483647 && i32[3] == -2147483647 && i32[4] == 536870912 &&
                  f32[2] == 1.0f && f32[3] == -1.0f && f32[4] == 0.25f,
              "A2 Conversion: interleaved L/R, clamped to +-1, Int16 x 32767, Int32 x 2147483647, "
              "Float32 exact");
    }
    {
        AudioStreamConfig c;
        bool ok2 = c.valid() && c.blockBytes() == 1024;
        c.sampleRate = 44100;
        ok2 = ok2 && !c.valid();
        c.sampleRate = SAMPLE_RATE;
        c.blockFrames = 0;
        ok2 = ok2 && !c.valid();
        c.blockFrames = 9000;
        ok2 = ok2 && !c.valid();
        c.blockFrames = 64;
        c.channels = 1;
        ok2 = ok2 && !c.valid();
        OfflineAudioDevice<64> tiny(AudioStreamConfig{});  // 1,024-byte blocks do not fit 64 bytes
        auto p = std::make_unique<Pipeline>();
        PipelineRenderer<Pipeline> r(*p);
        check(ok2 && !tiny.start(r) && !tiny.run(10),
              "A3 Stream configuration validated (48 kHz stereo only, 1..8192 frames, buffer size)");
    }
    {
        auto p = std::make_unique<Pipeline>();
        setupPipeline(*p);
        PipelineRenderer<Pipeline> renderer(*p);
        AudioStreamConfig cfg;
        cfg.blockFrames = 128;
        cfg.format = SampleFormat::Float32;
        auto device = std::make_unique<OfflineAudioDevice<>>(cfg);
        device->start(renderer);
        const std::size_t a0 = g_allocations.load();
        device->run(48000);
        const std::size_t allocs = g_allocations.load() - a0;
        check(allocs == 0 && renderer.renderedFrames() == 48000 && device->blockCount() == 375,
              "A4 0 allocations while the device renders 1 s (Float32, 128-frame blocks)");
    }
}

// ---------------------------------------------------------------------------
struct RecordingHandler final : ControlHandler {
    std::vector<std::string> log;
    void onButton(std::uint16_t id, bool down, std::int64_t t) override {
        log.push_back("B" + std::to_string(id) + (down ? "d" : "u") + "@" + std::to_string(t));
    }
    void onEncoder(std::uint16_t id, int delta, std::int64_t t) override {
        log.push_back("E" + std::to_string(id) + ":" + std::to_string(delta) + "@" + std::to_string(t));
    }
};

void inputTests() {
    std::cout << "\n-- Input boundary --\n";
    {
        // Pads through the router into the sequencer: sounds and is recorded.
        auto seq = std::make_unique<Sequencer>(48000);
        seq->clock().setTempo(120.0);
        seq->arm();
        seq->start();
        const InputEvent script[] = {{InputEventType::PadDown, 4, 100, 1000},
                                     {InputEventType::PadUp, 4, 0, 3000},
                                     {InputEventType::PadDown, 26, 7, 5000},
                                     {InputEventType::ButtonDown, 2, 0, 6000},
                                     {InputEventType::EncoderDelta, 1, -3, 6100},
                                     {InputEventType::ButtonUp, 2, 0, 7000},
                                     {InputEventType::PadDown, 27, 100, 8000},   // no pad 27
                                     {InputEventType::PadDown, 3, 0, 8100},      // velocity 0
                                     {InputEventType::PadDown, 3, 128, 8200},    // velocity 128
                                     {InputEventType::PadUp, 30, 0, 8300},       // no pad 30
                                     {InputEventType::PadDown, 0, 127, 9000}};
        ScriptedInputSource source(script, sizeof(script) / sizeof(script[0]));
        RecordingHandler handler;
        Sequencer& s = *seq;
        InputRouter router([&s](const LiveEvent& e) { return s.liveEvent(e); }, &handler);
        std::vector<std::pair<std::int64_t, int>> hits;
        for (std::int64_t pos = 0; pos < 12288; pos += 256) {
            source.advanceTo(pos + 256);  // events of this block
            router.pump(source);
            s.process(256, [&](const SequencerEvent& e) {
                if (e.kind == SequencerEvent::Kind::Drum) hits.push_back({pos + static_cast<std::int64_t>(e.offset), e.value});
            });
        }
        const RecordedTrack& t = s.recorder().track();
        const auto& st = router.stats();
        const bool ok = hits == std::vector<std::pair<std::int64_t, int>>{{1000, 4}, {5000, 26}, {9000, 0}} &&
                        t.size() == 3 && t.at(0).captureSample == 1000 && t.at(1).velocity == 7 &&
                        st.padHits == 3 && st.padReleases == 1 && st.controls == 3 && st.invalid == 4 &&
                        handler.log == std::vector<std::string>{"B2d@6000", "E1:-3@6100", "B2u@7000"} &&
                        source.finished();
        check(ok, "I1 PadDown -> LiveEvent at its sample (sounds and is recorded); PadUp counted; "
                  "buttons/encoders to the handler; invalid pad/velocity rejected and counted");
    }
    {
        // No handler installed: controls are counted as unhandled; a full
        // live queue is counted as rejected.
        auto seq = std::make_unique<Sequencer>(48000);
        Sequencer& s = *seq;
        InputRouter router([&s](const LiveEvent& e) { return s.postLive(e); });
        int delivered = 0;
        for (int i = 0; i < 300; ++i) delivered += router.dispatch({InputEventType::PadDown, 1, 64, 10}) ? 1 : 0;
        router.dispatch({InputEventType::EncoderDelta, 0, 1, 0});
        const auto& st = router.stats();
        check(delivered == 255 && st.rejected == 45 && st.unhandled == 1,
              "I2 Without a handler controls are counted; a full live queue counts as rejected");
    }
    {
        // An interrupt-like producer thread pushes through QueueInputSource
        // while the audio thread pumps it (run under TSan).
        auto seq = std::make_unique<Sequencer>(48000);
        Sequencer& s = *seq;
        s.start();
        static QueueInputSource<128> queue;
        InputRouter router([&s](const LiveEvent& e) { return s.liveEvent(e); });
        std::atomic<bool> done{false};
        std::atomic<int> pushed{0};
        std::thread producer([&] {
            for (int i = 0; i < 20000; ++i) {
                if (queue.push({InputEventType::PadDown, static_cast<std::uint16_t>(i % 27),
                                static_cast<std::int16_t>(1 + i % 127), 1000000000})) {
                    pushed.fetch_add(1, std::memory_order_relaxed);
                }
                if (i % 32 == 0) std::this_thread::yield();
            }
            done.store(true, std::memory_order_release);
        });
        std::size_t blocks = 0;
        while (!done.load(std::memory_order_acquire) || blocks < 500) {
            router.pump(queue);
            s.process(64, [](const SequencerEvent&) {});
            ++blocks;
        }
        producer.join();
        router.pump(queue, 100000);
        const auto& st = router.stats();
        check(static_cast<int>(st.padHits + st.rejected) == pushed.load() &&
                  pushed.load() + static_cast<int>(queue.droppedCount()) == 20000,
              "I3 QueueInputSource from a producer thread: every event routed or counted "
              "(lock-free SPSC; run under TSan)");
    }
    {
        auto seq = std::make_unique<Sequencer>(48000);
        Sequencer& s = *seq;
        s.start();
        static QueueInputSource<256> queue;
        InputRouter router([&s](const LiveEvent& e) { return s.liveEvent(e); });
        const std::size_t a0 = g_allocations.load();
        for (int b = 0; b < 200; ++b) {
            for (int i = 0; i < 8; ++i) queue.push({InputEventType::PadDown, static_cast<std::uint16_t>(i), 90, b * 128 + i * 10});
            router.pump(queue);
            s.process(128, [](const SequencerEvent&) {});
        }
        const std::size_t allocs = g_allocations.load() - a0;
        check(allocs == 0 && router.stats().padHits == 1600,
              "I4 0 allocations: queue push, pump, routing and sequencer processing (" +
                  std::to_string(allocs) + " allocations, " +
                  std::to_string(router.stats().padHits) + " hits)");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 029 - target portability boundary tests\n";
    std::cout << "Host-side tests; not validated on FM-1 hardware.\n";
    int128Tests();
    storageTests();
    audioTests();
    inputTests();
    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
