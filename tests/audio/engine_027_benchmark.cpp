// DATABREAKs Engine 027 - quantization HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose CPU and
// memory budget are not documented or verified.

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

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
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t sink = 0;

double us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

std::unique_ptr<Sequencer> makeSequencer() {
    auto s = std::make_unique<Sequencer>(48000);
    s->clock().setTempo(120.0);
    s->pattern().configure({4, 4}, 1, 240);
    return s;
}

// Same fill as the Engine 026 benchmark: `count` events through capture.
void fillTrack(Sequencer& s, int count) {
    s.start();
    int made = 0;
    while (made < count) {
        s.arm();
        const std::int64_t base = s.clock().samplePosition();
        const int n = std::min(500, count - made);
        for (int k = 0; k < n; ++k) {
            const std::int64_t at = base + (k * 191) % 90000;
            if (k % 8 == 0) {
                s.liveEvent({LiveEventType::NoteOn, static_cast<SynthTarget>(k % 3), 40 + k % 40, 90, at});
                s.liveEvent({LiveEventType::NoteOff, static_cast<SynthTarget>(k % 3), 40 + k % 40, 0, at + 2000});
            } else {
                s.liveEvent({LiveEventType::DrumHit, SynthTarget::Mono, k % 27, 100, at});
            }
        }
        for (int b = 0; b < 375; ++b) s.process(256, [](const SequencerEvent&) {});
        s.disarm();
        made += n;
    }
}

struct Row {
    double meanUs = 0.0;
    double worstUs = 0.0;
    std::uint64_t events = 0;
    std::size_t allocs = 0;
};

template <typename Before>
Row timeBlocks(Sequencer& s, std::size_t block, std::size_t blocks, Before before) {
    Row r;
    const std::size_t a0 = g_allocations.load();
    double total = 0.0;
    for (std::size_t b = 0; b < blocks; ++b) {
        before(b);
        const auto t0 = Clock::now();
        s.process(block, [&](const SequencerEvent& e) {
            ++r.events;
            sink += e.offset;
        });
        const double t = us(Clock::now() - t0);
        total += t;
        r.worstUs = std::max(r.worstUs, t);
    }
    r.meanUs = total / static_cast<double>(blocks);
    r.allocs = g_allocations.load() - a0;
    return r;
}

void print(const char* name, std::size_t block, const Row& r) {
    const double blockUs = 1e6 * static_cast<double>(block) / 48000.0;
    std::printf("  %-50s block %4zu  mean %7.3f us  worst %7.2f us  (%4.2f%% of block)  events %6llu  allocs %zu\n",
                name, block, r.meanUs, r.worstUs, 100.0 * r.meanUs / blockUs,
                static_cast<unsigned long long>(r.events), r.allocs);
}

QuantizeSettings q16(int strength, int swing = 0, bool noteOff = false) {
    QuantizeSettings q;
    q.grid = QuantizeGrid::Sixteenth;
    q.strength = strength;
    q.swing = swing;
    q.quantizeNoteOff = noteOff;
    return q;
}

auto nothing = [](std::size_t) {};

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 027 - quantization benchmark (HOST ONLY, not FM-1)\n\n");
    std::printf("Memory (bytes): RecordedEvent %zu (Engine 026: 88), RecordedTrack %zu, "
                "Sequencer %zu (Engine 026: 411,328), QuantizeSettings %zu, SequencerCommand %zu, "
                "AudioPipeline<16> %zu (Engine 026: 675,904)\n\n",
                sizeof(RecordedEvent), sizeof(RecordedTrack), sizeof(Sequencer),
                sizeof(QuantizeSettings), sizeof(SequencerCommand), sizeof(AudioPipeline<16>));

    {
        // Pure quantizer cost.
        const QuantizeSettings s = q16(75, 40, true);
        const std::int64_t L = 3840 * 1024;
        const int n = 2000000;
        std::int64_t acc = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < n; ++i) acc += quantize::position((i * 7919LL) % L, L, s);
        const double t = us(Clock::now() - t0);
        sink += static_cast<std::uint64_t>(acc);
        std::printf("quantize::position: %.1f ns per call\n\n", 1000.0 * t / n);
    }

    std::printf("Audio thread (Sequencer::process), full 2,048-event track, 8 loops:\n");
    for (std::size_t block : {64u, 256u}) {
        const std::size_t blocks = 8 * 96000 / block;
        for (int mode = 0; mode < 4; ++mode) {
            auto s = makeSequencer();
            fillTrack(*s, 2048);
            const char* name = "quantize off (Engine 026 path)";
            if (mode == 1) { s->setQuantize(q16(100)); name = "16th, 100%"; }
            if (mode == 2) { s->setQuantize(q16(60, 50, true)); name = "16th, 60%, swing 50, note-offs"; }
            if (mode == 3) {
                QuantizeSettings w;
                w.grid = QuantizeGrid::Whole;
                w.strength = 100;
                s->setQuantize(w);
                name = "whole note, 100%";
            }
            s->process(block, [](const SequencerEvent&) {});  // refresh outside the timing
            print(name, block, timeBlocks(*s, block, blocks, nothing));
        }
        {
            // A settings change every block (worst case: refresh each block).
            auto s = makeSequencer();
            fillTrack(*s, 2048);
            Row r = timeBlocks(*s, block, blocks, [&](std::size_t b) {
                s->setQuantize(q16(1 + static_cast<int>(b % 100), static_cast<int>(b % 7) * 10));
            });
            print("settings change EVERY block (refresh each block)", block, r);
        }
        {
            // Recording into a 1,536-event track with quantization on: each
            // block with a captured hit refreshes the cache.
            auto s = makeSequencer();
            fillTrack(*s, 1536);
            s->setQuantize(q16(100, 30));
            s->arm();
            std::int64_t next = s->clock().samplePosition();
            Row r = timeBlocks(*s, block, blocks / 2, [&](std::size_t) {
                const std::int64_t pos = s->clock().samplePosition();
                for (; next < pos + static_cast<std::int64_t>(block); next += 450) {
                    s->liveEvent({LiveEventType::DrumHit, SynthTarget::Mono, static_cast<int>(next) % 27, 90, next});
                }
            });
            print("capture into 1,536 events, quantize on", block, r);
        }
    }

    {
        auto s = makeSequencer();
        fillTrack(*s, 2048);
        double worst = 0.0;
        double total = 0.0;
        for (int i = 0; i < 200; ++i) {
            s->setQuantize(q16(1 + i % 100, i % 3 * 25, i % 2 == 0));
            const auto t0 = Clock::now();
            sink += static_cast<std::uint64_t>(s->playedStartFine(0));  // forces the refresh
            const double t = us(Clock::now() - t0);
            total += t;
            worst = std::max(worst, t);
        }
        std::printf("\nCache refresh (2,048 events): mean %.2f us, worst %.2f us; refreshes %llu\n",
                    total / 200.0, worst, static_cast<unsigned long long>(s->quantizeRefreshCount()));
    }
    {
        auto s = makeSequencer();
        SequencerCommand c;
        c.type = SequencerCommandType::SetQuantize;
        c.quantize = q16(50);
        const auto t0 = Clock::now();
        for (int i = 0; i < 63; ++i) s->postCommand(c);
        std::printf("Control thread: postCommand(SetQuantize) %.1f ns per call\n",
                    1000.0 * us(Clock::now() - t0) / 63);
    }
    std::printf("\nsink %llu\n", static_cast<unsigned long long>(sink % 10));
    return 0;
}
