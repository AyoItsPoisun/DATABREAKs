
// DATABREAKs Engine 023 - effects engine tests.
//
// Host-side assertion tests for core/audio/effects/* and their integration
// into AudioPipeline. Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
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

int passed = 0;
int failed = 0;

void check(bool condition, const std::string& description) {
    if (condition) {
        ++passed;
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++failed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

std::string num(double v) {
    std::ostringstream os;
    os.precision(4);
    os << v;
    return os.str();
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::size_t kSecond = static_cast<std::size_t>(SAMPLE_RATE);

using Buffer = std::vector<StereoFrame>;

Buffer sine(double frequency, double amplitude, std::size_t frames,
            double rightScale = 1.0) {
    Buffer b(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double v = amplitude * std::sin(2.0 * PI * frequency *
                                              static_cast<double>(i) /
                                              SAMPLE_RATE);
        b[i] = {v, v * rightScale};
    }
    return b;
}

double rms(const Buffer& b, std::size_t from, std::size_t to, bool left = true) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double v = left ? b[i].left : b[i].right;
        e += v * v;
    }
    return std::sqrt(e / static_cast<double>(to - from));
}

double peak(const Buffer& b, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = b.size();
    double p = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        p = std::max(p, std::max(std::abs(b[i].left), std::abs(b[i].right)));
    }
    return p;
}

double maxStep(const Buffer& b, std::size_t from, std::size_t to) {
    double worst = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) {
        worst = std::max(worst, std::abs(b[i].left - b[i - 1].left));
        worst = std::max(worst, std::abs(b[i].right - b[i - 1].right));
    }
    return worst;
}

bool finite(const Buffer& b) {
    for (const auto& f : b) {
        if (!std::isfinite(f.left) || !std::isfinite(f.right)) return false;
    }
    return true;
}

bool identical(const Buffer& a, const Buffer& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].left != b[i].left || a[i].right != b[i].right) return false;
    }
    return true;
}

bool allZero(const Buffer& b, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = b.size();
    for (std::size_t i = from; i < to; ++i) {
        if (b[i].left != 0.0 || b[i].right != 0.0) return false;
    }
    return true;
}

// Magnitude of the DFT bin at `frequency` (single bin, Goertzel-style sum).
double binMagnitude(const Buffer& b, double frequency, std::size_t from,
                    std::size_t to) {
    std::complex<double> acc = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double phase = -2.0 * PI * frequency *
                             static_cast<double>(i) / SAMPLE_RATE;
        acc += b[i].left * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::abs(acc) * 2.0 / static_cast<double>(to - from);
}

template <typename Effect>
Buffer run(Effect& effect, Buffer b, std::size_t block = 0) {
    if (block == 0) {
        effect.process(b.data(), b.size());
    } else {
        for (std::size_t pos = 0; pos < b.size(); pos += block) {
            effect.process(b.data() + pos, std::min(block, b.size() - pos));
        }
    }
    return b;
}

double gainDbAt(Equalizer& eq, double frequency) {
    eq.reset();
    const Buffer in = sine(frequency, 0.25, kSecond);
    const Buffer out = run(eq, in);
    return 20.0 * std::log10(rms(out, kSecond / 2, kSecond) /
                             rms(in, kSecond / 2, kSecond));
}

bool stable(const BiquadCoefficients& c) {
    // Poles of z^2 + a1 z + a2 inside the unit circle.
    return std::abs(c.a2) < 1.0 && std::abs(c.a1) < 1.0 + c.a2;
}

