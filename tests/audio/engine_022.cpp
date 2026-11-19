
// DATABREAKs Engine 022 - synthesizer integration tests.
//
// Host-side assertion tests for the stateful synth voices/engines
// (instruments/synthesizers/synth_voices.h, synth_engine.h) and their
// integration into AudioPipeline. Exits non-zero if any check fails.
// Not FM-1 firmware.

#include "audio_pipeline.h"
#include "../../instruments/synthesizers/fm_synth.h"
#include "../../instruments/synthesizers/mono_synth.h"
#include "../../instruments/synthesizers/poly_synth.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <thread>
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

std::string sci(double v) {
    std::ostringstream os;
    os << std::scientific << v;
    return os.str();
}

bool near(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <typename Engine>
std::vector<StereoFrame> render(Engine& engine, std::size_t frames) {
    std::vector<StereoFrame> out(frames);
    engine.render(out.data(), frames);
    return out;
}

bool silent(const std::vector<StereoFrame>& out) {
    for (const auto& f : out) {
        if (f.left != 0.0 || f.right != 0.0) {
            return false;
        }
    }
    return true;
}

bool finite(const std::vector<StereoFrame>& out) {
    for (const auto& f : out) {
        if (!std::isfinite(f.left) || !std::isfinite(f.right)) {
            return false;
        }
    }
    return true;
}

double maxStep(const std::vector<StereoFrame>& out, std::size_t from,
               std::size_t to) {
    double worst = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) {
        worst = std::max(worst, std::abs(out[i].left - out[i - 1].left));
        worst = std::max(worst, std::abs(out[i].right - out[i - 1].right));
    }
    return worst;
}

// ---------------------------------------------------------------------------
// The new stateful voices reproduce the original classes for a note that is
// held past attack + decay and then released (frame-exact gate).
void equivalence() {
    std::cout << "\n-- Equivalence with the original synth classes --\n";

    const std::size_t gateFrames = 40000;           // 0.8333 s
    const std::size_t totalFrames = 52000;          // + release
    const double gate = static_cast<double>(gateFrames) / SAMPLE_RATE;

    // FM
    {
        FMParameters p;
        p.envelope = {0.005, 0.12, 0.35, 0.08};
        FMSynth original;
        original.setParameters(p);
        FmSynthEngine<1> engine;
        engine.setParameters(p);
        engine.noteOn(64, 127);
        auto out = render(engine, gateFrames);
        engine.noteOff(64);
        auto rest = render(engine, totalFrames - gateFrames);
        out.insert(out.end(), rest.begin(), rest.end());
        double worst = 0.0;
        for (std::size_t n = 0; n < totalFrames; ++n) {
            const double ref = original.process(
                midiToHz(64), static_cast<double>(n) / SAMPLE_RATE, gate);
            worst = std::max(worst, std::abs(out[n].left - ref));
            worst = std::max(worst, std::abs(out[n].right - ref));
        }
        check(worst < 1e-9,
              "E1 FmVoice matches FMSynth sample for sample (max diff " +
                  sci(worst) + ")");
    }

    // Mono (one-pole filter state carried the same way)
    {
        MonoParameters p;
        p.envelope = {0.015, 0.15, 0.55, 0.16};
        MonoSynth original;
        original.setParameters(p);
        MonoSynthEngine engine;
        engine.setParameters(p);
        engine.noteOn(36, 127);
        auto out = render(engine, gateFrames);
        engine.noteOff(36);
        auto rest = render(engine, totalFrames - gateFrames);
        out.insert(out.end(), rest.begin(), rest.end());
        double worst = 0.0;
        for (std::size_t n = 0; n < totalFrames; ++n) {
            const double ref = original.process(
                midiToHz(36), static_cast<double>(n) / SAMPLE_RATE, gate);
            worst = std::max(worst, std::abs(out[n].left - ref));
        }
        check(worst < 1e-9,
              "E2 MonoVoice matches MonoSynth sample for sample (max diff " +
                  sci(worst) + ")");
    }

    // Poly: 4 voices, chord in slots 0..3, PolySynth's pan law and detune
    {
        PolyParameters p;
        p.envelope = {0.08, 0.22, 0.65, 0.25};
        PolySynth original;
        original.setParameters(p);
        PolySynthEngine<4> engine;
        engine.setParameters(p);
        const int chord[4] = {60, 64, 67, 71};
        std::array<double, 4> freqs{};
        for (int v = 0; v < 4; ++v) {
            engine.noteOn(chord[v], 127);
            freqs[static_cast<std::size_t>(v)] = midiToHz(chord[v]);
        }
        auto out = render(engine, gateFrames);
        for (int v = 0; v < 4; ++v) {
            engine.noteOff(chord[v]);
        }
        auto rest = render(engine, totalFrames - gateFrames);
        out.insert(out.end(), rest.begin(), rest.end());
        double worst = 0.0;
        for (std::size_t n = 0; n < totalFrames; ++n) {
            const StereoFrame ref = original.process(
                freqs, static_cast<double>(n) / SAMPLE_RATE, gate);
            worst = std::max(worst, std::abs(out[n].left - ref.left));
            worst = std::max(worst, std::abs(out[n].right - ref.right));
        }
        check(worst < 1e-9,
              "E3 PolySynthEngine<4> matches PolySynth (stereo) (max diff " +
                  sci(worst) + ")");
    }
}

