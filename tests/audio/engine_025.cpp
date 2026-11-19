
// DATABREAKs Engine 025 - sequencer foundation tests.
//
// Host-side assertion tests for core/sequencer/* (musical time, clock,
// pattern, sequencer) and the sample-accurate scheduled-event paths in
// DrumEngine and AudioPipeline. Exits non-zero if any check fails.
// Not FM-1 firmware.

#include "audio_pipeline.h"

#include <algorithm>
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

std::string num(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

using Buffer = std::vector<StereoFrame>;
__extension__ typedef __int128 i128;

// Independent exact formula: first sample at or after tick t for a fixed
// tempo starting at tick 0 / sample 0.
std::int64_t exactSample(std::int64_t tick, int fs, std::int64_t milliBpm) {
    const i128 num = static_cast<i128>(tick) * fs * 60000;
    const i128 den = static_cast<i128>(milliBpm) * TicksPerQuarter;
    return static_cast<std::int64_t>((num + den - 1) / den);
}

struct Captured {
    std::int64_t sample;
    SequencerEvent::Kind kind;
    SynthTarget target;
    int value;
    int velocity;
    bool operator==(const Captured& o) const {
        return sample == o.sample && kind == o.kind && target == o.target &&
               value == o.value && velocity == o.velocity;
    }
};

// Runs a standalone sequencer for `total` samples in blocks of `block` and
// records every event with its absolute sample position.
std::vector<Captured> capture(Sequencer& seq, std::int64_t total, std::size_t block) {
    std::vector<Captured> out;
    out.reserve(4096);
    std::int64_t pos = 0;
    while (pos < total) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(block), total - pos));
        const std::int64_t base = pos;
        seq.process(n, [&](const SequencerEvent& e) {
            out.push_back({base + static_cast<std::int64_t>(e.offset), e.kind,
                           e.target, e.value, e.velocity});
        });
        pos += static_cast<std::int64_t>(n);
    }
    return out;
}

Sequencer& freshSequencer(int fs = 48000) {
    static Sequencer seq;
    seq.stop();
    seq.clock().setSampleRate(fs);
    seq.clock().setTempo(120.0);
    seq.clock().setTimeSignature(4, 4);
    seq.pattern().clear();
    seq.pattern().configure(TimeSignature{4, 4}, 1, 240);
    seq.setSeed(1);
    seq.process(1, [](const SequencerEvent&) {});  // flush any notes
    seq.clock().reset();
    return seq;
}

