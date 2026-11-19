// DATABREAKs Engine 028 - pattern banks, switching, arrangement, persistence.
//
// Host-side assertion tests for core/sequencer/pattern_bank.h,
// arrangement.h, pattern_io.h and the switching / song logic in
// sequencer.h. Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"
#include "../../core/sequencer/pattern_io.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
#define DATABREAKS_COUNT_MUTEX 1
#include <dlfcn.h>
#include <pthread.h>
#endif

namespace {
std::atomic<std::size_t> g_allocations{0};
std::atomic<std::size_t> g_mutexLocks{0};
}

// Counting replacements; noinline so GCC -O3 does not pair the inlined
// malloc/free with new/delete (-Wmismatched-new-delete false positive).
__attribute__((noinline)) void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
__attribute__((noinline)) void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

#ifdef DATABREAKS_COUNT_MUTEX
// Interposes pthread_mutex_lock (which std::mutex uses) to count every lock
// taken in this process; the audio-path checks require zero. Disabled under
// sanitizers, which intercept the same symbol themselves.
extern "C" int pthread_mutex_lock(pthread_mutex_t* m) {
    using Fn = int (*)(pthread_mutex_t*);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
    g_mutexLocks.fetch_add(1, std::memory_order_relaxed);
    return real(m);
}
#endif

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
using Bank = DefaultPatternBank;
constexpr std::int64_t kStep = 6000;   // 16th at 120 BPM, 48 kHz
constexpr std::int64_t kBar = 96000;   // 4/4 bar

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

std::unique_ptr<Sequencer> makeSequencer(double bpm = 120.0) {
    auto seq = std::make_unique<Sequencer>(48000);
    seq->clock().setTempo(bpm);
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
    std::vector<LiveEvent> live;
    std::vector<std::pair<std::int64_t, std::function<void(Sequencer&)>>> actions;
};

// Positions relative to the clock at the start; actions at block starts
// (multiples of 4096 for block-size independence).
Events run(Sequencer& seq, Script script, std::int64_t total, std::size_t block) {
    std::stable_sort(script.live.begin(), script.live.end(),
                     [](const LiveEvent& a, const LiveEvent& b) { return a.sampleTime < b.sampleTime; });
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
            out.push_back({base + static_cast<std::int64_t>(e.offset), e.kind, e.target, e.value,
                           e.velocity});
        });
        pos = end;
    }
    return out;
}

std::size_t countDrums(const Events& ev) {
    std::size_t n = 0;
    for (const auto& e : ev) n += e.kind == Kind::Drum ? 1 : 0;
    return n;
}

Events drumsOf(const Events& all, int pad) {
    Events r;
    for (const auto& e : all) {
        if (e.kind == Kind::Drum && e.value == pad) r.push_back(e);
    }
    return r;
}

// ---- Equality of complete pattern states ------------------------------------------

bool sameStep(const DrumStep& a, const DrumStep& b) {
    return a.enabled == b.enabled && a.velocity == b.velocity && a.probability == b.probability &&
           a.microTicks == b.microTicks;
}

bool samePattern(const Pattern& a, const Pattern& b) {
    if (a.timeSignature().numerator != b.timeSignature().numerator ||
        a.timeSignature().denominator != b.timeSignature().denominator || a.bars() != b.bars() ||
        a.stepTicks() != b.stepTicks() || a.noteCount() != b.noteCount()) {
        return false;
    }
    for (std::size_t p = 0; p < Pattern::Lanes; ++p) {
        for (std::size_t k = 0; k < Pattern::MaxSteps; ++k) {
            if (!sameStep(*a.step(p, k), *b.step(p, k))) return false;
        }
    }
    for (std::size_t i = 0; i < a.noteCount(); ++i) {
        const NoteEvent& x = *a.note(i);
        const NoteEvent& y = *b.note(i);
        if (x.enabled != y.enabled || x.target != y.target || x.note != y.note ||
            x.velocity != y.velocity || x.probability != y.probability ||
            x.startTick != y.startTick || x.lengthTicks != y.lengthTicks) {
            return false;
        }
    }
    return true;
}

bool sameTrack(const RecordedTrack& a, const RecordedTrack& b) {
    if (a.size() != b.size() || a.passCount() != b.passCount() || a.nextPassId() != b.nextPassId()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!a.at(i).sameContent(b.at(i)) || a.at(i).createdPass != b.at(i).createdPass ||
            a.at(i).erasedPass != b.at(i).erasedPass || a.visible(i) != b.visible(i)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.passCount(); ++i) {
        if (a.pass(i).id != b.pass(i).id || a.pass(i).active != b.pass(i).active) return false;
    }
    return a.undoablePassCount() == b.undoablePassCount();
}

struct State {
    std::unique_ptr<PatternData> d = std::make_unique<PatternData>();
};

// Loads slot `id` of `bank` into a fresh state.
State loadSlot(const Bank& bank, int id) {
    State s;
    bank.load(id, s.d->pattern, s.d->track, s.d->quantize);
    return s;
}

bool sameState(const State& a, const State& b) {
    return samePattern(a.d->pattern, b.d->pattern) && sameTrack(a.d->track, b.d->track) &&
           a.d->quantize == b.d->quantize;
}

// A rich state: steps (incl. microtiming, probability, a step beyond the
// step count), notes (incl. one beyond the length), a recording with undo
// history (an undone pass, a replace pass), quantize settings.
State richState(int variant) {
    State s;
    Pattern& p = s.d->pattern;
    p.configure({4, 4}, 1, 240);
    for (int k = 0; k < 16; ++k) {
        DrumStep d;
        d.enabled = (k + variant) % 3 != 0;
        d.velocity = 20 + (k * 7 + variant) % 100;
        d.probability = 40 + k * 3;
        d.microTicks = (k % 5) * 20 - 40;
        p.setStep(static_cast<std::size_t>((k + variant) % 27), static_cast<std::size_t>(k), d);
    }
    DrumStep far;
    far.enabled = true;
    far.velocity = 99;
    p.restoreStep(26, 200, far);  // beyond the step count: kept, not played
    p.addNote({true, SynthTarget::Fm, 60 + variant, 90, 80, 480, 720});
    p.addNote({false, SynthTarget::Poly, 64, 70, 100, 1200, 240});
    p.restoreNote({true, SynthTarget::Mono, 40, 100, 100, 5000, 100});  // beyond the length
    RecordedTrack& t = s.d->track;
    const std::uint32_t p1 = t.openPass();
    for (int i = 0; i < 5; ++i) {
        RecordedEvent e;
        e.kind = RecordedEvent::Kind::Drum;
        e.value = i * 5;
        e.velocity = 10 + i * 20;
        e.startFine = 1000 + i * 700001;
        e.captureSample = 123 + i * 9999;
        e.captureFine = e.startFine + 7 * 3840 * 1024;
        e.captureLoop = 7;
        e.captureEpoch = 3;
        e.sequence = 100 + static_cast<std::uint64_t>(i);
        t.add(e, p1);
    }
    const std::uint32_t p2 = t.openPass();
    RecordedEvent n;
    n.kind = RecordedEvent::Kind::Note;
    n.target = SynthTarget::Poly;
    n.value = 72;
    n.velocity = 64;
    n.startFine = 2000000;
    n.lengthFine = 123456;
    n.captureSample = 777777;
    n.captureFine = 2000000;
    n.sequence = 200;
    t.add(n, p2);
    t.erase(0, 1500000, RecordDrums, p2);  // replace pass erases some drums
    const std::uint32_t p3 = t.openPass();
    RecordedEvent undone = n;
    undone.value = 30 + variant;
    undone.sequence = 300;
    t.add(undone, p3);
    t.undo();  // p3 undone (redo-able)
    s.d->quantize.grid = QuantizeGrid::Eighth;
    s.d->quantize.strength = 30 + variant;
    s.d->quantize.swing = 12;
    s.d->quantize.drumLanes = 0x5555u;
    s.d->quantize.quantizeNoteOff = true;
    return s;
}

