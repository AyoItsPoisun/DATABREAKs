// DATABREAKs Engine 026 - recording and performance capture tests.
//
// Host-side assertion tests for core/sequencer/recorder.h,
// recorded_track.h and the recording/playback logic in sequencer.h:
// free/loop record, overdub, replace, punch, undo/redo, boundaries,
// block-size independence, capacities, threading and allocation.
// Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
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

// std::stable_sort and friends use the nothrow form: count and pair it too.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

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

using Kind = SequencerEvent::Kind;
__extension__ typedef __int128 i128;

// 120 BPM, 48 kHz, 4/4, one bar: 25 samples per tick, loop = 96000 samples.
constexpr std::int64_t kLoop = 96000;
constexpr std::int64_t kLoopFine = 3840 * 1024;

struct Captured {
    std::int64_t sample;
    Kind kind;
    SynthTarget target;
    int value;
    int velocity;
    bool operator==(const Captured& o) const {
        return sample == o.sample && kind == o.kind && target == o.target &&
               value == o.value && velocity == o.velocity;
    }
};

using Events = std::vector<Captured>;

std::unique_ptr<Sequencer> makeSequencer(double bpm = 120.0, int bars = 1) {
    auto seq = std::make_unique<Sequencer>(48000);
    seq->clock().setTempo(bpm);
    seq->pattern().configure(TimeSignature{4, 4}, bars, 240);
    return seq;
}

LiveEvent hit(int pad, std::int64_t sample, int velocity = 100) {
    return {LiveEventType::DrumHit, SynthTarget::Mono, pad, velocity, sample};
}
LiveEvent on(SynthTarget t, int note, std::int64_t sample, int velocity = 100) {
    return {LiveEventType::NoteOn, t, note, velocity, sample};
}
LiveEvent off(SynthTarget t, int note, std::int64_t sample) {
    return {LiveEventType::NoteOff, t, note, 0, sample};
}

struct Script {
    std::vector<LiveEvent> live;  // sorted by sampleTime (stable)
    std::vector<std::pair<std::int64_t, std::function<void(Sequencer&)>>> actions;
};

// Runs `seq` for `total` samples in blocks of `block`. All sample positions
// in the script and in the result are relative to the clock position when
// run() starts. Actions run at the start of the block that begins at their
// sample (they must be multiples of 4096 to be block-size independent for
// every tested size). Live events are handed to the sequencer just before
// the block that contains them (timestamps converted to absolute clock
// samples).
Events run(Sequencer& seq, Script script, std::int64_t total, std::size_t block) {
    std::stable_sort(script.live.begin(), script.live.end(),
                     [](const LiveEvent& a, const LiveEvent& b) {
                         return a.sampleTime < b.sampleTime;
                     });
    Events out;
    out.reserve(8192);
    std::size_t li = 0;
    std::size_t ai = 0;
    std::int64_t pos = 0;
    const std::int64_t origin = seq.clock().samplePosition();
    while (pos < total) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(block), total - pos));
        while (ai < script.actions.size() && script.actions[ai].first <= pos) {
            script.actions[ai].second(seq);
            ++ai;
        }
        const std::int64_t end = pos + static_cast<std::int64_t>(n);
        while (li < script.live.size() && script.live[li].sampleTime < end) {
            LiveEvent e = script.live[li];
            e.sampleTime += origin;
            seq.liveEvent(e);
            ++li;
        }
        const std::int64_t base = pos;
        seq.process(n, [&](const SequencerEvent& e) {
            out.push_back({base + static_cast<std::int64_t>(e.offset), e.kind, e.target,
                           e.value, e.velocity});
        });
        pos = end;
    }
    return out;
}

Events between(const Events& all, std::int64_t from, std::int64_t to) {
    Events r;
    for (const auto& e : all) {
        if (e.sample >= from && e.sample < to) r.push_back(e);
    }
    return r;
}

Events shifted(Events e, std::int64_t by) {
    for (auto& x : e) x.sample += by;
    return e;
}

std::size_t countKind(const Events& all, Kind k) {
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(), [k](const Captured& e) { return e.kind == k; }));
}

std::vector<RecordedEvent> visibleEvents(const RecordedTrack& t) {
    std::vector<RecordedEvent> v;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t.visible(i)) v.push_back(t.at(i));
    }
    return v;
}

bool sameEvents(const std::vector<RecordedEvent>& a, const std::vector<RecordedEvent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!a[i].sameContent(b[i])) return false;
    }
    return true;
}

bool sameTrack(const RecordedTrack& a, const RecordedTrack& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!a.at(i).sameContent(b.at(i)) || a.at(i).createdPass != b.at(i).createdPass ||
            a.at(i).erasedPass != b.at(i).erasedPass || a.visible(i) != b.visible(i)) {
            return false;
        }
    }
    return a.undoablePassCount() == b.undoablePassCount() &&
           a.redoablePassCount() == b.redoablePassCount();
}

// Independent exact fine position of a sample at a fixed tempo from 0:
// largest f with f x N / (1024 D) <= s.
std::int64_t exactFine(std::int64_t s, int fs, std::int64_t milliBpm) {
    const i128 num = static_cast<i128>(s) * milliBpm * 960 * 1024;
    const i128 den = static_cast<i128>(fs) * 60000;
    return static_cast<std::int64_t>(num / den);
}

const std::size_t kBlocks[] = {1, 16, 32, 64, 128, 256, 512, 1024, 4096};

// ---------------------------------------------------------------------------
void timeTests() {
    std::cout << "\n-- Fine ticks and timestamps --\n";
    {
        bool ok = true;
        const int rates[] = {8000, 44100, 48000, 96000, 192000};
        const double bpms[] = {20.0, 60.0, 97.531, 120.0, 123.456, 300.0};
        for (int fs : rates) {
            for (double bpm : bpms) {
                MusicalClock c(fs);
                c.setTempo(bpm);
                for (std::int64_t t = 0; t < 50000; t += 97) {
                    ok = ok && c.sampleOfFine(t * 1024) == c.sampleOfTick(t);
                }
                for (std::int64_t s = 0; s < 400000; s += 7) {
                    const std::int64_t f = c.fineAtSample(s);
                    ok = ok && c.sampleOfFine(f) == s && c.sampleOfFine(f + 1) > s &&
                         f == exactFine(s, fs, c.tempoMilli());
                }
                ok = ok && c.fineAtSample(0) == 0;
            }
        }
        check(ok, "T1 Fine ticks: sampleOfFine(1024 t) == sampleOfTick(t); fineAtSample(s) is the "
                  "exact latest fine <= s and replays on s (5 rates x 6 tempos)");
    }
    {
        MusicalClock c(48000);
        c.setTempo(120.0);
        c.start();
        c.advance(1000);
        c.setTempo(133.3);  // new segment
        bool ok = true;
        for (std::int64_t s = 1000; s < 300000; s += 13) {
            ok = ok && c.sampleOfFine(c.fineAtSample(s)) == s;
        }
        check(ok, "T2 Fine timestamps replay exactly across a tempo change");
    }
}