// ---------------------------------------------------------------------------
void clockTests() {
    std::cout << "\n-- Musical time and clock --\n";

    {
        MusicalClock c(48000);
        c.setTempo(120.0);
        check(c.sampleOfTick(1) == 25 && c.sampleOfTick(960) == 24000 &&
              c.sampleOfTick(3840 * 100) == 9600000 && c.samplesPerTick() == 25.0,
              "CL1 120 BPM at 48 kHz: 25 samples/tick, quarter = 24000 samples");
    }
    {
        bool ok = true;
        const int rates[] = {44100, 48000};
        const double bpms[] = {60.0, 90.0, 120.0, 140.0, 123.456, 97.531, 20.0, 300.0};
        for (int fs : rates) {
            for (double bpm : bpms) {
                MusicalClock c(fs);
                ok = ok && c.setTempo(bpm);
                const std::int64_t milli = static_cast<std::int64_t>(std::llround(bpm * 1000.0));
                for (std::int64_t t = 0; t < 200000; t += 37) {
                    ok = ok && c.sampleOfTick(t) == exactSample(t, fs, milli);
                }
                // One hour later.
                const std::int64_t hour = static_cast<std::int64_t>(bpm / 60.0 * 3600.0 * 960.0);
                for (std::int64_t t = hour; t < hour + 5000; t += 7) {
                    ok = ok && c.sampleOfTick(t) == exactSample(t, fs, milli);
                }
            }
        }
        check(ok, "CL2 44.1/48 kHz x 60/90/120/140/123.456/97.531/20/300 BPM: every tick "
                  "matches the exact rational formula, also one hour in");
    }
    {
        MusicalClock c(44100);
        c.setTempo(90.0);  // 30.625 samples per tick
        check(c.sampleOfTick(1) == 31 && c.sampleOfTick(8) == 245 &&
              c.sampleOfTick(960) == 29400,
              "CL3 90 BPM at 44.1 kHz: 30.625 samples/tick, tick 1 at 31 (first sample at or after 30.625)");
    }
    {
        // 24 hours at 123.456 BPM, 44.1 kHz: exact vs a naive running sum.
        MusicalClock c(44100);
        c.setTempo(123.456);
        const std::int64_t ticks = static_cast<std::int64_t>(123.456 / 60.0 * 86400.0 * 960.0);
        const long double spt = 44100.0L * 60.0L / (123.456L * 960.0L);
        const long double exactTime = static_cast<long double>(ticks) * spt;
        const std::int64_t s = c.sampleOfTick(ticks);
        double naive = 0.0;
        const double sptD = 44100.0 * 60.0 / (123.456 * 960.0);
        for (std::int64_t t = 0; t < ticks; ++t) naive += sptD;
        const double naiveError = std::abs(naive - static_cast<double>(exactTime));
        check(static_cast<long double>(s) >= exactTime - 1e-6L &&
              static_cast<long double>(s) < exactTime + 1.0L && s == exactSample(ticks, 44100, 123456),
              "CL4 24 h at 123.456 BPM / 44.1 kHz: tick " + std::to_string(ticks) +
                  " lands within 1 sample of exact (a naive running sum drifts " +
                  num(naiveError) + " samples)");
    }
    {
        MusicalClock c(48000);
        c.setTempo(97.531);
        bool ok = true;
        for (std::int64_t s = 0; s < 3000000; s += 9973) {
            const std::int64_t t = c.firstTickAtOrAfter(s);
            ok = ok && c.sampleOfTick(t) >= s && c.sampleOfTick(t - 1) < s;
        }
        check(ok, "CL5 firstTickAtOrAfter(s) is the first tick delivered at or after s");
    }
    {
        bool ok = true;
        const int sigs[][2] = {{4, 4}, {3, 4}, {7, 8}, {6, 8}, {5, 4}, {12, 16}, {1, 32}};
        for (const auto& sig : sigs) {
            TimeSignature ts{sig[0], sig[1]};
            for (std::int64_t t = 0; t < 200000; t += 113) {
                ok = ok && toAbsoluteTick(toBarBeatTick(t, ts), ts) == t;
            }
        }
        const TimeSignature s78{7, 8};
        const BarBeatTick b = toBarBeatTick(3360 + 2 * 480 + 17, s78);
        check(ok && s78.ticksPerBeat() == 480 && s78.ticksPerBar() == 3360 &&
              b.bar == 1 && b.beat == 2 && b.tick == 17 &&
              TimeSignature{6, 8}.ticksPerBar() == 2880 &&
              TimeSignature{12, 16}.ticksPerBar() == 2880,
              "CL6 Bar/beat/tick round-trips in 4/4, 3/4, 7/8, 6/8, 5/4, 12/16, 1/32");
    }
    {
        MusicalClock c(48000);
        const bool idle = c.advance(1000) == 0 && c.samplePosition() == 0;
        c.start();
        c.advance(24000);
        const bool moved = c.samplePosition() == 24000 && c.tickPosition() == 960;
        c.stop();
        c.advance(500);
        const bool held = c.samplePosition() == 24000;
        c.reset();
        check(idle && moved && held && c.samplePosition() == 0 && c.tickPosition() == 0 &&
              !c.playing(),
              "CL7 Clock advances only while playing; stop holds; reset returns to 0");
    }
    {
        MusicalClock c(48000);
        c.setTempo(120.0);
        c.start();
        c.advance(24010);                 // just past tick 960 (sample 24000)
        c.setTempo(60.0);                 // next tick boundary: tick 961 at 24025
        const bool before = c.sampleOfTick(961) == 24025;
        const bool after = c.sampleOfTick(962) == 24075 &&
                           c.sampleOfTick(961 + 960) == 24025 + 48000;
        check(before && after && c.tempo() == 60.0,
              "CL8 Tempo change applies at the next tick boundary, then the new spacing exactly");
    }
    {
        MusicalClock c(48000);
        check(!c.setTempo(std::numeric_limits<double>::quiet_NaN()) && !c.setTempo(0.0) &&
              !c.setTempo(19.9) && !c.setTempo(300.01) && !c.setSampleRate(100) &&
              !c.setSampleRate(400000) && !c.setTimeSignature(0, 4) &&
              !c.setTimeSignature(4, 3) && !c.setTimeSignature(33, 4) &&
              c.tempo() == 120.0 && c.sampleRate() == 48000,
              "CL9 Invalid tempo, sample rate and time signature are rejected");
    }
}