// Simple patterns for playback tests.
void patternA(Pattern& p) {  // pad 0 on every quarter, 1 bar
    p.configure({4, 4}, 1, 240);
    p.clear();
    for (int k = 0; k < 16; k += 4) p.setHit(0, static_cast<std::size_t>(k), 110);
}
void patternB(Pattern& p) {  // pad 1 on every 16th, 1 bar
    p.configure({4, 4}, 1, 240);
    p.clear();
    for (int k = 0; k < 16; ++k) p.setHit(1, static_cast<std::size_t>(k), 50);
}
void patternC(Pattern& p) {  // 2 bars of 3/4, pad 2 on every 8th + a note
    p.configure({3, 4}, 2, 480);
    p.clear();
    for (int k = 0; k < 12; ++k) p.setHit(2, static_cast<std::size_t>(k), 77);
    p.addNote({true, SynthTarget::Mono, 36, 100, 100, 0, 1000});
}

// Puts A in slot 0 (active), B in 1, C in 2, A2 (2-bar A) in 3.
void setupABC(Sequencer& s) {
    auto d = std::make_unique<PatternData>();
    patternB(d->pattern);
    s.replacePattern(1, d->pattern, d->track, d->quantize);
    patternC(d->pattern);
    s.replacePattern(2, d->pattern, d->track, d->quantize);
    patternA(d->pattern);
    d->pattern.configure({4, 4}, 2, 240);
    for (int k = 16; k < 32; k += 4) d->pattern.setHit(0, static_cast<std::size_t>(k), 110);
    s.replacePattern(3, d->pattern, d->track, d->quantize);
    patternA(s.pattern());
}

Events expectA(std::int64_t from, std::int64_t to, std::int64_t origin = 0) {
    Events v;
    for (std::int64_t t = origin; t < to; t += 24000) {
        if (t >= from) v.push_back({t, Kind::Drum, SynthTarget::Mono, 0, 110});
    }
    return v;
}
Events expectB(std::int64_t from, std::int64_t to, std::int64_t origin) {
    Events v;
    for (std::int64_t t = origin; t < to; t += kStep) {
        if (t >= from) v.push_back({t, Kind::Drum, SynthTarget::Mono, 1, 50});
    }
    return v;
}

const std::size_t kBlocks[] = {1, 16, 32, 64, 128, 256, 512, 1024, 4096};

// ---------------------------------------------------------------------------
void bankTests() {
    std::cout << "\n-- Pattern bank --\n";
    {
        static Bank b;
        bool ok = Bank::Capacity == 16 && Bank::idOf(0, 0) == 0 && Bank::idOf(1, 7) == 15 &&
                  Bank::idOf(2, 0) == -1 && Bank::idOf(0, 8) == -1 && Bank::bankOf(13) == 1 &&
                  Bank::slotOf(13) == 5 && Bank::validId(15) && !Bank::validId(16) &&
                  !Bank::validId(-1) && b.usedCount() == 0 && b.firstEmpty() == 0;
        auto seq = makeSequencer();
        ok = ok && seq->bank().used(0) && seq->bank().usedCount() == 1 && seq->activePattern() == 0;
        check(ok, "K1 2 banks x 8 = 16 stable ids (bank x 8 + slot), lookup and validation; "
                  "the sequencer starts on pattern 0");
    }
    {
        static Bank b;
        State a = richState(1);
        bool ok = b.store(4, a.d->pattern, a.d->track, a.d->quantize) && b.used(4);
        State back = loadSlot(b, 4);
        ok = ok && sameState(a, back);
        State empty = loadSlot(b, 9);
        State fresh;
        ok = ok && !b.used(9) && sameState(empty, fresh) && empty.d->pattern.lengthTicks() == 3840;
        check(ok, "K2 Store/load is exact: every step (incl. beyond the step count), note (incl. "
                  "beyond the length), recorded event field, pass table / undo state and quantize "
                  "setting; an empty slot loads the empty default pattern");
    }
    {
        static Bank b;
        State a = richState(2);
        b.store(0, a.d->pattern, a.d->track, a.d->quantize);
        bool ok = b.copy(0, 5) && sameState(loadSlot(b, 5), a) && b.copy(5, 5);
        const int d1 = b.duplicate(0);
        ok = ok && d1 == 1 && sameState(loadSlot(b, 1), a);
        // Fill every slot, then duplicate must fail.
        int made = 0;
        while (b.firstEmpty() >= 0 && made < 20) {
            ok = ok && b.duplicate(0) >= 0;
            ++made;
        }
        const std::uint64_t full0 = b.fullCount();
        ok = ok && b.usedCount() == 16 && b.duplicate(0) == -1 && b.fullCount() == full0 + 1;
        for (int id = 0; id < 16 && ok; ++id) ok = sameState(loadSlot(b, id), a);
        check(ok, "K3 Copy and duplicate are exact; duplicate takes the lowest empty slot; a full "
                  "bank rejects duplicate (-1, counted) and nothing is corrupted");
    }
    {
        static Bank b;
        bool ok = !b.copy(-1, 0) && !b.copy(0, 16) && b.duplicate(3) == -1 && !b.erase(99) &&
                  !b.clearContents(-2) && !b.setShape(16, {4, 4}, 1, 240) &&
                  !b.setShape(0, {4, 4}, 1, 7) && !b.setShape(0, {5, 3}, 1, 240);
        State a = richState(0);
        ok = ok && !b.store(16, a.d->pattern, a.d->track, a.d->quantize) &&
             !b.store(-1, a.d->pattern, a.d->track, a.d->quantize);
        QuantizeSettings badQ;
        badQ.strength = 101;
        ok = ok && !b.store(0, a.d->pattern, a.d->track, badQ) && b.usedCount() == 0;
        auto seq = makeSequencer();
        ok = ok && !seq->requestPatternSwitch(16) && !seq->requestPatternSwitch(-1) &&
             !seq->copyPattern(0, 99) && seq->duplicatePattern(42) == -1 &&
             !seq->clearPattern(16) && !seq->setPatternLength(-1, {4, 4}, 1, 240) &&
             seq->activePattern() == 0;
        check(ok, "K4 Invalid ids, shapes and settings are rejected everywhere; nothing changes");
    }
    {
        // Pool overflow: events pool (2,048) shared by all patterns.
        static Bank b;
        State big;
        RecordedTrack& t = big.d->track;
        std::uint32_t pass = t.openPass();
        for (int i = 0; i < 1500; ++i) {
            if (i % 500 == 0 && i > 0) pass = t.openPass();
            RecordedEvent e;
            e.kind = RecordedEvent::Kind::Drum;
            e.value = i % 27;
            e.velocity = 100;
            e.startFine = i * 100;
            e.sequence = static_cast<std::uint64_t>(i + 1);
            t.add(e, pass);
        }
        State small = richState(3);
        bool ok = b.store(0, big.d->pattern, big.d->track, big.d->quantize) &&
                  b.store(1, small.d->pattern, small.d->track, small.d->quantize);
        const std::uint64_t full0 = b.fullCount();
        // A second 1,500-event pattern does not fit: rejected, slot 2 empty.
        ok = ok && !b.store(2, big.d->pattern, big.d->track, big.d->quantize) &&
             b.fullCount() == full0 + 1 && !b.used(2) && b.eventsUsed() == 1500 + 7;
        // Re-storing the big pattern into its own slot fits (its old space is reused).
        ok = ok && b.store(0, big.d->pattern, big.d->track, big.d->quantize);
        // Overwriting slot 1 with a too-large pattern keeps slot 1's old contents.
        ok = ok && !b.store(1, big.d->pattern, big.d->track, big.d->quantize) &&
             sameState(loadSlot(b, 1), small) && sameState(loadSlot(b, 0), big);
        check(ok, "K5 Pools full: store rejected and counted, previous contents of every slot kept "
                  "exactly");
    }
    {
        // Randomised store/copy/erase/clear against a reference model.
        static Bank b;
        std::vector<State> model(16);
        std::vector<bool> used(16, false);
        std::uint32_t r = 7;
        bool ok = true;
        for (int op = 0; op < 400 && ok; ++op) {
            r = r * 1664525u + 1013904223u;
            const int id = static_cast<int>((r >> 8) % 16);
            const int other = static_cast<int>((r >> 16) % 16);
            switch ((r >> 24) % 5) {
            case 0:
            case 1: {
                State s = richState(static_cast<int>((r >> 4) % 9));
                if (b.store(id, s.d->pattern, s.d->track, s.d->quantize)) {
                    model[static_cast<std::size_t>(id)] = std::move(s);
                    used[static_cast<std::size_t>(id)] = true;
                }
                break;
            }
            case 2:
                if (b.copy(id, other) && id != other) {
                    if (used[static_cast<std::size_t>(id)]) {
                        model[static_cast<std::size_t>(other)].d->pattern = model[static_cast<std::size_t>(id)].d->pattern;
                        model[static_cast<std::size_t>(other)].d->track = model[static_cast<std::size_t>(id)].d->track;
                        model[static_cast<std::size_t>(other)].d->quantize = model[static_cast<std::size_t>(id)].d->quantize;
                        used[static_cast<std::size_t>(other)] = true;
                    } else {
                        model[static_cast<std::size_t>(other)] = State{};
                        used[static_cast<std::size_t>(other)] = false;
                    }
                }
                break;
            case 3:
                b.erase(id);
                model[static_cast<std::size_t>(id)] = State{};
                used[static_cast<std::size_t>(id)] = false;
                break;
            default:
                b.clearContents(id);
                {
                    State cleared;
                    cleared.d->pattern = model[static_cast<std::size_t>(id)].d->pattern;
                    cleared.d->pattern.clear();
                    cleared.d->quantize = model[static_cast<std::size_t>(id)].d->quantize;
                    model[static_cast<std::size_t>(id)] = std::move(cleared);
                }
                used[static_cast<std::size_t>(id)] = true;
                break;
            }
            for (int k = 0; k < 16 && ok; ++k) {
                ok = b.used(k) == used[static_cast<std::size_t>(k)] &&
                     sameState(loadSlot(b, k), model[static_cast<std::size_t>(k)]);
            }
        }
        check(ok, "K6 400 random store/copy/erase/clear operations: every slot always equals a "
                  "reference model (pool compaction never corrupts another pattern)");
    }
}