// ---------------------------------------------------------------------------
void drumTests() {
    std::cout << "\n-- Free recording: drums --\n";
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        const std::int64_t times[] = {0, 6037, 24000, 47999, 60011, 90000};
        for (int i = 0; i < 6; ++i) sc.live.push_back(hit(i * 5, times[i], 69 + i * 9));
        sc.actions.push_back({kLoop - kLoop % 4096 + 4096, [](Sequencer& s) { s.disarm(); }});
        const Events ev = run(*seq, sc, 3 * kLoop, 256);
        const Events loop0 = between(ev, 0, kLoop);
        const Events loop1 = between(ev, kLoop, 2 * kLoop);
        const Events loop2 = between(ev, 2 * kLoop, 3 * kLoop);
        check(loop0.size() == 6 && countKind(loop0, Kind::Drum) == 6,
              "D1 Live hits sound once in the capture loop (no double from the recording)");
        check(loop1 == shifted(loop0, kLoop) && loop2 == shifted(loop0, 2 * kLoop),
              "D2 Recorded hits replay on exactly the captured sample in every later loop");
        const RecordedTrack& t = seq->recorder().track();
        bool fields = t.size() == 6 && t.undoablePassCount() == 1;
        for (std::size_t i = 0; i < t.size() && fields; ++i) {
            const RecordedEvent& e = t.at(i);
            fields = e.kind == RecordedEvent::Kind::Drum && e.captureSample == times[i] &&
                     e.startFine == exactFine(times[i], 48000, 120000) &&
                     e.captureFine == e.startFine && e.captureLoop == 0 &&
                     e.value == static_cast<int>(i) * 5 && e.createdPass == 1 &&
                     e.sequence == i + 1;
        }
        check(fields, "D3 Stored drum events: pad, velocity, raw fine timestamp, capture sample, "
                      "loop, pass, sequence");
        // 6037 samples = 241.48 ticks: nearest 16th step 1 (240), +1.48 ticks late.
        const RecordedEvent& e1 = t.at(1);
        check(e1.nearestStep(240) == 1 && e1.timingErrorFine(240) == e1.startFine - 240 * 1024 &&
                  e1.timingErrorFine(240) > 0 && t.at(3).nearestStep(240) == 8 &&
                  t.at(3).timingErrorFine(240) ==
                      exactFine(47999, 48000, 120000) - 8 * 240 * 1024 &&
                  t.at(3).timingErrorFine(240) < 0,
              "D4 Raw timing kept (no quantise): nearest grid step and signed error reported");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int p = 26; p >= 0; --p) sc.live.push_back(hit(p, 12000, 1 + p * 4));  // simultaneous
        for (int p = 0; p < 27; ++p) sc.live.push_back(hit(p, 30000 + p * 1000, 127 - p));
        const Events ev = run(*seq, sc, 2 * kLoop, 64);
        const Events at12k = between(ev, kLoop + 12000, kLoop + 12001);
        bool ordered = at12k.size() == 27;
        for (std::size_t i = 0; i < at12k.size() && ordered; ++i) {
            ordered = at12k[i].value == static_cast<int>(i) &&
                      at12k[i].velocity == 1 + static_cast<int>(i) * 4;
        }
        check(seq->recorder().track().size() == 54 && ordered,
              "D5 All 27 pads recorded with their velocities, incl. 27 simultaneous hits "
              "(replayed in pad order)");
        check(between(ev, kLoop, 2 * kLoop) == shifted(between(ev, 0, kLoop), kLoop),
              "D6 27-pad performance replays identically");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int k = 0; k < 64; ++k) sc.live.push_back(hit(7, 50000 + k * 3, 100));  // rapid
        for (int k = 0; k < 4; ++k) sc.live.push_back(hit(8, 70000, 90));            // repeated
        const Events ev = run(*seq, sc, 2 * kLoop, 128);
        check(seq->recorder().track().size() == 68 &&
                  between(ev, kLoop, 2 * kLoop) == shifted(between(ev, 0, kLoop), kLoop),
              "D7 Rapid (every 3 samples) and repeated same-sample hits all recorded and replayed");
    }
}