// ---------------------------------------------------------------------------
void findings() {
    std::cout << "\n-- Historical findings B1-B4 --\n";

    // B2 + B3: release during attack starts at once from the current level.
    {
        AdsrEnvelope env;
        env.setParameters({0.10, 0.20, 0.5, 0.10});
        env.noteOn();
        double last = 0.0;
        for (int i = 0; i < 2400; ++i) {  // 0.05 s: halfway up the attack
            last = env.next();
        }
        env.noteOff();
        const double first = env.next();
        bool falling = true;
        double prev = first;
        int frames = 1;
        while (env.active() && frames < 100000) {
            const double v = env.next();
            falling = falling && v <= prev;
            prev = v;
            ++frames;
        }
        check(near(last, 2399.0 / 4800.0) && first == last &&
              falling && frames == 4801,
              "B2 Release during attack begins immediately and keeps falling");
        check(std::abs(first - last) <= 1.0 / 4800.0 + 1e-12,
              "B3 Release starts from the current level (no jump to sustain)");
    }

    // Release during decay, and retrigger during release, are continuous.
    {
        AdsrEnvelope env;
        env.setParameters({0.01, 0.20, 0.3, 0.10});
        env.noteOn();
        double prev = 0.0;
        double worst = 0.0;
        for (int i = 0; i < 20000; ++i) {
            if (i == 3000) env.noteOff();   // mid-decay
            if (i == 5000) env.noteOn();    // mid-release
            if (i == 9000) env.noteOff();
            const double v = env.next();
            worst = std::max(worst, std::abs(v - prev));
            prev = v;
        }
        // Steepest legal slope here is the 10 ms attack: 1/480 per frame.
        check(worst <= 1.0 / 480.0 + 1e-12 && !env.active(),
              "B2/B3 Release in decay and retrigger in release never jump");
    }

    // B4: a sustained note rendered in odd-sized blocks equals one block,
    // and its phase stays exact over 10 s.
    {
        PolyParameters p;
        p.detune = 0.0;
        p.stereoWidth = 0.0;
        p.envelope = {0.001, 0.001, 1.0, 0.1};
        PolySynthEngine<1> whole;
        PolySynthEngine<1> pieces;
        whole.setParameters(p);
        pieces.setParameters(p);
        whole.noteOn(69, 127);
        pieces.noteOn(69, 127);
        const std::size_t total = 10 * static_cast<std::size_t>(SAMPLE_RATE);
        std::vector<StereoFrame> a(total);
        std::vector<StereoFrame> b(total);
        whole.render(a.data(), total);
        const std::size_t sizes[] = {1, 7, 64, 333, 4096};
        for (std::size_t pos = 0, k = 0; pos < total; ++k) {
            const std::size_t n = std::min(sizes[k % 5], total - pos);
            pieces.render(b.data() + pos, n);
            pos += n;
        }
        bool identical = true;
        for (std::size_t i = 0; i < total; ++i) {
            identical = identical && a[i].left == b[i].left &&
                        a[i].right == b[i].right;
        }
        // Compare with an ideal 440 Hz sine at the end of 10 s.
        const double amp = p.voiceVolume * p.volume * 0.5;  // centre pan
        double worst = 0.0;
        for (std::size_t n = total - 1000; n < total; ++n) {
            const double ideal =
                amp * std::sin(2.0 * PI * 440.0 *
                               static_cast<double>(n) / SAMPLE_RATE);
            worst = std::max(worst, std::abs(a[n].left - ideal));
        }
        check(identical, "B4a Blocks of 1/7/64/333/4096 match one 10 s block");
        check(worst < 1e-9,
              "B4b Phase is exact after 10 s (max error " +
                  sci(worst) + ")");
    }

    // B1: changing chords with every voice still sounding no longer cuts.
    {
        PolyParameters p;
        p.envelope = {0.08, 0.22, 0.65, 0.25};
        PolySynthEngine<4> engine;
        engine.setParameters(p);
        const int chords[2][4] = {{60, 64, 67, 71}, {57, 60, 64, 67}};
        for (int v = 0; v < 4; ++v) engine.noteOn(chords[0][v], 127);
        auto out = render(engine, 42240);  // 0.88 s
        for (int v = 0; v < 4; ++v) engine.noteOff(chords[0][v]);
        auto rest = render(engine, 5760);  // to 1.0 s: still releasing
        out.insert(out.end(), rest.begin(), rest.end());
        const bool stillSounding = engine.activeVoiceCount() == 4;
        for (int v = 0; v < 4; ++v) engine.noteOn(chords[1][v], 127);
        rest = render(engine, 4800);
        out.insert(out.end(), rest.begin(), rest.end());
        const double atChange = maxStep(out, 48000 - 4, 48000 + 4);
        const double steady = maxStep(out, 20000, 40000);
        check(stillSounding && atChange <= steady + 1e-12,
              "B1 Chord change while releasing: step at boundary " +
                  sci(atChange) + " <= steady-state step " +
                  sci(steady));
    }
}