// ---------------------------------------------------------------------------
void switchTests() {
    std::cout << "\n-- Switching --\n";
    {
        auto seq = makeSequencer();
        setupABC(*seq);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.requestPatternSwitch(1); }});  // stopped
        sc.actions.push_back({0, [](Sequencer& s) { s.start(); }});
        const Events ev = run(*seq, sc, kBar, 256);
        check(seq->activePattern() == 1 && ev == expectB(0, kBar, 0) &&
                  seq->patternSwitchCount() == 1,
              "W1 Stopped: the switch is immediate and the new pattern starts at its beginning");
    }
    {
        bool ok = true;
        std::string bad;
        for (std::size_t block : kBlocks) {
            auto seq = makeSequencer();
            setupABC(*seq);
            Script sc;
            sc.actions.push_back({0, [](Sequencer& s) { s.start(); }});
            sc.actions.push_back({4096 * 12, [](Sequencer& s) { s.requestPatternSwitch(1, SwitchTiming::NextLoop); }});
            const Events ev = run(*seq, sc, 2 * kBar, block);
            Events want = expectA(0, kBar);
            for (const auto& e : expectB(kBar, 2 * kBar, kBar)) want.push_back(e);
            if (!(ev == want)) {
                ok = false;
                bad += " " + std::to_string(block);
            }
        }
        check(ok, "W2 Playing, NextLoop: the current loop finishes, the new pattern starts exactly "
                  "on the loop boundary, for every block size 1..4096" + bad);
    }
    {
        // Boundaries with a 2-bar A (slot 3): beat / bar / loop / immediate.
        struct Case { SwitchTiming t; std::int64_t at; };
        const Case cases[] = {{SwitchTiming::NextBeat, 72000},
                              {SwitchTiming::NextBar, 96000},
                              {SwitchTiming::NextLoop, 192000}};
        bool ok = true;
        for (const Case& c : cases) {
            for (std::size_t block : {std::size_t(1), std::size_t(333), std::size_t(4096)}) {
                auto seq = makeSequencer();
                setupABC(*seq);
                seq->requestPatternSwitch(3);  // 2-bar A, stopped -> immediate
                Script sc;
                sc.actions.push_back({0, [](Sequencer& s) { s.start(); }});
                const SwitchTiming t = c.t;
                sc.actions.push_back({4096 * 12, [t](Sequencer& s) { s.requestPatternSwitch(1, t); }});
                const Events ev = run(*seq, sc, 3 * kBar, block);
                Events want = expectA(0, c.at);
                for (const auto& e : expectB(c.at, 3 * kBar, c.at)) want.push_back(e);
                ok = ok && ev == want && seq->patternOriginTick() == c.at / 25;
            }
        }
        // Immediate while playing: at the next undelivered tick (block start).
        auto seq = makeSequencer();
        setupABC(*seq);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.start(); }});
        sc.actions.push_back({4096 * 12, [](Sequencer& s) { s.requestPatternSwitch(1, SwitchTiming::Immediate); }});
        const Events ev = run(*seq, sc, kBar, 4096);
        const std::int64_t at = seq->clock().sampleOfTick(seq->clock().firstTickAtOrAfter(4096 * 12));
        Events want = expectA(0, at);
        for (const auto& e : expectB(at, kBar, at)) want.push_back(e);
        check(ok && ev == want && at == 49175,
              "W3 NextBeat / NextBar / NextLoop boundaries of the current pattern (72000 / 96000 "
              "/ 192000 samples) at blocks 1/333/4096; Immediate at the next tick (49175)");
    }
    {
        // Repeated requests: last wins; requesting the active pattern cancels.
        auto seq = makeSequencer();
        setupABC(*seq);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.start(); }});
        sc.actions.push_back({4096 * 2, [](Sequencer& s) {
                                  s.requestPatternSwitch(2);
                                  s.requestPatternSwitch(1);  // last wins: B
                              }});
        const Events ev1 = run(*seq, sc, 2 * kBar, 64);
        Events want = expectA(0, kBar);
        for (const auto& e : expectB(kBar, 2 * kBar, kBar)) want.push_back(e);
        const bool lastWins = ev1 == want && seq->patternSwitchCount() == 1;

        Script sc2;
        sc2.actions.push_back({4096 * 2, [](Sequencer& s) {
                                   s.requestPatternSwitch(2);
                                   s.requestPatternSwitch(1);  // the active one: cancels
                               }});
        const std::uint64_t switches = seq->patternSwitchCount();
        const std::int64_t origin = seq->patternOriginTick();
        const Events ev2 = run(*seq, sc2, kBar, 64);
        const bool cancelled = seq->activePattern() == 1 && seq->pendingPattern() == -1 &&
                               seq->patternSwitchCount() == switches &&
                               seq->patternOriginTick() == origin && ev2 == expectB(0, kBar, 0);
        seq->requestPatternSwitch(0);
        const bool pending = seq->pendingPattern() == 0;
        seq->cancelPatternSwitch();
        check(lastWins && cancelled && pending && seq->pendingPattern() == -1,
              "W4 Several requests: the last wins; requesting the active pattern cancels a "
              "pending switch and does not restart it; cancelPatternSwitch()");
    }
    {
        // Notes sounding across a switch end on their own note-off.
        auto seq = makeSequencer();
        setupABC(*seq);
        seq->pattern().addNote({true, SynthTarget::Poly, 50, 90, 100, 3600, 480});  // 90000 -> 102000
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.start(); s.requestPatternSwitch(1); }});
        const Events ev = run(*seq, sc, 2 * kBar, 100);
        int ons = 0;
        bool offOk = false;
        for (const auto& e : ev) {
            if (e.kind == Kind::NoteOn && e.value == 50) ++ons;
            if (e.kind == Kind::NoteOff && e.value == 50) offOk = e.sample == 102000;
        }
        check(ons == 1 && offOk && seq->activeNoteCount() == 0,
              "W5 A note sounding at the switch ends on its own note-off (no stuck notes)");
    }
    {
        // Different lengths: C (2 bars 3/4) starts at its own tick 0.
        auto seq = makeSequencer();
        setupABC(*seq);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.start(); s.requestPatternSwitch(2); }});
        const Events ev = run(*seq, sc, kBar + 2 * 144000, 512);
        const Events c = drumsOf(ev, 2);
        bool ok = c.size() == 24;
        for (std::size_t i = 0; i < c.size() && ok; ++i) {
            ok = c[i].sample == kBar + static_cast<std::int64_t>(i) * 12000;
        }
        check(ok, "W6 A pattern with another length/time signature starts at its own beginning "
                  "and loops with its own length");
    }
}