// ---------------------------------------------------------------------------
void boundaryTests() {
    std::cout << "\n-- Boundaries (start inclusive, end exclusive) --\n";
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(1, kLoop - 1));
        sc.live.push_back(hit(2, kLoop));
        sc.live.push_back(hit(3, kLoop + 1));
        run(*seq, sc, 4 * kLoop, 512);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 3 && t.at(0).captureLoop == 0 &&
                  t.at(0).startFine == kLoopFine - 41 && t.at(1).captureLoop == 1 &&
                  t.at(1).startFine == 0 && t.at(2).captureLoop == 1 && t.at(2).startFine == 40,
              "B1 Hit 1 sample before the loop end -> end of loop 0; exactly at the end -> "
              "loop 1 position 0; after -> loop 1");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.recorder().setRecordMode(RecordMode::Loop);
                                  s.arm();
                                  s.start();
                              }});
        sc.live.push_back(hit(1, kLoop - 1));
        sc.live.push_back(hit(2, kLoop));
        sc.live.push_back(hit(3, kLoop + 1));
        const Events ev = run(*seq, sc, 3 * kLoop, 1);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 3 && t.at(0).createdPass == 1 && t.at(1).createdPass == 2 &&
                  t.at(2).createdPass == 2 && t.undoablePassCount() == 2,
              "B2 Loop record: the hit exactly at the wrap belongs to the next loop's pass");
        const Events loop2 = between(ev, 2 * kLoop - 1, 2 * kLoop + 2);
        check(loop2.size() == 3 && loop2[0].sample == 2 * kLoop - 1 && loop2[0].value == 1 &&
                  loop2[1].sample == 2 * kLoop && loop2[1].value == 2 &&
                  loop2[2].sample == 2 * kLoop + 1 && loop2[2].value == 3,
              "B3 Boundary hits replay at loop end - 1, loop start and start + 1");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.recorder().setPunch(3840, 7680);  // samples [96000,192000)
                                  s.arm();
                                  s.start();
                              }});
        const std::int64_t times[] = {50000, 95999, 96000, 191999, 192000, 250000};
        for (std::int64_t x : times) sc.live.push_back(hit(4, x));
        const Events ev = run(*seq, sc, 4 * kLoop, 100);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 2 && t.at(0).captureSample == 96000 &&
                  t.at(1).captureSample == 191999 && t.undoablePassCount() == 1 &&
                  seq->recorder().openPass() == 0,
              "B4 Punch [in, out): in-sample recorded, out-sample not, outside not; pass closed "
              "at punch-out");
        check(countKind(between(ev, 0, 2 * kLoop), Kind::Drum) == 4,
              "B5 Playback continues while punching (all live hits sound)");
    }
    {
        // Punch at a non-integer tempo: boundary sample = delivery sample of the tick.
        auto seq = makeSequencer(123.456);
        const std::int64_t in = seq->clock().sampleOfTick(1000);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.recorder().setPunch(1000, 2000);
                                  s.arm();
                                  s.start();
                              }});
        sc.live.push_back(hit(4, in - 1));
        sc.live.push_back(hit(5, in));
        run(*seq, sc, 100000, 64);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 1 && t.at(0).value == 5 && t.at(0).startFine >= 1000 * 1024,
              "B6 Punch-in at 123.456 BPM: the tick's delivery sample is the first recorded");
    }
}

// ---------------------------------------------------------------------------
void synthTests() {
    std::cout << "\n-- Synth recording --\n";
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(on(SynthTarget::Mono, 40, 1000, 110));
        sc.live.push_back(off(SynthTarget::Mono, 40, 13500));
        sc.live.push_back(on(SynthTarget::Fm, 70, 20000, 90));
        sc.live.push_back(on(SynthTarget::Fm, 74, 30000, 80));   // overlaps
        sc.live.push_back(off(SynthTarget::Fm, 70, 40000));
        sc.live.push_back(off(SynthTarget::Fm, 74, 41234));
        for (int n : {60, 64, 67}) sc.live.push_back(on(SynthTarget::Poly, n, 50000, 70));
        sc.live.push_back(off(SynthTarget::Poly, 64, 60000));
        sc.live.push_back(off(SynthTarget::Poly, 60, 70000));
        sc.live.push_back(off(SynthTarget::Poly, 67, 80000));
        const Events ev = run(*seq, sc, 3 * kLoop, 333);
        const RecordedTrack& t = seq->recorder().track();
        bool lengths = t.size() == 6;
        const std::int64_t expectLen[] = {12500, 20000, 11234, 20000, 10000, 30000};
        for (std::size_t i = 0; i < t.size() && lengths; ++i) {
            const RecordedEvent& e = t.at(i);
            lengths = !e.pending && e.kind == RecordedEvent::Kind::Note &&
                      e.lengthFine == exactFine(e.captureSample + expectLen[i], 48000, 120000) -
                                          e.startFine;
        }
        check(lengths, "S1 Mono/FM/poly notes stored with exact durations (overlaps and "
                       "out-of-order note-offs)");
        check(between(ev, kLoop, 2 * kLoop) == shifted(between(ev, 0, kLoop), kLoop) &&
                  between(ev, 2 * kLoop, 3 * kLoop) == shifted(between(ev, 0, kLoop), 2 * kLoop),
              "S2 Recorded notes replay with note-on and note-off on the captured samples");
        check(countKind(between(ev, 0, kLoop), Kind::NoteOn) == 6 &&
                  countKind(between(ev, 0, kLoop), Kind::NoteOff) == 6,
              "S3 Live notes sound once while recording");
    }
    {
        // A note crossing the loop boundary (free record).
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(on(SynthTarget::Fm, 62, 90000));
        sc.live.push_back(off(SynthTarget::Fm, 62, 100000));
        const Events ev = run(*seq, sc, 3 * kLoop, 1024);
        const Events want = {{186000, Kind::NoteOn, SynthTarget::Fm, 62, 100},
                             {196000, Kind::NoteOff, SynthTarget::Fm, 62, 0},
                             {282000, Kind::NoteOn, SynthTarget::Fm, 62, 100}};
        check(seq->recorder().track().size() == 1 &&
                  seq->recorder().track().at(0).lengthFine == 10000 * 1024 / 25 &&
                  between(ev, 150000, 3 * kLoop) == want,
              "S4 Note crossing the loop end: length kept, replays across the boundary");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(on(SynthTarget::Mono, 50, 4000));
        sc.live.push_back(on(SynthTarget::Poly, 51, 5000));
        sc.actions.push_back({40960, [](Sequencer& s) { s.disarm(); }});
        sc.actions.push_back({81920, [](Sequencer& s) { s.arm(); }});
        sc.live.push_back(on(SynthTarget::Fm, 52, 82000));
        sc.actions.push_back({90112, [](Sequencer& s) { s.stop(); }});
        run(*seq, sc, 100000, 512);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 3 && !t.at(0).pending && t.at(0).lengthFine == exactFine(40960, 48000, 120000) - t.at(0).startFine &&
                  t.at(2).lengthFine == exactFine(90112, 48000, 120000) - t.at(2).startFine &&
                  seq->recorder().heldNoteCount() == 0 && t.undoablePassCount() == 2,
              "S5 Notes held at disarm / transport stop end there (no stuck pending notes)");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.recorder().setTargets(RecordDrums | RecordPoly);
                                  s.arm();
                                  s.start();
                              }});
        sc.live.push_back(off(SynthTarget::Mono, 70, 100));  // stray note-off
        sc.live.push_back(on(SynthTarget::Mono, 40, 1000));
        sc.live.push_back(on(SynthTarget::Poly, 41, 1000));
        sc.live.push_back(hit(0, 1500));
        sc.live.push_back(off(SynthTarget::Mono, 40, 3000));
        sc.live.push_back(off(SynthTarget::Poly, 41, 3000));
        const Events ev = run(*seq, sc, 10000, 64);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 2 && t.at(0).target == SynthTarget::Poly &&
                  t.at(1).kind == RecordedEvent::Kind::Drum && countKind(ev, Kind::NoteOn) == 2,
              "S6 Record targets: unarmed kinds still sound live but are not recorded; a stray "
              "note-off is ignored");
    }

    {
        // Same note number on two synths: note-offs match by target.
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(on(SynthTarget::Mono, 60, 1000));
        sc.live.push_back(on(SynthTarget::Poly, 60, 2000));
        sc.live.push_back(off(SynthTarget::Poly, 60, 3000));
        sc.live.push_back(off(SynthTarget::Mono, 60, 9000));
        run(*seq, sc, 12000, 128);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 2 && t.at(0).target == SynthTarget::Mono &&
                  t.at(0).lengthFine == exactFine(9000, 48000, 120000) - exactFine(1000, 48000, 120000) &&
                  t.at(1).lengthFine == exactFine(3000, 48000, 120000) - exactFine(2000, 48000, 120000),
              "S7 Note-offs end the held note of their own synth (same note number on two targets)");
    }
}

