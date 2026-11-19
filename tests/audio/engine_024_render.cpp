
// DATABREAKs Engine 024 - tape render demonstration.
//
// An original piece in seven 2-bar sections at 120 BPM (4 s each), rendered
// through AudioPipeline with the Engine 024 tape stage:
//   1. dry drums                              (tape bypassed)
//   2. drums on ferric tape (15 IPS, saturated)
//   3. the same drums on chrome tape
//   4. the same drums on metal tape
//   5. drums + FM: bar 1 at 7.5 IPS, bar 2 at 30 IPS (same content)
//   6. poly chords with heavy wow and flutter
//   7. full mix: drums, mono, FM, poly through EQ, compressor, tape, reverb
// Output: databreaks_engine_024_demo.wav (48 kHz, 16-bit stereo) in the
// current directory.
//
// This is a host software render, NOT a recording of FM-1 hardware.
// Self-checks (exit code 1 on failure): written, finite, peak in range, no
// master clipping, all events applied; compared with the same score
// rendered with tape bypassed, section 1 is bit-identical and every tape
// section differs; brightness orders ferric < chrome < metal and
// 7.5 < 30 IPS; a second render is bit-identical.

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
constexpr std::size_t kBar = 16 * kStep;                   // 2 s
constexpr std::size_t kSection = 2 * kBar;                 // 4 s
constexpr std::size_t kSections = 7;
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
        phase += 2.0 * PI * (52.0 + 90.0 * std::exp(-t * 30.0)) / SAMPLE_RATE;
        const double v = 0.6 * std::sin(phase) * std::exp(-t * 7.0);
        s[i] = {v, v};
    }
    return s;
}

std::vector<StereoFrame> makeSnare() {
    std::vector<StereoFrame> s(SAMPLE_RATE / 4);
    Noise noise{0x24u};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.25 * std::sin(2.0 * PI * 190.0 * t) * std::exp(-t * 22.0) +
                         0.24 * noise.next() * std::exp(-t * 14.0);
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
        const double v = 0.12 * (white - previous) * std::exp(-t * decay);
        previous = white;
        s[i] = {v, v * 0.85};
    }
    return s;
}

enum Pad : std::size_t { Kick = 0, Snare = 1, ClosedHat = 2, OpenHat = 3 };

struct Event {
    std::size_t frame;
    enum Kind { Drum, NoteOn, NoteOff } kind;
    SynthTarget target;
    int value;
    int velocity;
};

void drumBar(std::vector<Event>& ev, std::size_t start) {
    const std::size_t kicks[] = {0, 7, 8, 10};
    for (std::size_t k : kicks) {
        ev.push_back({start + k * kStep, Event::Drum, SynthTarget::Mono, Kick, k == 0 ? 125 : 100});
    }
    ev.push_back({start + 4 * kStep, Event::Drum, SynthTarget::Mono, Snare, 110});
    ev.push_back({start + 12 * kStep, Event::Drum, SynthTarget::Mono, Snare, 118});
    for (std::size_t h = 0; h < 16; h += 2) {
        const bool open = (h == 6);
        ev.push_back({start + h * kStep, Event::Drum, SynthTarget::Mono,
                      open ? static_cast<int>(OpenHat) : static_cast<int>(ClosedHat),
                      h % 4 == 0 ? 95 : 60});
    }
}

void note(std::vector<Event>& ev, SynthTarget t, std::size_t on, std::size_t len,
          int pitch, int velocity) {
    ev.push_back({on, Event::NoteOn, t, pitch, velocity});
    ev.push_back({on + len - 1, Event::NoteOff, t, pitch, 0});
}

void fmPhrase(std::vector<Event>& ev, std::size_t start) {
    const int phrase[][2] = {{0, 76}, {3, 79}, {6, 81}, {8, 79}, {12, 74}};
    for (const auto& p : phrase) {
        note(ev, SynthTarget::Fm, start + static_cast<std::size_t>(p[0]) * kStep,
             3 * kStep, p[1], 95);
    }
}

std::vector<Event> score() {
    std::vector<Event> ev;
    for (std::size_t s = 0; s < 4; ++s) {          // sections 1-4: same drums
        drumBar(ev, s * kSection);
        drumBar(ev, s * kSection + kBar);
    }
    {                                               // section 5: same bar twice
        const std::size_t base = 4 * kSection;
        for (std::size_t b = 0; b < 2; ++b) {
            drumBar(ev, base + b * kBar);
            fmPhrase(ev, base + b * kBar);
        }
    }
    {                                               // section 6: wobbly chords
        const std::size_t base = 5 * kSection;
        const int chords[2][4] = {{53, 57, 60, 64}, {50, 53, 57, 62}};
        for (std::size_t b = 0; b < 2; ++b) {
            for (int n : chords[b]) {
                note(ev, SynthTarget::Poly, base + b * kBar, kBar, n, 90);
            }
        }
    }
    {                                               // section 7: full mix
        const std::size_t base = 6 * kSection;
        const int chords[2][4] = {{48, 55, 60, 64}, {45, 52, 57, 60}};
        const int roots[2] = {36, 33};
        for (std::size_t b = 0; b < 2; ++b) {
            const std::size_t bar = base + b * kBar;
            drumBar(ev, bar);
            for (int n : chords[b]) note(ev, SynthTarget::Poly, bar, kBar, n, 80);
            for (std::size_t p : {0u, 3u, 6u, 8u, 11u, 14u}) {
                note(ev, SynthTarget::Mono, bar + p * kStep, 2 * kStep, roots[b], p == 0 ? 120 : 95);
            }
            fmPhrase(ev, bar);
        }
    }
    return ev;
}