// ---------------------------------------------------------------------------
void independenceTests() {
    std::cout << "\n-- Independent pattern state --\n";
    auto seq = makeSequencer();
    setupABC(*seq);
    // Record into A (loop 0), quantize A.
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) {
                              s.arm();
                              s.start();
                          }});
    sc.live.push_back(hit(5, 7777));
    sc.live.push_back(on(SynthTarget::Fm, 60, 30001));
    sc.live.push_back(off(SynthTarget::Fm, 60, 40000));
    sc.actions.push_back({4096 * 10, [](Sequencer& s) {
                              QuantizeSettings q;
                              q.grid = QuantizeGrid::Sixteenth;
                              q.strength = 100;
                              s.setQuantize(q);
                              s.requestPatternSwitch(1);  // at 96000
                          }});
    // In B (from 96000): a held note across the next switch, a hit.
    sc.live.push_back(hit(9, kBar + 11111));
    sc.live.push_back(on(SynthTarget::Poly, 70, kBar + 90000));
    sc.actions.push_back({4096 * 34, [](Sequencer& s) {
                              QuantizeSettings q;
                              q.grid = QuantizeGrid::Eighth;
                              q.strength = 50;
                              q.swing = 30;
                              s.setQuantize(q);
                              s.requestPatternSwitch(0);  // back to A at 192000
                          }});
    run(*seq, sc, 3 * kBar, 128);
    seq->disarm();
    // Now on A again.
    const RecordedTrack& ta = seq->recorder().track();
    bool aOk = seq->activePattern() == 0 && ta.size() == 2 && ta.at(0).value == 5 &&
               ta.at(0).captureSample == 7777 && seq->quantizeSettings().strength == 100 &&
               seq->quantizeSettings().grid == QuantizeGrid::Sixteenth &&
               seq->pattern().lengthTicks() == 3840;
    const auto bInfo = seq->bank().info(1);
    State b = loadSlot(seq->bank(), 1);
    const RecordedTrack& tb = b.d->track;
    bool bOk = tb.size() == 2 && tb.at(0).value == 9 && tb.at(1).value == 70 && !tb.at(1).pending &&
               tb.at(1).lengthFine == 6000 * 1024 / 25 && b.d->quantize.strength == 50 &&
               b.d->quantize.swing == 30 && bInfo.events == 2;
    check(aOk && bOk,
          "I1 Each pattern keeps its own recording (raw timing), quantize grid/strength/swing and "
          "length; a note held at the switch ends at the switch");
    // Undo in A only affects A; B's history is intact.
    const std::size_t bPasses = b.d->track.undoablePassCount();
    const bool undoA = seq->undo() && seq->recorder().track().visibleCount() == 0;
    State b2 = loadSlot(seq->bank(), 1);
    check(undoA && sameState(b, b2) && bPasses == 1,
          "I2 Undo works on the active pattern's own history; other patterns are untouched");
    seq->redo();
    // Switch away and back while stopped: the pattern is exactly preserved.
    seq->stop();
    run(*seq, Script{}, 16, 16);
    State before;
    before.d->pattern = seq->pattern();
    before.d->track = seq->recorder().track();
    before.d->quantize = seq->quantizeSettings();
    seq->requestPatternSwitch(2);
    seq->requestPatternSwitch(1);
    seq->requestPatternSwitch(0);
    State after;
    after.d->pattern = seq->pattern();
    after.d->track = seq->recorder().track();
    after.d->quantize = seq->quantizeSettings();
    check(sameState(before, after) && seq->patternSwitchCount() >= 5,
          "I3 Switching away and back does not alter the pattern (steps, notes, recording, undo "
          "state, quantize)");
}

// ---------------------------------------------------------------------------
void editingTests() {
    std::cout << "\n-- Editing via the sequencer --\n";
    auto seq = makeSequencer();
    setupABC(*seq);
    // Record something into A so the copies carry a recording.
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
    sc.live.push_back(hit(3, 5000));
    run(*seq, sc, kBar, 256);
    seq->disarm();
    seq->stop();
    run(*seq, Script{}, 1, 1);
    // Copy active A -> 5, duplicate B -> lowest empty (4).
    bool ok = seq->copyPattern(0, 5) && seq->duplicatePattern(1) == 4 && seq->lastDuplicateId() == 4;
    State a;
    a.d->pattern = seq->pattern();
    a.d->track = seq->recorder().track();
    a.d->quantize = seq->quantizeSettings();
    ok = ok && sameState(loadSlot(seq->bank(), 5), a) &&
         sameState(loadSlot(seq->bank(), 4), loadSlot(seq->bank(), 1));
    // Copy into the active pattern replaces the working state.
    ok = ok && seq->copyPattern(2, 0) && seq->pattern().lengthTicks() == 5760 &&
         seq->recorder().track().size() == 0;
    // Clear keeps shape and quantize.
    QuantizeSettings q;
    q.strength = 40;
    seq->setQuantize(q);
    ok = ok && seq->clearPattern(0) && seq->pattern().lengthTicks() == 5760 &&
         seq->pattern().noteCount() == 0 && seq->quantizeSettings().strength == 40 &&
         seq->clearPattern(5) && seq->bank().info(5).events == 0 && seq->bank().info(5).used;
    // Set length: active and stored.
    ok = ok && seq->setPatternLength(0, {7, 8}, 2, 120) && seq->pattern().lengthTicks() == 2 * 3360 &&
         seq->setPatternLength(6, {5, 4}, 3, 240) && seq->bank().info(6).bars == 3 &&
         !seq->setPatternLength(6, {5, 4}, 17, 240);
    // Replace contents of a stored and of the active pattern.
    State r = richState(4);
    ok = ok && seq->replacePattern(7, r.d->pattern, r.d->track, r.d->quantize) &&
         sameState(loadSlot(seq->bank(), 7), r) &&
         seq->replacePattern(0, r.d->pattern, r.d->track, r.d->quantize) &&
         samePattern(seq->pattern(), r.d->pattern) && sameTrack(seq->recorder().track(), r.d->track);
    check(ok, "E1 Copy, duplicate, clear (keeps shape + quantize), set length and replace work on "
              "stored and active patterns, exactly");
    // Recording undo/redo still works on the replaced active pattern.
    const std::size_t vis = seq->recorder().track().visibleCount();
    const bool redo = seq->redo() && seq->recorder().track().visibleCount() == vis + 1;
    check(redo, "E2 Recording undo/redo history travels with the pattern (redo of a copied "
                "undone pass)");
}