// ---------------------------------------------------------------------------
void equalizer() {
    std::cout << "\n-- Equalizer --\n";

    {
        Equalizer eq;
        const Buffer in = sine(440.0, 0.5, 4800, -0.7);
        check(identical(run(eq, in), in),
              "EQ1 Flat EQ (all bands 0 dB) passes audio bit-exactly");
    }
    {
        Equalizer eq;
        eq.setLow(120.0, 12.0);
        eq.reset();
        const double lowGain = gainDbAt(eq, 30.0);
        const double lowAbove = gainDbAt(eq, 5000.0);
        eq.setLow(120.0, 0.0);
        eq.setMid(1000.0, 9.0, 1.0);
        eq.reset();
        const double midGain = gainDbAt(eq, 1000.0);
        const double midFar = gainDbAt(eq, 60.0);
        eq.setMid(1000.0, 0.0);
        eq.setHigh(8000.0, -12.0);
        eq.reset();
        const double highGain = gainDbAt(eq, 18000.0);
        const double highBelow = gainDbAt(eq, 200.0);
        check(std::abs(lowGain - 12.0) < 0.5 && std::abs(lowAbove) < 0.3,
              "EQ2 Low shelf +12 dB: " + num(lowGain) + " dB at 30 Hz, " +
                  num(lowAbove) + " dB at 5 kHz");
        check(std::abs(midGain - 9.0) < 0.1 && std::abs(midFar) < 0.3,
              "EQ3 Mid peak +9 dB at 1 kHz: " + num(midGain) + " dB, " +
                  num(midFar) + " dB at 60 Hz");
        check(std::abs(highGain + 12.0) < 1.0 && std::abs(highBelow) < 0.3,
              "EQ4 High shelf -12 dB: " + num(highGain) + " dB at 18 kHz, " +
                  num(highBelow) + " dB at 200 Hz");
    }
    {
        bool allStable = true;
        const double freqs[] = {20.0, 100.0, 1000.0, 10000.0, 21600.0};
        for (double f : freqs) {
            for (double g : {-18.0, -6.0, 6.0, 18.0}) {
                for (double q : {0.2, 0.7, 8.0}) {
                    allStable = allStable &&
                        stable(designBiquad(BiquadShape::LowShelf, f, g, 0.7071)) &&
                        stable(designBiquad(BiquadShape::Peak, f, g, q)) &&
                        stable(designBiquad(BiquadShape::HighShelf, f, g, 0.7071));
                }
            }
        }
        check(allStable, "EQ5 Poles inside the unit circle at every range corner");
    }
    {
        Equalizer eq;
        eq.setMid(1000.0, 0.0);
        const Buffer in = sine(1000.0, 0.3, kSecond);
        Buffer out = in;
        eq.process(out.data(), kSecond / 2);
        eq.setMid(1000.0, 18.0, 2.0);     // +18 dB jump, smoothed
        eq.setLow(80.0, -18.0);
        eq.process(out.data() + kSecond / 2, kSecond / 2);
        const double during = maxStep(out, kSecond / 2 - 10, kSecond / 2 + 2000);
        const double after = maxStep(out, kSecond - 4800, kSecond);
        check(during <= after * 1.05 + 1e-9,
              "EQ6 +18 dB change glides: step " + num(during) +
                  " <= settled step " + num(after));
    }
    {
        Equalizer eq;
        eq.setLow(kNaN, kInf);
        eq.setMid(1e9, 99.0, -5.0);
        eq.setHigh(-1.0, -99.0);
        eq.setOutputGainDb(kNaN);
        // Frequencies are stored as log2(Hz), so compare with a tolerance.
        check(std::abs(eq.bandFrequency(0) - 120.0) < 1e-9 &&
              eq.bandGainDb(0) == 0.0 &&
              std::abs(eq.bandFrequency(1) - 10000.0) < 1e-6 &&
              eq.bandGainDb(1) == 18.0 &&
              std::abs(eq.bandFrequency(2) - 1000.0) < 1e-6 &&
              eq.bandGainDb(2) == -18.0,
              "EQ7 Non-finite values ignored, out-of-range values clamped");
    }
}

