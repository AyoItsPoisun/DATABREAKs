// DATABREAKs Engine 028 - pattern bank and arrangement render demonstration.
//
// Four patterns in the bank, played by a song arrangement with sample-exact
// transitions, ending with the transport stopping on the end sample:
//   A  2 bars 4/4: kick / snare / closed hats + a mono bass line;
//   B  2 bars 4/4: a busier groove; its hi-hats were RECORDED loosely
//      through the Engine 026 recorder in a pre-roll (not part of the
//      output) and are played back with Engine 027 quantization (16ths,
//      100 %, swing 50), plus FM stabs;
//   C  1 bar 7/8: a snare/tom fill (another length and time signature);
//   D  = a duplicate of A with one extra open hat (copy + edit).
// Arrangement: A x2, B x2, C x1, D x1, end Stop. 118 BPM. A is already
// active, so the song has 3 transitions (A->B, B->C, C->D).
//
// Rendered with block sizes 1, 64, 256, 1000 and 4096; all must be
// bit-identical. The 256-frame render is written to
// databreaks_engine_028_demo.wav (48 kHz, 16-bit stereo) in the current
// directory. Do not commit it.
//
// Host software render, NOT a recording of FM-1 hardware, and not listened
// to as part of the automated checks. Self-checks (exit code 1 on
// failure): written, finite, peak in range, no clipping, nothing dropped or
// undelivered, 3 transitions, the transport stopped exactly on the
// arrangement's end sample, the recording in B intact, identical output
// for every block size.

#include "audio_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

using namespace databreaks;

namespace {

using Pipeline = AudioPipeline<16>;
using Buffer = std::vector<StereoFrame>;

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
        phase += 2.0 * PI * (48.0 + 100.0 * std::exp(-t * 30.0)) / SAMPLE_RATE;
        const double v = 0.7 * std::sin(phase) * std::exp(-t * 7.5);
        s[i] = {v, v};
    }
    return s;
}

Buffer makeSnare() {
    Buffer s(SAMPLE_RATE / 4);
    Noise noise{0x27u};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.25 * std::sin(2.0 * PI * 190.0 * t) * std::exp(-t * 22.0) +
                         0.25 * noise.next() * std::exp(-t * 16.0);
        s[i] = {v, v * 0.9};
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

enum Pad : int { Kick = 0, Snare = 4, ClosedHat = 13, OpenHat = 14, Tom = 18 };

Buffer makeTom() {
    Buffer s(SAMPLE_RATE * 3 / 10);
    double phase = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        phase += 2.0 * PI * (110.0 + 60.0 * std::exp(-t * 25.0)) / SAMPLE_RATE;
        const double v = 0.45 * std::sin(phase) * std::exp(-t * 11.0);
        s[i] = {v * 0.8, v};
    }
    return s;
}

void instruments(Pipeline& p) {
    static const Buffer kick = makeKick();
    static const Buffer snare = makeSnare();
    static const Buffer closedHat = makeHat(0.05, 70.0, 0xC0DEu);
    static const Buffer openHat = makeHat(0.35, 8.0, 0xFACEu);
    static const Buffer tom = makeTom();
    p.drums().assignSample(Kick, &kick);
    p.drums().assignSample(Snare, &snare);
    p.drums().assignSample(ClosedHat, &closedHat);
    p.drums().assignSample(OpenHat, &openHat);
    p.drums().assignSample(Tom, &tom);
    p.drums().setChokeGroup(ClosedHat, 1);
    p.drums().setChokeGroup(OpenHat, 1);
    MonoParameters bass;
    bass.filterCutoff = 280.0;
    bass.filterEnvelopeAmount = 1400.0;
    bass.volume = 0.5;
    bass.envelope = {0.004, 0.2, 0.5, 0.06};
    p.mono().setParameters(bass);
    FMParameters stab;
    stab.modulationRatio = 2.0;
    stab.modulationDepth = 1.4;
    stab.volume = 0.22;
    stab.envelope = {0.003, 0.18, 0.2, 0.25};
    p.fm().setParameters(stab);
    p.setDrumLevel(0.75);
    p.setMonoLevel(0.7);
    p.setFmLevel(0.7);
    p.setMasterLevel(0.8);
}

