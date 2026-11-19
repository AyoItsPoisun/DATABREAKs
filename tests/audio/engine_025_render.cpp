
// DATABREAKs Engine 025 - sequencer render demonstration.
//
// An original 4-bar pattern at 118.5 BPM, played entirely by the Engine 025
// sequencer (no hand-posted events): drums on pads spread across the
// 27-pad range (with swing microtiming, probability ghost notes and an
// open/closed hi-hat choke), a mono bass line, an FM melody and poly chords,
// through EQ, compressor, tape and reverb. Rendered for 12 s.
//
// The same performance is rendered with block sizes 1, 64, 256, 1000 and
// 4096; all must be bit-identical. The 256-frame render is written to
// databreaks_engine_025_demo.wav (48 kHz, 16-bit stereo) in the current
// directory.
//
// This is a host software render, NOT a recording of FM-1 hardware.
// Self-checks (exit code 1 on failure): written, finite, peak in range, no
// master clipping, events delivered with none dropped or undelivered,
// identical for every block size.

#include "audio_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace databreaks;

namespace {

using Pipeline = AudioPipeline<16>;
using Buffer = std::vector<StereoFrame>;

constexpr std::size_t kTotal = 12 * static_cast<std::size_t>(SAMPLE_RATE);

struct Noise {
    std::uint32_t state;
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 8388607.5 - 1.0;
    }
};

Buffer makeKick() {
    Buffer s(SAMPLE_RATE * 4 / 10);
    double phase = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        phase += 2.0 * PI * (50.0 + 95.0 * std::exp(-t * 32.0)) / SAMPLE_RATE;
        const double v = 0.7 * std::sin(phase) * std::exp(-t * 7.0);
        s[i] = {v, v};
    }
    return s;
}

Buffer makeSnare() {
    Buffer s(SAMPLE_RATE / 4);
    Noise noise{0x25u};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.26 * std::sin(2.0 * PI * 185.0 * t) * std::exp(-t * 22.0) +
                         0.25 * noise.next() * std::exp(-t * 15.0);
        s[i] = {v * 0.92, v};
    }
    return s;
}

Buffer makeHat(double seconds, double decay, std::uint32_t seed) {
    Buffer s(static_cast<std::size_t>(SAMPLE_RATE * seconds));
    Noise noise{seed};
    double previous = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double white = noise.next();
        const double v = 0.12 * (white - previous) * std::exp(-t * decay);
        previous = white;
        s[i] = {v * 0.8, v};
    }
    return s;
}

Buffer makeRim() {
    Buffer s(SAMPLE_RATE / 20);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.3 * std::sin(2.0 * PI * 1700.0 * t) * std::exp(-t * 90.0);
        s[i] = {v, v * 0.7};
    }
    return s;
}

// Pads used: spread over the FM-1's 27 to show addressing beyond 0..3.
enum Pad : std::size_t { Kick = 0, Snare = 9, ClosedHat = 13, OpenHat = 14, Rim = 26 };