// ---------------------------------------------------------------------------
void compressor() {
    std::cout << "\n-- Compressor --\n";

    check(std::abs(Compressor::reductionFor(-6.0, -18.0, 4.0, 0.0) - 9.0) < 1e-12 &&
          Compressor::reductionFor(-20.0, -18.0, 4.0, 0.0) == 0.0 &&
          std::abs(Compressor::reductionFor(-18.0, -18.0, 4.0, 6.0) - 0.5625) < 1e-12 &&
          std::abs(Compressor::reductionFor(-15.0, -18.0, 4.0, 6.0) - 2.25) < 1e-12,
          "CO1 Static curve: 9 dB at 12 dB over (4:1); soft knee is continuous");

    {
        Compressor c;
        c.setThresholdDb(-18.0);
        c.setRatio(4.0);
        c.setKneeDb(0.0);
        c.setAttackMs(1.0);
        c.setReleaseMs(500.0);
        c.reset();
        const Buffer out = run(c, sine(1000.0, 0.5, kSecond));
        const double outPeak = peak(out, kSecond / 2, kSecond);
        const double expected = 0.5 * dbToGain(-9.0);
        check(std::abs(gainToDb(outPeak / expected)) < 0.5,
              "CO2 Sustained -6 dBFS tone, -18 dB / 4:1: peak " + num(outPeak) +
                  " vs " + num(expected) + " expected (within 0.5 dB)");
    }
    {
        Compressor c;
        c.setThresholdDb(-18.0);
        c.setRatio(4.0);
        c.setKneeDb(0.0);
        c.setAttackMs(10.0);
        c.setReleaseMs(100.0);
        c.reset();
        Buffer step(kSecond, StereoFrame{0.5, 0.5});
        double atAttack = 0.0;
        double atFive = 0.0;
        for (std::size_t i = 0; i < 2400; ++i) {
            c.process(&step[i], 1);
            if (i + 1 == 480) atAttack = c.gainReductionDb();
            if (i + 1 == 2400) atFive = c.gainReductionDb();
        }
        // After one attack time the envelope is 0.5 (1 - 1/e) = -9.98 dBFS.
        const double expectAttack = 0.75 * (gainToDb(0.5 * (1.0 - std::exp(-1.0))) + 18.0);
        Buffer silence(4800, StereoFrame{0.0, 0.0});
        for (std::size_t i = 0; i < 4800; ++i) {
            c.process(&silence[i], 1);
        }
        // After one release time from ~0.5: 0.5/e = -14.7 dBFS -> 2.48 dB.
        const double expectRelease = 0.75 * (gainToDb(0.5 * std::exp(-1.0)) + 18.0);
        check(std::abs(atAttack - expectAttack) < 0.1 && atFive > 8.9,
              "CO3 Transient: GR " + num(atAttack) + " dB after 10 ms attack (" +
                  num(expectAttack) + "), " + num(atFive) + " dB after 50 ms");
        check(std::abs(c.gainReductionDb() - expectRelease) < 0.15,
              "CO4 Release: GR " + num(c.gainReductionDb()) +
                  " dB after one 100 ms release (" + num(expectRelease) + ")");
    }
    {
        Compressor wet;
        Compressor par;
        par.setMix(0.5);
        par.reset();
        const Buffer in = sine(220.0, 0.8, 9600);
        const Buffer w = run(wet, in);
        const Buffer p = run(par, in);
        bool blend = true;
        for (std::size_t i = 0; i < in.size(); ++i) {
            blend = blend && std::abs(p[i].left - (0.5 * in[i].left + 0.5 * w[i].left)) < 1e-12;
        }
        Compressor makeup;
        makeup.setRatio(1.0);
        makeup.setMakeupDb(6.0);
        makeup.reset();
        const Buffer m = run(makeup, in);
        check(blend && std::abs(m[100].left - in[100].left * dbToGain(6.0)) < 1e-12,
              "CO5 Parallel mix 0.5 = average of dry and compressed; makeup +6 dB");
    }
    {
        Compressor c;
        c.setThresholdDb(kNaN);
        c.setRatio(100.0);
        c.setAttackMs(-5.0);
        c.setReleaseMs(kInf);
        c.setMakeupDb(99.0);
        check(c.thresholdDb() == -18.0 && c.ratio() == 20.0 &&
              c.attackMs() == 0.1 && c.releaseMs() == 120.0 &&
              c.makeupDb() == 24.0,
              "CO6 Compressor parameters validated and clamped");
    }
}

