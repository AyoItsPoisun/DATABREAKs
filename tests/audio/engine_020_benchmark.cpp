
// DATABREAKs Engine 020 - drum engine host benchmark.
//
// Reports measurements only; there are no pass/fail thresholds, because
// timings depend on the machine, the compiler flags and system load.
// These numbers describe THIS HOST and say nothing about FM-1 hardware,
// whose processor is not yet documented.
//
// Real-time factor = rendered audio seconds / CPU seconds spent rendering.
// Greater than 1 means faster than real time on this host.

#include "drum_engine.h"

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

constexpr double kSeconds = 10.0;  // audio rendered per measurement

double cpuSeconds() {
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
}

// High-resolution wall clock for short intervals.
double wallSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
        clock::now().time_since_epoch()).count();
}

// 12 s stereo test tone so voices stay active for the whole measurement.
std::vector<StereoFrame> makeTone(std::size_t pad) {
    const std::size_t frames = 12 * static_cast<std::size_t>(SAMPLE_RATE);
    std::vector<StereoFrame> s(frames);
    const double step = 2.0 * PI * (110.0 + 20.0 * pad) / SAMPLE_RATE;
    for (std::size_t i = 0; i < frames; ++i) {
        const double v = 0.1 * std::sin(step * static_cast<double>(i));
        s[i] = {v, -v};
    }
    return s;
}

struct Result {
    std::size_t activeVoices = 0;
    double cpu = 0.0;
    double realTimeFactor = 0.0;
    std::size_t allocations = 0;
    double checksum = 0.0;
};

template <std::size_t Capacity>
Result renderWith(std::size_t voicesWanted,
                  std::size_t blockSize,
                  const std::vector<std::vector<StereoFrame>>& tones) {
    static DrumEngine<Capacity> engine;  // static: large object, reused
    engine.stopAll();
    engine.setRetriggerPolicy(RetriggerPolicy::Overlap);
    for (std::size_t pad = 0; pad < 27; ++pad) {
        engine.assignSample(pad, &tones[pad]);
        engine.setChokeGroup(pad, 0);
    }
    for (std::size_t v = 0; v < voicesWanted; ++v) {
        engine.trigger(v % 27, 127);
    }

    const std::size_t total =
        static_cast<std::size_t>(kSeconds * SAMPLE_RATE);
    std::vector<StereoFrame> block(blockSize);

    Result r;
    const std::size_t allocBefore = g_allocations.load();
    const double start = cpuSeconds();
    for (std::size_t pos = 0; pos < total; pos += blockSize) {
        std::fill(block.begin(), block.end(), StereoFrame{});
        engine.processBlock(block.data(),
                            std::min(blockSize, total - pos));
        r.checksum += block[0].left;
    }
    r.cpu = cpuSeconds() - start;
    r.allocations = g_allocations.load() - allocBefore;
    r.activeVoices = engine.activeVoiceCount();
    r.realTimeFactor = r.cpu > 0.0 ? kSeconds / r.cpu : 0.0;
    return r;
}

void printRow(std::size_t capacity, std::size_t requested,
              std::size_t blockSize, const Result& r) {
    std::printf("%8zu %9zu %9zu %7zu %10.3f %11.4f %10.1f %7zu\n",
                capacity, requested, r.activeVoices, blockSize, kSeconds,
                r.cpu, r.realTimeFactor, r.allocations);
}