// ---------------------------------------------------------------------------
void synths() {
    std::cout << "\n-- Synth engines --\n";

    // FM / mono render, centred, bounded
    {
        FmSynthEngine<> fm;
        fm.noteOn(72, 127);
        const auto out = render(fm, 4800);
        bool centred = true;
        double peak = 0.0;
        for (const auto& f : out) {
            centred = centred && f.left == f.right;
            peak = std::max(peak, std::abs(f.left));
        }
        check(centred && peak > 0.1 && peak <= fm.parameters().volume + 1e-12,
              "F1 FM renders, centred, peak within its volume");
    }
    {
        MonoSynthEngine mono;
        mono.noteOn(40, 127);
        const auto out = render(mono, 4800);
        bool centred = true;
        double peak = 0.0;
        for (const auto& f : out) {
            centred = centred && f.left == f.right;
            peak = std::max(peak, std::abs(f.left));
        }
        check(centred && peak > 0.05 && finite(out),
              "M1 Mono renders, centred and finite");
    }

    // Poly stereo: slot 0 hard left, last slot hard right (width 1)
    {
        PolySynthEngine<4> poly;
        poly.noteOn(60, 127);  // slot 0
        auto out = render(poly, 2000);
        bool left = true;
        for (const auto& f : out) left = left && f.right == 0.0;
        poly.reset();
        poly.noteOn(60, 127);
        poly.noteOn(62, 127);
        poly.noteOn(64, 127);
        poly.noteOn(65, 127);  // slot 3
        poly.noteOff(60);
        poly.noteOff(62);
        poly.noteOff(64);
        poly.reset();
        check(left && poly.activeVoiceCount() == 0,
              "P1 Poly slot 0 is hard left with stereo width 1");
        PolySynthEngine<4> four;
        for (int n : {60, 62, 64, 65}) four.noteOn(n, 127);
        out = render(four, 2000);
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (const auto& f : out) {
            leftEnergy += f.left * f.left;
            rightEnergy += f.right * f.right;
        }
        check(four.activeVoiceCount() == 4 && leftEnergy > 0.0 &&
              rightEnergy > 0.0,
              "P2 Four-note chord uses four voices across both channels");
    }

    // Note on / off and silence afterwards
    {
        FmSynthEngine<> fm;
        FMParameters p;
        p.envelope = {0.005, 0.05, 0.5, 0.02};
        fm.setParameters(p);
        fm.noteOn(60, 100);
        render(fm, 1000);
        const bool held = fm.isNoteHeld(60) && fm.activeVoiceCount() == 1;
        fm.noteOff(60);
        const bool released = !fm.isNoteHeld(60) && fm.activeVoiceCount() == 1;
        // 20 ms = 960 frames; as in calculateEnvelope, the frame 960 after
        // the gate is the first silent one, after which the voice is idle.
        render(fm, 961);
        check(held && released && fm.activeVoiceCount() == 0 &&
              silent(render(fm, 512)),
              "N1 Note-on holds, note-off releases, then exact silence");
    }

    // Velocity
    {
        FmSynthEngine<> a;
        FmSynthEngine<> b;
        a.noteOn(60, 127);
        b.noteOn(60, 64);
        const auto loud = render(a, 2000);
        const auto soft = render(b, 2000);
        bool scaled = true;
        for (std::size_t i = 0; i < loud.size(); ++i) {
            scaled = scaled && near(soft[i].left, loud[i].left * 64.0 / 127.0);
        }
        check(scaled, "V1 Velocity 64 gives exactly 64/127 of velocity 127");
    }

    // Mono last-note priority
    {
        MonoSynthEngine mono;
        mono.noteOn(48, 100);
        mono.noteOn(55, 100);
        const bool second = mono.voiceNote(0) == 55;
        mono.noteOff(55);
        const bool back = mono.voiceNote(0) == 48 && mono.isNoteHeld(48);
        mono.noteOff(48);
        check(second && back && !mono.isNoteHeld(48) &&
              mono.voice(0).core.envelope.releasing(),
              "M2 Mono last-note priority returns to the held note, then releases");
    }

    // Poly stealing order and same-note retrigger
    {
        PolySynthEngine<2> poly;
        poly.noteOn(60, 100);       // slot 0 (oldest)
        poly.noteOn(64, 100);       // slot 1
        poly.noteOff(64);           // slot 1 releasing, but newer than slot 0
        poly.noteOn(67, 100);       // releasing-first: slot 1, not oldest slot 0
        const bool stoleReleasing = poly.voiceNote(1) == 67 &&
                                    poly.voiceNote(0) == 60;
        poly.noteOn(71, 100);       // none releasing: oldest held (60, slot 0)
        const bool stoleOldest = poly.voiceNote(0) == 71;
        poly.noteOn(71, 100);       // same note retriggers its own voice
        check(stoleReleasing && stoleOldest && poly.voiceNote(1) == 67 &&
              poly.voiceNote(0) == 71 && poly.activeVoiceCount() == 2,
              "P3 Voice allocation: releasing voice, then oldest, same-note retrigger");
    }

    // Pitch bend
    {
        FmSynthEngine<1> bent;
        FmSynthEngine<1> octave;
        bent.setPitchBend(12.0);
        bent.noteOn(60, 127);
        octave.noteOn(72, 127);
        const auto a = render(bent, 4000);
        const auto b = render(octave, 4000);
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, std::abs(a[i].left - b[i].left));
        }
        FmSynthEngine<1> midi;
        midi.setPitchBendMidi(16383, 2.0);
        const bool up = near(midi.pitchBend(), 2.0);
        midi.setPitchBendMidi(8192, 2.0);
        const bool centre = near(midi.pitchBend(), 0.0);
        midi.setPitchBendMidi(0, 2.0);
        const bool down = near(midi.pitchBend(), -2.0);
        midi.setPitchBend(kNaN);
        midi.setPitchBend(99.0);
        check(worst < 1e-9 && up && centre && down &&
              near(midi.pitchBend(), 24.0),
              "B Pitch bend: +12 = one octave; MIDI 0/8192/16383 = -2/0/+2; clamps");
    }

    // Invalid parameters and inputs
    {
        FmSynthEngine<> fm;
        FMParameters bad;
        bad.modulationDepth = kNaN;
        bad.volume = std::numeric_limits<double>::infinity();
        bad.envelope = {kNaN, -1.0, 7.0, 0.0};
        fm.setParameters(bad);
        const bool rejected = !fm.noteOn(128, 100) && !fm.noteOn(-1, 100) &&
                              !fm.noteOn(60, 0);
        fm.noteOn(60, 1000);  // clamped to 127
        const auto out = render(fm, 4800);
        double peak = 0.0;
        for (const auto& f : out) peak = std::max(peak, std::abs(f.left));
        fm.setLevel(kNaN);
        fm.render(nullptr, 64);
        check(rejected && finite(out) && peak > 0.0 &&
              peak <= fm.parameters().volume + 1e-12 &&
              near(fm.parameters().envelope.sustain, 1.0) &&
              near(fm.level(), 1.0),
              "I1 NaN/inf/out-of-range parameters are sanitised; bad notes rejected");
    }
}