// ---------------------------------------------------------------------------
void clipperAndDistortion() {
    std::cout << "\n-- Soft clipper and distortion --\n";

    {
        SoftClipper s;  // knee 0.7, ceiling 1
        const Buffer in = sine(300.0, 0.69, 4800, -1.0);
        const bool linear = identical(run(s, in), in);
        bool bounded = true;
        bool monotonic = true;
        double prev = -1e9;
        for (int i = -2000; i <= 2000; ++i) {
            const double x = i * 0.05;  // -100 .. 100
            const double y = SoftClipper::shape(x, 0.7, 1.0);
            bounded = bounded && std::abs(y) <= 1.0;
            monotonic = monotonic && y >= prev;
            prev = y;
        }
        const double eps = 1e-6;
        const double slope = (SoftClipper::shape(0.7 + eps, 0.7, 1.0) -
                              SoftClipper::shape(0.7, 0.7, 1.0)) / eps;
        check(linear, "SC1 Below the knee (|x| <= 0.7) the clipper is bit-exact");
        check(bounded && monotonic && std::abs(slope - 1.0) < 1e-4,
              "SC2 Curve is monotonic, never above the 1.0 ceiling for |x| up to 100, slope 1 at the knee");
        // Above the knee the curve really saturates: exact tanh values and a
        // slope that falls below 1 (a hard clip would stay linear here).
        const double at09 = SoftClipper::shape(0.9, 0.7, 1.0);
        const double at15 = SoftClipper::shape(1.5, 0.7, 1.0);
        const double slope09 = (SoftClipper::shape(0.9 + eps, 0.7, 1.0) - at09) / eps;
        check(std::abs(at09 - (0.7 + 0.3 * std::tanh(0.2 / 0.3))) < 1e-15 &&
              std::abs(at15 - (0.7 + 0.3 * std::tanh(0.8 / 0.3))) < 1e-15 &&
              at09 < 0.9 && slope09 < 0.7 && slope09 > 0.5 &&
              std::abs(SoftClipper::shape(-0.9, 0.7, 1.0) + at09) < 1e-15,
              "SC4 Above the knee: y(0.9) = " + num(at09) + ", slope " +
                  num(slope09) + " (soft, symmetric)");
        SoftClipper hot;
        hot.setDriveDb(24.0);
        hot.reset();
        const Buffer driven = run(hot, sine(100.0, 0.9, 9600));
        check(peak(driven) <= 1.0 && peak(driven) > 0.95,
              "SC3 +24 dB drive saturates up to, never above, the 1.0 ceiling (peak " +
                  num(peak(driven)) + ")");
    }
    {
        bool bounded = true;
        bool silent = true;
        for (DistortionMode mode : {DistortionMode::Soft,
                                    DistortionMode::Asymmetric,
                                    DistortionMode::Fuzz}) {
            Distortion d;
            d.setMode(mode);
            d.setDriveDb(36.0);
            d.setOutputDb(0.0);
            d.reset();
            const Buffer out = run(d, sine(110.0, 1.0, kSecond));
            bounded = bounded && finite(out) && peak(out) <= 2.0;
            Distortion quiet;
            quiet.setMode(mode);
            quiet.reset();
            silent = silent && allZero(run(quiet, Buffer(4800)));
        }
        check(bounded, "DI1 All modes at +36 dB drive stay finite and bounded");
        check(silent, "DI2 All modes map silence to exact silence (fuzz bias removed)");
    }
    {
        Distortion soft;
        soft.setMode(DistortionMode::Soft);
        soft.reset();
        Distortion asym;
        asym.setMode(DistortionMode::Asymmetric);
        asym.reset();
        const Buffer in = sine(500.0, 0.5, kSecond);
        const Buffer s = run(soft, in);
        const Buffer a = run(asym, in);
        const std::size_t from = kSecond / 2;
        const double softH2 = binMagnitude(s, 1000.0, from, kSecond) /
                              binMagnitude(s, 500.0, from, kSecond);
        const double softH3 = binMagnitude(s, 1500.0, from, kSecond) /
                              binMagnitude(s, 500.0, from, kSecond);
        const double asymH2 = binMagnitude(a, 1000.0, from, kSecond) /
                              binMagnitude(a, 500.0, from, kSecond);
        double meanL = 0.0;
        double meanR = 0.0;
        for (std::size_t i = from; i < kSecond; ++i) {
            meanL += a[i].left;
            meanR += a[i].right;
        }
        meanL /= static_cast<double>(kSecond - from);
        meanR /= static_cast<double>(kSecond - from);
        const double mean = std::max(std::abs(meanL), std::abs(meanR));
        check(softH2 < 1e-6 && softH3 > 0.01 && asymH2 > 0.01,
              "DI3 Soft: odd harmonics only (H2/H1 " + num(softH2) + ", H3/H1 " +
                  num(softH3) + "); asymmetric adds H2 (" + num(asymH2) + ")");
        check(std::abs(mean) < 1e-3,
              "DI4 DC blocker removes the asymmetric offset on both channels (worst mean " +
                  num(mean) + ")");
    }
    {
        Distortion d;
        d.setMode(DistortionMode::Soft);
        d.reset();
        const Buffer in = sine(200.0, 0.4, kSecond);
        Buffer out = in;
        d.process(out.data(), kSecond / 2);
        d.setMode(DistortionMode::Fuzz);
        d.process(out.data() + kSecond / 2, kSecond / 2);
        const double atSwitch = maxStep(out, kSecond / 2 - 2, kSecond / 2 + 960);
        const double settled = maxStep(out, kSecond - 9600, kSecond);
        check(atSwitch <= settled + 1e-9,
              "DI5 Mode change crossfades: step " + num(atSwitch) +
                  " <= settled fuzz step " + num(settled));
    }
}