void program(Pipeline& p) {
    static const Buffer kick = makeKick();
    static const Buffer snare = makeSnare();
    static const Buffer closedHat = makeHat(0.06, 65.0, 0xBEEFu);
    static const Buffer openHat = makeHat(0.45, 6.0, 0xF00Du);
    static const Buffer rim = makeRim();

    p.drums().assignSample(Kick, &kick);
    p.drums().assignSample(Snare, &snare);
    p.drums().assignSample(ClosedHat, &closedHat);
    p.drums().assignSample(OpenHat, &openHat);
    p.drums().assignSample(Rim, &rim);
    p.drums().setChokeGroup(ClosedHat, 1);
    p.drums().setChokeGroup(OpenHat, 1);

    MonoParameters bass;
    bass.filterCutoff = 240.0;
    bass.filterEnvelopeAmount = 1700.0;
    bass.volume = 0.55;
    bass.envelope = {0.004, 0.18, 0.5, 0.05};
    p.mono().setParameters(bass);
    FMParameters bell;
    bell.modulationRatio = 3.5;
    bell.modulationDepth = 2.0;
    bell.volume = 0.26;
    bell.envelope = {0.003, 0.35, 0.2, 0.45};
    p.fm().setParameters(bell);
    PolyParameters pad;
    pad.detune = 0.004;
    pad.voiceVolume = 0.09;
    pad.stereoWidth = 0.8;
    pad.envelope = {0.2, 0.5, 0.7, 0.7};
    p.poly().setParameters(pad);

    p.setDrumLevel(0.7);
    p.setMonoLevel(0.75);
    p.setFmLevel(0.7);
    p.setPolyLevel(0.8);
    p.setMasterLevel(0.7);

    p.eq().setLow(80.0, 2.0);
    p.eq().setHigh(9000.0, 2.0);
    p.compressor().setThresholdDb(-20.0);
    p.compressor().setRatio(3.0);
    p.tape().setSaturation(0.15);
    p.tape().setWow(0.1);
    p.tape().setFlutter(0.1);
    p.reverb().setDecaySeconds(1.8);
    p.reverb().setWetLevel(0.22);
    for (AudioEffect* e : {static_cast<AudioEffect*>(&p.eq()),
                           static_cast<AudioEffect*>(&p.compressor()),
                           static_cast<AudioEffect*>(&p.tape()),
                           static_cast<AudioEffect*>(&p.reverb())}) {
        e->setBypassed(false, true);
    }

    Sequencer& s = p.sequencer();
    s.clock().setTempo(118.5);
    Pattern& pat = s.pattern();
    pat.configure({4, 4}, 4, 240);  // 64 sixteenths
    for (std::size_t bar = 0; bar < 4; ++bar) {
        const std::size_t b = bar * 16;
        pat.setHit(Kick, b + 0, 124);
        pat.setHit(Kick, b + 7, 90);
        pat.setHit(Kick, b + 10, 112);
        if (bar == 3) pat.setHit(Kick, b + 14, 100);
        pat.setHit(Snare, b + 4, 112);
        pat.setHit(Snare, b + 12, 118);
        for (std::size_t k = 0; k < 16; ++k) {
            DrumStep h;
            h.enabled = true;
            h.velocity = (k % 4 == 0) ? 96 : (k % 2 == 0 ? 72 : 46);
            h.probability = (k % 2 == 0) ? 100 : 55;     // ghost 16ths
            h.microTicks = (k % 2 == 1) ? 40 : 0;          // swing
            if (k == 14) {
                pat.setStep(OpenHat, b + k, h);           // choked next bar
            } else {
                pat.setStep(ClosedHat, b + k, h);
            }
        }
        DrumStep rimStep;
        rimStep.enabled = true;
        rimStep.velocity = 80;
        rimStep.probability = 35;
        rimStep.microTicks = -30;
        pat.setStep(Rim, b + 11, rimStep);
    }

    const int roots[4] = {38, 41, 36, 43};
    const int chords[4][4] = {{50, 53, 57, 60}, {53, 57, 60, 64}, {48, 52, 55, 59}, {43, 50, 55, 59}};
    for (int bar = 0; bar < 4; ++bar) {
        const std::int64_t b = bar * 3840;
        for (int n : chords[bar]) pat.addNote({true, SynthTarget::Poly, n, 78, 100, b, 3700});
        const int pulse[] = {0, 3, 6, 8, 11, 14};
        for (int k : pulse) {
            pat.addNote({true, SynthTarget::Mono, (k == 14) ? roots[bar] + 12 : roots[bar],
                         k == 0 ? 118 : 92, 100, b + k * 240, 200});
        }
    }
    const int melody[][4] = {  // tick, note, velocity, length
        {3840, 74, 100, 700}, {4800, 77, 85, 460}, {5520, 76, 80, 220}, {5760, 74, 90, 900},
        {7680, 72, 100, 700}, {8640, 69, 85, 460}, {9360, 71, 80, 220}, {9600, 72, 95, 900},
        {11520, 79, 110, 480}, {12000, 77, 90, 480}, {12480, 76, 85, 480}, {12960, 74, 100, 1400}};
    for (const auto& m : melody) {
        pat.addNote({true, SynthTarget::Fm, m[1], m[2], 100, m[0], m[3]});
    }
    s.setSeed(2025);
}

struct Result {
    Buffer audio;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t undelivered = 0;
    std::uint64_t clipped = 0;
};