// ---------------------------------------------------------------------------
using Pipeline = AudioPipeline<16>;

void pipeline() {
    std::cout << "\n-- AudioPipeline integration --\n";

    const auto drumSample = std::vector<StereoFrame>(48000, StereoFrame{0.25, 0.125});

    // Drums + synth together: exactly the sum of the separate renders.
    {
        static Pipeline both;
        static Pipeline drumsOnly;
        static Pipeline synthOnly;
        both.drums().assignSample(0, &drumSample);
        drumsOnly.drums().assignSample(0, &drumSample);
        both.drums().trigger(0, 127);
        drumsOnly.drums().trigger(0, 127);
        both.poly().noteOn(60, 110);
        synthOnly.poly().noteOn(60, 110);
        const auto a = render(both, 3000);
        const auto d = render(drumsOnly, 3000);
        const auto s = render(synthOnly, 3000);
        bool sum = true;
        for (std::size_t i = 0; i < a.size(); ++i) {
            sum = sum && a[i].left == d[i].left + s[i].left &&
                  a[i].right == d[i].right + s[i].right;
        }
        check(sum, "G1 Drums + poly together equal the sum of separate renders");
    }

    // Gain staging: synth level x master, each once; bus audio untouched.
    {
        static Pipeline unity;
        static Pipeline scaled;
        unity.fm().noteOn(67, 127);
        scaled.fm().noteOn(67, 127);
        scaled.setFmLevel(0.5);
        scaled.setMasterLevel(0.5);
        const auto a = render(unity, 2000);
        const auto b = render(scaled, 2000);
        bool once = true;
        for (std::size_t i = 0; i < a.size(); ++i) {
            once = once && near(b[i].left, a[i].left * 0.25);
        }
        static Pipeline bus;
        bus.mono().noteOn(45, 127);
        bus.setMonoLevel(0.5);
        static Pipeline ref;
        ref.mono().noteOn(45, 127);
        std::vector<StereoFrame> mixed(500, StereoFrame{0.1, -0.1});
        bus.mixSources(mixed.data(), mixed.size());
        const auto raw = render(ref, 500);
        bool kept = true;
        for (std::size_t i = 0; i < mixed.size(); ++i) {
            kept = kept && near(mixed[i].left, 0.1 + 0.5 * raw[i].left) &&
                   near(mixed[i].right, -0.1 + 0.5 * raw[i].right);
        }
        check(once, "G2 Synth level 0.5 x master 0.5 = 0.25 (each applied once)");
        check(kept, "G3 Synths add into existing bus audio without rescaling it");
    }

    // Frame-accurate synth and drum events in the same block.
    {
        static Pipeline p;
        p.drums().assignSample(3, &drumSample);
        p.drums().postTrigger(3, 127, 37);
        p.postNoteOn(SynthTarget::Poly, 60, 127, 100);  // slot 0: left only
        p.setDrumLevel(1.0);
        const auto out = render(p, 256);
        bool before = true;
        for (std::size_t i = 0; i < 37; ++i) {
            before = before && out[i].left == 0.0 && out[i].right == 0.0;
        }
        bool drumOnly = true;
        for (std::size_t i = 37; i <= 100; ++i) {
            drumOnly = drumOnly && out[i].left == 0.25 &&
                       out[i].right == 0.125;
        }
        check(before && drumOnly && out[101].left != 0.25 &&
              out[101].right == 0.125 &&
              p.processedSynthEventCount() == 1,
              "T1 Drum at frame 37 and synth note at frame 100 land exactly");
    }

    // Arbitrary block sizes and determinism through the whole pipeline.
    auto scripted = [&drumSample](std::size_t blockSize) {
        static Pipeline p;
        p.drums().stopAll();
        p.mono().reset();
        p.fm().reset();
        p.poly().reset();
        p.drums().assignSample(0, &drumSample);
        p.setPolyLevel(0.7);
        p.setFmLevel(0.6);
        const std::size_t total = 30000;
        std::vector<StereoFrame> out(total);
        for (std::size_t pos = 0; pos < total; pos += blockSize) {
            const std::size_t n = std::min(blockSize, total - pos);
            for (std::size_t f = pos; f < pos + n; ++f) {
                const std::size_t o = f - pos;
                if (f % 3000 == 0) {
                    const int root = 48 + static_cast<int>((f / 3000) % 5) * 2;
                    p.postNoteOn(SynthTarget::Mono, root, 100, o);
                    p.postNoteOn(SynthTarget::Poly, root + 12, 90, o);
                    p.postNoteOn(SynthTarget::Poly, root + 16, 80, o);
                    p.drums().postTrigger(0, 120, o);
                }
                if (f % 3000 == 2000) {
                    const int root = 48 + static_cast<int>((f / 3000) % 5) * 2;
                    p.postNoteOff(SynthTarget::Poly, root + 12, o);
                    p.postNoteOff(SynthTarget::Poly, root + 16, o);
                    p.postNoteOn(SynthTarget::Fm, root + 24, 70, o);
                }
                if (f % 3000 == 2500) {
                    p.postAllNotesOff(SynthTarget::Fm, o);
                    p.postPitchBend(SynthTarget::Mono,
                                    (f / 3000) % 2 == 0 ? 0.5 : 0.0, o);
                }
            }
            p.render(out.data() + pos, n);
        }
        return out;
    };
    const auto reference = scripted(64);
    bool sameSizes = true;
    for (std::size_t size : {1u, 7u, 333u, 4096u, 30000u}) {
        const auto other = scripted(size);
        for (std::size_t i = 0; i < reference.size(); ++i) {
            sameSizes = sameSizes && reference[i].left == other[i].left &&
                        reference[i].right == other[i].right;
        }
    }
    const auto again = scripted(64);
    bool repeat = true;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        repeat = repeat && reference[i].left == again[i].left &&
                 reference[i].right == again[i].right;
    }
    check(sameSizes && finite(reference),
          "A1 Drums + 3 synths: block sizes 1, 7, 64, 333, 4096, 30000 identical");
    check(repeat, "D1 Repeated rendering of the same script is bit-identical");

    // Invalid posts and queue overflow
    {
        static Pipeline p;
        const bool invalid = !p.postNoteOn(SynthTarget::Fm, 128, 100) &&
                             !p.postNoteOff(SynthTarget::Fm, -1) &&
                             !p.postPitchBend(SynthTarget::Fm, kNaN);
        int accepted = 0;
        for (int i = 0; i < 300; ++i) {
            accepted += p.postNoteOn(SynthTarget::Poly, 60 + i % 12, 90) ? 1 : 0;
        }
        render(p, 16);
        check(invalid && accepted == 255 && p.droppedSynthEventCount() == 45 &&
              p.processedSynthEventCount() == 255,
              "I2 Invalid synth posts rejected; full queue drops and counts");
    }

    // Silence after everything finishes.
    {
        static Pipeline p;
        p.mono().noteOn(40, 127);
        p.fm().noteOn(70, 127);
        p.poly().noteOn(64, 127);
        render(p, 1000);
        p.mono().allNotesOff();
        p.fm().allNotesOff();
        p.poly().allNotesOff();
        render(p, 24000);  // longest default release is 0.2 s
        check(silent(render(p, 2048)) && p.mono().activeVoiceCount() == 0 &&
              p.fm().activeVoiceCount() == 0 &&
              p.poly().activeVoiceCount() == 0,
              "S1 Exact silence once every synth voice has released");
    }

    // One producer thread posts synth (and drum) events while this thread
    // renders: every event is applied or counted as dropped.
    {
        static Pipeline p;
        p.drums().assignSample(0, &drumSample);
        constexpr int kPosts = 20000;
        std::atomic<bool> done{false};
        std::atomic<int> accepted{0};
        std::thread producer([&] {
            int ok = 0;
            for (int i = 0; i < kPosts; ++i) {
                const SynthTarget target = static_cast<SynthTarget>(i % 3);
                const bool posted = (i % 2 == 0)
                    ? p.postNoteOn(target, 40 + i % 40, 1 + i % 127,
                                   static_cast<std::size_t>(i) % 32)
                    : p.postNoteOff(target, 40 + (i - 1) % 40,
                                    static_cast<std::size_t>(i) % 32);
                ok += posted ? 1 : 0;
            }
            accepted.store(ok);
            done.store(true);
        });
        std::vector<StereoFrame> block(32);
        bool ok = true;
        for (int spins = 0; spins < 2000000; ++spins) {
            std::fill(block.begin(), block.end(), StereoFrame{});
            p.render(block.data(), block.size());
            ok = ok && finite(block);
            if (done.load() && p.processedSynthEventCount() ==
                                   static_cast<std::uint64_t>(accepted.load())) {
                break;
            }
        }
        producer.join();
        check(ok && accepted.load() > 0 &&
              p.processedSynthEventCount() + p.droppedSynthEventCount() ==
                  static_cast<std::uint64_t>(kPosts),
              "T2 Two threads: every posted synth event is applied or counted dropped");
    }

    // No allocation in the integrated audio path.
    {
        static Pipeline p;
        p.drums().assignSample(0, &drumSample);
        StereoFrame block[128]{};
        const std::size_t before = g_allocations.load();
        for (int i = 0; i < 2000; ++i) {
            const int note = 40 + i % 30;
            p.postNoteOn(SynthTarget::Poly, note, 90,
                         static_cast<std::size_t>(i) % 64);
            p.postNoteOff(SynthTarget::Poly, note - 3, 70);
            p.postNoteOn(SynthTarget::Fm, note + 12, 80, 10);
            p.postPitchBend(SynthTarget::Mono, (i % 5) * 0.25, 20);
            p.mono().noteOn(note - 12, 100);
            p.fm().noteOff(note);
            p.drums().postTrigger(0, 100, 5);
            p.setPolyLevel(0.5 + 0.1 * (i % 3));
            std::fill(block, block + 128, StereoFrame{});
            p.render(block, 1 + static_cast<std::size_t>(i) % 128);
        }
        const std::size_t after = g_allocations.load();
        const bool noAllocation = after == before;
        check(noAllocation,
              "R1 No heap allocation: synth posts, notes, bends, drums, render");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 022 - Synthesizer Integration\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    equivalence();
    findings();
    synths();
    pipeline();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 022 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 022 PASSED\n";
    return EXIT_SUCCESS;
}