// ---------------------------------------------------------------------------
void reverb() {
    std::cout << "\n-- Reverb --\n";

    check(Reverb::delayMemoryBytes() == (19446 + 905) * sizeof(double) &&
          Reverb::delayMemoryBytes() == 162808,
          "RV1 Delay memory is fixed: 162,808 bytes (" +
              std::to_string(sizeof(Reverb)) + " bytes per Reverb object)");

    static Reverb r;
    r.setDecaySeconds(1.0);
    r.setDryLevel(0.0);
    r.setWetLevel(1.0);
    r.setDamping(0.0);
    r.reset();
    {
        Buffer impulse(3 * kSecond);
        impulse[0] = {1.0, 1.0};
        const Buffer out = run(r, impulse);
        const double early = rms(out, kSecond / 5, kSecond * 3 / 10);
        const double late = rms(out, kSecond * 7 / 10, kSecond * 8 / 10);
        const double dropDb = gainToDb(early / late);  // over 0.5 s
        check(finite(out) && peak(out) < 1.0 && std::abs(dropDb - 30.0) < 6.0,
              "RV2 Impulse decays " + num(dropDb) +
                  " dB in 0.5 s for RT60 = 1 s (expected ~30)");
    }
    {
        r.reset();
        Buffer impulse(kSecond / 4);
        impulse[0] = {1.0, 1.0};
        const Buffer out = run(r, impulse);
        check(out[0].left == 0.0 && allZero(out, 0, 300) && !allZero(out),
              "RV3 Pure wet output starts after the shortest delay, then sounds");
    }
    {
        static Reverb big;
        big.setDecaySeconds(10.0);
        big.setDamping(0.0);
        big.setRoomSize(1.0);
        big.reset();
        Buffer noise(kSecond);
        std::uint32_t state = 1;
        for (auto& f : noise) {
            state = state * 1664525u + 1013904223u;
            const double v = (static_cast<double>(state >> 8) / 8388607.5 - 1.0) * 0.5;
            f = {v, -v};
        }
        run(big, noise);
        double first = 0.0;
        double last = 0.0;
        bool ok = true;
        for (int second = 0; second < 60; ++second) {
            const Buffer out = run(big, Buffer(kSecond));
            ok = ok && finite(out) && peak(out) < 4.0;
            if (second == 0) first = rms(out, 0, kSecond);
            if (second == 59) last = rms(out, 0, kSecond);
        }
        check(ok && last < first * 1e-3,
              "RV4 Out-of-phase noise, max decay/size, no damping: 60 s stable, tail falls " +
                  num(gainToDb(first / last)) + " dB");
    }
    {
        static Reverb tail;
        tail.setDecaySeconds(0.5);
        tail.reset();
        Buffer burst = sine(440.0, 0.8, kSecond / 10);
        run(tail, burst);
        run(tail, Buffer(20 * kSecond));
        check(allZero(run(tail, Buffer(4800))),
              "RV5 Finished tail becomes exact digital silence");
    }
    {
        static Reverb dc;
        dc.setDryLevel(0.0);
        dc.reset();
        const Buffer out = run(dc, Buffer(5 * kSecond, StereoFrame{0.5, 0.5}));
        double mean = 0.0;
        for (std::size_t i = 4 * kSecond; i < 5 * kSecond; ++i) mean += out[i].left;
        mean /= static_cast<double>(kSecond);
        check(std::abs(mean) < 1e-4 && peak(out) < 0.5,
              "RV6 Constant DC input: no DC build-up (mean " + num(mean) + ")");
    }
    {
        static Reverb dryOnly;
        dryOnly.setWetLevel(0.0);
        dryOnly.reset();
        const Buffer in = sine(330.0, 0.5, 9600, 0.3);
        static Reverb mono;
        mono.setWidth(0.0);
        mono.setDryLevel(0.0);
        mono.reset();
        Buffer stereoIn = sine(330.0, 0.5, 9600, -0.5);
        const Buffer m = run(mono, stereoIn);
        bool same = true;
        for (const auto& f : m) same = same && f.left == f.right;
        check(identical(run(dryOnly, in), in),
              "RV7 Wet 0 / dry 1 passes the input bit-exactly");
        check(same && !allZero(m), "RV8 Width 0 gives identical left and right");
    }
    {
        static Reverb sz;
        sz.setRoomSize(0.4);
        sz.reset();
        const Buffer in = sine(220.0, 0.3, 2 * kSecond);
        Buffer out = in;
        sz.process(out.data(), kSecond);
        sz.setRoomSize(1.0);
        sz.setDecaySeconds(6.0);
        sz.process(out.data() + kSecond, kSecond);
        const double during = maxStep(out, kSecond - 2, kSecond + 2000);
        const double steady = maxStep(out, kSecond / 2, kSecond - 2);
        check(during < steady * 1.5,
              "RV9 Room size / decay change glides: step " + num(during) +
                  " vs " + num(steady) + " before");
    }
    {
        static Reverb bad;
        bad.setRoomSize(kNaN);
        bad.setDecaySeconds(1000.0);
        bad.setDamping(-3.0);
        bad.setWetLevel(kInf);
        bad.setWidth(5.0);
        check(bad.roomSize() == 0.7 && bad.decaySeconds() == 10.0 &&
              bad.damping() == 0.0 && bad.wetLevel() == 0.3 && bad.width() == 1.0,
              "RV10 Reverb parameters validated and clamped");
    }
}

