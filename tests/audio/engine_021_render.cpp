
// DATABREAKs Engine 021 - unified pipeline render demonstration.
//
// Renders two bars of an original drum pattern at 120 BPM through
// AudioPipeline (DrumEngine -> drum level -> StereoMixer master) and writes
// databreaks_engine_021_demo.wav (48 kHz, 16-bit stereo) into the current
// directory. The drum sounds are synthesised here from simple deterministic
// formulas; no external samples are used.
//
// This is a host software render, NOT a recording of FM-1 hardware.
// It also checks the result (exit code 1 on failure): audible, finite,
// unclipped, the open hi-hat is choked, and both channels carry signal.

#include "audio_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using namespace databreaks;

namespace {

using Pipeline = AudioPipeline<16>;

// Deterministic noise in [-1, 1] (32-bit LCG).
struct Noise {
    std::uint32_t state = 0x12345678u;
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 8388607.5 - 1.0;
    }
};

std::vector<StereoFrame> makeKick() {
    const std::size_t n = SAMPLE_RATE * 45 / 100;
    std::vector<StereoFrame> s(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double freq = 45.0 + 110.0 * std::exp(-t * 30.0);
        phase += 2.0 * PI * freq / SAMPLE_RATE;
        const double v = 0.8 * std::sin(phase) * std::exp(-t * 7.0);
        s[i] = {v, v};
    }
    return s;
}

std::vector<StereoFrame> makeSnare() {
    const std::size_t n = SAMPLE_RATE * 25 / 100;
    std::vector<StereoFrame> s(n);
    Noise noise;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double tone = std::sin(2.0 * PI * 190.0 * t) * std::exp(-t * 25.0);
        const double hiss = noise.next() * std::exp(-t * 16.0);
        const double v = 0.35 * tone + 0.3 * hiss;
        s[i] = {v * 0.95, v};  // slightly right of centre
    }
    return s;
}

std::vector<StereoFrame> makeHat(double seconds, double decay,
                                 std::uint32_t seed) {
    const std::size_t n = static_cast<std::size_t>(SAMPLE_RATE * seconds);
    std::vector<StereoFrame> s(n);
    Noise noise;
    noise.state = seed;
    double previous = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double white = noise.next();
        const double high = white - previous;  // crude high-pass
        previous = white;
        const double v = 0.18 * high * std::exp(-t * decay);
        s[i] = {v * 0.8, v};  // hats sit to the right
    }
    return s;
}

std::vector<StereoFrame> makeClap() {
    const std::size_t n = SAMPLE_RATE * 2 / 10;
    std::vector<StereoFrame> s(n);
    Noise noise;
    noise.state = 0xC1A9u;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        // Three short bursts then a tail.
        double env = std::exp(-t * 18.0);
        const double burst = std::fmod(t, 0.011);
        if (t < 0.033) {
            env = std::exp(-burst * 300.0);
        }
        const double v = 0.3 * noise.next() * env;
        s[i] = {v, v * 0.9};  // slightly left
    }
    return s;
}

std::vector<StereoFrame> makeTom() {
    const std::size_t n = SAMPLE_RATE * 35 / 100;
    std::vector<StereoFrame> s(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        phase += 2.0 * PI * (110.0 + 40.0 * std::exp(-t * 12.0)) / SAMPLE_RATE;
        const double v = 0.5 * std::sin(phase) * std::exp(-t * 9.0);
        s[i] = {v * 0.7, v};  // right
    }
    return s;
}

void write16(std::ofstream& f, std::uint16_t v) {
    f.put(static_cast<char>(v & 0xFF));
    f.put(static_cast<char>((v >> 8) & 0xFF));
}

void write32(std::ofstream& f, std::uint32_t v) {
    write16(f, static_cast<std::uint16_t>(v & 0xFFFF));
    write16(f, static_cast<std::uint16_t>((v >> 16) & 0xFFFF));
}

bool writeWav(const char* path, const std::vector<StereoFrame>& audio) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    const std::uint32_t dataSize =
        static_cast<std::uint32_t>(audio.size() * 4);
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
        const auto l = static_cast<std::int16_t>(
            std::lround(clampAudio(frame.left) * 32767.0));
        const auto r = static_cast<std::int16_t>(
            std::lround(clampAudio(frame.right) * 32767.0));
        write16(f, static_cast<std::uint16_t>(l));
        write16(f, static_cast<std::uint16_t>(r));
    }
    return static_cast<bool>(f);
}

struct Hit {
    int step;        // 16th-note step, 0..31 (two bars)
    std::size_t pad;
    int velocity;
};