// ---------------------------------------------------------------------------
void patterns() {
    std::cout << "\n-- Patterns --\n";

    static Pattern p;
    check(p.configure({4, 4}, 1, 240) && p.stepCount() == 16 &&
          p.configure({4, 4}, 4, 240) && p.stepCount() == 64 &&
          p.configure({4, 4}, 16, 240) && p.stepCount() == 256 &&
          p.configure({3, 4}, 1, 240) && p.stepCount() == 12 &&
          p.configure({7, 8}, 1, 240) && p.stepCount() == 14 &&
          p.configure({4, 4}, 2, 160) && p.stepCount() == 48,
          "PA1 Lengths: 1/4/16 bars of 16ths, 3/4, 7/8, 16th triplets");
    check(!p.configure({4, 4}, 16, 120) && !p.configure({7, 8}, 1, 960) &&
          !p.configure({4, 4}, 0, 240) && !p.configure({4, 4}, 17, 240) &&
          !p.configure({4, 4}, 1, 59) && !p.configure({4, 3}, 1, 240) &&
          p.stepCount() == 48,
          "PA2 Too many steps, non-dividing step, bad bars/step/signature rejected; shape unchanged");

    p.configure({4, 4}, 1, 240);
    DrumStep good;
    good.enabled = true;
    DrumStep v0 = good; v0.velocity = 0;
    DrumStep v128 = good; v128.velocity = 128;
    DrumStep pm = good; pm.probability = -1;
    DrumStep pp = good; pp.probability = 101;
    DrumStep mt = good; mt.microTicks = 121;
    DrumStep edge = good; edge.velocity = 127; edge.probability = 0; edge.microTicks = -120;
    check(p.setStep(26, 15, good) && !p.setStep(27, 0, good) && !p.setStep(0, 16, good) &&
          !p.setStep(0, 0, v0) && !p.setStep(0, 0, v128) && !p.setStep(0, 0, pm) &&
          !p.setStep(0, 0, pp) && !p.setStep(0, 0, mt) && p.setStep(0, 0, edge) &&
          p.step(0, 0)->velocity == 127 && p.step(27, 0) == nullptr,
          "PA3 Step validation: pads 0..26, velocity 1..127, probability 0..100, micro +/- half a step");

    NoteEvent n;
    NoteEvent badNote = n; badNote.note = 128;
    NoteEvent badStart = n; badStart.startTick = 3840;
    NoteEvent badLen = n; badLen.lengthTicks = 0;
    NoteEvent badTarget = n; badTarget.target = static_cast<SynthTarget>(7);
    const int first = p.addNote(n);
    const bool rejects = p.addNote(badNote) < 0 && p.addNote(badStart) < 0 &&
                         p.addNote(badLen) < 0 && p.addNote(badTarget) < 0;
    for (int i = 1; i < 512; ++i) {
        NoteEvent x = n;
        x.note = i % 128;
        p.addNote(x);
    }
    const bool full = p.noteCount() == 512 && p.addNote(n) == -1;
    p.removeNote(0);
    check(first == 0 && rejects && full && p.noteCount() == 511 && p.note(0)->note == 1,
          "PA4 Notes: validated, 512 capacity, removal keeps order");

    p.clearPad(26);
    const bool padCleared = !p.step(26, 15)->enabled && p.step(0, 0)->enabled;
    p.clear();
    check(padCleared && p.noteCount() == 0 && !p.step(0, 0)->enabled && p.stepCount() == 16,
          "PA5 clearPad / clear reset data but keep the shape");
}

// ---------------------------------------------------------------------------
void drums() {
    std::cout << "\n-- Drum sequencing --\n";

    {
        Sequencer& s = freshSequencer();
        s.pattern().configure({4, 4}, 2, 240);  // 32 steps
        for (std::size_t pad = 0; pad < 27; ++pad) s.pattern().setHit(pad, pad, 1 + static_cast<int>(pad));
        s.start();
        const auto ev = capture(s, 2 * 4 * 48000, 256);  // two loops of 2 bars
        bool ok = ev.size() == 54;
        for (std::size_t i = 0; i < ev.size() && ok; ++i) {
            const std::size_t pad = i % 27;
            const std::int64_t loop = static_cast<std::int64_t>(i / 27);
            ok = ev[i].kind == SequencerEvent::Kind::Drum && ev[i].value == static_cast<int>(pad) &&
                 ev[i].velocity == 1 + static_cast<int>(pad) &&
                 ev[i].sample == (loop * 7680 + static_cast<std::int64_t>(pad) * 240) * 25;
        }
        check(ok, "DR1 All 27 pads, each on its own step, at exact samples for two loops");
    }
    {
        Sequencer& s = freshSequencer();
        for (std::size_t pad = 0; pad < 27; ++pad) s.pattern().setHit(pad, 4, 100);
        s.start();
        const auto ev = capture(s, 48000, 64);
        bool ordered = ev.size() == 27;
        for (std::size_t i = 0; i < ev.size() && ordered; ++i) {
            ordered = ev[i].sample == 24000 && ev[i].value == static_cast<int>(i);
        }
        check(ordered, "DR2 27 simultaneous hits: same sample, delivered in pad order 0..26");
    }
    {
        Sequencer& s = freshSequencer();
        DrumStep st;
        st.enabled = true;
        st.microTicks = 30;
        s.pattern().setStep(5, 2, st);
        st.microTicks = -60;
        s.pattern().setStep(6, 0, st);  // loop 1's step 0 lands before the loop boundary
        s.start();
        const auto ev = capture(s, 2 * 96000 + 1000, 100);
        bool found5 = false;
        bool found6 = false;
        for (const auto& e : ev) {
            if (e.value == 5 && e.sample == (480 + 30) * 25) found5 = true;
            if (e.value == 6 && e.sample == (3840 - 60) * 25) found6 = true;
        }
        check(found5 && found6,
              "DR3 Microtiming: +30 ticks lands 750 samples late; -60 on step 0 lands before the loop starts");
    }
    {
        Sequencer& s = freshSequencer();
        DrumStep never;
        never.enabled = true;
        never.probability = 0;
        DrumStep always = never;
        always.probability = 100;
        DrumStep half = never;
        half.probability = 50;
        s.pattern().setStep(0, 0, never);
        s.pattern().setStep(1, 0, always);
        s.pattern().setStep(2, 0, half);
        s.start();
        const auto ev = capture(s, 1000LL * 96000, 4096);  // 1000 loops
        int c0 = 0, c1 = 0, c2 = 0;
        for (const auto& e : ev) {
            c0 += e.value == 0;
            c1 += e.value == 1;
            c2 += e.value == 2;
        }
        Sequencer& again = freshSequencer();
        again.pattern().setStep(2, 0, half);
        again.start();
        int r = 0;
        for (const auto& e : capture(again, 1000LL * 96000, 333)) r += e.value == 2;
        Sequencer& other = freshSequencer();
        other.setSeed(12345);
        other.pattern().setStep(2, 0, half);
        other.start();
        int o = 0;
        for (const auto& e : capture(other, 1000LL * 96000, 4096)) o += e.value == 2;
        check(c0 == 0 && c1 == 1000, "DR4 Probability 0 never fires, 100 always fires (1000 loops)");
        check(c2 > 430 && c2 < 570 && r == c2 && o != c2,
              "DR5 Probability 50: " + std::to_string(c2) +
                  "/1000, identical for the same seed (any block size), different seed gives " +
                  std::to_string(o));
    }
}

