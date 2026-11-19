// DATABREAKs Engine 027 - non-destructive quantization render demonstration.
//
// A deliberately loose drum performance (2 bars, 120 BPM, kick / snare /
// closed and open hats, timing errors up to +-30 ms) is recorded once with
// the Engine 026 recorder and then replayed from the recording with
// different quantize settings. Nothing is re-recorded or edited:
//   loop 0  the live, loose performance (being recorded);
//   loop 1  replay, original timing (strength 0);
//   loop 2  partial quantization: 16th grid, strength 50%;
//   loop 3  full quantization: 16th grid, strength 100%;
//   loop 4  full quantization with swing 50 (odd 16ths a quarter-step late);
//   loop 5  strength back to 0: the original timing again.
// Settings change exactly at loop starts (the render call is split there,
// as a host would do for a sample-accurate parameter change).
//
// Rendered with block sizes 1, 64, 256, 1000 and 4096; all must be
// bit-identical. The 256-frame render is written to
// databreaks_engine_027_demo.wav (48 kHz, 16-bit stereo) in the current
// directory. Do not commit it.
//
// Host software render, NOT a recording of FM-1 hardware, and not listened
// to as part of the automated checks. Self-checks (exit code 1 on failure):
// written, finite, peak in range, no clipping, nothing dropped/undelivered,
// the recording untouched by quantization, loop 5 timing == loop 1 timing
// (event positions, and audio after the previous loop's tails), and
// identical output for every block size.

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

constexpr std::int64_t kLoop = 192000;  // 2 bars at 120 BPM, 48 kHz
constexpr std::int64_t kBar = kLoop / 2;
constexpr std::int64_t kSixteenth = kBar / 16;  // 6000
constexpr std::size_t kLoops = 6;
constexpr std::size_t kTotal = kLoops * static_cast<std::size_t>(kLoop);

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

enum Pad : int { Kick = 0, Snare = 4, ClosedHat = 13, OpenHat = 14 };

void program(Pipeline& p) {
    static const Buffer kick = makeKick();
    static const Buffer snare = makeSnare();
    static const Buffer closedHat = makeHat(0.05, 70.0, 0xC0DEu);
    static const Buffer openHat = makeHat(0.3, 9.0, 0xFACEu);
    p.drums().assignSample(Kick, &kick);
    p.drums().assignSample(Snare, &snare);
    p.drums().assignSample(ClosedHat, &closedHat);
    p.drums().assignSample(OpenHat, &openHat);
    p.drums().setChokeGroup(ClosedHat, 1);
    p.drums().setChokeGroup(OpenHat, 1);
    p.setDrumLevel(0.8);
    p.setMasterLevel(0.8);
    Sequencer& s = p.sequencer();
    s.clock().setTempo(120.0);
    s.pattern().configure({4, 4}, 2, 240);  // empty pattern: 2-bar loop
}

// The loose performance: every hit is off the grid by a deterministic
// amount of up to +-1440 samples (+-30 ms; a 16th is 125 ms).
std::vector<LiveEvent> performance() {
    std::vector<LiveEvent> v;
    Noise jitter{0x2027u};
    auto add = [&](int pad, std::int64_t sixteenth, int velocity) {
        const std::int64_t t = sixteenth * kSixteenth +
                               static_cast<std::int64_t>(std::lround(jitter.next() * 1440.0));
        v.push_back({LiveEventType::DrumHit, SynthTarget::Mono, pad, velocity,
                     std::max<std::int64_t>(0, t)});
    };
    for (int bar = 0; bar < 2; ++bar) {
        const std::int64_t b = bar * 16;
        for (int k : {0, 3, 8, 10}) add(Kick, b + k, 118);
        if (bar == 1) add(Kick, b + 14, 96);
        for (int k : {4, 12}) add(Snare, b + k, 112);
        for (int k = 0; k < 16; ++k) {
            const bool open = (k == 6 || (bar == 1 && k == 14));
            add(open ? OpenHat : ClosedHat, b + k, k % 4 == 0 ? 96 : (k % 2 == 0 ? 74 : 52));
        }
    }
    std::stable_sort(v.begin(), v.end(), [](const LiveEvent& a, const LiveEvent& b) {
        return a.sampleTime < b.sampleTime;
    });
    return v;
}

QuantizeSettings sixteenths(int strength, int swing) {
    QuantizeSettings q;
    q.grid = QuantizeGrid::Sixteenth;
    q.strength = strength;
    q.swing = swing;
    return q;
}

struct Result {
    Buffer audio;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t undelivered = 0;
    std::uint64_t clipped = 0;
    std::size_t recorded = 0;
    bool rawUntouched = false;
    bool loop5MatchesLoop1 = false;
    std::int64_t maxShift50 = 0;   // largest move at 50% (samples)
    std::int64_t maxShift100 = 0;  // at 100%
};

