
// DATABREAKs Engine 025 - sequencer HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose CPU and
// memory budget are not documented or verified.

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <vector>

namespace {
std::atomic<std::size_t> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace databreaks;

namespace {

double cpuSeconds() { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }
std::uint64_t sink = 0;

void denseDrums(Pattern& p) {
    p.configure({4, 4}, 1, 240);
    for (std::size_t pad = 0; pad < 27; ++pad) {
        for (std::size_t k = 0; k < 16; ++k) p.setHit(pad, k, 100);
    }
}

void denseNotes(Pattern& p) {
    for (int i = 0; i < 48; ++i) {
        p.addNote({true, static_cast<SynthTarget>(i % 3), 40 + (i * 5) % 40, 90, 100,
                   static_cast<std::int64_t>(i) * 80, 200});
    }
}

// Sequencer alone (events go to a counting sink): seconds of audio covered
// per CPU second, and microseconds per block.
void sequencerRow(const char* name, Sequencer& s, std::size_t block, double seconds) {
    const std::size_t frames = static_cast<std::size_t>(seconds * SAMPLE_RATE);
    s.clock().reset();
    s.start();
    std::uint64_t events = 0;
    const std::size_t allocBefore = g_allocations.load();
    const double t0 = cpuSeconds();
    std::size_t blocks = 0;
    for (std::size_t pos = 0; pos < frames; pos += block, ++blocks) {
        s.process(std::min(block, frames - pos), [&](const SequencerEvent& e) {
            ++events;
            sink += e.offset;
        });
    }
    const double cpu = cpuSeconds() - t0;
    s.stop();
    s.process(1, [](const SequencerEvent&) {});
    std::printf("  %-34s block %5zu  %8.3f us/block  %7.1f ns/frame  RT x%9.1f  events %8llu  allocs %zu\n",
                name, block, 1e6 * cpu / static_cast<double>(blocks),
                1e9 * cpu / static_cast<double>(frames), cpu > 0 ? seconds / cpu : 0.0,
                static_cast<unsigned long long>(events), g_allocations.load() - allocBefore);
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 025 - sequencer HOST benchmark\n");
    std::printf("Not FM-1 hardware. Results depend on this machine and build.\n");
#if defined(__OPTIMIZE__)
    std::printf("Compiler optimisation: ON\n");
#else
    std::printf("Compiler optimisation: OFF (unoptimised build; slower)\n");
#endif
    std::printf("sizeof(MusicalClock) %zu, sizeof(Pattern) %zu, sizeof(Sequencer) %zu, "
                "sizeof(AudioPipeline<16>) %zu bytes\n\n",
                sizeof(MusicalClock), sizeof(Pattern), sizeof(Sequencer),
                sizeof(AudioPipeline<16>));

    std::printf("A. Clock conversions (123.456 BPM, 44.1 kHz)\n");
    {
        MusicalClock c(44100);
        c.setTempo(123.456);
        const int n = 20000000;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) sink += static_cast<std::uint64_t>(c.sampleOfTick(i * 7));
        auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) sink += static_cast<std::uint64_t>(c.firstTickAtOrAfter(i * 13));
        auto t2 = std::chrono::steady_clock::now();
        std::printf("  sampleOfTick       %.2f ns/call\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        std::printf("  firstTickAtOrAfter %.2f ns/call\n",
                    std::chrono::duration<double, std::nano>(t2 - t1).count() / n);
    }

    static Sequencer s;
    std::printf("\nB. Sequencer alone, 120 BPM, 60 s of audio\n");
    for (std::size_t block : {16u, 128u, 1024u, 4096u}) {
        sequencerRow("Empty pattern", s, block, 60.0);
    }
    denseDrums(s.pattern());
    for (std::size_t block : {16u, 128u, 1024u, 4096u}) {
        sequencerRow("27 pads on every 16th", s, block, 60.0);
    }
    denseNotes(s.pattern());
    for (std::size_t block : {16u, 128u, 1024u, 4096u}) {
        sequencerRow("27 pads + 48 notes / bar", s, block, 60.0);
    }

    std::printf("\nC. Maximum event load: 1024 events in one block (27 pads x 16ths, 300 BPM)\n");
    {
        static Sequencer m;
        m.clock().setTempo(300.0);
        m.pattern().configure({4, 4}, 16, 240);
        for (std::size_t pad = 0; pad < 27; ++pad)
            for (std::size_t k = 0; k < 256; ++k) m.pattern().setHit(pad, k, 100);
        std::size_t events = 0;
        const int reps = 2000;
        const std::size_t allocBefore = g_allocations.load();
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r) {
            m.clock().reset();
            m.start();
            events = 0;
            m.process(96000, [&](const SequencerEvent& e) { ++events; sink += e.offset; });
        }
        auto t1 = std::chrono::steady_clock::now();
        std::printf("  %zu events (cap %zu, %llu dropped in total): %.1f us per block, %.1f ns per event, allocs %zu\n",
                    events, Sequencer::MaxEventsPerBlock,
                    static_cast<unsigned long long>(m.droppedEventCount()),
                    std::chrono::duration<double, std::micro>(t1 - t0).count() / reps,
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / reps / static_cast<double>(events),
                    g_allocations.load() - allocBefore);
    }

    std::printf("\nD. Complete pipeline, sequencer playing (drums + 3 synths, effects bypassed / on)\n");
    {
        static AudioPipeline<16> p;
        static std::vector<StereoFrame> hit(9600, StereoFrame{0.1, 0.1});
        for (std::size_t pad = 0; pad < 27; ++pad) p.drums().assignSample(pad, &hit);
        denseDrums(p.sequencer().pattern());
        denseNotes(p.sequencer().pattern());
        for (int fx = 0; fx < 2; ++fx) {
            for (std::size_t i = 0; i < p.effects().size(); ++i) {
                p.effects().slot(i)->setBypassed(fx == 0, true);
            }
            for (std::size_t block : {16u, 128u, 1024u}) {
                const double seconds = 10.0;
                const std::size_t frames = static_cast<std::size_t>(seconds * SAMPLE_RATE);
                std::vector<StereoFrame> buf(block);
                p.sequencer().clock().reset();
                p.sequencer().start();
                const std::size_t allocBefore = g_allocations.load();
                const double t0 = cpuSeconds();
                for (std::size_t pos = 0; pos < frames; pos += block) {
                    std::fill(buf.begin(), buf.end(), StereoFrame{});
                    p.render(buf.data(), std::min(block, frames - pos));
                    sink += static_cast<std::uint64_t>(buf[0].left * 1000.0);
                }
                const double cpu = cpuSeconds() - t0;
                p.sequencer().stop();
                std::printf("  %-34s block %5zu  %7.1f ns/frame  RT x%7.1f  allocs %zu\n",
                            fx == 0 ? "Pipeline, effects bypassed" : "Pipeline, all 6 effects on",
                            block, 1e9 * cpu / static_cast<double>(frames),
                            cpu > 0 ? seconds / cpu : 0.0, g_allocations.load() - allocBefore);
            }
        }
    }

    std::printf("\nsink (prevents dead-code elimination): %llu\n",
                static_cast<unsigned long long>(sink));
    return 0;
}