// ---------------------------------------------------------------------------
void arrangementTests() {
    std::cout << "\n-- Arrangement --\n";
    {
        bool ok = true;
        std::string bad;
        for (std::size_t block : kBlocks) {
            auto seq = makeSequencer();
            setupABC(*seq);
            seq->arrangementAppend(0, 2);
            seq->arrangementAppend(1, 1);
            seq->arrangementAppend(0, 1);
            seq->arrangementAppend(2, 1);
            seq->arrangementSetEnd(ArrangementEnd::Stop);
            const bool started = seq->startArrangement();
            seq->start();
            const Events ev = run(*seq, Script{}, 700000, block);
            // A x2 [0, 192000), B [192000, 288000), A [288000, 384000), C [384000, 528000).
            Events want = expectA(0, 192000);
            for (const auto& e : expectB(192000, 288000, 192000)) want.push_back(e);
            for (const auto& e : expectA(288000, 384000, 288000)) want.push_back(e);
            Events c;
            for (int i = 0; i < 12; ++i) c.push_back({384000 + i * 12000, Kind::Drum, SynthTarget::Mono, 2, 77});
            // C's note: on at 384000, off at 384000 + 1000 ticks = 409000.
            Events merged = want;
            merged.push_back({384000, Kind::NoteOn, SynthTarget::Mono, 36, 100});
            for (const auto& e : c) merged.push_back(e);
            merged.push_back({409000, Kind::NoteOff, SynthTarget::Mono, 36, 0});
            std::stable_sort(merged.begin(), merged.end(), [](const Captured& x, const Captured& y) {
                if (x.sample != y.sample) return x.sample < y.sample;
                return x.kind < y.kind;
            });
            const bool same = started && ev == merged && !seq->playing() &&
                              seq->clock().samplePosition() == 528000 && seq->songFinished() &&
                              !seq->songActive();
            if (!same) bad += " " + std::to_string(block);
            ok = ok && same;
        }
        check(ok, "A1 Arrangement A x2, B, A, C, end Stop: order and repeats exact, transitions on "
                  "loop boundaries, transport stops exactly on the end sample (528000), for every "
                  "block size" + bad);
    }
    {
        auto seq = makeSequencer();
        setupABC(*seq);
        seq->arrangementAppend(1, 1);
        seq->arrangementAppend(0, 1);
        seq->arrangementSetEnd(ArrangementEnd::Loop);
        seq->startArrangement();
        seq->start();
        const Events ev = run(*seq, Script{}, 4 * kBar, 256);
        Events want = expectB(0, kBar, 0);
        for (const auto& e : expectA(kBar, 2 * kBar, kBar)) want.push_back(e);
        for (const auto& e : expectB(2 * kBar, 3 * kBar, 2 * kBar)) want.push_back(e);
        for (const auto& e : expectA(3 * kBar, 4 * kBar, 3 * kBar)) want.push_back(e);
        const bool loopOk = ev == want && seq->songActive() && seq->songEntry() == 1;

        auto seq2 = makeSequencer();
        setupABC(*seq2);
        seq2->arrangementAppend(0, 1);
        seq2->arrangementAppend(1, 1);
        seq2->arrangementSetEnd(ArrangementEnd::HoldLast);
        seq2->startArrangement();
        seq2->start();
        const Events ev2 = run(*seq2, Script{}, 4 * kBar, 256);
        Events want2 = expectA(0, kBar);
        for (const auto& e : expectB(kBar, 4 * kBar, kBar)) want2.push_back(e);
        check(loopOk && ev2 == want2 && seq2->songFinished() && seq2->playing() &&
                  seq2->activePattern() == 1,
              "A2 End modes: Loop restarts at entry 0; HoldLast keeps looping the last pattern "
              "(finished, still playing)");
    }
    {
        auto seq = makeSequencer();
        setupABC(*seq);
        // Same pattern twice in a row continues (no restart).
        seq->arrangementAppend(0, 1);
        seq->arrangementAppend(0, 2);
        seq->arrangementAppend(1, 1);
        seq->startArrangement();
        seq->start();
        const Events ev = run(*seq, Script{}, 4 * kBar, 4096);
        Events want = expectA(0, 3 * kBar);
        for (const auto& e : expectB(3 * kBar, 4 * kBar, 3 * kBar)) want.push_back(e);
        const bool same = ev == want && seq->patternSwitchCount() == 1;
        // Playing start: entry 0 begins at the next loop boundary.
        auto seq2 = makeSequencer();
        setupABC(*seq2);
        seq2->arrangementAppend(1, 1);
        seq2->arrangementSetEnd(ArrangementEnd::HoldLast);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.start(); }});
        sc.actions.push_back({4096 * 5, [](Sequencer& s) { s.startArrangement(); }});
        const Events ev2 = run(*seq2, sc, 2 * kBar, 64);
        Events want2 = expectA(0, kBar);
        for (const auto& e : expectB(kBar, 2 * kBar, kBar)) want2.push_back(e);
        check(same && ev2 == want2,
              "A3 Consecutive entries of one pattern continue it (repeats add up, no reload); "
              "started while playing, the arrangement begins at the next loop boundary");
    }
    {
        auto seq = makeSequencer();
        bool ok = !seq->startArrangement();                     // empty
        ok = ok && seq->arrangementAppend(9, 1) && !seq->startArrangement();  // unused slot
        seq->arrangementClear();
        ok = ok && !seq->arrangementAppend(16, 1) && !seq->arrangementAppend(0, 0) &&
             !seq->arrangementAppend(0, 1000) && seq->arrangement().empty();
        for (int i = 0; i < 64; ++i) ok = ok && seq->arrangementAppend(0, 1);
        ok = ok && !seq->arrangementAppend(0, 1) && seq->startArrangement();
        // Editing is rejected while the song runs; a manual switch stops song mode.
        ok = ok && !seq->arrangementAppend(0, 1) && !seq->arrangementClear() &&
             !seq->arrangementSetEnd(ArrangementEnd::Loop);
        seq->requestPatternSwitch(0);
        ok = ok && !seq->songActive() && seq->arrangementClear();
        check(ok, "A4 Arrangement validation (empty, unused or invalid pattern, repeats 1..999, "
                  "64 entries) and editing rejected while the song runs; a manual switch stops it");
    }
    {
        // Reset restarts the song.
        auto seq = makeSequencer();
        setupABC(*seq);
        seq->arrangementAppend(1, 1);
        seq->arrangementAppend(2, 1);
        seq->startArrangement();
        seq->start();
        run(*seq, Script{}, kBar + 5000, 256);  // into entry 1 (C)
        const bool inC = seq->activePattern() == 2 && seq->songEntry() == 1;
        seq->reset();
        const Events ev = run(*seq, Script{}, kBar, 256);
        // The transition back to C is due on sample 96000: the first sample
        // of the next block, so B is still active here.
        // reset() also releases C's sounding note (a note-off at offset 0).
        bool released = false;
        for (const auto& e : ev) released = released || (e.kind == Kind::NoteOff && e.sample == 0 && e.value == 36);
        check(inC && released && seq->songEntry() == 0 && drumsOf(ev, 1) == expectB(0, kBar, 0) &&
                  countDrums(ev) == 16 && seq->activePattern() == 1,
              "A5 reset() restarts a running arrangement at entry 0");
    }
}

