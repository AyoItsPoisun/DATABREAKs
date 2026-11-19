
// DATABREAKs Engine 023 - effects render demonstration.
//
// An original piece in six 2-bar sections at 120 BPM (4 s each), rendered
// through AudioPipeline with the Engine 023 effects chain:
//   1. dry drums                    (all effects bypassed)
//   2. compressed drums             (Compressor)
//   3. distorted drums              (Distortion: asymmetric, then fuzz)
//   4. FM melody with reverb        (Reverb)
//   5. mono bass with EQ            (Equalizer)
//   6. poly chords + drums through the complete chain
// Effects switch at section boundaries through their 20 ms bypass fades.
// Output: databreaks_engine_023_demo.wav (48 kHz, 16-bit stereo) in the
// current directory.
//
// This is a host software render, NOT a recording of FM-1 hardware.
// Self-checks (exit code 1 on failure): written, finite, peak in range, no
// master clipping, all events applied, every effect section differs
// measurably from the same score rendered with all effects bypassed while
// the dry section is bit-identical, and a second render is bit-identical.

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

constexpr std::size_t kStep = SAMPLE_RATE * 60 / 120 / 4;  // 16th note
constexpr std::size_t kSection = 32 * kStep;               // 2 bars, 4 s
constexpr std::size_t kSections = 6;
constexpr std::size_t kTotal = kSections * kSection + 2 * SAMPLE_RATE;
constexpr std::size_t kBlock = 256;

struct Noise {
    std::uint32_t state;
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 8388607.5 - 1.0;
    }
};

std::vector<StereoFrame> makeKick() {
    std::vector<StereoFrame> s(SAMPLE_RATE * 4 / 10);
    double phase = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        phase += 2.0 * PI * (50.0 + 95.0 * std::exp(-t * 30.0)) / SAMPLE_RATE;
        const double v = 0.7 * std::sin(phase) * std::exp(-t * 7.0);
        s[i] = {v, v};
    }
    return s;
}

std::vector<StereoFrame> makeSnare() {
    std::vector<StereoFrame> s(SAMPLE_RATE / 4);
    Noise noise{0x51u};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.28 * std::sin(2.0 * PI * 185.0 * t) * std::exp(-t * 22.0) +
                         0.26 * noise.next() * std::exp(-t * 14.0);
        s[i] = {v * 0.9, v};
    }
    return s;
}

std::vector<StereoFrame> makeHat(double seconds, double decay, std::uint32_t seed) {
    std::vector<StereoFrame> s(static_cast<std::size_t>(SAMPLE_RATE * seconds));
    Noise noise{seed};
    double previous = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double white = noise.next();
        const double v = 0.13 * (white - previous) * std::exp(-t * decay);
        previous = white;
        s[i] = {v, v * 0.8};
    }
    return s;
}

enum Pad : std::size_t { Kick = 0, Snare = 1, ClosedHat = 2, OpenHat = 3 };

struct Event {
    std::size_t frame;
    enum Kind { Drum, NoteOn, NoteOff } kind;
    SynthTarget target;
    int value;     // pad or note
    int velocity;
};

void drumBar(std::vector<Event>& ev, std::size_t start, int accent) {
    const std::size_t kicks[] = {0, 6, 8, 11};
    for (std::size_t k : kicks) {
        ev.push_back({start + k * kStep, Event::Drum, SynthTarget::Mono, Kick,
                      k == 0 ? 127 : 95 + accent});
    }
    ev.push_back({start + 4 * kStep, Event::Drum, SynthTarget::Mono, Snare, 112});
    ev.push_back({start + 12 * kStep, Event::Drum, SynthTarget::Mono, Snare, 120});
    for (std::size_t h = 0; h < 16; h += 2) {
        const bool open = (h == 14);
        ev.push_back({start + h * kStep, Event::Drum, SynthTarget::Mono,
                      open ? static_cast<int>(OpenHat) : static_cast<int>(ClosedHat),
                      h % 4 == 0 ? 100 : 62});
    }
}

