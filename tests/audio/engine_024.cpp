
// DATABREAKs Engine 024 - tape integration tests.
//
// Host-side assertion tests for core/audio/effects/tape_effect.h (the
// TapeMachine adapter) and tape inside the AudioPipeline effects chain.
// Exits non-zero if any check fails. Not FM-1 firmware.

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
constexpr std::size_t kSecond = static_cast<std::size_t>(SAMPLE_RATE);
constexpr std::size_t kLatency = TapeEffect::LatencyFrames;

using Buffer = std::vector<StereoFrame>;

Buffer sine(double frequency, double amplitude, std::size_t frames,
            double rightScale = 1.0) {
    Buffer b(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double v = amplitude * std::sin(2.0 * PI * frequency *
                                              static_cast<double>(i) / SAMPLE_RATE);
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

double maxDiff(const Buffer& a, const Buffer& b) {
    double d = 0.0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        d = std::max(d, std::abs(a[i].left - b[i].left));
        d = std::max(d, std::abs(a[i].right - b[i].right));
    }
    return d;
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

double binMagnitude(const Buffer& b, double frequency, std::size_t from,
                    std::size_t to) {
    std::complex<double> acc = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double phase = -2.0 * PI * frequency * static_cast<double>(i) / SAMPLE_RATE;
        acc += b[i].left * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::abs(acc) * 2.0 / static_cast<double>(to - from);
}

Buffer run(AudioEffect& effect, Buffer b, std::size_t block = 0) {
    if (block == 0) {
        effect.process(b.data(), b.size());
    } else {
        for (std::size_t pos = 0; pos < b.size(); pos += block) {
            effect.process(b.data() + pos, std::min(block, b.size() - pos));
        }
    }
    return b;
}

// A tape effect at the given settings, reset so the settings are in place.
struct TapeSetup {
    TapeSpeed speed = TapeSpeed::IPS_15;
    TapeType type = TapeType::Ferric;
    double wow = 0, flutter = 0, hiss = 0, saturation = 0, headBump = 0;
    double bias = 0.5, compression = 0, crosstalk = 0, printThrough = 0, age = 0;
};

void configure(TapeEffect& t, const TapeSetup& s) {
    t.setSpeed(s.speed);
    t.setTapeType(s.type);
    t.setWow(s.wow);
    t.setFlutter(s.flutter);
    t.setHiss(s.hiss);
    t.setSaturation(s.saturation);
    t.setHeadBump(s.headBump);
    t.setBias(s.bias);
    t.setCompression(s.compression);
    t.setCrosstalk(s.crosstalk);
    t.setPrintThrough(s.printThrough);
    t.setAge(s.age);
    t.reset();
}

void configure(TapeMachine& m, const TapeSetup& s) {
    m.setSpeed(s.speed);
    m.setTapeType(s.type);
    m.setWow(s.wow);
    m.setFlutter(s.flutter);
    m.setHiss(s.hiss);
    m.setSaturation(s.saturation);
    m.setHeadBump(s.headBump);
    m.setBias(s.bias);
    m.setCompression(s.compression);
    m.setCrosstalk(s.crosstalk);
    m.setPrintThrough(s.printThrough);
    m.setAge(s.age);
}

Buffer through(const TapeSetup& s, const Buffer& in) {
    static TapeEffect t;  // reused; fully reset by configure()
    configure(t, s);
    return run(t, in);
}

// ---------------------------------------------------------------------------
void adapter() {
    std::cout << "\n-- Adapter --\n";

    const TapeSetup busy{TapeSpeed::IPS_30, TapeType::Metal, 0.6, 0.4, 0.3, 0.7,
                         0.5, 0.3, 0.6, 0.2, 0.4, 0.3};
    const Buffer music = [] {
        Buffer b = sine(220.0, 0.4, 2 * kSecond, -0.6);
        const Buffer hi = sine(3100.0, 0.15, 2 * kSecond, 0.9);
        for (std::size_t i = 0; i < b.size(); ++i) {
            b[i].left += hi[i].left;
            b[i].right += hi[i].right;
        }
        return b;
    }();

    {
        TapeMachine machine;
        configure(machine, busy);
        Buffer direct = music;
        for (auto& f : direct) f = machine.process(f);
        check(identical(through(busy, music), direct),
              "A1 Wet output is exactly TapeMachine::process (all controls active)");
    }
    {
        const Buffer reference = through(busy, music);
        bool same = true;
        for (std::size_t block : {1u, 7u, 64u, 333u, 4096u}) {
            static TapeEffect t;
            configure(t, busy);
            same = same && identical(run(t, music, block), reference);
        }
        check(same, "A2 Block sizes 1/7/64/333/4096 bit-identical (state carries across blocks)");
    }
    {
        static TapeEffect t;
        configure(t, TapeSetup{});
        t.setMix(0.0);
        t.reset();
        const Buffer out = run(t, music);
        bool delayed = allZero(out, 0, kLatency);
        for (std::size_t i = kLatency; i < out.size(); ++i) {
            delayed = delayed && out[i].left == music[i - kLatency].left &&
                      out[i].right == music[i - kLatency].right;
        }
        Buffer impulse(2000);
        impulse[0] = {0.5, 0.5};
        const Buffer wet = through(TapeSetup{}, impulse);
        std::size_t first = 0;
        while (first < wet.size() && std::abs(wet[first].left) < 1e-6) ++first;
        check(delayed, "A3 Mix 0 = input delayed by exactly 576 frames (latency-aligned dry)");
        check(first >= kLatency - 1 && first <= kLatency + 1,
              "A4 Tape wet signal arrives at frame " + std::to_string(first) +
                  " (TapeMachine's 12 ms delay = 576)");
    }
    {
        static TapeEffect half;
        configure(half, busy);
        half.setMix(0.5);
        half.reset();
        const Buffer h = run(half, music);
        const Buffer w = through(busy, music);
        bool mid = true;
        for (std::size_t i = kLatency; i < music.size(); ++i) {
            const double aligned = music[i - kLatency].left;
            mid = mid && std::abs(h[i].left - (0.5 * aligned + 0.5 * w[i].left)) < 1e-12;
        }
        check(mid, "A5 Mix 0.5 = exact midpoint of aligned dry and tape");
    }
    {
        static TapeEffect t;
        configure(t, busy);
        Buffer out = music;
        t.process(out.data(), kSecond);
        t.setBypassed(true);
        t.process(out.data() + kSecond, kSecond);
        const std::size_t fadeEnd = kSecond + SmoothedValue::DefaultRampFrames;
        bool exact = true;
        for (std::size_t i = fadeEnd; i < out.size(); ++i) {
            exact = exact && out[i].left == music[i].left && out[i].right == music[i].right;
        }
        const double fade = maxStep(out, kSecond - 2, fadeEnd + 2);
        const double around = std::max(maxStep(out, kSecond / 2, kSecond - 2),
                                       maxStep(music, 0, music.size()));
        check(exact && t.fullyBypassed(),
              "A6 Fully bypassed tape is bit-exact and undelayed");
        check(fade <= around * 1.1,
              "A7 Bypass crossfade has no jump (step " + num(fade) + " vs " + num(around) + ")");
    }
    {
        // 50 bypass/enable cycles: finite, and after each re-enable the
        // output equals a freshly reset effect (no leaked state).
        static TapeEffect cycling;
        static TapeEffect fresh;
        configure(cycling, busy);
        bool ok = true;
        bool clean = true;
        for (int c = 0; c < 50; ++c) {
            Buffer a = sine(150.0 + c, 0.5, 2400);
            cycling.process(a.data(), a.size());
            cycling.setBypassed(true);
            Buffer b(1200);
            cycling.process(b.data(), b.size());
            cycling.setBypassed(false);
            ok = ok && finite(a) && finite(b) && cycling.fullyBypassed() == false;
            if (c == 49) {
                configure(fresh, busy);
                fresh.setBypassed(true, true);
                fresh.setBypassed(false);
                Buffer x = sine(500.0, 0.4, 4800);
                Buffer y = x;
                cycling.process(x.data(), x.size());
                fresh.process(y.data(), y.size());
                clean = identical(x, y);
            }
        }
        check(ok && clean, "A8 50 bypass/enable cycles: finite, re-enable starts clean");
    }
    {
        static TapeEffect used;
        configure(used, busy);
        run(used, music);
        used.reset();
        check(identical(run(used, music), through(busy, music)),
              "A9 reset() makes a used tape (hiss included) equal a fresh one");
    }
    {
        TapeEffect t;
        t.setWow(kNaN);
        t.setHiss(std::numeric_limits<double>::infinity());
        t.setSaturation(5.0);
        t.setBias(-2.0);
        t.setAge(0.25);
        t.reset();
        Buffer out = run(t, music);
        check(t.wow() == 0.0 && t.hiss() == 0.0 && t.saturation() == 1.0 &&
              t.bias() == 0.0 && t.age() == 0.25 && finite(out),
              "A10 Non-finite controls ignored, others clamped to 0..1");
    }
    {
        // Wow jump 0 -> 1 mid-tone: adapter glides; the raw machine jumps.
        const Buffer tone = sine(300.0, 0.5, kSecond);
        static TapeEffect t;
        configure(t, TapeSetup{});
        Buffer a = tone;
        t.process(a.data(), kSecond / 4);
        t.setWow(1.0);
        t.setFlutter(1.0);
        t.process(a.data() + kSecond / 4, kSecond * 3 / 4);
        TapeMachine m;
        Buffer b = tone;
        for (std::size_t i = 0; i < kSecond; ++i) {
            if (i == kSecond / 4) {
                m.setWow(1.0);
                m.setFlutter(1.0);
            }
            b[i] = m.process(b[i]);
        }
        const double glide = maxStep(a, kSecond / 4 - 2, kSecond / 4 + 2000);
        const double jump = maxStep(b, kSecond / 4 - 2, kSecond / 4 + 2);
        const double settled = maxStep(a, kSecond / 2, kSecond);
        check(glide <= settled * 1.1 && jump > glide * 2.0,
              "A11 Wow/flutter changes glide: step " + num(glide) +
                  " (raw TapeMachine jump " + num(jump) + ", settled " + num(settled) + ")");
        // Speed + type switch with high saturation: the adapter ducks
        // through the aligned dry signal (5 ms + 5 ms); the raw machine steps
        // (the drive changes; measured up to ~0.2 on a 300 Hz tone). A slow
        // 2 Hz input (per-sample change ~1e-4) makes any step stand out.
        const Buffer slow = sine(2.0, 0.5, kSecond);
        const TapeSetup before{TapeSpeed::IPS_15, TapeType::Ferric, 0, 0, 0, 0.8};
        const TapeSetup after{TapeSpeed::IPS_7_5, TapeType::Metal, 0, 0, 0, 0.8};
        static TapeEffect s;
        configure(s, before);
        Buffer c = slow;
        s.process(c.data(), kSecond / 2);
        s.setSpeed(after.speed);
        s.setTapeType(after.type);
        const bool ducking = s.switching();
        s.process(c.data() + kSecond / 2, kSecond / 2);
        TapeMachine raw;
        configure(raw, before);
        Buffer r = slow;
        for (std::size_t i = 0; i < kSecond; ++i) {
            if (i == kSecond / 2) {
                raw.setSpeed(after.speed);
                raw.setTapeType(after.type);
            }
            r[i] = raw.process(r[i]);
        }
        const double duckStep = maxStep(c, kSecond / 2 - 2, kSecond / 2 + 1000);
        const double rawStep = maxStep(r, kSecond / 2 - 2, kSecond / 2 + 2);
        // After the duck the adapter runs the new settings exactly.
        const double settledDiff = maxDiff(
            Buffer(c.begin() + kSecond * 3 / 4, c.end()),
            Buffer(r.begin() + kSecond * 3 / 4, r.end()));
        check(ducking && !s.switching() && duckStep < 0.005 &&
              rawStep > 0.02 && settledDiff == 0.0,
              "A12 Speed + type switch ducks without a step: " + num(duckStep) +
                  " (raw TapeMachine " + num(rawStep) + "), then matches the raw machine exactly");
    }
}

// ---------------------------------------------------------------------------
void character() {
    std::cout << "\n-- Tape character --\n";

    const Buffer high = sine(10000.0, 0.2, kSecond);
    const Buffer low = sine(80.0, 0.2, kSecond);
    const Buffer mid = sine(200.0, 0.5, kSecond);
    const Buffer silence(kSecond);
    const std::size_t from = kSecond / 2;

    auto hf = [&](const TapeSetup& s) { return rms(through(s, high), from, kSecond); };
    auto noise = [&](const TapeSetup& s) { return rms(through(s, silence), from, kSecond); };
    auto h3 = [&](const TapeSetup& s) {
        const Buffer o = through(s, mid);
        return binMagnitude(o, 600.0, from, kSecond) / binMagnitude(o, 200.0, from, kSecond);
    };

    {
        TapeSetup s;
        s.speed = TapeSpeed::IPS_7_5;
        const double slow = hf(s);
        s.speed = TapeSpeed::IPS_15;
        const double normal = hf(s);
        s.speed = TapeSpeed::IPS_30;
        const double fast = hf(s);
        check(slow < normal && normal < fast,
              "C1 Speeds: 10 kHz level 7.5 < 15 < 30 IPS (" + num(slow) + ", " +
                  num(normal) + ", " + num(fast) + ")");
        s.hiss = 1.0;
        const double hissFast = noise(s);
        s.speed = TapeSpeed::IPS_7_5;
        check(noise(s) > hissFast, "C2 Speeds: slower tape is noisier");
    }
    {
        TapeSetup s;
        double h[3], n[3], d[3];
        const TapeType types[3] = {TapeType::Ferric, TapeType::Chrome, TapeType::Metal};
        for (int i = 0; i < 3; ++i) {
            s = TapeSetup{};
            s.type = types[i];
            h[i] = hf(s);
            s.hiss = 1.0;
            n[i] = noise(s);
            s.hiss = 0.0;
            s.saturation = 1.0;
            d[i] = h3(s);
        }
        check(h[0] < h[1] && h[1] < h[2],
              "C3 Types: high end ferric < chrome < metal");
        check(n[0] > n[1] && n[1] > n[2],
              "C4 Types: hiss ferric > chrome > metal");
        check(d[0] > d[1] && d[1] > d[2],
              "C5 Types: saturation (H3/H1) ferric " + num(d[0]) + " > chrome " +
                  num(d[1]) + " > metal " + num(d[2]));
    }
    {
        const Buffer none = through(TapeSetup{}, mid);
        TapeSetup w;
        w.wow = 1.0;
        const Buffer wow = through(w, mid);
        TapeSetup f;
        f.flutter = 1.0;
        const Buffer flutter = through(f, mid);
        TapeSetup b;
        b.wow = 1.0;
        b.flutter = 1.0;
        const Buffer both = through(b, mid);
        check(maxDiff(none, wow) > 0.05 && maxDiff(none, flutter) > 0.001 &&
              maxDiff(both, wow) > 0.001 && maxDiff(both, flutter) > 0.05 &&
              finite(both) && peak(both) <= 1.0,
              "C6 Wow and flutter each modulate, alone and together (diffs " +
                  num(maxDiff(none, wow)) + ", " + num(maxDiff(none, flutter)) + ")");
    }
    {
        TapeSetup s;
        const double clean = h3(s);
        s.saturation = 0.5;
        const double some = h3(s);
        s.saturation = 1.0;
        const double full = h3(s);
        check(clean < some && some < full,
              "C7 Saturation raises H3/H1: " + num(clean) + " < " + num(some) + " < " + num(full));
    }
    {
        TapeSetup s;
        const bool silent = allZero(through(s, silence));
        s.hiss = 0.5;
        const double half = noise(s);
        s.hiss = 1.0;
        const double full = noise(s);
        Buffer hissy = through(s, silence);
        double mean = 0.0;
        for (const auto& f : hissy) mean += f.left;
        mean /= static_cast<double>(hissy.size());
        check(silent && half > 0.0 && full > half && std::abs(mean) < full * 0.05,
              "C8 Hiss: 0 is exact silence, grows with the control, no DC (mean " +
                  num(mean) + ")");
    }
    {
        TapeSetup s;
        s.bias = 0.0;
        const double hfLow = hf(s);
        s.saturation = 1.0;
        const double dLow = h3(s);
        s = TapeSetup{};
        s.bias = 1.0;
        const double hfHigh = hf(s);
        s.saturation = 1.0;
        const double dHigh = h3(s);
        check(hfHigh > hfLow && dHigh < dLow,
              "C9 Bias: more bias = more high end, less saturation");
    }
    {
        const Buffer loud = sine(200.0, 0.9, kSecond);
        TapeSetup s;
        const double open = rms(through(s, loud), from, kSecond);
        s.compression = 1.0;
        const double squeezed = rms(through(s, loud), from, kSecond);
        check(squeezed < open * 0.95,
              "C10 Compression lowers a loud tone: " + num(open) + " -> " + num(squeezed));
    }
    {
        TapeSetup s;
        const double flat = rms(through(s, low), from, kSecond);
        s.headBump = 1.0;
        const double bumped = rms(through(s, low), from, kSecond);
        check(bumped > flat * 1.05,
              "C11 Head bump lifts 80 Hz: " + num(flat) + " -> " + num(bumped));
    }
    {
        const Buffer leftOnly = sine(300.0, 0.5, kSecond, 0.0);
        TapeSetup s;
        const Buffer isolated = through(s, leftOnly);
        s.crosstalk = 1.0;
        const Buffer bled = through(s, leftOnly);
        const double ratio = rms(bled, from, kSecond, false) / rms(bled, from, kSecond, true);
        check(rms(isolated, 0, kSecond, false) == 0.0 && std::abs(ratio - 0.25) < 0.01,
              "C12 Stereo: no crosstalk keeps channels separate; crosstalk 1 bleeds " +
                  num(ratio) + " (0.2/0.8)");
    }
    {
        Buffer burst(kSecond);
        for (std::size_t i = 0; i < 2400; ++i) {
            const double v = 0.5 * std::sin(2.0 * PI * 500.0 * static_cast<double>(i) / SAMPLE_RATE);
            burst[i] = {v, v};
        }
        const std::size_t echo = kLatency + 8640;  // 0.18 s print-through
        TapeSetup s;
        const double without = rms(through(s, burst), echo, echo + 2400);
        s.printThrough = 1.0;
        const double with = rms(through(s, burst), echo, echo + 2400);
        check(without < 1e-12 && with > 1e-3,
              "C13 Print-through adds a quiet copy 0.18 s later (" + num(with) + ")");
    }
    {
        TapeSetup s;
        const double hfNew = hf(s);
        s.hiss = 0.5;
        const double nNew = noise(s);
        s = TapeSetup{};
        s.age = 1.0;
        const double hfOld = hf(s);
        s.hiss = 0.5;
        const double nOld = noise(s);
        check(hfOld < hfNew && nOld > nNew,
              "C14 Aging: duller and noisier");
    }
    {
        TapeSetup extreme{TapeSpeed::IPS_7_5, TapeType::Ferric, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        const Buffer out = through(extreme, sine(60.0, 1.0, 5 * kSecond));
        check(finite(out) && peak(out) <= 1.0,
              "C15 Every control at maximum on a full-scale tone: finite, never above 1.0");
    }
    {
        // Long-running streaming: 5 minutes with modulation and print-through.
        static TapeEffect t;
        configure(t, TapeSetup{TapeSpeed::IPS_15, TapeType::Chrome, 0.7, 0.5, 0.4, 0.6,
                               0.4, 0.5, 0.5, 0.2, 0.5, 0.4});
        Buffer block(512);
        bool ok = true;
        double worst = 0.0;
        double meanL = 0.0;
        std::size_t frames = 0;
        for (std::size_t pos = 0; pos < 300 * kSecond; pos += 512) {
            for (std::size_t i = 0; i < 512; ++i) {
                const double tt = static_cast<double>(pos + i) / SAMPLE_RATE;
                const double v = 0.4 * std::sin(2.0 * PI * 110.0 * tt);
                block[i] = {v, -v};
            }
            t.process(block.data(), block.size());
            for (const auto& f : block) {
                ok = ok && std::isfinite(f.left) && std::isfinite(f.right);
                worst = std::max(worst, std::max(std::abs(f.left), std::abs(f.right)));
                meanL += f.left;
                ++frames;
            }
        }
        meanL /= static_cast<double>(frames);
        check(ok && worst <= 1.0 && std::abs(meanL) < 1e-3,
              "C16 5 minutes streaming: finite, <= 1.0, no DC (mean " + num(meanL) + ")");
    }
}

// ---------------------------------------------------------------------------
using Pipeline = AudioPipeline<16>;

const Buffer& kick() {
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

void enableChain(Pipeline& p, bool on) {
    for (std::size_t i = 0; i < p.effects().size(); ++i) {
        p.effects().slot(i)->setBypassed(!on, true);
    }
}

Buffer band(std::size_t blockSize, bool tapeOn) {
    static Pipeline p;
    p.drums().stopAll();
    p.mono().reset();
    p.fm().reset();
    p.poly().reset();
    p.effects().resetAll();
    p.resetDiagnostics();
    p.drums().assignSample(0, &kick());
    p.setDrumLevel(0.5);
    p.setMonoLevel(0.5);
    p.setPolyLevel(0.5);
    p.setFmLevel(0.4);
    p.setMasterLevel(0.8);
    p.tape().setTapeType(TapeType::Chrome);
    p.tape().setWow(0.3);
    p.tape().setFlutter(0.3);
    p.tape().setHiss(0.2);
    p.tape().setSaturation(0.4);
    p.tape().setHeadBump(0.4);
    p.tape().setPrintThrough(0.2);
    enableChain(p, false);
    if (tapeOn) {
        p.tape().setBypassed(false, true);
        p.compressor().setBypassed(false, true);
        p.reverb().setBypassed(false, true);
    }
    const std::size_t total = 3 * kSecond;
    Buffer out(total);
    for (std::size_t pos = 0; pos < total; pos += blockSize) {
        const std::size_t n = std::min(blockSize, total - pos);
        for (std::size_t f = pos; f < pos + n; ++f) {
            const std::size_t o = f - pos;
            if (f % 12000 == 0) {
                const int root = 45 + static_cast<int>((f / 12000) % 4) * 2;
                p.drums().postTrigger(0, 110, o);
                p.postNoteOn(SynthTarget::Mono, root - 12, 100, o);
                p.postNoteOn(SynthTarget::Poly, root + 12, 80, o);
                p.postNoteOn(SynthTarget::Poly, root + 16, 80, o);
                p.postNoteOn(SynthTarget::Fm, root + 24, 90, o);
            }
            if (f % 12000 == 9000) {
                p.postAllNotesOff(SynthTarget::Mono, o);
                p.postAllNotesOff(SynthTarget::Poly, o);
                p.postAllNotesOff(SynthTarget::Fm, o);
            }
        }
        p.render(out.data() + pos, n);
    }
    Buffer tail(4 * kSecond);
    p.render(tail.data(), tail.size());
    return out;
}

void pipeline() {
    std::cout << "\n-- Pipeline --\n";
    {
        static Pipeline p;
        const char* expected[] = {"Equalizer", "Compressor", "SoftClipper",
                                  "Tape", "Distortion", "Reverb"};
        bool order = p.effects().size() == 6;
        for (std::size_t i = 0; i < 6 && order; ++i) {
            order = std::string(p.effects().slot(i)->name()) == expected[i] &&
                    p.effects().slot(i)->fullyBypassed();
        }
        check(order && p.effects().slot(3) == &p.tape() &&
              p.effects().slot(3)->latencyFrames() == kLatency,
              "P1 Chain EQ > Comp > Clip > Tape > Dist > Reverb; tape bypassed by default");
    }
    {
        // Tape alone in the chain equals TapeEffect on the summed bus.
        static Pipeline p;
        p.drums().assignSample(0, &kick());
        p.tape().setSaturation(0.5);
        p.tape().setHiss(0.3);
        p.tape().setBypassed(false, true);
        p.drums().trigger(0, 120);
        p.poly().noteOn(60, 100);
        Buffer out(kSecond);
        p.render(out.data(), out.size());
        static Pipeline dry;
        dry.drums().assignSample(0, &kick());
        dry.drums().trigger(0, 120);
        dry.poly().noteOn(60, 100);
        Buffer bus(kSecond);
        dry.mixSources(bus.data(), bus.size());
        static TapeEffect t;
        t.setSaturation(0.5);
        t.setHiss(0.3);
        t.reset();
        t.process(bus.data(), bus.size());
        dry.applyMaster(bus.data(), bus.size());
        check(identical(out, bus),
              "P2 Tape enabled alone = TapeEffect over the summed bus, master once");
    }
    {
        const Buffer reference = band(64, true);
        bool same = true;
        for (std::size_t size : {1u, 7u, 333u, 4096u}) {
            same = same && identical(band(size, true), reference);
        }
        const Buffer without = band(64, false);
        check(finite(reference) && peak(reference) < 1.0 &&
              maxDiff(reference, without) > 0.05,
              "P3 Drums + mono + FM + poly through tape: finite, peak " +
                  num(peak(reference)) + ", changed by tape");
        check(same, "P4 Through tape: block sizes 1/7/64/333/4096 bit-identical");
        check(identical(band(64, true), reference), "P5 Through tape: repeat render identical");
    }
    {
        static Pipeline p;
        p.tape().setBypassed(false, true);
        p.drums().assignSample(3, &kick());
        p.drums().postTrigger(3, 127, 37);
        Buffer out(1024);
        p.render(out.data(), out.size());
        std::size_t first = 0;
        while (first < out.size() && out[first].left == 0.0) ++first;
        // kick() starts at sin(0) = 0, so its first audible frame is one later.
        check(first >= 37 + kLatency - 1 && first <= 37 + kLatency + 2,
              "P6 Timing: drum at frame 37 leaves tape at frame " +
                  std::to_string(first) + " (37 + 576 tape latency)");
    }
    {
        static Pipeline p;
        p.drums().assignSample(0, &kick());
        enableChain(p, true);
        StereoFrame block[128]{};
        const std::size_t before = g_allocations.load();
        for (int i = 0; i < 2000; ++i) {
            p.postNoteOn(SynthTarget::Poly, 50 + i % 20, 90, static_cast<std::size_t>(i) % 64);
            p.postNoteOff(SynthTarget::Poly, 47 + i % 20, 70);
            p.drums().postTrigger(0, 100, 5);
            p.tape().setWow((i % 10) / 10.0);
            p.tape().setSaturation((i % 7) / 7.0);
            p.tape().setSpeed(static_cast<TapeSpeed>(i % 3));
            p.tape().setTapeType(static_cast<TapeType>((i / 3) % 3));
            if (i % 40 == 0) p.tape().setBypassed(i % 80 == 0);
            if (i % 333 == 0) p.tape().reset();
            std::fill(block, block + 128, StereoFrame{});
            p.render(block, 1 + static_cast<std::size_t>(i) % 128);
        }
        const std::size_t after = g_allocations.load();
        const bool noAllocation = after == before;
        check(noAllocation,
              "P7 No heap allocation: tape processing, control changes, bypass cycles, reset");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 024 - Tape Integration\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    adapter();
    character();
    pipeline();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 024 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 024 PASSED\n";
    return EXIT_SUCCESS;
}