// ---------------------------------------------------------------------------
// Builds a sequencer with varied content in several patterns.
std::unique_ptr<Sequencer> contentSequencer() {
    auto seq = makeSequencer();
    setupABC(*seq);
    // Every synth target in notes and recordings (target encoding).
    seq->pattern().addNote({true, SynthTarget::Fm, 67, 81, 90, 960, 300});
    seq->pattern().addNote({true, SynthTarget::Poly, 55, 61, 70, 1920, 900});
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) {
                              s.recorder().setRecordMode(RecordMode::Loop);
                              s.arm();
                              s.start();
                          }});
    sc.live.push_back(hit(4, 1234, 33));
    sc.live.push_back(on(SynthTarget::Mono, 45, 20000, 70));
    sc.live.push_back(off(SynthTarget::Mono, 45, 30000));
    sc.live.push_back(hit(6, kBar + 4321, 44));
    sc.live.push_back(on(SynthTarget::Poly, 62, kBar + 50000, 77));
    sc.live.push_back(off(SynthTarget::Poly, 62, kBar + 60000));
    sc.live.push_back(on(SynthTarget::Fm, 74, kBar + 61000, 88));
    sc.live.push_back(off(SynthTarget::Fm, 74, kBar + 65000));
    sc.actions.push_back({4096 * 47, [](Sequencer& s) {  // in loop 1 (A)
                              s.undo();
                              QuantizeSettings q;
                              q.grid = QuantizeGrid::ThirtySecond;
                              q.strength = 65;
                              q.swing = 20;
                              q.targets = RecordDrums | RecordMono;
                              q.drumLanes = 0x7FFFFFFu & ~1u;
                              s.setQuantize(q);
                              s.requestPatternSwitch(2);  // to C at 192000
                          }});
    sc.live.push_back(hit(8, 2 * kBar + 9999, 55));
    run(*seq, sc, 2 * kBar + 144000, 256);
    seq->disarm();
    seq->stop();
    seq->arrangementAppend(0, 2);
    seq->arrangementAppend(2, 1);
    seq->arrangementAppend(1, 3);
    seq->arrangementSetEnd(ArrangementEnd::Loop);
    run(*seq, Script{}, 1, 1);
    return seq;
}

std::vector<std::uint8_t> saveSequencer(Sequencer& s) {
    s.commitActivePattern();
    std::vector<std::uint8_t> bytes;
    pattern_io::save(s.bank(), s.arrangement(), s.activePattern(), bytes);
    return bytes;
}