// ---------------------------------------------------------------------------
void synths() {
    std::cout << "\n-- Synth sequencing --\n";

    {
        Sequencer& s = freshSequencer();
        NoteEvent fm{true, SynthTarget::Fm, 72, 90, 100, 0, 480};
        NoteEvent mono{true, SynthTarget::Mono, 36, 110, 100, 960, 240};
        NoteEvent poly{true, SynthTarget::Poly, 60, 80, 100, 1920, 1000};
        s.pattern().addNote(fm);
        s.pattern().addNote(mono);
        s.pattern().addNote(poly);
        s.start();
        const auto ev = capture(s, 96000, 128);
        const std::vector<Captured> expected = {
            {0, SequencerEvent::Kind::NoteOn, SynthTarget::Fm, 72, 90},
            {480 * 25, SequencerEvent::Kind::NoteOff, SynthTarget::Fm, 72, 0},
            {960 * 25, SequencerEvent::Kind::NoteOn, SynthTarget::Mono, 36, 110},
            {1200 * 25, SequencerEvent::Kind::NoteOff, SynthTarget::Mono, 36, 0},
            {1920 * 25, SequencerEvent::Kind::NoteOn, SynthTarget::Poly, 60, 80},
            {2920 * 25, SequencerEvent::Kind::NoteOff, SynthTarget::Poly, 60, 0},
            {3840 * 25, SequencerEvent::Kind::NoteOn, SynthTarget::Fm, 72, 90}};
        bool ok = ev.size() == 6;
        for (std::size_t i = 0; i < 6 && ok; ++i) ok = ev[i] == expected[i];
        check(ok, "SY1 FM, mono and poly notes: on and off at exact samples with their durations");
    }
    {
        Sequencer& s = freshSequencer();
        s.pattern().addNote({true, SynthTarget::Poly, 60, 100, 100, 0, 960});
        s.pattern().addNote({true, SynthTarget::Poly, 64, 100, 100, 240, 960});
        s.pattern().addNote({true, SynthTarget::Poly, 67, 100, 100, 480, 960});
        s.start();
        const auto ev = capture(s, 48000, 64);
        int on = 0;
        int maxHeld = 0;
        int held = 0;
        for (const auto& e : ev) {
            if (e.kind == SequencerEvent::Kind::NoteOn) { ++on; ++held; }
            if (e.kind == SequencerEvent::Kind::NoteOff) --held;
            maxHeld = std::max(maxHeld, held);
        }
        check(on == 3 && maxHeld == 3 && held == 0,
              "SY2 Overlapping poly notes: three held at once, all released");
    }
    {
        // Note in the last step, two steps long: its note-off lands in the
        // next loop; across 50 loops every off is exact.
        Sequencer& s = freshSequencer();
        s.pattern().addNote({true, SynthTarget::Mono, 40, 100, 100, 3600, 480});
        s.start();
        const auto ev = capture(s, 50LL * 96000 + 20000, 777);
        bool ok = true;
        int offs = 0;
        for (const auto& e : ev) {
            if (e.kind == SequencerEvent::Kind::NoteOff) {
                ok = ok && e.sample == (static_cast<std::int64_t>(offs) * 3840 + 4080) * 25;
                ++offs;
            }
        }
        check(ok && offs == 50, "SY3 Note-offs across the pattern boundary are exact (50 loops)");
    }
    {
        // Same pitch re-struck at the instant it ends: off before on.
        Sequencer& s = freshSequencer();
        s.pattern().addNote({true, SynthTarget::Poly, 50, 100, 100, 0, 3840});
        s.start();
        const auto ev = capture(s, 96000 + 100, 4096);
        check(ev.size() == 3 && ev[1].sample == 96000 &&
              ev[1].kind == SequencerEvent::Kind::NoteOff &&
              ev[2].kind == SequencerEvent::Kind::NoteOn && ev[2].sample == 96000,
              "SY4 At the same sample a note-off comes before a note-on");
    }
    {
        Sequencer& s = freshSequencer();
        s.pattern().addNote({true, SynthTarget::Fm, 70, 100, 100, 0, 3000});
        s.pattern().addNote({true, SynthTarget::Poly, 55, 100, 100, 0, 3000});
        s.start();
        auto ev = capture(s, 10000, 1000);
        s.stop();
        const auto after = capture(s, 2000, 1000);
        check(s.activeNoteCount() == 0 && after.size() == 2 && after[0].sample == 0 &&
              after[0].kind == SequencerEvent::Kind::NoteOff &&
              after[1].kind == SequencerEvent::Kind::NoteOff,
              "SY5 stop() releases sounding sequenced notes at the start of the next block");
    }
}

