
// DATABREAKs Engine 022 - drums + synthesizers render demonstration.
//
// Renders an original 4-bar piece at 120 BPM through AudioPipeline:
//   * drums (DrumEngine, synthesised kit, hi-hat choke group),
//   * mono bass (MonoSynthEngine) with a pitch-bend slide,
//   * FM bell melody (FmSynthEngine),
//   * poly pad chords (PolySynthEngine, 8 voices),
// all scheduled through the frame-accurate event queues in 256-frame blocks,
// and writes databreaks_engine_022_demo.wav (48 kHz, 16-bit stereo) into
// the current directory.
//
// This is a host software render, NOT a recording of FM-1 hardware.
// Self-checks (exit code 1 on failure): written, finite, unclipped, peak in
// range, all events applied with none dropped, every source audible, the
// full mix equals the sum of the solo renders, and a second render is
// bit-identical.

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
constexpr std::size_t kBars = 4;
constexpr std::size_t kSteps = kBars * 16;
constexpr std::size_t kTotal = kSteps * kStep + SAMPLE_RATE * 3 / 2;
constexpr std::size_t kBlock = 256;

// ---- Synthesised drum kit (deterministic formulas) --------------------------

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
        phase += 2.0 * PI * (48.0 + 100.0 * std::exp(-t * 28.0)) / SAMPLE_RATE;
        const double v = 0.75 * std::sin(phase) * std::exp(-t * 7.5);
        s[i] = {v, v};
    }
    return s;
}

std::vector<StereoFrame> makeSnare() {
    std::vector<StereoFrame> s(SAMPLE_RATE / 4);
    Noise noise{0x5A17u};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double v = 0.3 * std::sin(2.0 * PI * 200.0 * t) *
                             std::exp(-t * 24.0) +
                         0.28 * noise.next() * std::exp(-t * 15.0);
        s[i] = {v, v * 0.92};
    }
    return s;
}

std::vector<StereoFrame> makeHat(double seconds, double decay,
                                 std::uint32_t seed) {
    std::vector<StereoFrame> s(static_cast<std::size_t>(SAMPLE_RATE * seconds));
    Noise noise{seed};
    double previous = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double white = noise.next();
        const double v = 0.14 * (white - previous) * std::exp(-t * decay);
        previous = white;
        s[i] = {v * 0.75, v};
    }
    return s;
}

enum Pad : std::size_t { Kick = 0, Snare = 1, ClosedHat = 2, OpenHat = 3 };

struct Note {
    std::size_t step;     // 16th-note step where the note starts
    std::size_t length;   // in 16th notes
    SynthTarget target;
    int note;
    int velocity;
};

struct DrumHit {
    std::size_t step;
    std::size_t pad;
    int velocity;
};

// Original progression: Dm7 | Bbmaj7 | Fmaj7/A | C(add9)/E, one per bar.
const int kChords[kBars][4] = {
    {50, 53, 57, 60}, {46, 50, 53, 57}, {45, 48, 52, 57}, {52, 55, 60, 62}};
const int kRoots[kBars] = {38, 34, 33, 40};

std::vector<Note> buildNotes() {
    std::vector<Note> notes;
    for (std::size_t bar = 0; bar < kBars; ++bar) {
        const std::size_t base = bar * 16;
        for (int n : kChords[bar]) {
            notes.push_back({base, 15, SynthTarget::Poly, n, 80});
        }
        // Bass: eighth-note pulse with accents.
        const std::size_t pulse[] = {0, 3, 6, 8, 10, 14};
        for (std::size_t p : pulse) {
            const int velocity = (p == 0 || p == 8) ? 120 : 85;
            const int note = (p == 14) ? kRoots[bar] + 12 : kRoots[bar];
            notes.push_back({base + p, 2, SynthTarget::Mono, note, velocity});
        }
    }
    // FM bell melody over bars 2-4.
    const int melody[][3] = {
        // step, note, velocity
        {16, 69, 100}, {19, 72, 80}, {22, 74, 90}, {24, 72, 70},
        {28, 69, 85},  {32, 76, 105}, {35, 74, 80}, {38, 72, 75},
        {40, 69, 90},  {44, 67, 70},  {48, 74, 100}, {51, 72, 80},
        {54, 69, 85},  {56, 72, 95},  {60, 74, 110}};
    for (const auto& m : melody) {
        notes.push_back({static_cast<std::size_t>(m[0]), 3, SynthTarget::Fm,
                         m[1], m[2]});
    }
    return notes;
}