// ---------------------------------------------------------------------------
void modeTests() {
    std::cout << "\n-- Overdub, replace, loop record --\n";
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(0, 1000));
        sc.live.push_back(hit(1, 50000));
        sc.actions.push_back({94208, [](Sequencer& s) { s.disarm(); s.arm(); }});  // pass 2
        sc.live.push_back(hit(2, kLoop + 20000));
        sc.live.push_back(on(SynthTarget::Poly, 60, kLoop + 30000));
        sc.live.push_back(off(SynthTarget::Poly, 60, kLoop + 40000));
        const Events ev = run(*seq, sc, 3 * kLoop, 256);
        const Events loop2 = between(ev, 2 * kLoop, 3 * kLoop);
        check(seq->recorder().track().visibleCount() == 4 && countKind(loop2, Kind::Drum) == 3 &&
                  countKind(loop2, Kind::NoteOn) == 1 && seq->recorder().track().undoablePassCount() == 2,
              "M1 Overdub: second pass adds to the first; all play back");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int k = 0; k < 8; ++k) sc.live.push_back(hit(k, 5000 + k * 11000));
        sc.live.push_back(on(SynthTarget::Mono, 45, 7000));
        sc.live.push_back(off(SynthTarget::Mono, 45, 8000));
        // Pass 2: replace drums only, punched over the second half of loop 1.
        sc.actions.push_back({94208, [](Sequencer& s) {
                                  s.disarm();
                                  s.recorder().setWriteMode(WriteMode::Replace);
                                  s.recorder().setTargets(RecordDrums);
                                  s.recorder().setPunch(3840 + 1920, 7680);
                                  s.arm();
                              }});
        sc.live.push_back(hit(20, kLoop + 60000));
        const Events ev = run(*seq, sc, 3 * kLoop, 64);
        const Events loop2 = between(ev, 2 * kLoop, 3 * kLoop);
        std::vector<int> pads;
        for (const auto& e : loop2) {
            if (e.kind == Kind::Drum) pads.push_back(e.value);
        }
        // Hits at 5000..82000 step 11000: pads 0..3 (< 48000) kept, 4..7 erased.
        check(pads == std::vector<int>({0, 1, 2, 3, 20}) && countKind(loop2, Kind::NoteOn) == 1,
              "M2 Replace erases only the covered region (punched second half) and only the "
              "armed kinds (note kept)");
        Events loop1 = between(ev, kLoop, 2 * kLoop);
        check(countKind(loop1, Kind::Drum) == 5,
              "M3 Replace takes effect as the head passes (loop 1: old hits before the punch, "
              "the live hit inside it)");
        seq->undo();
        const Events after = run(*seq, Script{}, kLoop, 64);  // loop 3
        check(countKind(after, Kind::Drum) == 8,
              "M4 Undo of a replace pass restores the erased events");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.recorder().setRecordMode(RecordMode::Loop);
                                  s.arm();
                                  s.start();
                              }});
        for (int loop = 0; loop < 3; ++loop) {
            sc.live.push_back(hit(loop, loop * kLoop + 10000 + loop * 100));
        }
        run(*seq, sc, 3 * kLoop, 256);
        RecordedTrack& t = seq->recorder().track();
        const bool three = t.undoablePassCount() == 3 && t.visibleCount() == 3;
        seq->disarm();
        const bool u1 = seq->undo() && t.visibleCount() == 2;
        const bool u2 = seq->undo() && t.visibleCount() == 1;
        const bool r1 = seq->redo() && t.visibleCount() == 2 && t.redoablePassCount() == 1;
        check(three && u1 && u2 && r1,
              "M5 Loop record: one pass (undo step) per loop; undo/redo per loop");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.recorder().setRecordMode(RecordMode::Loop);
                                  s.arm();
                                  s.start();
                              }});
        run(*seq, sc, 5 * kLoop, 4096);
        const bool emptyOverdub = seq->recorder().track().undoablePassCount() == 0;
        seq->recorder().setWriteMode(WriteMode::Replace);
        run(*seq, Script{}, 2 * kLoop, 4096);
        check(emptyOverdub && seq->recorder().track().undoablePassCount() == 0,
              "M6 Passes open lazily: idle loops (overdub or replace over nothing) add no undo "
              "steps");
    }
}