// ---------------------------------------------------------------------------
void common() {
    std::cout << "\n-- Bypass, mix, reset and chain --\n";

    // Bypass transparency, crossfade and full bypass.
    {
        static Reverb r;
        const Buffer in = sine(440.0, 0.5, kSecond);
        Buffer out = in;
        r.process(out.data(), kSecond / 2);
        r.setBypassed(true);
        r.process(out.data() + kSecond / 2, kSecond / 2);
        const std::size_t fadeEnd = kSecond / 2 + SmoothedValue::DefaultRampFrames;
        bool exact = true;
        for (std::size_t i = fadeEnd; i < kSecond; ++i) {
            exact = exact && out[i].left == in[i].left && out[i].right == in[i].right;
        }
        check(exact && r.fullyBypassed(),
              "BY1 After the 20 ms fade, bypass is bit-exact and the effect stops");
        const double fadeStep = maxStep(out, kSecond / 2 - 2, fadeEnd + 2);
        const double before = std::max(maxStep(out, kSecond / 4, kSecond / 2 - 2),
                                       maxStep(in, 0, kSecond));
        check(fadeStep <= before * 1.05,
              "BY2 Bypass crossfade has no jump (step " + num(fadeStep) +
                  " vs " + num(before) + " outside the fade)");

        Buffer again = in;
        r.process(again.data(), again.size());
        check(identical(again, in), "BY3 Fully bypassed effect leaves buffers untouched");
    }
    // Wet/dry endpoints.
    {
        Distortion d;
        d.setMix(0.0);
        d.reset();
        const Buffer in = sine(150.0, 0.5, 4800);
        check(identical(run(d, in), in), "MX1 Mix 0 outputs the input bit-exactly");
        Distortion full;
        full.reset();
        Distortion half;
        half.setMix(0.5);
        half.reset();
        const Buffer w = run(full, in);
        const Buffer h = run(half, in);
        bool mid = true;
        for (std::size_t i = 0; i < in.size(); ++i) {
            mid = mid && std::abs(h[i].left - 0.5 * (in[i].left + w[i].left)) < 1e-12;
        }
        check(mid, "MX2 Mix 0.5 is the exact midpoint of dry and wet");
    }
    // Reset: after use, output equals a fresh instance.
    {
        Buffer impulse(kSecond / 2);
        impulse[0] = {0.9, -0.4};
        impulse[100] = {0.3, 0.3};
        static Reverb used;
        static Reverb fresh;
        run(used, sine(300.0, 0.7, kSecond));
        used.reset();
        Compressor cUsed;
        Compressor cFresh;
        run(cUsed, sine(300.0, 0.9, kSecond));
        cUsed.reset();
        Equalizer eUsed;
        Equalizer eFresh;
        eUsed.setLow(100.0, 6.0);
        eFresh.setLow(100.0, 6.0);
        run(eUsed, sine(80.0, 0.9, kSecond));
        eUsed.reset();
        eFresh.reset();
        check(identical(run(used, impulse), run(fresh, impulse)) &&
              identical(run(cUsed, impulse), run(cFresh, impulse)) &&
              identical(run(eUsed, impulse), run(eFresh, impulse)),
              "RS1 reset() makes reverb, compressor and EQ behave like new");
    }
    // Null / empty buffers.
    {
        static Reverb r;
        Compressor c;
        r.process(nullptr, 64);
        c.process(nullptr, 64);
        StereoFrame f{0.5, 0.5};
        r.process(&f, 0);
        check(f.left == 0.5, "NL1 Null and empty buffers are ignored safely");
    }
    // Chain editing.
    {
        EffectChain<3> chain;
        Equalizer a;
        Compressor b;
        SoftClipper c;
        Distortion d;
        const bool ok = chain.append(&a) && chain.append(&c) &&
                        chain.insert(1, &b) && !chain.append(&d) &&
                        !chain.insert(0, nullptr);
        const bool order = chain.slot(0) == &a && chain.slot(1) == &b &&
                           chain.slot(2) == &c && chain.indexOf(&d) == 3;
        const bool removed = chain.remove(0) && chain.slot(0) == &b &&
                             chain.size() == 2 && chain.insert(2, &d) &&
                             chain.slot(2) == &d;
        check(ok && order && removed,
              "CH1 Chain: append, insert at a position, capacity, remove, indexOf");
    }
}

// ---------------------------------------------------------------------------
using Pipeline = AudioPipeline<16>;

const Buffer& drumSample() {
    static const Buffer s = [] {
        Buffer b(9600);
        for (std::size_t i = 0; i < b.size(); ++i) {
            const double t = static_cast<double>(i) / SAMPLE_RATE;
            const double v = 0.6 * std::sin(2.0 * PI * 70.0 * t) * std::exp(-t * 12.0);
            b[i] = {v, v};
        }
        return b;
    }();
    return s;
}