void note(std::vector<Event>& ev, SynthTarget target, std::size_t on,
          std::size_t length, int pitch, int velocity) {
    ev.push_back({on, Event::NoteOn, target, pitch, velocity});
    ev.push_back({on + length - 1, Event::NoteOff, target, pitch, 0});
}

std::vector<Event> score() {
    std::vector<Event> ev;
    // Sections 1-3: the same two drum bars, three treatments.
    for (std::size_t s = 0; s < 3; ++s) {
        drumBar(ev, s * kSection, 0);
        drumBar(ev, s * kSection + 16 * kStep, 10);
    }
    // Section 4: FM melody, no drums.
    {
        const std::size_t base = 3 * kSection;
        const int melody[][3] = {{0, 72, 110}, {3, 76, 85}, {6, 79, 95},
                                 {8, 77, 80},  {12, 74, 100}, {16, 72, 105},
                                 {19, 69, 80}, {22, 71, 90}, {24, 72, 115}};
        for (const auto& m : melody) {
            note(ev, SynthTarget::Fm, base + static_cast<std::size_t>(m[0]) * kStep,
                 3 * kStep, m[1], m[2]);
        }
    }
    // Section 5: mono bass line with hats.
    {
        const std::size_t base = 4 * kSection;
        const int line[] = {36, 36, 48, 36, 43, 36, 46, 48,
                            34, 34, 46, 34, 41, 34, 43, 46};
        for (std::size_t i = 0; i < 16; ++i) {
            note(ev, SynthTarget::Mono, base + i * 2 * kStep, 2 * kStep - kStep / 4,
                 line[i], i % 4 == 0 ? 120 : 90);
            ev.push_back({base + i * 2 * kStep, Event::Drum, SynthTarget::Mono,
                          ClosedHat, 70});
        }
    }
    // Section 6: poly chords + drums + bass, full chain.
    {
        const std::size_t base = 5 * kSection;
        const int chords[2][4] = {{48, 55, 60, 64}, {46, 53, 58, 62}};
        for (std::size_t bar = 0; bar < 2; ++bar) {
            for (int n : chords[bar]) {
                note(ev, SynthTarget::Poly, base + bar * 16 * kStep, 16 * kStep, n, 85);
            }
            note(ev, SynthTarget::Mono, base + bar * 16 * kStep, 6 * kStep,
                 chords[bar][0] - 12, 110);
            note(ev, SynthTarget::Mono, base + bar * 16 * kStep + 8 * kStep, 6 * kStep,
                 chords[bar][0] - 12, 100);
            drumBar(ev, base + bar * 16 * kStep, 0);
        }
    }
    return ev;
}

// Effect settings for a section. With `effectsOn` false everything stays
// bypassed (the reference render).
void configureSection(Pipeline& p, std::size_t section, bool effectsOn) {
    Equalizer& eq = p.eq();
    Compressor& comp = p.compressor();
    SoftClipper& clip = p.softClipper();
    Distortion& dist = p.distortion();
    Reverb& rev = p.reverb();
    const bool on = effectsOn;
    switch (section) {
    case 0:
        break;  // dry
    case 1:
        comp.setThresholdDb(-26.0);
        comp.setRatio(6.0);
        comp.setAttackMs(4.0);
        comp.setReleaseMs(90.0);
        comp.setMakeupDb(7.0);
        comp.setEnabled(on);
        break;
    case 2:
        comp.setEnabled(false);
        dist.setMode(DistortionMode::Asymmetric);
        dist.setDriveDb(20.0);
        dist.setOutputDb(-7.0);
        dist.setMix(0.75);
        dist.setEnabled(on);
        break;
    case 3:
        dist.setEnabled(false);
        rev.setRoomSize(0.85);
        rev.setDecaySeconds(3.5);
        rev.setDamping(0.35);
        rev.setWetLevel(0.45);
        rev.setDryLevel(0.9);
        rev.setEnabled(on);
        break;
    case 4:
        rev.setEnabled(false);
        eq.setLow(90.0, 7.0);
        eq.setMid(450.0, -6.0, 1.2);
        eq.setHigh(5000.0, 5.0);
        eq.setEnabled(on);
        break;
    case 5:
        eq.setLow(80.0, 3.0);
        eq.setMid(1200.0, -2.0, 0.8);
        eq.setHigh(8000.0, 3.0);
        comp.setThresholdDb(-20.0);
        comp.setRatio(3.0);
        comp.setAttackMs(8.0);
        comp.setReleaseMs(150.0);
        comp.setMakeupDb(3.0);
        clip.setThreshold(0.6);
        clip.setDriveDb(2.0);
        dist.setMode(DistortionMode::Soft);
        dist.setDriveDb(10.0);
        dist.setOutputDb(-4.0);
        dist.setMix(0.25);
        rev.setRoomSize(0.7);
        rev.setDecaySeconds(2.2);
        rev.setWetLevel(0.3);
        rev.setDryLevel(1.0);
        eq.setEnabled(on);
        comp.setEnabled(on);
        clip.setEnabled(on);
        dist.setEnabled(on);
        rev.setEnabled(on);
        break;
    default:
        break;
    }
}