void patternA(Pattern& p) {
    p.configure({4, 4}, 2, 240);
    p.clear();
    for (int bar = 0; bar < 2; ++bar) {
        const std::size_t b = static_cast<std::size_t>(bar) * 16;
        for (int k : {0, 6, 10}) p.setHit(Kick, b + static_cast<std::size_t>(k), 118);
        for (int k : {4, 12}) p.setHit(Snare, b + static_cast<std::size_t>(k), 110);
        for (int k = 0; k < 16; k += 2) p.setHit(ClosedHat, b + static_cast<std::size_t>(k), k % 4 == 0 ? 90 : 60);
        const int roots[2] = {36, 41};
        for (int k : {0, 3, 8, 11}) {
            p.addNote({true, SynthTarget::Mono, roots[bar], 100, 100,
                       static_cast<std::int64_t>(b + static_cast<std::size_t>(k)) * 240, 200});
        }
    }
}

void patternB(Pattern& p) {  // hats come from the recording
    p.configure({4, 4}, 2, 240);
    p.clear();
    for (int bar = 0; bar < 2; ++bar) {
        const std::size_t b = static_cast<std::size_t>(bar) * 16;
        for (int k : {0, 3, 7, 10, 14}) p.setHit(Kick, b + static_cast<std::size_t>(k), 112);
        for (int k : {4, 12}) p.setHit(Snare, b + static_cast<std::size_t>(k), 116);
        for (int k : {2, 9}) {
            p.addNote({true, SynthTarget::Fm, 60 + bar * 3, 96, 100,
                       static_cast<std::int64_t>(b + static_cast<std::size_t>(k)) * 240, 300});
        }
    }
}

void patternC(Pattern& p) {  // 7/8 fill
    p.configure({7, 8}, 1, 240);
    p.clear();
    for (int k = 0; k < 14; ++k) {
        p.setHit(k % 3 == 2 ? Snare : Tom, static_cast<std::size_t>(k), 70 + k * 3);
    }
}

struct Result {
    Buffer audio;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t undelivered = 0;
    std::uint64_t clipped = 0;
    std::uint64_t switches = 0;
    std::int64_t endSample = -1;
    bool stoppedAtEnd = false;
    std::size_t recordedInB = 0;
};

constexpr std::size_t kMaxFrames = 30 * static_cast<std::size_t>(SAMPLE_RATE);