void enableAll(Pipeline& p) {
    p.eq().setLow(100.0, 4.0);
    p.eq().setHigh(9000.0, 2.0);
    p.compressor().setMakeupDb(3.0);
    p.distortion().setMix(0.3);
    for (std::size_t i = 0; i < p.effects().size(); ++i) {
        // Engine 024 added Tape to the chain; Engine 023 checks keep
        // measuring the five Engine 023 effects (tape: engine_024).
        if (std::string(p.effects().slot(i)->name()) == "Tape") continue;
        p.effects().slot(i)->setBypassed(false, true);
    }
}

void disableAll(Pipeline& p) {
    for (std::size_t i = 0; i < p.effects().size(); ++i) {
        p.effects().slot(i)->setBypassed(true, true);
    }
}

// Drums + three synths with every effect on, rendered with a block size.
Buffer performance(std::size_t blockSize, bool effectsOn, std::uint64_t* clipped) {
    static Pipeline p;
    p.drums().stopAll();
    p.mono().reset();
    p.fm().reset();
    p.poly().reset();
    p.effects().resetAll();
    p.resetDiagnostics();
    p.drums().assignSample(0, &drumSample());
    if (effectsOn) enableAll(p); else disableAll(p);
    // Levels chosen so that the dry (all bypassed) mix does not clip either.
    p.setDrumLevel(0.6);
    p.setMonoLevel(0.7);
    p.setPolyLevel(0.6);
    p.setFmLevel(0.5);
    const std::size_t total = 3 * kSecond;
    Buffer out(total);
    for (std::size_t pos = 0; pos < total; pos += blockSize) {
        const std::size_t n = std::min(blockSize, total - pos);
        for (std::size_t f = pos; f < pos + n; ++f) {
            const std::size_t o = f - pos;
            if (f % 12000 == 0) {
                const int root = 45 + static_cast<int>((f / 12000) % 4) * 3;
                p.drums().postTrigger(0, 110, o);
                p.postNoteOn(SynthTarget::Mono, root - 12, 100, o);
                p.postNoteOn(SynthTarget::Poly, root + 12, 80, o);
                p.postNoteOn(SynthTarget::Poly, root + 16, 80, o);
                p.postNoteOn(SynthTarget::Fm, root + 24, 90, o);
            }
            if (f % 12000 == 9000) {
                const int root = 45 + static_cast<int>((f / 12000) % 4) * 3;
                p.postNoteOff(SynthTarget::Mono, root - 12, o);
                p.postNoteOff(SynthTarget::Poly, root + 12, o);
                p.postNoteOff(SynthTarget::Poly, root + 16, o);
                p.postNoteOff(SynthTarget::Fm, root + 24, o);
            }
        }
        p.render(out.data() + pos, n);
    }
    if (clipped != nullptr) *clipped = p.clippedSampleCount();
    // Drain tails so the next call starts from silence.
    Buffer tail(4 * kSecond);
    p.postAllNotesOff(SynthTarget::Mono);
    p.postAllNotesOff(SynthTarget::Fm);
    p.postAllNotesOff(SynthTarget::Poly);
    p.render(tail.data(), tail.size());
    return out;
}