// ---------------------------------------------------------------------------
void undoTests() {
    std::cout << "\n-- Undo / redo --\n";
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int k = 0; k < 10; ++k) sc.live.push_back(hit(k, 3000 + k * 9000, 50 + k));
        sc.live.push_back(on(SynthTarget::Fm, 66, 4000));
        sc.live.push_back(off(SynthTarget::Fm, 66, 9000));
        sc.actions.push_back({94208, [](Sequencer& s) { s.disarm(); }});
        run(*seq, sc, 94208, 256);
        RecordedTrack& t = seq->recorder().track();
        const auto before = visibleEvents(t);

        Script sc2;
        sc2.actions.push_back({0, [](Sequencer& s) {
                                   s.recorder().setWriteMode(WriteMode::Replace);
                                   s.arm();
                               }});
        for (int k = 0; k < 5; ++k) sc2.live.push_back(hit(20 + k, 2000 + k * 7000));
        sc2.actions.push_back({94208, [](Sequencer& s) { s.disarm(); }});
        run(*seq, sc2, 2 * 94208, 256);  // replaces ~all of the loop
        const auto afterReplace = visibleEvents(t);
        const bool changed = !sameEvents(before, afterReplace);
        seq->undo();
        const bool undone = sameEvents(visibleEvents(t), before);
        seq->redo();
        const bool redone = sameEvents(visibleEvents(t), afterReplace);
        seq->undo();
        check(changed && undone && redone && sameEvents(visibleEvents(t), before),
              "U1 Undo restores the exact previous state (field by field); redo the exact "
              "replaced state");
        // A new pass after undo discards the redo history.
        Script sc3;
        sc3.actions.push_back({0, [](Sequencer& s) {
                                   s.recorder().setWriteMode(WriteMode::Overdub);
                                   s.arm();
                               }});
        sc3.live.push_back(hit(26, 100));
        run(*seq, sc3, 4096 * 2, 256);
        check(t.redoablePassCount() == 0 && !seq->redo() && t.visibleCount() == before.size() + 1,
              "U2 Recording after undo discards the redo history");
    }
    {
        // Depth: 20 passes, 16 kept (oldest 4 flattened, no longer undoable).
        auto seq = makeSequencer();
        seq->start();
        RecordedTrack& t = seq->recorder().track();
        for (int pass = 0; pass < 20; ++pass) {
            seq->arm();
            const std::int64_t base = seq->clock().samplePosition();
            seq->liveEvent(hit(pass % 27, base + 10));
            seq->process(64, [](const SequencerEvent&) {});
            seq->disarm();
        }
        const bool kept = t.undoablePassCount() == 16 && t.flattenedPassCount() == 4 &&
                          t.size() == 20;
        int undos = 0;
        while (seq->undo()) ++undos;
        check(kept && undos == 16 && t.visibleCount() == 4,
              "U3 Undo depth 16: 20 passes -> 4 flattened, exactly 16 undos, 4 events remain");
        int redos = 0;
        while (seq->redo()) ++redos;
        check(redos == 16 && t.visibleCount() == 20, "U4 Redo restores all 16 undone passes");
    }
    {
        // Undo while a pass is open (recording continues in a new pass).
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(1, 1000));
        sc.live.push_back(on(SynthTarget::Poly, 60, 2000));
        sc.actions.push_back({4096, [](Sequencer& s) { s.undo(); }});
        sc.live.push_back(off(SynthTarget::Poly, 60, 5000));
        sc.live.push_back(hit(2, 6000));
        run(*seq, sc, 8192, 64);
        const RecordedTrack& t = seq->recorder().track();
        const auto vis = visibleEvents(t);
        check(vis.size() == 1 && vis[0].value == 2 && t.undoablePassCount() == 1 &&
                  seq->recorder().heldNoteCount() == 0 && seq->recorder().armed(),
              "U5 Undo during recording closes and removes the open pass (held notes dropped); "
              "later input opens a new pass");
    }
    {
        auto seq = makeSequencer();
        check(!seq->undo() && !seq->redo(), "U6 Undo/redo with no history are rejected");
    }
    {
        // Undo drops held notes of the undone pass immediately.
        auto seq = makeSequencer();
        seq->arm();
        seq->start();
        seq->liveEvent(on(SynthTarget::Fm, 50, 10));
        seq->process(64, [](const SequencerEvent&) {});
        const bool heldBefore = seq->recorder().heldNoteCount() == 1;
        seq->undo();
        check(heldBefore && seq->recorder().heldNoteCount() == 0,
              "U7 Undo of the open pass forgets its held notes at once");
    }
}

// ---------------------------------------------------------------------------
Script fullScenario() {
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) {
                              s.pattern().setHit(0, 0, 120);
                              s.pattern().setHit(3, 8, 90);
                              s.pattern().addNote({true, SynthTarget::Mono, 36, 100, 100, 0, 600});
                              s.arm();
                              s.start();
                          }});
    // Free drums and notes, deliberately odd timing.
    for (int k = 0; k < 40; ++k) {
        sc.live.push_back(hit((k * 7) % 27, 137 + k * 2311, 20 + (k * 13) % 100));
    }
    sc.live.push_back(on(SynthTarget::Fm, 72, 15003));
    sc.live.push_back(off(SynthTarget::Fm, 72, 33333));
    sc.live.push_back(on(SynthTarget::Poly, 60, 95000));  // crosses the loop end
    sc.live.push_back(off(SynthTarget::Poly, 60, 99999));
    sc.actions.push_back({kLoop + 4096 * 2 - (kLoop % 4096), [](Sequencer& s) {
                              s.disarm();
                              s.recorder().setRecordMode(RecordMode::Loop);
                              s.recorder().setWriteMode(WriteMode::Overdub);
                              s.arm();
                          }});
    for (int k = 0; k < 30; ++k) {
        sc.live.push_back(hit(26 - k % 27, 2 * kLoop + 501 + k * 3001));
    }
    sc.live.push_back(on(SynthTarget::Mono, 43, 2 * kLoop + 7777));
    sc.live.push_back(off(SynthTarget::Mono, 43, 2 * kLoop + 8888));
    sc.actions.push_back({4096 * 72, [](Sequencer& s) {  // loop 3
                              s.recorder().setWriteMode(WriteMode::Replace);
                              s.recorder().setPunch(3 * 3840 + 960, 3 * 3840 + 2880);
                          }});
    sc.live.push_back(hit(13, 3 * kLoop + 40001));
    sc.actions.push_back({4096 * 100, [](Sequencer& s) { s.disarm(); s.undo(); s.undo(); s.redo(); }});
    sc.live.push_back(hit(9, 4096 * 100 + 3));   // live only (disarmed)
    sc.actions.push_back({4096 * 110, [](Sequencer& s) { s.reset(); }});
    return sc;
}