void printHeader() {
    std::printf("%8s %9s %9s %7s %10s %11s %10s %7s\n",
                "capacity", "requested", "active", "block",
                "audio_s", "cpu_s", "RT_factor", "allocs");
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 020 - drum engine HOST benchmark\n");
    std::printf("Not FM-1 hardware. Results depend on this machine and build.\n");
#if defined(__OPTIMIZE__)
    std::printf("Compiler optimisation: ON\n");
#else
    std::printf("Compiler optimisation: OFF (unoptimised build; slower)\n");
#endif
    std::printf("Sample rate: %d Hz, stereo, double precision\n", SAMPLE_RATE);
    std::printf("Audio per measurement: %.1f s\n\n", kSeconds);

    std::vector<std::vector<StereoFrame>> tones;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        tones.push_back(makeTone(pad));
    }

    double checksum = 0.0;

    std::printf("A. Block size sweep (16-voice engine, 16 voices active)\n");
    printHeader();
    for (std::size_t block : {16u, 32u, 64u, 128u, 256u, 512u, 1024u}) {
        const Result r = renderWith<16>(16, block, tones);
        printRow(16, 16, block, r);
        checksum += r.checksum;
    }

    std::printf("\nB. Active voice sweep (block 128)\n");
    printHeader();
    for (std::size_t voices : {0u, 1u, 4u, 8u, 16u}) {
        const Result r = renderWith<16>(voices, 128, tones);
        printRow(16, voices, 128, r);
        checksum += r.checksum;
    }
    for (std::size_t voices : {32u, 64u}) {
        const Result r = renderWith<64>(voices, 128, tones);
        printRow(64, voices, 128, r);
        checksum += r.checksum;
    }

    std::printf("\nC. Trigger handling (16-voice engine)\n");
    {
        static DrumEngine<16> engine;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            engine.assignSample(pad, &tones[pad]);
            engine.setChokeGroup(pad, static_cast<unsigned int>(pad % 4));
        }
        StereoFrame block[64]{};
        constexpr std::size_t kTriggers = 200000;

        // Direct trigger() calls in bursts of 64, with a 64-frame block
        // between bursts so steals complete. Only the bursts are timed,
        // with the high-resolution steady clock (wall time).
        double triggerWall = 0.0;
        const std::size_t allocBefore = g_allocations.load();
        for (std::size_t i = 0; i < kTriggers; i += 64) {
            const double t0 = wallSeconds();
            for (std::size_t k = 0; k < 64; ++k) {
                engine.trigger(((i + k) * 7) % 27, 100);
            }
            triggerWall += wallSeconds() - t0;
            engine.processBlock(block, 64);
        }
        const std::size_t triggerAllocs = g_allocations.load() - allocBefore;
        const std::uint64_t triggerSteals = engine.stolenVoiceCount();
        const std::uint64_t triggerReplaced =
            engine.droppedPendingSoundCount();

        // post + apply through the event queue (single thread).
        engine.stopAll();
        const std::size_t allocBefore2 = g_allocations.load();
        const double t1 = cpuSeconds();
        for (std::size_t i = 0; i < kTriggers; i += 32) {
            for (std::size_t k = 0; k < 32; ++k) {
                engine.postTrigger(((i + k) * 5) % 27, 100, k * 2);
            }
            engine.processBlock(block, 64);
        }
        const double queueCpu = cpuSeconds() - t1;
        const std::size_t queueAllocs = g_allocations.load() - allocBefore2;

        std::printf("  trigger(): %zu calls in bursts of 64, %.4f wall_s, "
                    "%.0f ns/call, allocs %zu, steals %llu, queued sounds "
                    "replaced %llu\n",
                    kTriggers, triggerWall,
                    triggerWall > 0 ? 1e9 * triggerWall / kTriggers : 0.0,
                    triggerAllocs,
                    static_cast<unsigned long long>(triggerSteals),
                    static_cast<unsigned long long>(triggerReplaced));
        std::printf("  postTrigger()+processBlock(64): %zu events, %.4f cpu_s "
                    "(includes %.2f s of audio rendering), allocs %zu, "
                    "dropped %u\n",
                    kTriggers, queueCpu,
                    (kTriggers / 32) * 64.0 / SAMPLE_RATE, queueAllocs,
                    engine.droppedEventCount());
        checksum += block[0].left;
    }

    std::printf("\nchecksum (prevents dead-code elimination): %.6f\n", checksum);
    return 0;
}