void persistenceTests() {
    std::cout << "\n-- Persistence (DBPB v1) --\n";
    auto seq = contentSequencer();
    const std::vector<std::uint8_t> bytes = saveSequencer(*seq);
    const std::vector<std::uint8_t> again = saveSequencer(*seq);
    check(bytes == again && bytes.size() > 100, "F1 Saving is deterministic (" +
                                                    std::to_string(bytes.size()) + " bytes)");
    {
        static Bank loaded;
        Arrangement arr;
        int active = -1;
        const auto status = pattern_io::load(bytes.data(), bytes.size(), loaded, arr, active);
        bool ok = status == pattern_io::LoadStatus::Ok && active == seq->activePattern() &&
                  arr == seq->arrangement() && loaded.usedCount() == seq->bank().usedCount();
        for (int id = 0; id < 16 && ok; ++id) {
            ok = loaded.used(id) == seq->bank().used(id) &&
                 sameState(loadSlot(loaded, id), loadSlot(seq->bank(), id));
        }
        std::vector<std::uint8_t> resaved;
        pattern_io::save(loaded, arr, active, resaved);
        check(ok && resaved == bytes,
              "F2 Round trip: every pattern field, recording field, undo state, quantize setting, "
              "the arrangement and the active id are preserved; re-saving gives identical bytes");
        // Into a sequencer: playback identical to the original.
        auto copy = makeSequencer();
        const bool playingRejected = [&] {
            copy->start();
            const bool r = !copy->loadBankState(loaded, arr, active);
            copy->stop();
            run(*copy, Script{}, 1, 1);
            copy->reset();
            return r;
        }();
        seq->reset();
        const bool loadedOk = copy->loadBankState(loaded, arr, active);
        seq->startArrangement();
        copy->startArrangement();
        seq->start();
        copy->start();
        const Events e1 = run(*seq, Script{}, 8 * kBar, 512);
        const Events e2 = run(*copy, Script{}, 8 * kBar, 512);
        check(playingRejected && loadedOk && e1 == e2 && e1.size() > 50,
              "F3 A loaded bank plays its arrangement identically to the original; loading while "
              "playing is rejected");
    }
    {
        // Malformed and unsupported data: destination never changes.
        static Bank dest;
        Arrangement destArr;
        destArr.append(0, 3);
        int destActive = 7;
        State marker = richState(5);
        dest.store(3, marker.d->pattern, marker.d->track, marker.d->quantize);
        auto unchanged = [&] {
            return destActive == 7 && destArr.size() == 1 && dest.usedCount() == 1 &&
                   sameState(loadSlot(dest, 3), marker);
        };
        auto tryLoad = [&](const std::vector<std::uint8_t>& d) {
            return pattern_io::load(d.data(), d.size(), dest, destArr, destActive);
        };
        auto withCrc = [](std::vector<std::uint8_t> d) {
            const std::uint32_t c = pattern_io::crc32(d.data(), d.size() - 4);
            for (int i = 0; i < 4; ++i) d[d.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(c >> (8 * i));
            return d;
        };
        using LS = pattern_io::LoadStatus;
        bool ok = true;
        // Every truncation.
        for (std::size_t n = 0; n < bytes.size() && ok; ++n) {
            std::vector<std::uint8_t> t(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            ok = tryLoad(t) != LS::Ok && unchanged();
        }
        const bool truncOk = ok;
        // Every single-byte corruption (CRC catches it).
        for (std::size_t i = 0; i < bytes.size() && ok; ++i) {
            std::vector<std::uint8_t> t = bytes;
            t[i] ^= 0x5A;
            ok = tryLoad(t) != LS::Ok && unchanged();
        }
        const bool flipOk = ok;
        std::vector<std::uint8_t> m = bytes;
        m[0] = 'X';
        const bool magic = tryLoad(m) == LS::BadMagic;
        m = bytes;
        m[4] = 2;
        const bool version = tryLoad(withCrc(m)) == LS::UnsupportedVersion;
        m = bytes;
        m[6] = 1;
        const bool flags = tryLoad(withCrc(m)) == LS::UnsupportedVersion;
        m = bytes;
        m[m.size() - 1] ^= 1;
        const bool crc = tryLoad(m) == LS::BadChecksum;
        // First record is PATN (id at 24). Unknown tag:
        m = bytes;
        m[16] = 'Q';
        const bool tag = tryLoad(withCrc(m)) == LS::Malformed;
        // Wrong record length.
        m = bytes;
        m[20] += 1;
        const bool len = tryLoad(withCrc(m)) == LS::Malformed;
        // Record count too large.
        m = bytes;
        m[12] += 1;
        const bool count = tryLoad(withCrc(m)) == LS::Malformed;
        // Pattern id out of range (16).
        m = bytes;
        m[24] = 16;
        m[25] = 0;
        const bool id = tryLoad(withCrc(m)) == LS::OutOfRange;
        // Invalid time signature (denominator 3).
        m = bytes;
        m[27] = 3;
        const bool shape = tryLoad(withCrc(m)) == LS::OutOfRange;
        // Quantize strength 101.
        m = bytes;
        m[35] = 101;
        const bool strength = tryLoad(withCrc(m)) == LS::OutOfRange;
        // A step velocity of 0 (first step record at 24 + 34, velocity at +3).
        m = bytes;
        m[24 + 34 + 3] = 0;
        const bool vel = tryLoad(withCrc(m)) == LS::OutOfRange;
        check(truncOk && flipOk && magic && version && flags && crc && tag && len && count && id &&
                  shape && strength && vel && unchanged(),
              "F4 Rejected: every truncation, every single-byte corruption, bad magic, version 2, "
              "flags, checksum, unknown tag, record length, record count, id, time signature, "
              "quantize and step values - the destination is never modified");
    }
    {
        // Inconsistent and capacity-exceeding data.
        using LS = pattern_io::LoadStatus;
        static Bank dest;
        Arrangement arr;
        int active = 0;
        // Duplicate pattern id: serialise a bank, then duplicate its PATN record.
        static Bank one;
        State s = richState(1);
        one.store(2, s.d->pattern, s.d->track, s.d->quantize);
        Arrangement a2;
        std::vector<std::uint8_t> d;
        pattern_io::save(one, a2, 2, d);
        // Record layout: header 16, PATN record [16, 16 + 8 + len).
        std::uint32_t len = 0;
        for (int i = 0; i < 4; ++i) len |= static_cast<std::uint32_t>(d[20 + static_cast<std::size_t>(i)]) << (8 * i);
        std::vector<std::uint8_t> dup(d.begin(), d.end() - 4);
        dup.insert(dup.begin() + 16 + 8 + static_cast<std::ptrdiff_t>(len), d.begin() + 16,
                   d.begin() + 16 + 8 + static_cast<std::ptrdiff_t>(len));
        dup[12] += 1;  // one more record
        dup.resize(dup.size() + 4);
        const std::uint32_t c = pattern_io::crc32(dup.data(), dup.size() - 4);
        for (int i = 0; i < 4; ++i) dup[dup.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(c >> (8 * i));
        const bool dupId = pattern_io::load(dup.data(), dup.size(), dest, arr, active) == LS::Malformed;
        // Active pattern not stored.
        std::vector<std::uint8_t> d2;
        pattern_io::save(one, a2, 5, d2);
        const bool act = pattern_io::load(d2.data(), d2.size(), dest, arr, active) == LS::Inconsistent;
        // Arrangement referring to an empty slot.
        Arrangement a3;
        a3.append(9, 1);
        std::vector<std::uint8_t> d3;
        pattern_io::save(one, a3, 2, d3);
        const bool arng = pattern_io::load(d3.data(), d3.size(), dest, arr, active) == LS::Inconsistent;
        // An event referring to an unknown pass: patch the first event's createdPass.
        // Event area starts after 34 + 7*steps + 13*notes + 5*passes.
        const auto info = one.info(2);
        const std::size_t ev0 = 24 + 34 + 7 * info.steps + 13 * info.notes + 5 * info.passes;
        std::vector<std::uint8_t> d4 = d;
        d4[ev0 + 5 + 40 + 4 + 8] = 99;  // createdPass low byte
        const std::uint32_t c4 = pattern_io::crc32(d4.data(), d4.size() - 4);
        for (int i = 0; i < 4; ++i) d4[d4.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(c4 >> (8 * i));
        const bool pass = pattern_io::load(d4.data(), d4.size(), dest, arr, active) == LS::Inconsistent;
        // A file that does not fit a smaller bank.
        using SmallBank = PatternBank<1, 4, 64, 8, 4>;
        static SmallBank small;
        Arrangement sa;
        int sActive = 0;
        const bool cap = pattern_io::load(d.data(), d.size(), small, sa, sActive) == LS::CapacityExceeded &&
                         small.usedCount() == 0;
        check(dupId && act && arng && pass && cap && dest.usedCount() == 0,
              "F5 Rejected: duplicate pattern id, unstored active pattern, arrangement on an empty "
              "slot, event on an unknown pass, data larger than the bank's pools");
    }
}

// ---------------------------------------------------------------------------
void commandTests() {
    std::cout << "\n-- Commands, threads, real-time safety --\n";
    {
        auto seq = makeSequencer();
        setupABC(*seq);
        auto post = [&](SequencerCommandType t, std::int64_t a = 0, std::int64_t b = 0) {
            SequencerCommand c;
            c.type = t;
            c.a = a;
            c.b = b;
            return seq->postCommand(c);
        };
        post(SequencerCommandType::CopyPattern, 1, 6);
        post(SequencerCommandType::DuplicatePattern, 2);
        post(SequencerCommandType::ClearBankPattern, 6);
        SequencerCommand len;
        len.type = SequencerCommandType::SetPatternLength;
        len.a = 8;
        len.timeSignature = {6, 8};
        len.bars = 2;
        len.stepTicks = 240;
        seq->postCommand(len);
        post(SequencerCommandType::ArrangementAppend, 1, 2);
        post(SequencerCommandType::ArrangementAppend, 0, 1);
        post(SequencerCommandType::ArrangementSetEnd, 2);
        post(SequencerCommandType::SwitchPattern, 99, 0);   // invalid id
        post(SequencerCommandType::SwitchPattern, 1, 9);    // invalid timing
        post(SequencerCommandType::ArrangementSetEnd, 7);   // invalid end
        post(SequencerCommandType::CopyPattern, 0, 77);     // invalid
        run(*seq, Script{}, 16, 16);
        bool ok = seq->bank().used(6) && seq->bank().info(6).steps == 0 && seq->lastDuplicateId() == 4 &&
                  seq->bank().info(8).bars == 2 && seq->arrangement().size() == 2 &&
                  seq->arrangement().end() == ArrangementEnd::HoldLast &&
                  seq->invalidCommandCount() == 4;
        // ReplacePattern with a PatternData owned by the sender.
        auto data = std::make_unique<PatternData>();
        patternB(data->pattern);
        data->pattern.setHit(20, 3, 3);
        SequencerCommand rep;
        rep.type = SequencerCommandType::ReplacePattern;
        rep.a = 9;
        rep.data = data.get();
        seq->postCommand(rep);
        post(SequencerCommandType::StartArrangement);
        post(SequencerCommandType::SwitchPattern, 2, static_cast<std::int64_t>(SwitchTiming::NextBar));
        run(*seq, Script{}, 16, 16);
        ok = ok && data->status.load() == 1 && seq->bank().info(9).steps == 17 &&
             seq->activePattern() == 2 && !seq->songActive();
        // Command queue overflow.
        int accepted = 0;
        for (int i = 0; i < 100; ++i) accepted += post(SequencerCommandType::CancelPatternSwitch) ? 1 : 0;
        run(*seq, Script{}, 16, 16);
        check(ok && accepted == 63 && seq->droppedCommandCount() == 37,
              "C1 Every bank / switch / arrangement command works through the queue; invalid ones "
              "counted; ReplacePattern reports status; queue overflow counted");
    }
    {
        // Two threads: a control thread posts switches, copies and song
        // commands while the audio thread renders (run under TSan).
        auto seq = makeSequencer();
        setupABC(*seq);
        seq->start();
        std::atomic<bool> done{false};
        std::atomic<int> accepted{0};
        std::thread producer([&] {
            for (int k = 0; k < 20000; ++k) {
                SequencerCommand c;
                switch (k % 6) {
                case 0: c.type = SequencerCommandType::SwitchPattern; c.a = k % 4; c.b = k % 4; break;
                case 1: c.type = SequencerCommandType::CopyPattern; c.a = k % 3; c.b = 10 + k % 5; break;
                case 2: c.type = SequencerCommandType::ArrangementClear; break;
                case 3: c.type = SequencerCommandType::ArrangementAppend; c.a = k % 3; c.b = 1; break;
                case 4: c.type = SequencerCommandType::StartArrangement; break;
                default: c.type = SequencerCommandType::DuplicatePattern; c.a = 1; break;
                }
                if (seq->postCommand(c)) accepted.fetch_add(1, std::memory_order_relaxed);
                if (k % 64 == 0) std::this_thread::yield();
            }
            done.store(true, std::memory_order_release);
        });
        std::size_t blocks = 0;
        while (!done.load(std::memory_order_acquire) || blocks < 2000) {
            seq->process(128, [](const SequencerEvent&) {});
            ++blocks;
        }
        producer.join();
        seq->process(128, [](const SequencerEvent&) {});
        bool ok = accepted.load() + static_cast<int>(seq->droppedCommandCount()) == 20000 &&
                  Bank::validId(seq->activePattern()) && seq->bank().used(seq->activePattern());
        check(ok, "C2 Commands from a second thread while rendering: all applied or counted, "
                  "state consistent (lock-free SPSC; run under TSan)");
    }
    {
        auto seq = makeSequencer();
        setupABC(*seq);
        auto replacement = std::make_unique<PatternData>();
        patternC(replacement->pattern);
        auto sink = [](const SequencerEvent&) {};
        const std::size_t before = g_allocations.load();
        const std::size_t locksBefore = g_mutexLocks.load();
        seq->arm();
        seq->start();
        for (std::int64_t pos = 0, n = 0; pos < 20 * kBar; pos += 256, ++n) {
            if (n % 37 == 0) seq->liveEvent(hit(static_cast<int>(n % 27), pos + 100));
            if (n == 100) seq->requestPatternSwitch(1, SwitchTiming::NextBar);
            if (n == 900) seq->requestPatternSwitch(2, SwitchTiming::NextBeat);
            if (n == 1500) seq->copyPattern(2, 11);
            if (n == 1600) seq->duplicatePattern(0);
            if (n == 1700) seq->clearPattern(11);
            if (n == 1800) seq->replacePattern(12, replacement->pattern, replacement->track, replacement->quantize);
            if (n == 2000) {
                seq->arrangementAppend(0, 1);
                seq->arrangementAppend(1, 2);
                seq->arrangementSetEnd(ArrangementEnd::Loop);
                seq->startArrangement();
            }
            if (n == 3000) seq->undo();
            if (n == 3100) seq->commitActivePattern();
            seq->process(256, sink);
        }
        seq->stopArrangement();
        seq->stop();
        seq->process(256, sink);
        const std::size_t allocations = g_allocations.load() - before;
        const std::size_t locks = g_mutexLocks.load() - locksBefore;
#ifdef DATABREAKS_COUNT_MUTEX
        // Positive control: the counter does see a real std::mutex lock.
        std::mutex probe;
        const std::size_t c0 = g_mutexLocks.load();
        probe.lock();
        probe.unlock();
        const bool counterWorks = g_mutexLocks.load() > c0;
        const std::string lockInfo = std::to_string(locks) + " mutex locks; counter verified";
#else
        const bool counterWorks = true;
        const std::string lockInfo = "mutex count not available under sanitizers";
#endif
        check(allocations == 0 && locks == 0 && counterWorks && seq->patternSwitchCount() >= 3,
              "C3 Zero allocations and zero mutex locks while recording, switching, copying, "
              "duplicating, clearing, replacing, running an arrangement and undoing (" +
                  std::to_string(allocations) + " allocations, " + lockInfo + ")");
    }
    {
        // A switch that cannot be stored is rejected; the pattern keeps playing.
        auto seq = makeSequencer();
        setupABC(*seq);
        // Fill the event pool with patterns stored in other slots.
        State big;
        std::uint32_t pass = big.d->track.openPass();
        for (int i = 0; i < 2000; ++i) {
            if (i % 500 == 0 && i > 0) pass = big.d->track.openPass();
            RecordedEvent e;
            e.kind = RecordedEvent::Kind::Drum;
            e.velocity = 90;
            e.startFine = i * 10;
            e.sequence = static_cast<std::uint64_t>(i + 1);
            big.d->track.add(e, pass);
        }
        bool ok = seq->replacePattern(5, big.d->pattern, big.d->track, big.d->quantize);
        // Record 100 events into the active pattern: the bank cannot hold them.
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        for (int i = 0; i < 100; ++i) sc.live.push_back(hit(1, 100 + i * 900));
        run(*seq, sc, kBar, 256);
        const std::uint64_t full0 = seq->bankFullCount();
        Script sc2;
        sc2.actions.push_back({0, [](Sequencer& s) { s.requestPatternSwitch(1); }});
        const Events ev = run(*seq, sc2, 2 * kBar, 256);  // the switch is due at 192000
        ok = ok && seq->activePattern() == 0 && seq->rejectedSwitchCount() == 1 &&
             seq->bankFullCount() == full0 + 1 && seq->recorder().track().size() == 100 &&
             countDrums(ev) > 0;
        check(ok, "C4 Bank full: a switch that cannot save the working pattern is rejected and "
                  "counted; the current pattern and its recording keep playing intact");
    }
}

// ---------------------------------------------------------------------------
Script blockScenario() {
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) {
                              s.arm();
                              s.start();
                          }});
    for (int k = 0; k < 40; ++k) sc.live.push_back(hit(k % 27, 777 + k * 11111, 20 + k * 2));
    sc.live.push_back(on(SynthTarget::Poly, 60, 90000));
    sc.live.push_back(off(SynthTarget::Poly, 60, 110000));  // across the switch
    sc.actions.push_back({4096 * 5, [](Sequencer& s) { s.requestPatternSwitch(1, SwitchTiming::NextBar); }});
    sc.actions.push_back({4096 * 30, [](Sequencer& s) {
                              QuantizeSettings q;
                              q.strength = 80;
                              q.swing = 40;
                              s.setQuantize(q);
                              s.requestPatternSwitch(2, SwitchTiming::NextBeat);
                          }});
    sc.actions.push_back({4096 * 60, [](Sequencer& s) {
                              s.disarm();
                              s.arrangementAppend(0, 1);
                              s.arrangementAppend(2, 2);
                              s.arrangementAppend(1, 1);
                              s.arrangementSetEnd(ArrangementEnd::Stop);
                              s.startArrangement();
                          }});
    return sc;
}

void blockTests() {
    std::cout << "\n-- Block-size independence --\n";
    for (double bpm : {120.0, 123.456}) {
        Events ref;
        bool have = false;
        bool same = true;
        std::string diff;
        for (std::size_t block : kBlocks) {
            auto seq = makeSequencer(bpm);
            setupABC(*seq);
            const Events ev = run(*seq, blockScenario(), 4096 * 200, block);
            if (!have) {
                ref = ev;
                have = true;
            } else if (!(ev == ref)) {
                same = false;
                diff += " " + std::to_string(block);
            }
        }
        check(same && ref.size() > 150,
              std::string("D") + (bpm == 120.0 ? "1" : "2") +
                  " Identical events for blocks 1..4096 with recording, quantize, NextBar / "
                  "NextBeat switches and an arrangement ending in Stop (" +
                  (bpm == 120.0 ? "120" : "123.456") + " BPM)" + diff);
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 028 - pattern banks, switching and arrangement tests\n";
    std::cout << "Host-side tests; not validated on FM-1 hardware.\n";
    bankTests();
    switchTests();
    independenceTests();
    editingTests();
    arrangementTests();
    persistenceTests();
    commandTests();
    blockTests();
    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