// ---------------------------------------------------------------------------
void scheduler() {
    std::cout << "\n-- Scheduler --\n";

    auto busy = [](Sequencer& s) {
        s.clock().setTempo(137.25);
        s.pattern().configure({7, 8}, 2, 240);  // 28 steps
        for (std::size_t pad = 0; pad < 27; ++pad) {
            for (std::size_t k = pad % 3; k < 28; k += 3 + pad % 4) {
                DrumStep st;
                st.enabled = true;
                st.velocity = 30 + static_cast<int>((pad * 7 + k * 3) % 97);
                st.probability = 40 + static_cast<int>((pad * 13 + k) % 61);
                st.microTicks = static_cast<int>((pad * 17 + k * 5) % 121) - 60;
                s.pattern().setStep(pad, k, st);
            }
        }
        for (int i = 0; i < 40; ++i) {
            s.pattern().addNote({true, static_cast<SynthTarget>(i % 3), 36 + (i * 7) % 48,
                                 40 + i, 50 + i, static_cast<std::int64_t>(i) * 157 % 6720,
                                 60 + static_cast<std::int64_t>(i) * 211 % 4000});
        }
        s.setSeed(99);
        s.start();
    };
    {
        Sequencer& a = freshSequencer(44100);
        busy(a);
        const auto reference = capture(a, 20LL * 44100, 4096);
        bool same = true;
        for (std::size_t block : {1u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 4096u, 1000u, 777u}) {
            Sequencer& b = freshSequencer(44100);
            busy(b);
            same = same && capture(b, 20LL * 44100, block) == reference;
        }
        check(same && reference.size() > 1000,
              "SC1 " + std::to_string(reference.size()) +
                  " events (7/8, 137.25 BPM, microtiming, probability, notes) identical for "
                  "blocks 1,16,32,64,128,256,512,1024,4096 (and 777, 1000)");
    }
    {
        // Block size equal to the step length: every hit at offset 0.
        Sequencer& s = freshSequencer();
        s.pattern().setHit(0, 0);
        s.pattern().setHit(0, 1);
        s.pattern().setHit(0, 15);
        bool atZero = true;
        std::size_t seen = 0;
        s.start();
        for (int b = 0; b < 32; ++b) {
            s.process(6000, [&](const SequencerEvent& e) {
                atZero = atZero && e.offset == 0;
                ++seen;
            });
        }
        // Block 4096: offsets inside blocks.
        Sequencer& t = freshSequencer();
        t.pattern().setHit(0, 1);  // sample 6000 = block 1, offset 1904
        t.start();
        std::size_t off = 0;
        int whichBlock = -1;
        for (int b = 0; b < 3; ++b) {
            t.process(4096, [&](const SequencerEvent& e) { off = e.offset; whichBlock = b; });
        }
        check(atZero && seen == 6 && whichBlock == 1 && off == 6000 - 4096,
              "SC2 Events on a block boundary land at offset 0; others at their exact offset in a later block");
    }
    {
        Sequencer& s = freshSequencer();
        s.clock().setTempo(300.0);
        s.pattern().configure({4, 4}, 16, 240);
        for (std::size_t pad = 0; pad < 27; ++pad)
            for (std::size_t k = 0; k < 256; ++k) s.pattern().setHit(pad, k, 100);
        s.start();
        std::size_t delivered = 0;
        s.process(96000, [&](const SequencerEvent&) { ++delivered; });
        check(delivered == Sequencer::MaxEventsPerBlock && s.droppedEventCount() > 0,
              "SC3 Over-full block: " + std::to_string(delivered) + " delivered, " +
                  std::to_string(s.droppedEventCount()) + " dropped and counted");
        static AudioPipeline<16> p;
        int ok = 0;
        for (int i = 0; i < 1100; ++i) ok += p.drums().scheduleTrigger(0, 100, 5) ? 1 : 0;
        const bool drumFull = ok == 1024 && p.drums().droppedScheduledCount() == 76;
        int synthOk = 0;
        for (int i = 0; i < 1030; ++i) synthOk += p.scheduleNoteOn(SynthTarget::Poly, 60, 90, 5) ? 1 : 0;
        Buffer out(16);
        p.render(out.data(), out.size());
        check(drumFull && synthOk == 1024 && p.droppedScheduledSynthCount() == 6 &&
              !p.drums().scheduleTrigger(27, 100, 0) && !p.scheduleNoteOn(SynthTarget::Fm, 128, 90, 0),
              "SC4 Scheduled lists hold 1024 per block; overflow and invalid events rejected and counted");
    }
    {
        Sequencer& s = freshSequencer();
        for (std::size_t k = 0; k < 16; k += 2) s.pattern().setHit(3, k);
        s.pattern().addNote({true, SynthTarget::Mono, 33, 100, 100, 120, 600});
        s.start();
        const std::int64_t minutes = 10;
        const auto ev = capture(s, minutes * 60 * 48000, 512);
        std::size_t drumsSeen = 0;
        std::int64_t lastDrum = -1;
        for (const auto& e : ev) {
            if (e.kind == SequencerEvent::Kind::Drum) {
                ++drumsSeen;
                lastDrum = e.sample;
            }
        }
        // Last hit: bar 299, step 14.
        const bool lastExact = lastDrum == (299LL * 3840 + 14 * 240) * 25;
        // 10 minutes at 120 BPM = 300 bars; 8 hits per bar.
        check(drumsSeen == 300 * 8 && s.clock().samplePosition() == minutes * 60 * 48000 &&
              s.clock().tickPosition() == 300 * 3840 && lastExact,
              "SC5 10 minutes: 2400 hits, clock exactly at bar 300 (tick " +
                  std::to_string(s.clock().tickPosition()) + ")");
    }
}