void blockTests() {
    std::cout << "\n-- Block-size independence --\n";
    for (double bpm : {120.0, 123.456}) {
        std::unique_ptr<Sequencer> ref;
        Events refEvents;
        bool same = true;
        std::string diff;
        for (std::size_t block : kBlocks) {
            auto seq = makeSequencer(bpm);
            Events ev = run(*seq, fullScenario(), 4096 * 140, block);
            if (!ref) {
                ref = std::move(seq);
                refEvents = std::move(ev);
                continue;
            }
            if (!(ev == refEvents) || !sameTrack(seq->recorder().track(), ref->recorder().track())) {
                same = false;
                diff += " " + std::to_string(block);
            }
        }
        const bool sane = ref->recorder().track().size() > 60 && refEvents.size() > 200 &&
                          ref->droppedEventCount() == 0;
        check(same && sane, "K" + std::string(bpm == 120.0 ? "1" : "2") +
                                " Identical events and recordings for block sizes 1..4096 at " +
                                (bpm == 120.0 ? "120" : "123.456") + " BPM (free, loop, overdub, "
                                "replace, punch, undo/redo, reset)" + diff);
    }
}

// ---------------------------------------------------------------------------
void orderTests() {
    std::cout << "\n-- Ordering, latency, transport --\n";
    {
        auto seq = makeSequencer();
        seq->pattern().setHit(3, 4, 11);  // tick 960 = sample 24000
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(3, 24000, 22));
        sc.actions.push_back({4096 * 23, [](Sequencer& s) { s.disarm(); }});
        sc.live.push_back(hit(3, kLoop + 24000, 33));  // live only
        const Events ev = run(*seq, sc, 2 * kLoop, 32);
        const Events at = between(ev, kLoop + 24000, kLoop + 24001);
        check(at.size() == 3 && at[0].velocity == 11 && at[1].velocity == 22 &&
                  at[2].velocity == 33,
              "O1 Same pad, same sample: pattern, then recorded, then live (stable order)");
    }
    {
        auto seq = makeSequencer();
        seq->arm();
        seq->start();
        seq->process(1000, [](const SequencerEvent&) {});
        seq->liveEvent(hit(5, 500));  // already in the past
        Events ev;
        seq->process(100, [&](const SequencerEvent& e) {
            ev.push_back({static_cast<std::int64_t>(e.offset), e.kind, e.target, e.value, e.velocity});
        });
        check(ev.size() == 1 && ev[0].sample == 0 && seq->recorder().lateLiveCount() == 1 &&
                  seq->recorder().track().size() == 0,
              "O2 Late input (timestamp before the block) sounds at offset 0, is not recorded, "
              "and is counted (no silent compensation)");
    }
    {
        // A late note-off still ends the held recorded note (at the block start).
        auto seq = makeSequencer();
        seq->arm();
        seq->start();
        seq->liveEvent(on(SynthTarget::Mono, 48, 100));
        seq->process(1000, [](const SequencerEvent&) {});
        seq->liveEvent(off(SynthTarget::Mono, 48, 900));  // late
        seq->process(1000, [](const SequencerEvent&) {});
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == 1 && !t.at(0).pending && seq->recorder().heldNoteCount() == 0 &&
                  t.at(0).lengthFine == exactFine(1000, 48000, 120000) - exactFine(100, 48000, 120000) &&
                  seq->recorder().lateLiveCount() == 1,
              "O5 A late note-off ends its held note at the block start (counted as late)");
    }
    {
        auto seq = makeSequencer();
        seq->arm();
        seq->liveEvent(hit(5, 123456));
        Events ev;
        seq->process(64, [&](const SequencerEvent& e) {
            ev.push_back({static_cast<std::int64_t>(e.offset), e.kind, e.target, e.value, e.velocity});
        });
        check(ev.size() == 1 && ev[0].sample == 0 && seq->recorder().track().size() == 0 &&
                  seq->recorder().pendingLiveCount() == 0,
              "O3 Transport stopped: live input sounds immediately and is not recorded");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(6, 10000));
        sc.actions.push_back({4096 * 30, [](Sequencer& s) { s.disarm(); s.reset(); }});
        const Events ev = run(*seq, sc, 4096 * 30 + 20000, 256);
        const Events tail = between(ev, 4096 * 30, 4096 * 30 + 20000);
        check(tail.size() == 1 && tail[0].sample == 4096 * 30 + 10000 && seq->timelineEpoch() == 1,
              "O4 After reset (new epoch) the recording plays in loop 0 again");
    }
}