enum Pad : std::size_t {
    Kick = 0, Snare = 1, ClosedHat = 2, OpenHat = 3, Clap = 4, Tom = 5
};

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 021 - unified pipeline render demo\n");
    std::printf("Host software render; not an FM-1 hardware recording.\n");

    const auto kick = makeKick();
    const auto snare = makeSnare();
    const auto closedHat = makeHat(0.08, 60.0, 0xBEEFu);
    const auto openHat = makeHat(0.6, 5.0, 0xF00Du);
    const auto clap = makeClap();
    const auto tom = makeTom();

    static Pipeline pipeline;
    auto& drums = pipeline.drums();
    drums.assignSample(Kick, &kick);
    drums.assignSample(Snare, &snare);
    drums.assignSample(ClosedHat, &closedHat);
    drums.assignSample(OpenHat, &openHat);
    drums.assignSample(Clap, &clap);
    drums.assignSample(Tom, &tom);
    drums.setChokeGroup(ClosedHat, 1);  // closed hat chokes open hat
    drums.setChokeGroup(OpenHat, 1);
    drums.setVelocitySensitivity(ClosedHat, 0.8);
    drums.setPitch(Tom, -3.0);
    pipeline.setDrumLevel(0.8);
    pipeline.setMasterLevel(0.9);

    const Hit pattern[] = {
        {0, Kick, 127}, {0, ClosedHat, 100}, {2, ClosedHat, 60},
        {4, Snare, 110}, {4, ClosedHat, 90}, {6, OpenHat, 95},
        {8, ClosedHat, 100},  // chokes the open hat from step 6
        {8, Kick, 115}, {10, ClosedHat, 55}, {11, Kick, 80},
        {12, Snare, 120}, {12, Clap, 90}, {12, ClosedHat, 85},
        {14, OpenHat, 100}, {16, Kick, 127},
        {16, ClosedHat, 100},  // chokes the open hat from step 14
        {18, ClosedHat, 60}, {20, Snare, 110}, {20, ClosedHat, 90},
        {22, ClosedHat, 70}, {24, Kick, 120}, {24, ClosedHat, 100},
        {26, Tom, 100}, {27, Tom, 85}, {28, Snare, 127}, {28, Clap, 110},
        {29, Tom, 70}, {30, OpenHat, 110},  // left ringing at the end
    };

    const std::size_t stepFrames = SAMPLE_RATE * 60 / 120 / 4;  // 16ths
    const std::size_t totalFrames = stepFrames * 32 + SAMPLE_RATE;  // + tail
    const std::size_t blockSize = 256;

    std::vector<StereoFrame> audio(totalFrames);
    // An open-hat hit counts as choked if it stops playing (releasing or
    // gone) before its sample could have ended on its own.
    int openHatChokes = 0;
    std::size_t openHatStart = 0;
    bool openHatLive = false;
    for (std::size_t pos = 0; pos < totalFrames; pos += blockSize) {
        const std::size_t n = std::min(blockSize, totalFrames - pos);
        for (const Hit& hit : pattern) {
            const std::size_t at =
                static_cast<std::size_t>(hit.step) * stepFrames;
            if (at >= pos && at < pos + n) {
                drums.postTrigger(hit.pad, hit.velocity, at - pos);
                if (hit.pad == OpenHat) {
                    openHatStart = at;
                    openHatLive = true;
                }
            }
        }
        pipeline.render(audio.data() + pos, n);
        if (openHatLive && !drums.isPadPlaying(OpenHat)) {
            openHatLive = false;
            if (pos + n - openHatStart < openHat.size()) {
                ++openHatChokes;
            }
        }
    }
    const bool openHatChoked = openHatChokes == 2;

    double peak = 0.0;
    double energyL = 0.0;
    double energyR = 0.0;
    bool finite = true;
    for (const auto& f : audio) {
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
        energyL += f.left * f.left;
        energyR += f.right * f.right;
    }

    const char* path = "databreaks_engine_021_demo.wav";
    const bool written = writeWav(path, audio);

    std::printf("Frames: %zu (%.2f s), block %zu, events %llu\n",
                totalFrames, static_cast<double>(totalFrames) / SAMPLE_RATE,
                blockSize,
                static_cast<unsigned long long>(drums.processedEventCount()));
    std::printf("Peak: %.4f, clipped samples: %llu, RMS L/R: %.4f / %.4f\n",
                peak,
                static_cast<unsigned long long>(pipeline.clippedSampleCount()),
                std::sqrt(energyL / totalFrames),
                std::sqrt(energyR / totalFrames));
    std::printf("Open hat hits choked by the closed hat: %d (expected 2)\n",
                openHatChokes);
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok = written && finite && peak > 0.05 && peak < 1.0 &&
                    pipeline.clippedSampleCount() == 0 && openHatChoked &&
                    energyL > 0.0 && energyR > 0.0 &&
                    drums.processedEventCount() ==
                        sizeof(pattern) / sizeof(pattern[0]) &&
                    drums.droppedEventCount() == 0;
    std::printf(ok ? "ENGINE 021 RENDER PASSED\n" : "ENGINE 021 RENDER FAILED\n");
    return ok ? 0 : 1;
}