// Tape and effect settings at the start of each section (and at bar 2 of
// section 5). With tapeOn false the tape stays bypassed (reference render);
// everything else is identical.
void configure(Pipeline& p, std::size_t section, bool secondBar, bool tapeOn) {
    TapeEffect& tape = p.tape();
    switch (section) {
    case 0:
        break;
    case 1:
        tape.setTapeType(TapeType::Ferric);
        tape.setSpeed(TapeSpeed::IPS_15);
        // TapeMachine's saturation also raises small-signal gain (about
        // drive / tanh(drive)); keep it moderate so the mix stays clean.
        tape.setSaturation(0.35);
        tape.setHeadBump(0.5);
        tape.setHiss(0.15);
        tape.setEnabled(tapeOn);
        break;
    case 2:
        tape.setTapeType(TapeType::Chrome);
        break;
    case 3:
        tape.setTapeType(TapeType::Metal);
        break;
    case 4:
        tape.setTapeType(TapeType::Chrome);
        tape.setSaturation(0.25);
        tape.setSpeed(secondBar ? TapeSpeed::IPS_30 : TapeSpeed::IPS_7_5);
        break;
    case 5:
        tape.setSpeed(TapeSpeed::IPS_15);
        tape.setWow(1.0);
        tape.setFlutter(0.8);
        tape.setAge(0.4);
        break;
    case 6:
        tape.setWow(0.2);
        tape.setFlutter(0.2);
        tape.setAge(0.1);
        tape.setSaturation(0.2);
        tape.setCompression(0.4);
        tape.setPrintThrough(0.15);
        tape.setCrosstalk(0.1);
        p.eq().setLow(90.0, 3.0);
        p.eq().setHigh(9000.0, 2.0);
        p.compressor().setThresholdDb(-20.0);
        p.compressor().setRatio(3.0);
        p.compressor().setMakeupDb(2.0);
        p.reverb().setDecaySeconds(2.0);
        p.reverb().setWetLevel(0.25);
        p.eq().setEnabled(true);
        p.compressor().setEnabled(true);
        p.reverb().setEnabled(true);
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

Result renderPiece(bool tapeOn) {
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
    bass.filterCutoff = 250.0;
    bass.filterEnvelopeAmount = 1600.0;
    bass.volume = 0.55;
    bass.envelope = {0.004, 0.2, 0.5, 0.06};
    p.mono().setParameters(bass);
    FMParameters bell;
    bell.modulationRatio = 3.5;
    bell.modulationDepth = 2.0;
    bell.volume = 0.28;
    bell.envelope = {0.003, 0.4, 0.2, 0.5};
    p.fm().setParameters(bell);
    PolyParameters pad;
    pad.detune = 0.004;
    pad.voiceVolume = 0.1;
    pad.stereoWidth = 0.8;
    pad.envelope = {0.25, 0.6, 0.7, 0.8};
    p.poly().setParameters(pad);

    p.setDrumLevel(0.65);
    p.setMonoLevel(0.7);
    p.setFmLevel(0.7);
    p.setPolyLevel(0.8);
    p.setMasterLevel(0.6);

    // Start from TapeMachine's defaults every time.
    p.tape().setTapeType(TapeType::Ferric);
    p.tape().setSpeed(TapeSpeed::IPS_15);
    p.tape().setWow(0.0);
    p.tape().setFlutter(0.0);
    p.tape().setHiss(0.0);
    p.tape().setSaturation(0.0);
    p.tape().setHeadBump(0.0);
    p.tape().setBias(0.5);
    p.tape().setCompression(0.0);
    p.tape().setCrosstalk(0.0);
    p.tape().setPrintThrough(0.0);
    p.tape().setAge(0.0);

    Result r;
    r.audio.assign(kTotal, StereoFrame{});
    std::size_t section = 0;
    bool secondBar = false;
    configure(p, 0, false, tapeOn);
    for (std::size_t pos = 0; pos < kTotal; pos += kBlock) {
        const std::size_t n = std::min(kBlock, kTotal - pos);
        const std::size_t s = std::min(pos / kSection, kSections - 1);
        const bool bar2 = (pos % kSection) >= kBar && pos < kSections * kSection;
        if (s != section || bar2 != secondBar) {
            const bool newSection = s != section;
            section = s;
            secondBar = bar2;
            if (newSection || section == 4) {
                configure(p, section, secondBar, tapeOn);
            }
        }
        for (const Event& e : events) {
            if (e.frame < pos || e.frame >= pos + n) continue;
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
    r.events = p.drums().processedEventCount() + p.processedSynthEventCount() - eventsBefore;
    r.dropped = p.drums().droppedEventCount() + p.droppedSynthEventCount() - droppedBefore;
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

double rmsRange(const std::vector<StereoFrame>& a, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) e += a[i].left * a[i].left + a[i].right * a[i].right;
    return std::sqrt(e / (2.0 * static_cast<double>(to - from)));
}

// Brightness: RMS of the first difference relative to RMS (rises with
// high-frequency content).
double brightness(const std::vector<StereoFrame>& a, std::size_t from, std::size_t to) {
    double e = 0.0;
    double d = 0.0;
    for (std::size_t i = from + 1; i < to; ++i) {
        e += a[i].left * a[i].left;
        const double diff = a[i].left - a[i - 1].left;
        d += diff * diff;
    }
    return std::sqrt(d / e);
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 024 - tape render demo\n");
    std::printf("Host software render; not an FM-1 hardware recording.\n");

    const Result tape = renderPiece(true);
    const Result reference = renderPiece(false);
    const Result again = renderPiece(true);

    double peak = 0.0;
    bool finite = true;
    bool identical = true;
    for (std::size_t i = 0; i < kTotal; ++i) {
        const StereoFrame& f = tape.audio[i];
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
        identical = identical && f.left == again.audio[i].left && f.right == again.audio[i].right;
    }

    const char* names[kSections] = {"dry drums", "ferric tape", "chrome tape", "metal tape",
                                    "7.5 then 30 IPS", "wow + flutter", "full mix"};
    bool sectionsOk = true;
    for (std::size_t s = 0; s < kSections; ++s) {
        const std::size_t from = s * kSection;
        const std::size_t to = from + kSection;
        std::vector<StereoFrame> diff(kSection);
        bool same = true;
        for (std::size_t i = from; i < to; ++i) {
            diff[i - from] = {tape.audio[i].left - reference.audio[i].left,
                              tape.audio[i].right - reference.audio[i].right};
            same = same && diff[i - from].left == 0.0 && diff[i - from].right == 0.0;
        }
        const double level = rmsRange(tape.audio, from, to);
        const double change = rmsRange(diff, 0, kSection);
        const double changeDb = change > 0.0 ? 20.0 * std::log10(change / level) : -999.0;
        std::printf("Section %zu %-16s RMS %.4f  brightness %.4f  vs tape bypassed: %s",
                    s + 1, names[s], level, brightness(tape.audio, from, to),
                    same ? "bit-identical\n" : "");
        if (!same) std::printf("%.1f dB\n", changeDb);
        sectionsOk = sectionsOk && (s == 0 ? same : (!same && changeDb > -30.0));
    }

    // Character ordering on identical material (skip the first 0.25 s of a
    // section, where a type change is still ducking).
    const std::size_t skip = SAMPLE_RATE / 4;
    const double ferric = brightness(tape.audio, 1 * kSection + skip, 2 * kSection);
    const double chrome = brightness(tape.audio, 2 * kSection + skip, 3 * kSection);
    const double metal = brightness(tape.audio, 3 * kSection + skip, 4 * kSection);
    const double slow = brightness(tape.audio, 4 * kSection + skip, 4 * kSection + kBar);
    const double fast = brightness(tape.audio, 4 * kSection + kBar + skip, 5 * kSection);
    const bool typesOrdered = ferric < chrome && chrome < metal;
    const bool speedsOrdered = slow < fast;
    std::printf("Brightness: ferric %.4f < chrome %.4f < metal %.4f: %s\n", ferric, chrome,
                metal, typesOrdered ? "yes" : "NO");
    std::printf("Brightness: 7.5 IPS %.4f < 30 IPS %.4f: %s\n", slow, fast,
                speedsOrdered ? "yes" : "NO");

    const char* path = "databreaks_engine_024_demo.wav";
    const bool written = writeWav(path, tape.audio);

    std::printf("Frames: %zu (%.2f s), block %zu, events %llu, dropped %u\n", kTotal,
                static_cast<double>(kTotal) / SAMPLE_RATE, kBlock,
                static_cast<unsigned long long>(tape.events), tape.dropped);
    std::printf("Peak %.4f, master clipped samples %llu\n", peak,
                static_cast<unsigned long long>(tape.clipped));
    std::printf("Second render bit-identical: %s\n", identical ? "yes" : "no");
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok = written && finite && peak > 0.1 && peak < 1.0 && tape.clipped == 0 &&
                    tape.dropped == 0 && tape.events == score().size() && sectionsOk &&
                    typesOrdered && speedsOrdered && identical;
    std::printf(ok ? "ENGINE 024 RENDER PASSED\n" : "ENGINE 024 RENDER FAILED\n");
    return ok ? 0 : 1;
}