Result renderWith(std::size_t block) {
    static Pipeline p;
    Sequencer& s = p.sequencer();
    s.stop();
    s.disarm();
    Buffer flush(1);
    p.render(flush.data(), 1);
    s.clearRecording();
    s.setQuantize(QuantizeSettings{});
    p.drums().stopAll();
    s.pattern().clear();
    s.clock().reset();
    p.resetDiagnostics();
    program(p);
    const std::uint64_t d0 = s.deliveredEventCount();
    const std::uint64_t x0 = s.droppedEventCount();
    const std::uint64_t u0 = p.undeliveredSequencerEventCount();

    static const std::vector<LiveEvent> live = performance();
    const QuantizeSettings perLoop[kLoops] = {sixteenths(0, 0),   sixteenths(0, 0),
                                              sixteenths(50, 0),  sixteenths(100, 0),
                                              sixteenths(100, 50), sixteenths(0, 50)};
    Result r;
    r.audio.assign(kTotal, StereoFrame{});
    std::vector<RecordedEvent> raw;
    std::vector<std::int64_t> loop1Positions;
    s.arm();
    s.start();
    std::size_t li = 0;
    std::size_t pos = 0;
    for (std::size_t loop = 0; loop < kLoops; ++loop) {
        // Exact, sample-accurate changes at the loop start.
        if (loop == 1) {
            s.disarm();
            const RecordedTrack& t = s.recorder().track();
            for (std::size_t i = 0; i < t.size(); ++i) raw.push_back(t.at(i));
        }
        s.setQuantize(perLoop[loop]);
        if (loop >= 1) {
            const RecordedTrack& t = s.recorder().track();
            std::vector<std::int64_t> positions;
            for (std::size_t i = 0; i < t.size(); ++i) {
                positions.push_back(s.playedStartFine(i));
                const std::int64_t moved =
                    std::abs(s.clock().sampleOfFine(s.playedStartFine(i)) -
                             s.clock().sampleOfFine(t.at(i).startFine));
                if (loop == 2) r.maxShift50 = std::max(r.maxShift50, moved);
                if (loop == 3) r.maxShift100 = std::max(r.maxShift100, moved);
            }
            if (loop == 1) loop1Positions = positions;
            if (loop == 5) r.loop5MatchesLoop1 = positions == loop1Positions;
        }
        const std::size_t end = (loop + 1) * static_cast<std::size_t>(kLoop);
        while (pos < end) {
            const std::size_t n = std::min(block, end - pos);
            while (li < live.size() && live[li].sampleTime < static_cast<std::int64_t>(pos + n)) {
                s.liveEvent(live[li++]);
            }
            p.render(r.audio.data() + pos, n);
            pos += n;
        }
    }
    const RecordedTrack& t = s.recorder().track();
    r.recorded = t.size();
    r.rawUntouched = t.size() == raw.size();
    for (std::size_t i = 0; i < t.size() && r.rawUntouched; ++i) {
        r.rawUntouched = t.at(i).sameContent(raw[i]);
    }
    r.delivered = s.deliveredEventCount() - d0;
    r.dropped = s.droppedEventCount() - x0;
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
    std::printf("DATABREAKs Engine 027 - non-destructive quantization render demo\n");
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

    // Loop 5 sounds like loop 1 once the previous loop's tails have died
    // away (0.5 s; the longest sample is 0.4 s). Voice slots may differ,
    // which can change the summation order by rounding only.
    double maxDiff = 0.0;
    for (std::size_t i = SAMPLE_RATE / 2; i < static_cast<std::size_t>(kLoop); ++i) {
        const StereoFrame a = reference.audio[static_cast<std::size_t>(kLoop) + i];
        const StereoFrame b = reference.audio[static_cast<std::size_t>(5 * kLoop) + i];
        maxDiff = std::max(maxDiff, std::max(std::abs(a.left - b.left), std::abs(a.right - b.right)));
    }

    double peak = 0.0;
    bool finite = true;
    for (const auto& f : reference.audio) {
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
    }
    const char* path = "databreaks_engine_027_demo.wav";
    const bool written = writeWav(path, reference.audio);

    std::printf("Frames: %zu (%.2f s), 120 BPM, 2-bar loop x 6, empty pattern\n", kTotal,
                static_cast<double>(kTotal) / SAMPLE_RATE);
    std::printf("Loops: 0 live loose | 1 original | 2 16th 50%% | 3 16th 100%% | 4 + swing 50 | "
                "5 original again\n");
    std::printf("Recorded hits %zu; largest move at 50%% %lld samples, at 100%% %lld samples\n",
                reference.recorded, static_cast<long long>(reference.maxShift50),
                static_cast<long long>(reference.maxShift100));
    std::printf("Recording untouched: %s; loop 5 positions == loop 1: %s; loop 5 vs loop 1 audio "
                "max difference after 0.5 s: %.3g\n",
                reference.rawUntouched ? "yes" : "NO", reference.loop5MatchesLoop1 ? "yes" : "NO",
                maxDiff);
    std::printf("Sequencer events delivered %llu, dropped %llu, undelivered %llu\n",
                static_cast<unsigned long long>(reference.delivered),
                static_cast<unsigned long long>(reference.dropped),
                static_cast<unsigned long long>(reference.undelivered));
    std::printf("Peak %.4f, master clipped samples %llu\n", peak,
                static_cast<unsigned long long>(reference.clipped));
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok = written && finite && peak > 0.1 && peak < 1.0 && reference.clipped == 0 &&
                    reference.dropped == 0 && reference.undelivered == 0 &&
                    reference.recorded == 45 && reference.rawUntouched &&
                    reference.loop5MatchesLoop1 && maxDiff < 1e-9 &&
                    reference.maxShift100 > 1000 && reference.maxShift50 > 500 &&
                    reference.maxShift50 < reference.maxShift100 && identical;
    std::printf(ok ? "ENGINE 027 RENDER PASSED\n" : "ENGINE 027 RENDER FAILED\n");
    return ok ? 0 : 1;
}