void pipeline() {
    std::cout << "\n-- Pipeline integration --\n";

    {
        static Pipeline p;
        // Engine 024 inserted Tape after SoftClipper (6 slots).
        bool allBypassed = p.effects().size() == 6;
        for (std::size_t i = 0; i < p.effects().size(); ++i) {
            allBypassed = allBypassed && p.effects().slot(i)->fullyBypassed();
        }
        const char* expected[] = {"Equalizer", "Compressor", "SoftClipper",
                                  "Tape", "Distortion", "Reverb"};
        bool order = true;
        for (std::size_t i = 0; i < 6; ++i) {
            order = order && std::string(p.effects().slot(i)->name()) == expected[i];
        }
        check(allBypassed && order,
              "PI1 Chain order EQ > Compressor > SoftClipper > Tape > Distortion > Reverb, all bypassed by default");
    }
    {
        std::uint64_t clippedOn = 0;
        std::uint64_t clippedOff = 0;
        const Buffer dry = performance(64, false, &clippedOff);
        const Buffer wet = performance(64, true, &clippedOn);
        double diff = 0.0;
        for (std::size_t i = 0; i < dry.size(); ++i) {
            diff = std::max(diff, std::abs(dry[i].left - wet[i].left));
        }
        check(finite(wet) && diff > 0.05 && clippedOn == 0 && clippedOff == 0 &&
              peak(wet) < 1.0,
              "PI2 Drums + mono + FM + poly through all five effects: finite, changed (" +
                  num(diff) + "), no master clipping (peak " + num(peak(wet)) + ", clipped on/off " +
                  num(static_cast<double>(clippedOn)) + "/" +
                  num(static_cast<double>(clippedOff)) + ", dry peak " +
                  num(peak(dry)) + ")");
        bool same = true;
        for (std::size_t size : {1u, 7u, 333u, 4096u}) {
            same = same && identical(performance(size, true, nullptr), wet);
        }
        check(same, "PI3 Full chain: block sizes 1, 7, 64, 333, 4096 bit-identical");
        check(identical(performance(64, true, nullptr), wet),
              "PI4 Full chain: repeated rendering is bit-identical");
    }
    {
        static Pipeline p;
        enableAll(p);
        p.drums().assignSample(3, &drumSample());
        p.drums().postTrigger(3, 127, 37);
        p.postNoteOn(SynthTarget::Fm, 72, 127, 100);
        Buffer out(256);
        p.render(out.data(), out.size());
        check(allZero(out, 0, 37) && !allZero(out, 37, 40),
              "PI5 Event timing intact through the chain: silent before frame 37, sound at 37");
    }
    {
        // Long-term stability: 60 s of performance with everything on.
        static Pipeline p;
        enableAll(p);
        p.reverb().setDecaySeconds(10.0);
        p.drums().assignSample(0, &drumSample());
        Buffer block(256);
        bool ok = true;
        double worst = 0.0;
        for (std::size_t pos = 0, n = 0; pos < 60 * kSecond; pos += 256, ++n) {
            if (n % 47 == 0) {
                p.drums().trigger(0, 100);
                p.poly().noteOn(48 + static_cast<int>(n % 24), 90);
                p.fm().noteOn(60 + static_cast<int>(n % 12), 80);
            }
            if (n % 47 == 30) {
                p.poly().allNotesOff();
                p.fm().allNotesOff();
            }
            std::fill(block.begin(), block.end(), StereoFrame{});
            p.render(block.data(), block.size());
            ok = ok && finite(block);
            worst = std::max(worst, peak(block));
        }
        check(ok && worst <= 1.0,
              "PI6 60 s with all effects (10 s reverb): finite and within full scale");
    }
    {
        // Parameter automation every block must not create extreme jumps.
        static Pipeline p;
        enableAll(p);
        p.poly().noteOn(57, 100);
        p.poly().noteOn(64, 100);
        Buffer out(2 * kSecond);
        for (std::size_t pos = 0, n = 0; pos < out.size(); pos += 128, ++n) {
            const double t = static_cast<double>(n % 20) / 19.0;
            p.eq().setMid(300.0 + 3000.0 * t, -12.0 + 24.0 * t, 1.0);
            p.compressor().setThresholdDb(-40.0 + 30.0 * t);
            p.distortion().setDriveDb(36.0 * t);
            p.softClipper().setDriveDb(12.0 * t);
            p.reverb().setRoomSize(0.25 + 0.75 * t);
            p.reverb().setWetLevel(t);
            p.render(out.data() + pos, 128);
        }
        check(finite(out) && maxStep(out, 1, out.size()) < 0.25,
              "AU1 Per-block automation of every effect: largest step " +
                  num(maxStep(out, 1, out.size())) + " (< 0.25)");
    }
    {
        static Pipeline p;
        enableAll(p);
        p.drums().assignSample(0, &drumSample());
        StereoFrame block[128]{};
        const std::size_t before = g_allocations.load();
        for (int i = 0; i < 2000; ++i) {
            p.postNoteOn(SynthTarget::Poly, 50 + i % 20, 90, static_cast<std::size_t>(i) % 64);
            p.postNoteOff(SynthTarget::Poly, 47 + i % 20, 70);
            p.drums().postTrigger(0, 100, 5);
            p.eq().setMid(500.0 + i % 7 * 100.0, (i % 5) - 2.0);
            p.compressor().setRatio(1.0 + i % 6);
            p.distortion().setMode(static_cast<DistortionMode>(i % 3));
            p.reverb().setRoomSize(0.3 + 0.1 * (i % 7));
            if (i % 50 == 0) p.reverb().setBypassed(i % 100 == 0);
            std::fill(block, block + 128, StereoFrame{});
            p.render(block, 1 + static_cast<std::size_t>(i) % 128);
        }
        const std::size_t after = g_allocations.load();
        const bool noAllocation = after == before;
        check(noAllocation,
              "RT1 No heap allocation: effects processing, parameter changes, bypass, events");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 023 - Effects Engine\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    equalizer();
    compressor();
    clipperAndDistortion();
    reverb();
    common();
    pipeline();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 023 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 023 PASSED\n";
    return EXIT_SUCCESS;
}
