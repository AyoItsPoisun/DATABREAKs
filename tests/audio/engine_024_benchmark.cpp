
// DATABREAKs Engine 024 - tape HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose CPU,
// FPU and RAM budget are not documented or verified.
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
double checksum = 0.0;

double cpuSeconds() { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }

std::vector<StereoFrame> source() {
    std::vector<StereoFrame> s(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.3 * std::sin(2.0 * PI * 110.0 * t) +
                         0.1 * std::sin(2.0 * PI * 2300.0 * t);
        s[i] = {v, -0.8 * v};
    }
    return s;
}

template <typename Process>
void row(const char* name, std::size_t block, const std::vector<StereoFrame>& input,
         Process process) {
    std::vector<StereoFrame> buf = input;
    const std::size_t allocBefore = g_allocations.load();
    const double t0 = cpuSeconds();
    for (std::size_t pos = 0; pos < kFrames; pos += block) {
        process(buf.data() + pos, std::min(block, kFrames - pos));
    }
    const double cpu = cpuSeconds() - t0;
    checksum += buf[kFrames / 2].left;
    std::printf("  %-34s block %5zu  cpu %.4f s  RT x%8.1f  %6.1f ns/frame  allocs %zu\n",
                name, block, cpu, cpu > 0 ? kSeconds / cpu : 0.0,
                1e9 * cpu / static_cast<double>(kFrames),
                g_allocations.load() - allocBefore);
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 024 - tape HOST benchmark\n");
    std::printf("Not FM-1 hardware. Results depend on this machine and build.\n");
#if defined(__OPTIMIZE__)
    std::printf("Compiler optimisation: ON\n");
#else
    std::printf("Compiler optimisation: OFF (unoptimised build; slower)\n");
#endif
    std::printf("Sample rate %d Hz, stereo, double precision, %.0f s per row\n\n",
                SAMPLE_RATE, kSeconds);

    std::printf("Memory:\n");
    std::printf("  TapeMachine heap buffers (allocated once, in its constructor): %zu bytes\n",
                TapeEffect::machineBufferBytes());
    std::printf("  sizeof(TapeMachine) %zu, sizeof(TapeEffect) %zu (aligned dry line %zu bytes)\n",
                sizeof(TapeMachine), sizeof(TapeEffect),
                TapeEffect::LatencyFrames * sizeof(StereoFrame));
    std::printf("  sizeof(AudioPipeline<16>) %zu (+ tape heap buffers above)\n",
                sizeof(AudioPipeline<16>));
    std::printf("  Tape latency: %zu frames (%.1f ms)\n\n", TapeEffect::LatencyFrames,
                1000.0 * TapeEffect::LatencyFrames / SAMPLE_RATE);

    const std::vector<StereoFrame> input = source();
    static TapeEffect tape;
    auto viaTape = [](StereoFrame* b, std::size_t n) { tape.process(b, n); };

    std::printf("A. Tape alone (block 128)\n");
    {
        static TapeMachine machine;
        row("TapeMachine::process (raw)", 128, input, [](StereoFrame* b, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) b[i] = machine.process(b[i]);
        });
    }
    tape.reset();
    row("TapeEffect, defaults", 128, input, viaTape);
    for (TapeSpeed s : {TapeSpeed::IPS_7_5, TapeSpeed::IPS_15, TapeSpeed::IPS_30}) {
        tape.setSpeed(s);
        tape.reset();
        row(s == TapeSpeed::IPS_7_5 ? "TapeEffect, 7.5 IPS"
            : s == TapeSpeed::IPS_15 ? "TapeEffect, 15 IPS" : "TapeEffect, 30 IPS",
            128, input, viaTape);
    }
    tape.setWow(0.8);
    tape.setFlutter(0.8);
    tape.reset();
    row("TapeEffect, wow + flutter", 128, input, viaTape);
    tape.setHiss(0.5);
    tape.setSaturation(0.6);
    tape.setHeadBump(0.5);
    tape.setCompression(0.5);
    tape.setCrosstalk(0.3);
    tape.setPrintThrough(0.4);
    tape.setAge(0.3);
    tape.reset();
    row("TapeEffect, every control active", 128, input, viaTape);
    for (std::size_t block : {16u, 1024u}) {
        tape.reset();
        row("TapeEffect, every control active", block, input, viaTape);
    }
    {
        // Continuous automation: a control changes every block (glides).
        std::size_t n = 0;
        tape.reset();
        row("TapeEffect, automated every block", 128, input, [&n](StereoFrame* b, std::size_t k) {
            tape.setWow(static_cast<double>(n++ % 10) / 10.0);
            tape.process(b, k);
        });
    }

    std::printf("\nB. Full chain EQ > Comp > Clip > Tape > Dist > Reverb (pipeline effects)\n");
    static AudioPipeline<16> p;
    for (std::size_t i = 0; i < p.effects().size(); ++i) {
        p.effects().slot(i)->setBypassed(false, true);
    }
    p.tape().setWow(0.3);
    p.tape().setSaturation(0.4);
    for (std::size_t block : {16u, 128u, 1024u}) {
        p.effects().resetAll();
        row("Full chain with tape", block, input,
            [](StereoFrame* b, std::size_t n) { p.applyEffects(b, n); });
    }

    std::printf("\nC. Complete pipeline: drums + mono + FM + poly + chain + master\n");
    {
        std::vector<StereoFrame> kick(19200);
        for (std::size_t i = 0; i < kick.size(); ++i) {
            const double t = static_cast<double>(i) / SAMPLE_RATE;
            const double v = 0.6 * std::sin(2.0 * PI * 60.0 * t) * std::exp(-t * 8.0);
            kick[i] = {v, v};
        }
        p.drums().assignSample(0, &kick);
        const std::vector<StereoFrame> silence(kFrames);
        for (int mode = 0; mode < 3; ++mode) {
            for (std::size_t i = 0; i < p.effects().size(); ++i) {
                const bool isTape = p.effects().slot(i) == &p.tape();
                const bool on = mode == 2 || (mode == 1 && isTape);
                p.effects().slot(i)->setBypassed(!on, true);
            }
            p.mono().reset();
            p.fm().reset();
            p.poly().reset();
            std::size_t n = 0;
            row(mode == 0 ? "Pipeline, all effects bypassed"
                : mode == 1 ? "Pipeline, tape only" : "Pipeline, full chain with tape",
                128, silence, [&n](StereoFrame* b, std::size_t k) {
                    if (n % 94 == 0) {
                        p.drums().postTrigger(0, 110, 0);
                        p.postNoteOn(SynthTarget::Mono, 36 + static_cast<int>(n % 5), 100, 0);
                        p.postNoteOn(SynthTarget::Fm, 72 + static_cast<int>(n % 7), 90, 0);
                        for (int v = 0; v < 4; ++v) {
                            p.postNoteOn(SynthTarget::Poly, 55 + v * 4 + static_cast<int>(n % 3), 80, 0);
                        }
                    }
                    if (n % 94 == 60) {
                        p.postAllNotesOff(SynthTarget::Poly);
                        p.postAllNotesOff(SynthTarget::Fm);
                    }
                    ++n;
                    p.render(b, k);
                });
        }
    }

    std::printf("\nchecksum (prevents dead-code elimination): %.6f\n", checksum);
    return 0;
}