// ---------------------------------------------------------------------------
void capacityTests() {
    std::cout << "\n-- Capacities and overflow --\n";
    {
        auto seq = makeSequencer(120.0, 4);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int k = 0; k < 600; ++k) sc.live.push_back(hit(k % 27, 100 + k * 500));
        const Events ev = run(*seq, sc, 320000, 256);
        const RecordedTrack& t = seq->recorder().track();
        check(t.size() == RecordedTrack::MaxEventsPerPass && t.passFullCount() == 88 &&
                  countKind(ev, Kind::Drum) == 600 && t.at(511).captureSample == 100 + 511 * 500,
              "C1 Per-pass capacity 512: excess dropped from the recording and counted, still "
              "heard live, stored events intact");
    }
    {
        auto seq = makeSequencer(120.0, 16);
        seq->start();
        const RecordedTrack& t = seq->recorder().track();
        for (int pass = 0; pass < 5; ++pass) {
            seq->arm();
            for (int k = 0; k < 500; ++k) {
                const std::int64_t base = seq->clock().samplePosition();
                seq->liveEvent(hit(k % 27, base));
                seq->process(1, [](const SequencerEvent&) {});
            }
            seq->disarm();
        }
        bool intact = t.size() == RecordedTrack::MaxEvents;
        for (std::size_t i = 1; i < t.size() && intact; ++i) {
            intact = t.at(i).sequence == t.at(i - 1).sequence + 1 &&
                     t.at(i).captureSample == t.at(i - 1).captureSample + 1;
        }
        check(intact && t.trackFullCount() == 452,
              "C2 Track capacity 2048: overflow counted, stored events consecutive and intact");
    }
    {
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int n = 0; n < 70; ++n) sc.live.push_back(on(SynthTarget::Poly, n, 1000));
        for (int n = 0; n < 70; ++n) sc.live.push_back(off(SynthTarget::Poly, n, 5000));
        run(*seq, sc, 8000, 64);
        const RecordedTrack& t = seq->recorder().track();
        bool finished = t.size() == 64;
        for (std::size_t i = 0; i < t.size() && finished; ++i) finished = !t.at(i).pending;
        check(finished && seq->recorder().heldFullCount() == 6,
              "C3 Held-note capacity 64: 70 simultaneous notes -> 64 recorded, 6 counted");
    }
    {
        auto seq = makeSequencer();
        int accepted = 0;
        for (int k = 0; k < 600; ++k) accepted += seq->liveEvent(hit(0, 100000 + k)) ? 1 : 0;
        check(accepted == 512 && seq->recorder().pendingFullCount() == 88,
              "C4 Pending live capacity 512: excess rejected and counted");
    }
    {
        auto seq = makeSequencer();
        int accepted = 0;
        for (int k = 0; k < 300; ++k) accepted += seq->postLive(hit(0, 1000 + k)) ? 1 : 0;
        int commands = 0;
        SequencerCommand c;
        c.type = SequencerCommandType::SetTargets;
        for (int k = 0; k < 100; ++k) commands += seq->postCommand(c) ? 1 : 0;
        check(accepted == 255 && seq->droppedLiveQueueCount() == 45 && commands == 63 &&
                  seq->droppedCommandCount() == 37,
              "C5 Live queue (255) and command queue (63) overflow rejected and counted");
    }
    {
        auto seq = makeSequencer();
        int bad = 0;
        bad += seq->liveEvent(hit(27, 10)) ? 0 : 1;
        bad += seq->liveEvent(hit(-1, 10)) ? 0 : 1;
        bad += seq->liveEvent(hit(0, 10, 0)) ? 0 : 1;
        bad += seq->liveEvent(hit(0, 10, 128)) ? 0 : 1;
        bad += seq->liveEvent(on(SynthTarget::Mono, 128, 10)) ? 0 : 1;
        bad += seq->liveEvent(hit(0, -5)) ? 0 : 1;
        bad += seq->liveEvent(on(static_cast<SynthTarget>(9), 60, 10)) ? 0 : 1;
        check(bad == 7 && seq->recorder().invalidLiveCount() == 7 &&
                  seq->recorder().pendingLiveCount() == 0,
              "C6 Invalid live input (pad, velocity, note, time, target) rejected and counted");
    }
    {
        // 2048 recorded hits on one sample (4 passes) -> 1024 per block limit.
        auto seq = makeSequencer();
        seq->start();
        for (int pass = 0; pass < 4; ++pass) {
            seq->arm();
            for (int k = 0; k < 512; ++k) seq->liveEvent(hit(k % 27, 1000 + pass * 4096));
            Events tmp = run(*seq, Script{}, 4096, 4096);
            seq->disarm();
        }
        // All captured at different loop positions; move to the next loop.
        const std::uint64_t dropped0 = seq->droppedEventCount();
        const Events ev = run(*seq, Script{}, kLoop, 4096);
        check(seq->recorder().track().size() == 2048 &&
                  countKind(ev, Kind::Drum) + (seq->droppedEventCount() - dropped0) == 2048 &&
                  seq->droppedEventCount() - dropped0 == 0,
              "C7 2048 recorded events play back (512 per block here: under the limit)");
        // Same with every hit on one sample in one block via 4 passes.
        auto seq2 = makeSequencer();
        seq2->start();
        for (int pass = 0; pass < 4; ++pass) {
            seq2->arm();
            const std::int64_t base = seq2->clock().samplePosition();
            for (int k = 0; k < 512; ++k) seq2->liveEvent(hit(k % 27, base));
            run(*seq2, Script{}, 1, 1);
            seq2->disarm();
            run(*seq2, Script{}, kLoop - 1, 4096);  // next loop: same loop position
        }
        const std::uint64_t d0 = seq2->droppedEventCount();
        const Events ev2 = run(*seq2, Script{}, 1, 1);
        check(seq2->recorder().track().size() == 2048 && countKind(ev2, Kind::Drum) == 1024 &&
                  seq2->droppedEventCount() - d0 == 1024,
              "C8 Per-block limit 1024 also bounds recorded playback: excess dropped and counted");
    }
    {
        // MaxActiveNotes: 250 long pattern notes + 20 recorded notes.
        auto seq = makeSequencer();
        for (int i = 0; i < 250; ++i) {
            seq->pattern().addNote({true, SynthTarget::Poly, i % 128, 100, 100, 0, 3000});
        }
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int n = 0; n < 20; ++n) sc.live.push_back(on(SynthTarget::Fm, n, 10));
        for (int n = 0; n < 20; ++n) sc.live.push_back(off(SynthTarget::Fm, n, 80000));
        run(*seq, sc, kLoop + 100, 512);
        check(seq->activeNoteCount() == 256 && seq->droppedEventCount() == 14,
              "C9 256 sounding notes shared by pattern and recording: excess dropped and counted");
    }
}

// ---------------------------------------------------------------------------
void pipelineTests() {
    std::cout << "\n-- Pipeline: recorded playback through the instruments --\n";
    static AudioPipeline<16> p;
    static std::vector<StereoFrame> closed(1200), open(9000), kick(5000);
    for (std::size_t i = 0; i < closed.size(); ++i) {
        const double v = 0.3 * ((i * 7919) % 200 / 100.0 - 1.0) * (1.0 - i / 1200.0);
        closed[i] = {v, v * 0.8};
    }
    for (std::size_t i = 0; i < open.size(); ++i) {
        const double v = 0.2 * ((i * 104729) % 300 / 150.0 - 1.0) * (1.0 - i / 9000.0);
        open[i] = {v * 0.7, v};
    }
    for (std::size_t i = 0; i < kick.size(); ++i) {
        const double v = 0.5 * std::sin(i * 0.02) * (1.0 - i / 5000.0);
        kick[i] = {v, v};
    }
    p.drums().assignSample(0, &kick);
    p.drums().assignSample(13, &closed);
    p.drums().assignSample(14, &open);
    p.drums().setChokeGroup(13, 1);
    p.drums().setChokeGroup(14, 1);
    p.setMasterLevel(0.5);
    Sequencer& s = p.sequencer();
    s.clock().setTempo(120.0);
    s.pattern().configure({4, 4}, 1, 240);
    s.arm();
    s.start();

    // Loop 0: live performance with choke (open hat cut by closed hat),
    // simultaneous and repeated hits; tails end before the loop end.
    std::vector<LiveEvent> live = {hit(0, 2000, 120), hit(14, 2000, 90), hit(13, 6000, 70),
                                   hit(14, 20000, 100), hit(13, 23000, 60), hit(13, 23000, 80),
                                   hit(0, 40000, 127), hit(0, 40100, 64), hit(14, 60000, 110),
                                   hit(13, 63999, 50)};
    for (const auto& e : live) s.liveEvent(e);
    std::vector<StereoFrame> out(static_cast<std::size_t>(2 * kLoop));
    for (std::size_t pos = 0; pos < out.size(); pos += 480) {
        p.render(out.data() + pos, std::min<std::size_t>(480, out.size() - pos));
    }
    bool same = true;
    double energy = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kLoop); ++i) {
        const StereoFrame a = out[i];
        const StereoFrame b = out[i + static_cast<std::size_t>(kLoop)];
        same = same && a.left == b.left && a.right == b.right;
        energy += a.left * a.left;
    }
    check(same && energy > 1.0 && s.recorder().track().size() == live.size(),
          "P1 Through DrumEngine (choke, simultaneous, repeated): the recorded loop renders "
          "bit-identical to the live loop");
    check(p.undeliveredSequencerEventCount() == 0 && p.clippedSampleCount() == 0,
          "P2 All live and recorded events delivered; no clipping");
}