Result renderWith(std::size_t block) {
    static Pipeline p;  // reset to a clean state for every render
    p.sequencer().stop();
    Buffer flush(1);
    p.render(flush.data(), 1);
    p.drums().stopAll();
    p.mono().reset();
    p.fm().reset();
    p.poly().reset();
    p.effects().resetAll();
    p.sequencer().pattern().clear();
    p.sequencer().clock().reset();
    p.resetDiagnostics();
    program(p);
    p.effects().resetAll();
    const std::uint64_t d0 = p.sequencer().deliveredEventCount();
    const std::uint64_t x0 = p.sequencer().droppedEventCount();
    const std::uint64_t u0 = p.undeliveredSequencerEventCount();

    p.sequencer().start();
    Result r;
    r.audio.assign(kTotal, StereoFrame{});
    for (std::size_t pos = 0; pos < kTotal; pos += block) {
        p.render(r.audio.data() + pos, std::min(block, kTotal - pos));
    }
    r.delivered = p.sequencer().deliveredEventCount() - d0;
    r.dropped = p.sequencer().droppedEventCount() - x0;
    r.undelivered = p.undeliveredSequencerEventCount() - u0;
    r.clipped = p.clippedSampleCount();
    return r;
}

void write16(std::ofstream& f, std::uint16_t v) {
    f.put(static_cast<char>(v & 0xFF));
    f.put(static_cast<char>((v >> 8) & 0xFF));
}

void write32(std::ofstream& f, std::uint32_t v) {
    write16(f, static_cast<std::uint16_t>(v & 0xFFFF));
    write16(f, static_cast<std::uint16_t>((v >> 16) & 0xFFFF));
}

bool writeWav(const char* path, const Buffer& audio) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(audio.size() * 4);
    f.write("RIFF", 4);
    write32(f, 36 + dataSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write32(f, 16);
    write16(f, 1);
    write16(f, 2);
    write32(f, SAMPLE_RATE);
    write32(f, SAMPLE_RATE * 4);
    write16(f, 4);
    write16(f, 16);
    f.write("data", 4);
    write32(f, dataSize);
    for (const auto& frame : audio) {
        const auto l = static_cast<std::int16_t>(std::lround(clampAudio(frame.left) * 32767.0));
        const auto r = static_cast<std::int16_t>(std::lround(clampAudio(frame.right) * 32767.0));
        write16(f, static_cast<std::uint16_t>(l));
        write16(f, static_cast<std::uint16_t>(r));
    }
    return static_cast<bool>(f);
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 025 - sequencer render demo\n");
    std::printf("Host software render; not an FM-1 hardware recording.\n");

    const Result reference = renderWith(256);
    bool identical = true;
    for (std::size_t block : {1u, 64u, 1000u, 4096u, 256u}) {
        const Result other = renderWith(block);
        bool same = other.delivered == reference.delivered;
        for (std::size_t i = 0; i < kTotal && same; ++i) {
            same = other.audio[i].left == reference.audio[i].left &&
                   other.audio[i].right == reference.audio[i].right;
        }
        std::printf("Block %4zu: %s\n", block, same ? "bit-identical" : "DIFFERENT");
        identical = identical && same;
    }

    double peak = 0.0;
    double energyL = 0.0;
    double energyR = 0.0;
    bool finite = true;
    for (const auto& f : reference.audio) {
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
        energyL += f.left * f.left;
        energyR += f.right * f.right;
    }

    const char* path = "databreaks_engine_025_demo.wav";
    const bool written = writeWav(path, reference.audio);

    std::printf("Frames: %zu (%.2f s), 118.5 BPM, 4-bar pattern looped\n", kTotal,
                static_cast<double>(kTotal) / SAMPLE_RATE);
    std::printf("Sequencer events delivered %llu, dropped %llu, undelivered %llu\n",
                static_cast<unsigned long long>(reference.delivered),
                static_cast<unsigned long long>(reference.dropped),
                static_cast<unsigned long long>(reference.undelivered));
    std::printf("Peak %.4f, master clipped samples %llu, RMS L/R %.4f / %.4f\n", peak,
                static_cast<unsigned long long>(reference.clipped),
                std::sqrt(energyL / kTotal), std::sqrt(energyR / kTotal));
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok = written && finite && peak > 0.1 && peak < 1.0 && reference.clipped == 0 &&
                    reference.delivered > 200 && reference.dropped == 0 &&
                    reference.undelivered == 0 && identical && energyL > 0.0 && energyR > 0.0;
    std::printf(ok ? "ENGINE 025 RENDER PASSED\n" : "ENGINE 025 RENDER FAILED\n");
    return ok ? 0 : 1;
}
