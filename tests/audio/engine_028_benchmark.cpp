// DATABREAKs Engine 028 - pattern bank HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose CPU and
// memory budget are not documented or verified.

#include "audio_pipeline.h"
#include "../../core/sequencer/pattern_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

namespace {
std::atomic<std::size_t> g_allocations{0};
}

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
using Clock = std::chrono::steady_clock;
using Bank = DefaultPatternBank;

namespace {

std::uint64_t sink = 0;
double us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

// Typical: 1 bar, 16 steps on 6 pads, 8 notes, 64 recorded events.
// Heavy: 4 bars, 64 steps on 27 pads (1,728 steps), 128 notes, 500 events.
void fill(PatternData& d, bool heavy) {
    Pattern& p = d.pattern;
    p.configure({4, 4}, heavy ? 4 : 1, 240);
    p.clear();
    const std::size_t steps = heavy ? 64 : 16;
    const std::size_t pads = heavy ? 27 : 6;
    for (std::size_t pad = 0; pad < pads; ++pad) {
        for (std::size_t k = 0; k < steps; ++k) p.setHit(pad, k, 60 + static_cast<int>(k % 60));
    }
    const int notes = heavy ? 128 : 8;
    for (int i = 0; i < notes; ++i) {
        p.addNote({true, static_cast<SynthTarget>(i % 3), 40 + i % 60, 90, 100, i * 100, 200});
    }
    d.track.clear();
    const int events = heavy ? 500 : 64;
    const std::uint32_t pass = d.track.openPass();
    for (int i = 0; i < events; ++i) {
        RecordedEvent e;
        e.kind = RecordedEvent::Kind::Drum;
        e.value = i % 27;
        e.velocity = 100;
        e.startFine = i * 4000;
        e.sequence = static_cast<std::uint64_t>(i + 1);
        d.track.add(e, pass);
    }
}

std::unique_ptr<Sequencer> sequencerWith(bool heavy, int patterns) {
    auto s = std::make_unique<Sequencer>(48000);
    s->clock().setTempo(120.0);
    auto d = std::make_unique<PatternData>();
    fill(*d, heavy);
    for (int id = 1; id < patterns; ++id) s->replacePattern(id, d->pattern, d->track, d->quantize);
    s->replacePattern(0, d->pattern, d->track, d->quantize);
    return s;
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 028 - pattern bank benchmark (HOST ONLY, not FM-1)\n\n");

    std::printf("Memory (bytes):\n");
    std::printf("  Pattern %zu, RecordedTrack %zu, QuantizeSettings %zu (the working set, once)\n",
                sizeof(Pattern), sizeof(RecordedTrack), sizeof(QuantizeSettings));
    std::printf("  PatternBank<2,8,8192,1024,2048> %zu: StepRecord %zu x 8192, NoteRecord %zu x 1024, "
                "EventRecord %zu x 2048, slot metadata %zu x 16\n",
                sizeof(Bank), sizeof(Bank::StepRecord), sizeof(Bank::NoteRecord),
                sizeof(Bank::EventRecord),
                (sizeof(Bank) - 8192 * sizeof(Bank::StepRecord) - 1024 * sizeof(Bank::NoteRecord) -
                 2048 * sizeof(Bank::EventRecord)) / 16);
    std::printf("  Arrangement %zu, SequencerCommand %zu, SequencerEvent %zu\n", sizeof(Arrangement),
                sizeof(SequencerCommand), sizeof(SequencerEvent));
    std::printf("  Sequencer %zu (Engine 027: 429,328), AudioPipeline<16> %zu (Engine 027: 693,904)\n",
                sizeof(Sequencer), sizeof(AudioPipeline<16>));
    std::printf("  For comparison, 16 full working sets would be %zu\n\n",
                16 * (sizeof(Pattern) + sizeof(RecordedTrack)));

    for (bool heavy : {false, true}) {
        auto d = std::make_unique<PatternData>();
        fill(*d, heavy);
        std::printf("%s pattern: %zu steps, %zu notes, %zu events -> bank bytes %zu (steps %zu + notes %zu + events %zu)\n",
                    heavy ? "Heavy" : "Typical", Bank::stepsNeeded(d->pattern), d->pattern.noteCount(),
                    d->track.size(),
                    Bank::stepsNeeded(d->pattern) * sizeof(Bank::StepRecord) +
                        d->pattern.noteCount() * sizeof(Bank::NoteRecord) +
                        d->track.size() * sizeof(Bank::EventRecord),
                    Bank::stepsNeeded(d->pattern) * sizeof(Bank::StepRecord),
                    d->pattern.noteCount() * sizeof(Bank::NoteRecord),
                    d->track.size() * sizeof(Bank::EventRecord));
    }
    std::printf("\n");

    // ---- Audio thread: switching -------------------------------------------
    for (bool heavy : {false, true}) {
        const int patterns = heavy ? 4 : 16;
        auto s = sequencerWith(heavy, patterns);
        s->start();
        double worst = 0.0;
        double total = 0.0;
        int n = 0;
        const std::size_t a0 = g_allocations.load();
        for (int i = 0; i < 400; ++i) {
            s->requestPatternSwitch((i + 1) % patterns, SwitchTiming::Immediate);
            const auto t0 = Clock::now();
            s->process(256, [](const SequencerEvent& e) { sink += e.offset; });
            const double t = us(Clock::now() - t0);
            total += t;
            worst = std::max(worst, t);
            ++n;
        }
        std::printf("Switch inside a 256-frame block (%s, %d patterns): mean %.1f us, worst %.1f us, "
                    "allocs %zu (block = 5333 us)\n",
                    heavy ? "heavy" : "typical", patterns, total / n, worst,
                    g_allocations.load() - a0);
        // Same blocks without switching, for reference.
        double plain = 0.0;
        for (int i = 0; i < 400; ++i) {
            const auto t0 = Clock::now();
            s->process(256, [](const SequencerEvent& e) { sink += e.offset; });
            plain += us(Clock::now() - t0);
        }
        std::printf("  same pattern, no switch: mean %.1f us per block\n", plain / 400);
    }
    {
        // Song mode with short loops: several transitions per block.
        auto s = sequencerWith(false, 4);
        for (int id = 0; id < 4; ++id) s->setPatternLength(id, {1, 32}, 1, 60);  // 120-tick loops
        for (int i = 0; i < 64; ++i) s->arrangementAppend(i % 4, 1);
        s->arrangementSetEnd(ArrangementEnd::Loop);
        s->startArrangement();
        s->start();
        double worst = 0.0;
        double total = 0.0;
        for (int i = 0; i < 200; ++i) {
            const auto t0 = Clock::now();
            s->process(4096, [](const SequencerEvent& e) { sink += e.offset; });
            const double t = us(Clock::now() - t0);
            total += t;
            worst = std::max(worst, t);
        }
        std::printf("Song with 120-tick loops (%.1f transitions per 4096-frame block): mean %.1f us, worst %.1f us\n",
                    static_cast<double>(s->patternSwitchCount()) / 200.0, total / 200, worst);
    }

    // ---- Bank operations ---------------------------------------------------------
    std::printf("\nBank operations (typical patterns, 12 of 16 slots used):\n");
    {
        static Bank b;
        auto d = std::make_unique<PatternData>();
        fill(*d, false);
        for (int id = 0; id < 12; ++id) b.store(id, d->pattern, d->track, d->quantize);
        const int reps = 2000;
        auto time = [&](const char* name, auto&& fn) {
            const auto t0 = Clock::now();
            for (int i = 0; i < reps; ++i) fn(i);
            std::printf("  %-34s %8.2f us per call\n", name, us(Clock::now() - t0) / reps);
        };
        time("lookup (validId + used + info)", [&](int i) {
            const int id = i % 20;
            sink += Bank::validId(id) && b.used(id) ? b.info(id).steps : 0;
        });
        time("store (re-store slot 5)", [&](int) { b.store(5, d->pattern, d->track, d->quantize); });
        time("load (slot 5)", [&](int) { b.load(5, d->pattern, d->track, d->quantize); });
        time("copy (slot 3 -> 13)", [&](int) { b.copy(3, 13); });
        time("duplicate + erase", [&](int) { b.erase(b.duplicate(2)); });
        time("clearContents + re-store", [&](int) {
            b.clearContents(7);
            b.store(7, d->pattern, d->track, d->quantize);
        });
    }

    // ---- Serialisation (not real-time) ---------------------------------------------
    std::printf("\nSerialisation (outside the audio thread):\n");
    for (bool heavy : {false, true}) {
        const int patterns = heavy ? 4 : 16;
        auto s = sequencerWith(heavy, patterns);
        for (int i = 0; i < patterns; ++i) s->arrangementAppend(i, 2);
        s->commitActivePattern();
        std::vector<std::uint8_t> bytes;
        const auto t0 = Clock::now();
        for (int i = 0; i < 20; ++i) pattern_io::save(s->bank(), s->arrangement(), 0, bytes);
        const double save = us(Clock::now() - t0) / 20;
        static Bank loaded;
        Arrangement arr;
        int active = 0;
        const auto t1 = Clock::now();
        pattern_io::LoadStatus st = pattern_io::LoadStatus::Ok;
        for (int i = 0; i < 20; ++i) st = pattern_io::load(bytes.data(), bytes.size(), loaded, arr, active);
        const double load = us(Clock::now() - t1) / 20;
        std::printf("  %d %s patterns + arrangement: %zu bytes (%.0f per pattern); save %.0f us, load %.0f us (%s)\n",
                    patterns, heavy ? "heavy" : "typical", bytes.size(),
                    static_cast<double>(bytes.size()) / patterns, save, load, pattern_io::toString(st));
    }
    {
        auto s = std::make_unique<Sequencer>(48000);
        std::vector<std::uint8_t> bytes;
        pattern_io::save(s->bank(), s->arrangement(), 0, bytes);
        std::printf("  empty default bank (1 empty pattern): %zu bytes\n", bytes.size());
    }
    std::printf("\nsink %llu\n", static_cast<unsigned long long>(sink % 10));
    return 0;
}