// ---------------------------------------------------------------------------
using Pipeline = AudioPipeline<16>;

const Buffer& marker(std::size_t pad) {
    static std::vector<Buffer> samples;
    if (samples.empty()) {
        for (std::size_t p = 0; p < 27; ++p) {
            samples.push_back(Buffer(1, StereoFrame{0.001 * static_cast<double>(p + 1), 0.0}));
        }
    }
    return samples[pad];
}

void audio() {
    std::cout << "\n-- Pipeline audio --\n";

    {
        // Each pad's one-frame marker appears exactly at its step's sample.
        static Pipeline p;
        for (std::size_t pad = 0; pad < 27; ++pad) p.drums().assignSample(pad, &marker(pad));
        auto& s = p.sequencer();
        s.pattern().configure({4, 4}, 2, 240);
        for (std::size_t pad = 0; pad < 27; ++pad) s.pattern().setHit(pad, pad, 127);
        s.start();
        Buffer out(2 * 96000);
        for (std::size_t pos = 0; pos < out.size(); pos += 333) {
            p.render(out.data() + pos, std::min<std::size_t>(333, out.size() - pos));
        }
        bool exact = true;
        std::size_t nonZero = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i].left != 0.0) ++nonZero;
        }
        for (std::size_t pad = 0; pad < 27; ++pad) {
            const std::size_t at = pad * 240 * 25;
            exact = exact && std::abs(out[at].left - 0.001 * static_cast<double>(pad + 1)) < 1e-15;
        }
        check(exact && nonZero == 27,
              "AU1 Sequenced pads sound on exactly their samples (27 one-frame markers, nothing else)");
        p.sequencer().stop();
    }
    {
        // Velocity and choke through the real DrumEngine.
        static Pipeline p;
        static const Buffer longSample(48000, StereoFrame{0.5, 0.5});
        p.drums().assignSample(2, &longSample);
        p.drums().assignSample(3, &longSample);
        p.drums().setChokeGroup(2, 1);
        p.drums().setChokeGroup(3, 1);
        p.drums().setReleaseFadeFrames(4);
        auto& s = p.sequencer();
        s.pattern().setHit(3, 0, 64);
        s.pattern().setHit(2, 2, 127);
        s.start();
        Buffer out(14000);
        p.render(out.data(), out.size());
        const bool velocity = std::abs(out[100].left - 0.5 * 64.0 / 127.0) < 1e-12;
        const std::size_t chokeAt = 2 * 240 * 25;  // 12000
        const bool choked = std::abs(out[chokeAt].left - (0.5 * 64.0 / 127.0 + 0.5)) < 1e-12 &&
                            std::abs(out[chokeAt + 4].left - 0.5) < 1e-12 &&
                            p.drums().isPadReleasing(3) == false && !p.drums().isPadActive(3);
        check(velocity, "AU2 Sequenced velocity 64 plays at 64/127 of the pad level");
        check(choked, "AU3 Sequenced choke: pad 2 at sample 12000 fades pad 3 out over 4 frames");
    }
    {
        // Sequenced synth notes = the same notes scheduled by hand.
        static Pipeline seq;
        static Pipeline manual;
        seq.sequencer().pattern().addNote({true, SynthTarget::Poly, 60, 100, 100, 100, 700});
        seq.sequencer().pattern().addNote({true, SynthTarget::Fm, 79, 80, 100, 333, 300});
        seq.sequencer().pattern().addNote({true, SynthTarget::Mono, 40, 120, 100, 0, 1500});
        seq.sequencer().start();
        Buffer a(48000);
        seq.render(a.data(), a.size());
        Buffer b(48000);
        manual.scheduleNoteOn(SynthTarget::Mono, 40, 120, 0);
        manual.scheduleNoteOn(SynthTarget::Poly, 60, 100, 2500);
        manual.scheduleNoteOn(SynthTarget::Fm, 79, 80, 8325);
        manual.scheduleNoteOff(SynthTarget::Fm, 79, 15825);
        manual.scheduleNoteOff(SynthTarget::Poly, 60, 20000);
        manual.scheduleNoteOff(SynthTarget::Mono, 40, 37500);
        manual.render(b.data(), b.size());
        bool same = true;
        for (std::size_t i = 0; i < a.size(); ++i) {
            same = same && a[i].left == b[i].left && a[i].right == b[i].right;
        }
        check(same, "AU4 Sequenced FM/mono/poly notes are bit-identical to hand-scheduled ones");
    }
    {
        // Manual posting still works while the sequencer runs, merged in order.
        static Pipeline p;
        for (std::size_t pad = 0; pad < 27; ++pad) p.drums().assignSample(pad, &marker(pad));
        p.sequencer().pattern().setHit(0, 0, 127);
        p.sequencer().start();
        p.drums().postTrigger(1, 127, 10);
        p.drums().trigger(2, 127);
        Buffer out(64);
        p.render(out.data(), out.size());
        check(std::abs(out[0].left - (0.001 + 0.003)) < 1e-15 &&
              std::abs(out[10].left - 0.002) < 1e-15,
              "AU5 Manual trigger() and postTrigger() still work alongside the sequencer");
    }
    {
        // Documented tie rule: at the same frame, scheduled (sequencer)
        // events are applied before queued (posted) ones. Choke order and
        // mono last-note priority make the order audible.
        static Pipeline p;
        static const Buffer level1(48000, StereoFrame{0.25, 0.0});
        static const Buffer level2(48000, StereoFrame{0.0, 0.5});
        p.drums().assignSample(7, &level1);
        p.drums().assignSample(8, &level2);
        p.drums().setChokeGroup(7, 3);
        p.drums().setChokeGroup(8, 3);
        p.drums().setReleaseFadeFrames(4);
        p.drums().scheduleTrigger(7, 127, 10);   // as the sequencer would
        p.drums().postTrigger(8, 127, 10);        // a producer thread's hit
        p.scheduleNoteOn(SynthTarget::Mono, 40, 100, 0);
        p.postNoteOn(SynthTarget::Mono, 52, 100, 0);
        Buffer out(64);
        p.render(out.data(), out.size());
        // The mono note is centred (adds equally to L and R), so R - L
        // isolates the drums: only pad 8 (right, 0.5) may remain.
        check(std::abs((out[20].right - out[20].left) - 0.5) < 1e-12 &&
              !p.drums().isPadActive(7) && p.drums().isPadActive(8) &&
              p.mono().voiceNote(0) == 52,
              "SC6 Same-frame tie: scheduled first, then queued (queued pad chokes, queued mono note sounds)");
    }
    {
        // Full pipeline (all effects on) is identical for every block size.
        auto run = [](std::size_t block) {
            static Pipeline p;
            static Buffer kick = [] {
                Buffer k(9600);
                for (std::size_t i = 0; i < k.size(); ++i) {
                    const double t = static_cast<double>(i) / SAMPLE_RATE;
                    const double v = 0.5 * std::sin(2.0 * PI * 60.0 * t) * std::exp(-t * 10.0);
                    k[i] = {v, v};
                }
                return k;
            }();
            p.sequencer().stop();
            p.render(nullptr, 0);
            Buffer flush(1);
            p.render(flush.data(), 1);  // deliver the stop's note-offs
            p.drums().stopAll();
            p.mono().reset();
            p.fm().reset();
            p.poly().reset();
            for (std::size_t i = 0; i < p.effects().size(); ++i) p.effects().slot(i)->setBypassed(false, true);
            p.effects().resetAll();
            p.drums().assignSample(0, &kick);
            p.drums().assignSample(5, &kick);
            p.setMasterLevel(0.6);
            auto& s = p.sequencer();
            s.clock().reset();
            s.clock().setTempo(128.5);
            s.pattern().clear();
            s.pattern().configure({4, 4}, 1, 240);
            for (std::size_t k = 0; k < 16; k += 4) s.pattern().setHit(0, k, 110);
            DrumStep ghost;
            ghost.enabled = true;
            ghost.velocity = 50;
            ghost.probability = 60;
            ghost.microTicks = 37;
            for (std::size_t k = 2; k < 16; k += 4) s.pattern().setStep(5, k, ghost);
            s.pattern().addNote({true, SynthTarget::Mono, 36, 110, 100, 0, 700});
            s.pattern().addNote({true, SynthTarget::Poly, 60, 80, 100, 960, 1500});
            s.pattern().addNote({true, SynthTarget::Poly, 64, 80, 100, 960, 1500});
            s.pattern().addNote({true, SynthTarget::Fm, 76, 90, 80, 2880, 400});
            s.setSeed(7);
            s.start();
            Buffer out(5 * 48000);
            for (std::size_t pos = 0; pos < out.size(); pos += block) {
                p.render(out.data() + pos, std::min(block, out.size() - pos));
            }
            return out;
        };
        const Buffer reference = run(4096);
        bool same = true;
        std::string failedAt;
        for (std::size_t block : {1u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u}) {
            const Buffer o = run(block);
            bool eq = true;
            for (std::size_t i = 0; i < o.size(); ++i) {
                eq = eq && o[i].left == reference[i].left && o[i].right == reference[i].right;
            }
            if (!eq) failedAt += " " + std::to_string(block);
            same = same && eq;
        }
        double energy = 0.0;
        for (const auto& f : reference) energy += f.left * f.left;
        check(same && energy > 1.0,
              "AU6 Full pipeline audio (drums + 3 synths + all 6 effects) bit-identical for blocks "
              "1,16,32,64,128,256,512,1024,4096" + failedAt);
    }
}