std::vector<DrumHit> buildDrums() {
    std::vector<DrumHit> hits;
    for (std::size_t bar = 0; bar < kBars; ++bar) {
        const std::size_t b = bar * 16;
        hits.push_back({b + 0, Kick, 125});
        hits.push_back({b + 7, Kick, 80});
        hits.push_back({b + 10, Kick, 110});
        hits.push_back({b + 4, Snare, 110});
        hits.push_back({b + 12, Snare, 118});
        for (std::size_t s = 0; s < 16; s += 2) {
            if (s == 6 || s == 14) {
                hits.push_back({b + s, OpenHat, 95});  // choked 2 steps later
            } else {
                hits.push_back({b + s, ClosedHat, s % 4 == 0 ? 100 : 65});
            }
        }
    }
    return hits;
}

struct Levels {
    double drums;
    double mono;
    double fm;
    double poly;
};

struct RenderResult {
    std::vector<StereoFrame> audio;
    std::uint64_t synthEvents = 0;
    std::uint64_t drumEvents = 0;
    std::uint32_t dropped = 0;
    std::uint64_t clipped = 0;
};

RenderResult renderPiece(const Levels& levels) {
    static const auto kick = makeKick();
    static const auto snare = makeSnare();
    static const auto closedHat = makeHat(0.07, 65.0, 0xBEEFu);
    static const auto openHat = makeHat(0.5, 6.0, 0xF00Du);
    static const auto notes = buildNotes();
    static const auto drums = buildDrums();

    static Pipeline p;  // large object; reset fully below
    p.drums().stopAll();
    p.mono().reset();
    p.fm().reset();
    p.poly().reset();
    p.resetDiagnostics();
    const std::uint64_t synthBefore = p.processedSynthEventCount();
    const std::uint64_t drumBefore = p.drums().processedEventCount();
    const std::uint32_t droppedBefore =
        p.droppedSynthEventCount() + p.drums().droppedEventCount();

    p.drums().assignSample(Kick, &kick);
    p.drums().assignSample(Snare, &snare);
    p.drums().assignSample(ClosedHat, &closedHat);
    p.drums().assignSample(OpenHat, &openHat);
    p.drums().setChokeGroup(ClosedHat, 1);
    p.drums().setChokeGroup(OpenHat, 1);

    MonoParameters bass;
    bass.filterCutoff = 280.0;
    bass.filterEnvelopeAmount = 1600.0;
    bass.volume = 0.6;
    bass.envelope = {0.004, 0.18, 0.45, 0.07};
    p.mono().setParameters(bass);

    FMParameters bell;
    bell.modulationRatio = 3.0;
    bell.modulationDepth = 2.5;
    bell.volume = 0.32;
    bell.envelope = {0.003, 0.35, 0.15, 0.45};
    p.fm().setParameters(bell);

    PolyParameters pad;
    pad.detune = 0.004;
    pad.voiceVolume = 0.11;
    pad.stereoWidth = 0.8;
    pad.envelope = {0.35, 0.6, 0.65, 0.9};
    p.poly().setParameters(pad);

    p.setDrumLevel(levels.drums);
    p.setMonoLevel(levels.mono);
    p.setFmLevel(levels.fm);
    p.setPolyLevel(levels.poly);
    p.setMasterLevel(0.85);

    RenderResult result;
    result.audio.assign(kTotal, StereoFrame{});

    for (std::size_t pos = 0; pos < kTotal; pos += kBlock) {
        const std::size_t n = std::min(kBlock, kTotal - pos);
        for (const DrumHit& h : drums) {
            const std::size_t at = h.step * kStep;
            if (at >= pos && at < pos + n) {
                p.drums().postTrigger(h.pad, h.velocity, at - pos);
            }
        }
        for (const Note& note : notes) {
            const std::size_t on = note.step * kStep;
            const std::size_t off = (note.step + note.length) * kStep - 1;
            if (on >= pos && on < pos + n) {
                p.postNoteOn(note.target, note.note, note.velocity, on - pos);
            }
            if (off >= pos && off < pos + n) {
                p.postNoteOff(note.target, note.note, off - pos);
            }
        }
        // Bass slide: bend up 2 semitones over the last beat of bar 4,
        // in 16 steps, then back to centre after the piece.
        const std::size_t slideStart = (kSteps - 4) * kStep;
        for (std::size_t k = 0; k <= 16; ++k) {
            const std::size_t at = slideStart + k * (4 * kStep / 16);
            if (at >= pos && at < pos + n) {
                p.postPitchBend(SynthTarget::Mono, 2.0 * k / 16.0, at - pos);
            }
        }
        const std::size_t reset = kSteps * kStep;
        if (reset >= pos && reset < pos + n) {
            p.postPitchBend(SynthTarget::Mono, 0.0, reset - pos);
        }
        p.render(result.audio.data() + pos, n);
    }

    result.synthEvents = p.processedSynthEventCount() - synthBefore;
    result.drumEvents = p.drums().processedEventCount() - drumBefore;
    result.dropped = p.droppedSynthEventCount() + p.drums().droppedEventCount() -
                     droppedBefore;
    result.clipped = p.clippedSampleCount();
    return result;
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

double energy(const std::vector<StereoFrame>& audio, bool left) {
    double e = 0.0;
    for (const auto& f : audio) {
        const double v = left ? f.left : f.right;
        e += v * v;
    }
    return e;
}

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 022 - drums + synthesizers render demo\n");
    std::printf("Host software render; not an FM-1 hardware recording.\n");

    const Levels mix{0.75, 0.8, 0.55, 0.7};
    const RenderResult full = renderPiece(mix);

    // Solo renders: each source alone, everything else at level 0.
    const RenderResult drumsSolo = renderPiece({mix.drums, 0, 0, 0});
    const RenderResult monoSolo = renderPiece({0, mix.mono, 0, 0});
    const RenderResult fmSolo = renderPiece({0, 0, mix.fm, 0});
    const RenderResult polySolo = renderPiece({0, 0, 0, mix.poly});
    const RenderResult again = renderPiece(mix);

    double peak = 0.0;
    double sumError = 0.0;
    bool finite = true;
    bool identical = true;
    for (std::size_t i = 0; i < kTotal; ++i) {
        const StereoFrame& f = full.audio[i];
        finite = finite && std::isfinite(f.left) && std::isfinite(f.right);
        peak = std::max(peak, std::max(std::abs(f.left), std::abs(f.right)));
        const double sumL = drumsSolo.audio[i].left + monoSolo.audio[i].left +
                            fmSolo.audio[i].left + polySolo.audio[i].left;
        const double sumR = drumsSolo.audio[i].right + monoSolo.audio[i].right +
                            fmSolo.audio[i].right + polySolo.audio[i].right;
        sumError = std::max(sumError, std::abs(f.left - sumL));
        sumError = std::max(sumError, std::abs(f.right - sumR));
        identical = identical && f.left == again.audio[i].left &&
                    f.right == again.audio[i].right;
    }

    const char* path = "databreaks_engine_022_demo.wav";
    const bool written = writeWav(path, full.audio);

    const std::size_t expectedSynth = buildNotes().size() * 2 + 17 + 1;
    const std::size_t expectedDrums = buildDrums().size();

    std::printf("Frames: %zu (%.2f s), block %zu, 120 BPM, %zu bars\n", kTotal,
                static_cast<double>(kTotal) / SAMPLE_RATE, kBlock, kBars);
    std::printf("Events: synth %llu (expected %zu), drums %llu (expected %zu), "
                "dropped %u\n",
                static_cast<unsigned long long>(full.synthEvents), expectedSynth,
                static_cast<unsigned long long>(full.drumEvents), expectedDrums,
                full.dropped);
    std::printf("Peak %.4f, clipped samples %llu, RMS L/R %.4f / %.4f\n", peak,
                static_cast<unsigned long long>(full.clipped),
                std::sqrt(energy(full.audio, true) / kTotal),
                std::sqrt(energy(full.audio, false) / kTotal));
    std::printf("Solo RMS (L+R): drums %.4f, mono %.4f, fm %.4f, poly %.4f\n",
                std::sqrt((energy(drumsSolo.audio, true) +
                           energy(drumsSolo.audio, false)) / (2 * kTotal)),
                std::sqrt((energy(monoSolo.audio, true) +
                           energy(monoSolo.audio, false)) / (2 * kTotal)),
                std::sqrt((energy(fmSolo.audio, true) +
                           energy(fmSolo.audio, false)) / (2 * kTotal)),
                std::sqrt((energy(polySolo.audio, true) +
                           energy(polySolo.audio, false)) / (2 * kTotal)));
    std::printf("Full mix vs sum of solos: max difference %.3e\n", sumError);
    std::printf("Second render bit-identical: %s\n", identical ? "yes" : "no");
    std::printf("Wrote %s: %s\n", path, written ? "ok" : "FAILED");

    const bool ok =
        written && finite && peak > 0.1 && peak < 1.0 && full.clipped == 0 &&
        full.synthEvents == expectedSynth && full.drumEvents == expectedDrums &&
        full.dropped == 0 && energy(drumsSolo.audio, true) > 0.0 &&
        energy(monoSolo.audio, true) > 0.0 && energy(fmSolo.audio, true) > 0.0 &&
        energy(polySolo.audio, true) > 0.0 && energy(polySolo.audio, false) > 0.0 &&
        sumError < 1e-12 && identical;
    std::printf(ok ? "ENGINE 022 RENDER PASSED\n" : "ENGINE 022 RENDER FAILED\n");
    return ok ? 0 : 1;
}
