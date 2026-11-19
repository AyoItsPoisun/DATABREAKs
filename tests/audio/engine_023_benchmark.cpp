
// DATABREAKs Engine 023 - effects HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose
// processor, FPU and memory are not documented.
//
// Each row processes 10 s of 48 kHz stereo audio in place.
// Real-time factor = 10 s / process CPU seconds (std::clock).

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string>
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

constexpr double kSeconds = 10.0;
constexpr std::size_t kFrames = static_cast<std::size_t>(kSeconds * SAMPLE_RATE);

double cpuSeconds() { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }

std::vector<StereoFrame> source() {
    std::vector<StereoFrame> s(kFrames);
    std::uint32_t state = 7;
    for (std::size_t i = 0; i < kFrames; ++i) {
        state = state * 1664525u + 1013904223u;
        const double noise = static_cast<double>(state >> 8) / 8388607.5 - 1.0;
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.3 * std::sin(2.0 * PI * 110.0 * t) + 0.05 * noise;
        s[i] = {v, 0.8 * v};
    }
    return s;
}

double checksum = 0.0;

void row(const char* name, AudioEffect& effect, std::size_t block,
         const std::vector<StereoFrame>& input) {
    std::vector<StereoFrame> buf = input;
    effect.reset();
    const std::size_t allocBefore = g_allocations.load();
    const double t0 = cpuSeconds();
    for (std::size_t pos = 0; pos < kFrames; pos += block) {
        effect.process(buf.data() + pos, std::min(block, kFrames - pos));
    }
    const double cpu = cpuSeconds() - t0;
    const std::size_t allocs = g_allocations.load() - allocBefore;
    checksum += buf[kFrames / 2].left;
    std::printf("  %-26s block %5zu  cpu %.4f s  RT x%8.1f  %6.1f ns/frame  allocs %zu\n",
                name, block, cpu, cpu > 0 ? kSeconds / cpu : 0.0,
                1e9 * cpu / static_cast<double>(kFrames), allocs);
}