Result renderWith(std::size_t block) {
    auto p = std::make_unique<Pipeline>();  // a fresh pipeline per render
    instruments(*p);
    Sequencer& s = p->sequencer();
    s.clock().setTempo(118.0);

    // Bank: A (0, active), B (1), C (2), D = duplicate of A + an open hat.
    patternA(s.pattern());
    auto staged = std::make_unique<PatternData>();
    patternB(staged->pattern);
    staged->quantize.grid = QuantizeGrid::Sixteenth;
    staged->quantize.strength = 100;
    staged->quantize.swing = 50;
    s.replacePattern(1, staged->pattern, staged->track, staged->quantize);
    patternC(staged->pattern);
    staged->quantize = QuantizeSettings{};
    s.replacePattern(2, staged->pattern, staged->track, staged->quantize);
    const int d = s.duplicatePattern(0);

    // Pre-roll (not in the output): record a loose 16th hi-hat part into B.
    s.requestPatternSwitch(1);
    s.arm();
    s.start();
    const std::int64_t loopB = s.clock().sampleOfTick(s.pattern().lengthTicks());
    const std::int64_t sixteenth = loopB / 32;
    Noise jitter{0x2028u};
    std::int64_t hatTime[32];
    for (int k = 0; k < 32; ++k) {
        const std::int64_t t = k * sixteenth +
                               static_cast<std::int64_t>(std::lround(jitter.next() * 900.0));
        hatTime[k] = std::clamp<std::int64_t>(t, 0, loopB - 1);
    }
    Buffer scratch(4096);
    for (std::int64_t pos = 0; pos < loopB; pos += 4096) {
        for (int k = 0; k < 32; ++k) {
            if (hatTime[k] >= pos && hatTime[k] < pos + 4096) {
                s.liveEvent({LiveEventType::DrumHit, SynthTarget::Mono, ClosedHat,
                             k % 4 == 0 ? 92 : (k % 2 == 0 ? 70 : 48), hatTime[k]});
            }
        }
        const std::size_t n = static_cast<std::size_t>(std::min<std::int64_t>(4096, loopB - pos));
        std::fill(scratch.begin(), scratch.end(), StereoFrame{});
        p->render(scratch.data(), n);
    }
    s.disarm();
    s.stop();
    std::fill(scratch.begin(), scratch.end(), StereoFrame{});
    p->render(scratch.data(), 1);
    const std::size_t recorded = s.recorder().track().size();
    s.requestPatternSwitch(d);  // edit D: one extra open hat at the end of bar 2
    s.pattern().setHit(OpenHat, 30, 100);
    s.requestPatternSwitch(0);
    s.reset();
    p->drums().stopAll();
    p->mono().reset();
    p->fm().reset();
    p->poly().reset();
    p->resetDiagnostics();

    s.arrangementAppend(0, 2);
    s.arrangementAppend(1, 2);
    s.arrangementAppend(2, 1);
    s.arrangementAppend(d, 1);
    s.arrangementSetEnd(ArrangementEnd::Stop);
    const std::uint64_t d0 = s.deliveredEventCount();
    const std::uint64_t x0 = s.droppedEventCount();
    const std::uint64_t sw0 = s.patternSwitchCount();
    s.startArrangement();
    s.start();

    Result r;
    r.audio.assign(kMaxFrames, StereoFrame{});
    std::size_t pos = 0;
    while (pos < kMaxFrames) {
        const std::size_t n = std::min(block, kMaxFrames - pos);
        p->render(r.audio.data() + pos, n);
        pos += n;
        if (!s.playing() && r.endSample < 0) r.endSample = s.clock().samplePosition();
    }
    // Expected end tick: A (7,680 ticks) x2 + B (7,680) x2 + C (7/8 bar,
    // 3,360) + D (7,680).
    const std::int64_t endTick = 2 * 7680 + 2 * 7680 + 3360 + 7680;
    r.stoppedAtEnd = !s.playing() && r.endSample == s.clock().sampleOfTick(endTick) &&
                     s.songFinished();
    r.delivered = s.deliveredEventCount() - d0;
    r.dropped = s.droppedEventCount() - x0;
    r.undelivered = p->undeliveredSequencerEventCount();
    r.clipped = p->clippedSampleCount();
    r.switches = s.patternSwitchCount() - sw0;
    r.recordedInB = recorded;
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
    std::printf("DATABREAKs Engine 028 - pattern bank and arrangement render demo\n");
    std::printf("Host software render; not an FM-1 hardware recording.\n");

    const Result reference = renderWith(256);
    bool identical = true;
    for (std::size_t block : {1u, 64u, 1000u, 4096u}) {
        const Result other = renderWith(block);
        bool same = other.delivered == reference.delivered && other.endSample == reference.endSample;
        for (std::size_t i = 0; i < kMaxFrames && same; ++i) {
            same = other.audio[i].left == reference.audio[i].left &&
                   other.audio[i].right == reference.audio[i].right;
        }
        std::printf("Block %4zu: %s\n", block, same ? "bit-identical" : "DIFFERENT");
        identical = identical && same;
    }

    // Keep 1.5 s of tail after the end of the arrangement.
    const std::size_t frames = std::min(kMaxFrames, static_cast<std::size_t>(reference.endSample) +
                                                         static_cast<std::size_t>(SAMPLE_RATE * 3 / 2));
    Buffer out(reference.audio.begin(), reference.audio.begin() + static_cast<std::ptrdiff_t>(frames));
    double peak = 0.0;
    bool finite = true;
    for (const auto& f : out) {
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
    }
    const char* path = "databreaks_engine_028_demo.wav";
    const bool written = writeWav(path, out);

    std::printf("Arrangement: A x2 | B x2 (recorded hats, quantized 16th 100%% swing 50) | C (7/8 fill) | D (copy of A) | stop\n");
    std::printf("Frames written: %zu (%.2f s); transport stopped at sample %lld (%s)\n", frames,
                static_cast<double>(frames) / SAMPLE_RATE, static_cast<long long>(reference.endSample),
                reference.stoppedAtEnd ? "exactly on the end boundary" : "WRONG");
    std::printf("Pattern switches %llu; hats recorded into B %zu\n",
                static_cast<unsigned long long>(reference.switches), reference.recordedInB);
    std::printf("Sequencer events delivered %llu, dropped %llu, undelivered %llu\n",
                static_cast<unsigned long long>(reference.delivered),
                static_cast<unsigned long long>(reference.dropped),
                static_cast<unsigned long long>(reference.undelivered));
    std::printf("Peak %.4f, master clipped samples %llu\n", peak,
                static_cast<unsigned long long>(reference.clipped));
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok = written && finite && peak > 0.1 && peak < 1.0 && reference.clipped == 0 &&
                    reference.dropped == 0 && reference.undelivered == 0 &&
                    reference.switches == 3 && reference.stoppedAtEnd &&
                    reference.recordedInB == 32 && identical;
    std::printf(ok ? "ENGINE 028 RENDER PASSED\n" : "ENGINE 028 RENDER FAILED\n");
    return ok ? 0 : 1;
}