struct Result {
    std::vector<StereoFrame> audio;
    std::uint64_t clipped = 0;
    std::uint64_t events = 0;
    std::uint32_t dropped = 0;
};

Result renderPiece(bool effectsOn) {
    static const auto kick = makeKick();
    static const auto snare = makeSnare();
    static const auto closedHat = makeHat(0.07, 60.0, 0xBEEFu);
    static const auto openHat = makeHat(0.5, 6.0, 0xF00Du);
    static const auto events = score();

    static Pipeline p;
    p.drums().stopAll();
    p.mono().reset();
    p.fm().reset();
    p.poly().reset();
    for (std::size_t i = 0; i < p.effects().size(); ++i) {
        p.effects().slot(i)->setBypassed(true, true);
    }
    p.resetDiagnostics();
    const std::uint64_t eventsBefore =
        p.drums().processedEventCount() + p.processedSynthEventCount();
    const std::uint32_t droppedBefore =
        p.drums().droppedEventCount() + p.droppedSynthEventCount();

    p.drums().assignSample(Kick, &kick);
    p.drums().assignSample(Snare, &snare);
    p.drums().assignSample(ClosedHat, &closedHat);
    p.drums().assignSample(OpenHat, &openHat);
    p.drums().setChokeGroup(ClosedHat, 1);
    p.drums().setChokeGroup(OpenHat, 1);

    MonoParameters bass;
    bass.filterCutoff = 260.0;
    bass.filterEnvelopeAmount = 1800.0;
    bass.volume = 0.6;
    bass.envelope = {0.004, 0.2, 0.5, 0.06};
    p.mono().setParameters(bass);
    FMParameters bell;
    bell.modulationRatio = 3.5;
    bell.modulationDepth = 2.2;
    bell.volume = 0.3;
    bell.envelope = {0.003, 0.4, 0.2, 0.5};
    p.fm().setParameters(bell);
    PolyParameters pad;
    pad.detune = 0.005;
    pad.voiceVolume = 0.11;
    pad.stereoWidth = 0.8;
    pad.envelope = {0.3, 0.6, 0.7, 1.0};
    p.poly().setParameters(pad);

    p.setDrumLevel(0.7);
    p.setMonoLevel(0.75);
    p.setFmLevel(0.7);
    p.setPolyLevel(0.75);
    p.setMasterLevel(0.85);

    Result r;
    r.audio.assign(kTotal, StereoFrame{});
    std::size_t section = 0;
    configureSection(p, 0, effectsOn);
    for (std::size_t pos = 0; pos < kTotal; pos += kBlock) {
        const std::size_t n = std::min(kBlock, kTotal - pos);
        const std::size_t s = std::min(pos / kSection, kSections - 1);
        if (s != section) {
            section = s;
            configureSection(p, section, effectsOn);
        }
        for (const Event& e : events) {
            if (e.frame < pos || e.frame >= pos + n) {
                continue;
            }
            const std::size_t o = e.frame - pos;
            switch (e.kind) {
            case Event::Drum:
                p.drums().postTrigger(static_cast<std::size_t>(e.value), e.velocity, o);
                break;
            case Event::NoteOn:
                p.postNoteOn(e.target, e.value, e.velocity, o);
                break;
            case Event::NoteOff:
                p.postNoteOff(e.target, e.value, o);
                break;
            }
        }
        p.render(r.audio.data() + pos, n);
    }
    r.clipped = p.clippedSampleCount();
    r.events = p.drums().processedEventCount() + p.processedSynthEventCount() -
               eventsBefore;
    r.dropped = p.drums().droppedEventCount() + p.droppedSynthEventCount() -
                droppedBefore;
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

bool writeWav(const char* path, const std::vector<StereoFrame>& audio) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
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

double rmsRange(const std::vector<StereoFrame>& a, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        e += a[i].left * a[i].left + a[i].right * a[i].right;
    }
    return std::sqrt(e / (2.0 * static_cast<double>(to - from)));
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 023 - effects render demo\n");
    std::printf("Host software render; not an FM-1 hardware recording.\n");

    const Result fx = renderPiece(true);
    const Result reference = renderPiece(false);
    const Result again = renderPiece(true);

    double peak = 0.0;
    bool finite = true;
    bool identical = true;
    for (std::size_t i = 0; i < kTotal; ++i) {
        const StereoFrame& f = fx.audio[i];
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
        identical = identical && f.left == again.audio[i].left &&
                    f.right == again.audio[i].right;
    }

    const char* names[kSections] = {"dry drums", "compressed drums",
                                    "distorted drums", "FM + reverb",
                                    "mono bass + EQ", "poly + full chain"};
    bool sectionsOk = true;
    for (std::size_t s = 0; s < kSections; ++s) {
        const std::size_t from = s * kSection;
        const std::size_t to = from + kSection;
        std::vector<StereoFrame> diff(kSection);
        bool same = true;
        for (std::size_t i = from; i < to; ++i) {
            diff[i - from] = {fx.audio[i].left - reference.audio[i].left,
                              fx.audio[i].right - reference.audio[i].right};
            same = same && diff[i - from].left == 0.0 && diff[i - from].right == 0.0;
        }
        const double level = rmsRange(fx.audio, from, to);
        const double change = rmsRange(diff, 0, kSection);
        const double changeDb = change > 0.0 ? 20.0 * std::log10(change / level) : -999.0;
        std::printf("Section %zu %-18s RMS %.4f, difference from all-bypassed %s\n",
                    s + 1, names[s], level,
                    same ? "none (bit-identical)" : "");
        if (!same) {
            std::printf("    %.1f dB relative to the section\n", changeDb);
        }
        sectionsOk = sectionsOk && (s == 0 ? same : (!same && changeDb > -30.0));
    }

    const char* path = "databreaks_engine_023_demo.wav";
    const bool written = writeWav(path, fx.audio);

    std::printf("Frames: %zu (%.2f s), block %zu, events %llu, dropped %u\n",
                kTotal, static_cast<double>(kTotal) / SAMPLE_RATE, kBlock,
                static_cast<unsigned long long>(fx.events), fx.dropped);
    std::printf("Peak %.4f, clipped samples %llu\n", peak,
                static_cast<unsigned long long>(fx.clipped));
    std::printf("Second render bit-identical: %s\n", identical ? "yes" : "no");
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok = written && finite && peak > 0.1 && peak < 1.0 &&
                    fx.clipped == 0 && fx.dropped == 0 &&
                    fx.events == score().size() && sectionsOk && identical;
    std::printf(ok ? "ENGINE 023 RENDER PASSED\n" : "ENGINE 023 RENDER FAILED\n");
    return ok ? 0 : 1;
}