template <typename Chain>
void chainRow(const char* name, Chain& chain, std::size_t block,
              const std::vector<StereoFrame>& input) {
    std::vector<StereoFrame> buf = input;
    chain.resetAll();
    const std::size_t allocBefore = g_allocations.load();
    const double t0 = cpuSeconds();
    for (std::size_t pos = 0; pos < kFrames; pos += block) {
        chain.process(buf.data() + pos, std::min(block, kFrames - pos));
    }
    const double cpu = cpuSeconds() - t0;
    const std::size_t allocs = g_allocations.load() - allocBefore;
    checksum += buf[kFrames / 2].left;
    std::printf("  %-26s block %5zu  cpu %.4f s  RT x%8.1f  %6.1f ns/frame  allocs %zu\n",
                name, block, cpu, cpu > 0 ? kSeconds / cpu : 0.0,
                1e9 * cpu / static_cast<double>(kFrames), allocs);
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 023 - effects HOST benchmark\n");
    std::printf("Not FM-1 hardware. Results depend on this machine and build.\n");
#if defined(__OPTIMIZE__)
    std::printf("Compiler optimisation: ON\n");
#else
    std::printf("Compiler optimisation: OFF (unoptimised build; slower)\n");
#endif
    std::printf("Sample rate %d Hz, stereo, double precision, %.0f s per row\n",
                SAMPLE_RATE, kSeconds);
    std::printf("Reverb delay memory: %zu bytes; sizeof: Equalizer %zu, "
                "Compressor %zu, SoftClipper %zu, Distortion %zu, Reverb %zu\n\n",
                Reverb::delayMemoryBytes(), sizeof(Equalizer), sizeof(Compressor),
                sizeof(SoftClipper), sizeof(Distortion), sizeof(Reverb));

    const std::vector<StereoFrame> input = source();

    static Equalizer eq;
    eq.setLow(100.0, 4.0);
    eq.setMid(1000.0, -3.0, 1.0);
    eq.setHigh(8000.0, 2.0);
    static Compressor comp;
    static SoftClipper clip;
    static Distortion dist;
    static Reverb rev;

    std::printf("A. Individual effects (enabled, block 128)\n");
    row("Equalizer (3 bands)", eq, 128, input);
    row("Compressor", comp, 128, input);
    row("SoftClipper", clip, 128, input);
    for (DistortionMode m : {DistortionMode::Soft, DistortionMode::Asymmetric,
                             DistortionMode::Fuzz}) {
        dist.setMode(m);
        row(m == DistortionMode::Soft ? "Distortion (soft)"
            : m == DistortionMode::Asymmetric ? "Distortion (asymmetric)"
                                              : "Distortion (fuzz)",
            dist, 128, input);
    }
    row("Reverb", rev, 128, input);

    static EffectChain<8> chain;
    chain.append(&eq);
    chain.append(&comp);
    chain.append(&clip);
    chain.append(&dist);
    chain.append(&rev);

    std::printf("\nB. Full chain EQ > Comp > Clip > Dist > Reverb\n");
    for (std::size_t block : {16u, 64u, 128u, 256u, 1024u}) {
        chainRow("Full chain", chain, block, input);
    }
    for (std::size_t i = 0; i < chain.size(); ++i) {
        chain.slot(i)->setBypassed(true, true);
    }
    chainRow("Full chain, all bypassed", chain, 128, input);

    std::printf("\nC. Complete pipeline: drums + mono + FM + poly + full chain + master\n");
    {
        static AudioPipeline<16> p;
        std::vector<StereoFrame> kick(19200);
        for (std::size_t i = 0; i < kick.size(); ++i) {
            const double t = static_cast<double>(i) / SAMPLE_RATE;
            const double v = 0.6 * std::sin(2.0 * PI * 60.0 * t) * std::exp(-t * 8.0);
            kick[i] = {v, v};
        }
        p.drums().assignSample(0, &kick);
        for (bool fx : {false, true}) {
            for (std::size_t i = 0; i < p.effects().size(); ++i) {
                // The Engine 023 chain only; tape is benchmarked in Engine 024.
                const bool tape =
                    std::string(p.effects().slot(i)->name()) == "Tape";
                p.effects().slot(i)->setBypassed(!fx || tape, true);
            }
            p.mono().reset();
            p.fm().reset();
            p.poly().reset();
            std::vector<StereoFrame> block(128);
            const std::size_t allocBefore = g_allocations.load();
            const double t0 = cpuSeconds();
            for (std::size_t pos = 0, n = 0; pos < kFrames; pos += 128, ++n) {
                if (n % 94 == 0) {  // about every 250 ms
                    p.drums().postTrigger(0, 110, 0);
                    p.postNoteOn(SynthTarget::Mono, 36 + static_cast<int>(n % 5), 100, 0);
                    p.postNoteOn(SynthTarget::Fm, 72 + static_cast<int>(n % 7), 90, 0);
                    for (int k = 0; k < 4; ++k) {
                        p.postNoteOn(SynthTarget::Poly, 55 + k * 4 + static_cast<int>(n % 3), 80, 0);
                    }
                }
                if (n % 94 == 60) {
                    p.postAllNotesOff(SynthTarget::Poly);
                    p.postAllNotesOff(SynthTarget::Fm);
                }
                std::fill(block.begin(), block.end(), StereoFrame{});
                p.render(block.data(), block.size());
                checksum += block[0].left;
            }
            const double cpu = cpuSeconds() - t0;
            std::printf("  %-26s block   128  cpu %.4f s  RT x%8.1f  %6.1f ns/frame  allocs %zu\n",
                        fx ? "Pipeline, effects on" : "Pipeline, effects bypassed",
                        cpu, cpu > 0 ? kSeconds / cpu : 0.0,
                        1e9 * cpu / static_cast<double>(kFrames),
                        g_allocations.load() - allocBefore);
        }
    }

    std::printf("\nchecksum (prevents dead-code elimination): %.6f\n", checksum);
    return 0;
}