// ---------------------------------------------------------------------------
void realtime() {
    std::cout << "\n-- Real-time safety --\n";

    {
        static Pipeline p;
        static Buffer hit(4800, StereoFrame{0.2, 0.2});
        for (std::size_t pad = 0; pad < 27; ++pad) p.drums().assignSample(pad, &hit);
        auto& s = p.sequencer();
        s.clock().setTempo(174.0);
        for (std::size_t pad = 0; pad < 27; ++pad)
            for (std::size_t k = pad % 2; k < 16; k += 2) s.pattern().setHit(pad, k, 90);
        for (int i = 0; i < 60; ++i)
            s.pattern().addNote({true, static_cast<SynthTarget>(i % 3), 40 + i % 30, 90, 70,
                                 static_cast<std::int64_t>(i) * 61, 200});
        for (std::size_t i = 0; i < p.effects().size(); ++i) p.effects().slot(i)->setBypassed(false, true);
        s.start();
        StereoFrame block[512]{};
        const std::size_t before = g_allocations.load();
        for (int i = 0; i < 3000; ++i) {
            if (i % 500 == 250) s.postTransport({TransportCommandType::SetTempo, 140000 + i});
            if (i % 700 == 699) s.stop();
            if (i % 700 == 0) s.start();
            std::fill(block, block + 512, StereoFrame{});
            p.render(block, 1 + static_cast<std::size_t>(i * 37) % 512);
        }
        const std::size_t after = g_allocations.load();
        const bool none = after == before;
        check(none && p.undeliveredSequencerEventCount() == 0,
              "RT1 No heap allocation: 3000 blocks of dense 27-pad + note sequencing, transport changes, all effects");
    }
    {
        // Transport commands from one producer thread while audio renders.
        static Pipeline p;
        auto& s = p.sequencer();
        s.pattern().setHit(0, 0);
        std::atomic<bool> done{false};
        std::atomic<int> accepted{0};
        std::thread producer([&] {
            int ok = 0;
            for (int i = 0; i < 5000; ++i) {
                TransportCommand c;
                c.type = (i % 4 == 0) ? TransportCommandType::Start
                       : (i % 4 == 1) ? TransportCommandType::SetTempo
                       : (i % 4 == 2) ? TransportCommandType::Stop
                                      : TransportCommandType::Reset;
                c.milliBpm = 60000 + (i % 200) * 1000;
                ok += s.postTransport(c) ? 1 : 0;
            }
            accepted.store(ok);
            done.store(true);
        });
        Buffer block(64);
        for (int spins = 0; spins < 2000000 && !done.load(); ++spins) {
            p.render(block.data(), block.size());
        }
        producer.join();
        for (int i = 0; i < 4; ++i) p.render(block.data(), block.size());
        check(accepted.load() + static_cast<int>(s.droppedTransportCount()) == 5000,
              "RT2 Transport commands from a second thread: all applied or counted as dropped");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 025 - Sequencer Foundation\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    clockTests();
    patterns();
    drums();
    synths();
    scheduler();
    audio();
    realtime();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 025 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 025 PASSED\n";
    return EXIT_SUCCESS;
}