// ---------------------------------------------------------------------------
void threadTests() {
    std::cout << "\n-- Threading and allocation --\n";
    {
        auto seq = makeSequencer();
        seq->start();
        std::atomic<bool> done{false};
        std::atomic<std::int64_t> audioPos{0};
        std::atomic<int> liveAccepted{0};
        std::atomic<int> commandsAccepted{0};
        std::thread producer([&] {
            for (int k = 0; k < 20000; ++k) {
                const std::int64_t at = audioPos.load(std::memory_order_acquire) + 4096;
                if (seq->postLive(hit(k % 27, at + (k % 200), 1 + k % 127))) {
                    liveAccepted.fetch_add(1, std::memory_order_relaxed);
                }
                SequencerCommand c;
                const int kind = k % 7;
                c.type = kind == 0 ? SequencerCommandType::Arm
                       : kind == 1 ? SequencerCommandType::SetWriteMode
                       : kind == 2 ? SequencerCommandType::Undo
                       : kind == 3 ? SequencerCommandType::Redo
                       : kind == 4 ? SequencerCommandType::SetRecordMode
                       : kind == 5 ? SequencerCommandType::Disarm
                                   : SequencerCommandType::SetStep;
                c.writeMode = (k % 3 == 0) ? WriteMode::Replace : WriteMode::Overdub;
                c.recordMode = (k % 5 == 0) ? RecordMode::Loop : RecordMode::Free;
                c.a = k % 27;
                c.b = k % 16;
                c.step.enabled = true;
                c.step.velocity = 99;
                if (seq->postCommand(c)) commandsAccepted.fetch_add(1, std::memory_order_relaxed);
                if (k % 64 == 0) std::this_thread::yield();
            }
            done.store(true, std::memory_order_release);
        });
        std::size_t delivered = 0;
        std::size_t blocks = 0;
        while (!done.load(std::memory_order_acquire) || blocks < 2000) {
            seq->process(128, [&](const SequencerEvent&) { ++delivered; });
            audioPos.store(seq->clock().samplePosition(), std::memory_order_release);
            ++blocks;
        }
        // Drain remaining pending input.
        for (int k = 0; k < 64; ++k) seq->process(4096, [&](const SequencerEvent&) { ++delivered; });
        producer.join();
        const Recorder& r = seq->recorder();
        check(r.pendingLiveCount() == 0 &&
                  liveAccepted.load() + static_cast<int>(seq->droppedLiveQueueCount()) == 20000 &&
                  commandsAccepted.load() + static_cast<int>(seq->droppedCommandCount()) == 20000 &&
                  r.invalidLiveCount() == 0 && r.track().size() <= RecordedTrack::MaxEvents,
              "R1 Live input and commands from a second thread during rendering: every item "
              "applied or counted as dropped (lock-free SPSC; run under TSan)");
    }
    {
        auto seq = makeSequencer(120.0, 2);
        seq->pattern().setHit(0, 0);
        seq->pattern().addNote({true, SynthTarget::Poly, 60, 100, 100, 0, 480});
        Script sc = fullScenario();
        // Warm up (std::function etc. allocate outside the measurement).
        std::vector<LiveEvent> live;
        for (int k = 0; k < 400; ++k) {
            live.push_back(hit(k % 27, 1000 + k * 997));
            if (k % 10 == 0) live.push_back(on(SynthTarget::Fm, 40 + k % 30, 1000 + k * 997));
            if (k % 10 == 5) live.push_back(off(SynthTarget::Fm, 40 + (k - 5) % 30, 1000 + k * 997));
        }
        SequencerCommand replace;
        replace.type = SequencerCommandType::SetWriteMode;
        replace.writeMode = WriteMode::Replace;
        SequencerCommand undo;
        undo.type = SequencerCommandType::Undo;
        SequencerCommand loopMode;
        loopMode.type = SequencerCommandType::SetRecordMode;
        loopMode.recordMode = RecordMode::Loop;
        SequencerCommand arm;
        arm.type = SequencerCommandType::Arm;
        auto sink = [](const SequencerEvent&) {};
        const std::size_t before = g_allocations.load();
        seq->postCommand(arm);
        seq->postCommand(loopMode);
        seq->start();
        std::size_t li = 0;
        for (std::int64_t pos = 0; pos < 3 * 192000; pos += 256) {
            while (li < live.size() && live[li].sampleTime < pos + 256) seq->postLive(live[li++]);
            if (pos == 192000) seq->postCommand(replace);
            if (pos == 400000) seq->postCommand(undo);
            seq->process(256, sink);
        }
        seq->stop();
        seq->undo();
        seq->redo();
        seq->clearRecording();
        const std::size_t allocations = g_allocations.load() - before;
        check(allocations == 0, "R2 Zero allocations: capture, loop record, replace, undo/redo, "
                                "playback, stop, clear (" + std::to_string(allocations) + ")");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 026 - recording and performance capture tests\n";
    std::cout << "Host-side tests; not validated on FM-1 hardware.\n";
    timeTests();
    drumTests();
    boundaryTests();
    synthTests();
    modeTests();
    undoTests();
    blockTests();
    orderTests();
    capacityTests();
    pipelineTests();
    threadTests();
    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
