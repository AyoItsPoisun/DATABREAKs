// DATABREAKs Engine 027 - non-destructive quantization tests.
//
// Host-side assertion tests for core/sequencer/quantize.h and the
// quantized playback of the recorded track in sequencer.h. The quantizer
// is checked against an independent brute-force reference (every grid
// point enumerated), and playback against exact sample positions.
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
#include <utility>
#include <vector>

namespace {
std::atomic<std::size_t> g_allocations{0};
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

// std::stable_sort and friends use the nothrow form: count and pair it too.
__attribute__((noinline)) void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}

__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

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
constexpr std::int64_t kSixteenth = 6000;  // samples
constexpr std::int64_t kSixteenthFine = 240 * 1024;

const QuantizeGrid kGrids[] = {QuantizeGrid::Whole,     QuantizeGrid::Half,
                               QuantizeGrid::Quarter,   QuantizeGrid::Eighth,
                               QuantizeGrid::Sixteenth, QuantizeGrid::ThirtySecond,
                               QuantizeGrid::SixtyFourth};
const std::int64_t kGridTicks[] = {3840, 1920, 960, 480, 240, 120, 60};

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

std::unique_ptr<Sequencer> makeSequencer(double bpm = 120.0, int bars = 1,
                                         TimeSignature ts = {4, 4}) {
    auto seq = std::make_unique<Sequencer>(48000);
    seq->clock().setTempo(bpm);
    seq->pattern().configure(ts, bars, 60);
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

// Same harness as engine_026: positions relative to the clock at the start;
// actions at block starts (multiples of 4096 for block-size independence).
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

// Exact fine position of a sample at a fixed tempo from 0.
std::int64_t exactFine(std::int64_t s, int fs, std::int64_t milliBpm) {
    const i128 num = static_cast<i128>(s) * milliBpm * 960 * 1024;
    const i128 den = static_cast<i128>(fs) * 60000;
    return static_cast<std::int64_t>(num / den);
}

// ---- Independent reference quantizer (brute force) --------------------------

std::int64_t refDestination(std::int64_t p, std::int64_t loopFine, std::int64_t gridTicks,
                            int swing) {
    const std::int64_t gf = gridTicks * 1024;
    const std::int64_t sw = static_cast<std::int64_t>(swing) * gf / 200;
    std::vector<std::int64_t> points;
    for (std::int64_t k = 0; k * gf < loopFine; ++k) {
        const std::int64_t pt = k * gf + (k % 2 == 1 ? sw : 0);
        if (pt < loopFine) points.push_back(pt);
    }
    points.push_back(loopFine);
    std::int64_t best = points[0];
    for (std::int64_t pt : points) {
        const std::int64_t d = pt > p ? pt - p : p - pt;
        const std::int64_t bd = best > p ? best - p : p - best;
        if (d < bd || (d == bd && pt > best)) best = pt;
    }
    return best;
}

std::int64_t refPosition(std::int64_t p, std::int64_t loopFine, std::int64_t gridTicks,
                         int swing, int strength) {
    const std::int64_t d = refDestination(p, loopFine, gridTicks, swing);
    // Move |d - p| x strength / 100, rounded toward p (never past d).
    if (d >= p) return p + (d - p) * strength / 100;
    return p - (p - d) * strength / 100;
}

QuantizeSettings settings(QuantizeGrid g, int strength, int swing = 0,
                          unsigned targets = RecordAll) {
    QuantizeSettings s;
    s.grid = g;
    s.strength = strength;
    s.swing = swing;
    s.targets = targets;
    return s;
}

const std::size_t kBlocks[] = {1, 16, 32, 64, 128, 256, 512, 1024, 4096};

// ---------------------------------------------------------------------------
void mathTests() {
    std::cout << "\n-- Quantizer math (against a brute-force reference) --\n";
    {
        bool ok = true;
        for (int i = 0; i < 7; ++i) ok = ok && quantize::gridTicks(kGrids[i]) == kGridTicks[i];
        check(ok, "Q1 Grid table: whole 3840 ... sixty-fourth 60 ticks (exact at 960 PPQ)");
    }
    {
        // Every grid x swing x loop shape, positions just before/on/after
        // every grid point plus a dense sweep.
        const std::int64_t loops[] = {3840 * 1024, 2880 * 1024 /*3/4*/, 3360 * 1024 /*7/8*/,
                                      2 * 4800 * 1024 /*2 bars 5/4*/, 120 * 1024 /*1/32*/};
        bool ok = true;
        std::size_t cases = 0;
        for (int gi = 0; gi < 7 && ok; ++gi) {
            for (int swing : {0, 25, 50, 67, 100}) {
                for (std::int64_t L : loops) {
                    QuantizeSettings s = settings(kGrids[gi], 100, swing);
                    const std::int64_t gf = kGridTicks[gi] * 1024;
                    std::vector<std::int64_t> ps;
                    for (std::int64_t k = 0; k * gf <= L + gf; ++k) {
                        const std::int64_t sw = (k % 2) ? swing * gf / 200 : 0;
                        for (std::int64_t base : {k * gf, k * gf + sw}) {
                            for (std::int64_t d : {-1, 0, 1}) {
                                if (base + d >= 0 && base + d < L) ps.push_back(base + d);
                            }
                        }
                        // Midpoints (ties) between consecutive points.
                        const std::int64_t mid = k * gf + gf / 2;
                        for (std::int64_t d : {-1, 0, 1}) {
                            if (mid + d >= 0 && mid + d < L) ps.push_back(mid + d);
                        }
                    }
                    for (std::int64_t p = 0; p < L; p += 7919) ps.push_back(p);
                    for (std::int64_t p : ps) {
                        ++cases;
                        ok = ok && quantize::destination(p, L, s) ==
                                       refDestination(p, L, kGridTicks[gi], swing);
                        if (!ok) {
                            std::cout << "  mismatch grid " << gi << " swing " << swing << " L "
                                      << L << " p " << p << '\n';
                            break;
                        }
                    }
                    if (!ok) break;
                }
                if (!ok) break;
            }
        }
        check(ok && cases > 100000,
              "Q2 Destination = nearest grid point (ties later) for 7 grids x 5 swings x 5 loop "
              "shapes, before/on/after every point and midpoint (" + std::to_string(cases) +
                  " cases)");
    }
    {
        bool ok = true;
        const std::int64_t L = kLoopFine;
        for (int strength : {0, 1, 25, 50, 75, 99, 100}) {
            for (int gi = 0; gi < 7; ++gi) {
                QuantizeSettings s = settings(kGrids[gi], strength, 30);
                for (std::int64_t p = 0; p < L; p += 1237) {
                    const std::int64_t q = quantize::position(p, L, s);
                    const std::int64_t d = refDestination(p, L, kGridTicks[gi], 30);
                    ok = ok && q == refPosition(p, L, kGridTicks[gi], 30, strength) &&
                         q >= std::min(p, d) && q <= std::max(p, d) && q >= 0 && q <= L;
                    if (strength == 0) ok = ok && q == p;
                    if (strength == 100) ok = ok && q == d;
                    // Exact rational check: |q - (p + (d - p) x P / 100)| < 1.
                    const i128 exact100 = static_cast<i128>(p) * 100 + static_cast<i128>(d - p) * strength;
                    const i128 diff = static_cast<i128>(q) * 100 - exact100;
                    ok = ok && diff > -100 && diff < 100;
                }
            }
        }
        check(ok, "Q3 Strength 0/1/25/50/75/99/100: proportional move, never past the grid point, "
                  "0 = original exactly, 100 = grid point exactly (error < 1 fine tick)");
    }
    {
        // Swing placement, exactly.
        const std::int64_t gf = kSixteenthFine;
        bool ok = true;
        for (int swing : {0, 10, 50, 67, 100}) {
            QuantizeSettings s = settings(QuantizeGrid::Sixteenth, 100, swing);
            for (std::int64_t k = 0; k < 16; ++k) {
                const std::int64_t expect = k * gf + (k % 2 ? swing * gf / 200 : 0);
                ok = ok && quantize::destination(expect, kLoopFine, s) == expect;
            }
        }
        QuantizeSettings s50 = settings(QuantizeGrid::Sixteenth, 100, 50);
        QuantizeSettings s100 = settings(QuantizeGrid::Sixteenth, 100, 100);
        ok = ok && quantize::swingOffsetFine(s50) == gf / 4 && quantize::swingOffsetFine(s100) == gf / 2;
        // Straight at 0: odd points unmoved.
        ok = ok && quantize::destination(gf + 5, kLoopFine, settings(QuantizeGrid::Sixteenth, 100, 0)) == gf;
        // 7/8 bar with eighth grid: 7 points, the last (k = 6) is even.
        ok = ok && quantize::destination(3360 * 1024 - 1, 3360 * 1024,
                                         settings(QuantizeGrid::Eighth, 100, 100)) == 3360 * 1024;
        check(ok, "Q4 Swing: odd points delayed by floor(S x step / 200) (S = 50: quarter step, "
                  "100: half step), even points fixed, 0 = straight");
    }
    {
        // A swung point beyond the loop end does not exist; the loop end is
        // never swung.
        const std::int64_t L = 3 * kSixteenthFine;  // points 0, 1(swung), 2, end
        QuantizeSettings s = settings(QuantizeGrid::Eighth, 100, 100);  // eighth = 2 sixteenths
        // Eighth points: k=0 at 0, k=1 at 2 sixteenths + 1 sixteenth = 3 sixteenths = L -> gone.
        check(quantize::destination(2 * kSixteenthFine + 10, L, s) == L &&
                  quantize::destination(kSixteenthFine - 1, L, s) == 0,
              "Q5 Swung point at the loop end is dropped; the loop end (next loop's 0) is a point");
    }
}

// ---------------------------------------------------------------------------
// A loose recorded drum performance: one hit per 16th, offsets up to +-40%.
std::vector<LiveEvent> loosePerformance(std::int64_t loopStart, int pads = 27) {
    std::vector<LiveEvent> v;
    std::uint32_t r = 12345;
    for (int k = 0; k < 16; ++k) {
        r = r * 1664525u + 1013904223u;
        const std::int64_t jitter = static_cast<std::int64_t>(r >> 16) % 4801 - 2400;
        const std::int64_t at = std::max<std::int64_t>(0, k * kSixteenth + jitter);
        v.push_back(hit(k % pads, loopStart + at, 40 + k * 5));
    }
    return v;
}

// Records `live` from sample 0 for `loops` loops, then disarms (the next
// run starts at a loop start).
std::unique_ptr<Sequencer> recorded(const std::vector<LiveEvent>& live, double bpm = 120.0,
                                    int loops = 1) {
    auto seq = makeSequencer(bpm);
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
    sc.live = live;
    run(*seq, sc, loops * kLoop, 256);
    seq->disarm();
    return seq;
}

// Expected playback of a recorded drum track in loop `loop` (absolute
// loop index), from the reference quantizer.
Events expectedDrums(Sequencer& seq, std::int64_t loop, std::int64_t gridTicks, int swing,
                     int strength) {
    Events v;
    const RecordedTrack& t = seq.recorder().track();
    const std::int64_t L = seq.pattern().lengthTicks() * 1024;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const RecordedEvent& e = t.at(i);
        for (std::int64_t occ : {loop - 1, loop}) {
            const std::int64_t q = refPosition(e.startFine, L, gridTicks, swing, strength);
            const std::int64_t fine = occ * L + q;
            if (fine < loop * L || fine >= (loop + 1) * L || occ == e.captureLoop) continue;
            v.push_back({seq.clock().sampleOfFine(fine), Kind::Drum, SynthTarget::Mono, e.value,
                         e.velocity});
        }
    }
    std::stable_sort(v.begin(), v.end(), [](const Captured& a, const Captured& b) {
        if (a.sample != b.sample) return a.sample < b.sample;
        return a.value < b.value;
    });
    return v;
}

void playbackTests() {
    std::cout << "\n-- Quantized playback --\n";
    const auto live = loosePerformance(0);
    {
        bool ok = true;
        std::string bad;
        for (int gi = 0; gi < 7; ++gi) {
            for (int strength : {0, 25, 50, 75, 100}) {
                auto seq = recorded(live);
                seq->setQuantize(settings(kGrids[gi], strength));
                const Events ev = run(*seq, Script{}, 2 * kLoop, 512);  // loops 1, 2
                const bool same = between(ev, 0, kLoop) == shifted(expectedDrums(*seq, 1, kGridTicks[gi], 0, strength), -kLoop) &&
                                  between(ev, kLoop, 2 * kLoop) ==
                                      shifted(expectedDrums(*seq, 2, kGridTicks[gi], 0, strength), -kLoop);
                if (!same) bad += " " + std::to_string(gi) + "/" + std::to_string(strength);
                ok = ok && same;
            }
        }
        check(ok, "P1 Loose 16-hit performance replays at the exact quantized samples for all 7 "
                  "grids x strength 0/25/50/75/100, over two loops" + bad);
    }
    {
        bool ok = true;
        for (int swing : {0, 20, 50, 67, 100}) {
            auto seq = recorded(live);
            seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100, swing));
            const Events ev = run(*seq, Script{}, 3 * kLoop, 333);
            for (std::int64_t l = 0; l < 3; ++l) {
                ok = ok && between(ev, l * kLoop, (l + 1) * kLoop) ==
                               shifted(expectedDrums(*seq, l + 1, 240, swing, 100), -kLoop);
            }
        }
        // Exact sample check for swing 50 on a hit played exactly on 16th 3.
        auto seq = recorded({hit(2, 3 * kSixteenth)});
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100, 50));
        const Events ev = run(*seq, Script{}, 2 * kLoop, 64);
        ok = ok && ev.size() == 2 && ev[0].sample == 3 * kSixteenth + kSixteenth / 4 &&
             ev[1].sample == kLoop + 3 * kSixteenth + kSixteenth / 4;
        check(ok, "P2 Swing 0/20/50/67/100 at full strength over three loops; swing 50 delays "
                  "an off-beat 16th by exactly 1500 samples in every loop");
    }
    {
        // Loop boundaries: 1 sample before / on / after the loop end.
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.setQuantize(settings(QuantizeGrid::Sixteenth, 100));
                                  s.arm();
                                  s.start();
                              }});
        sc.live.push_back(hit(1, kLoop - 1));
        sc.live.push_back(hit(2, kLoop));
        sc.live.push_back(hit(3, kLoop + 1));
        const Events ev = run(*seq, sc, 4 * kLoop, 128);
        // Pad 1 (captured in loop 0) quantizes to the loop end: occurrence 0
        // (sample 96000) is its capture and is skipped; then 192000, 288000.
        Events pad1;
        Events pad23;
        for (const auto& e : ev) (e.value == 1 ? pad1 : pad23).push_back(e);
        const Events want1 = {{kLoop - 1, Kind::Drum, SynthTarget::Mono, 1, 100},
                              {2 * kLoop, Kind::Drum, SynthTarget::Mono, 1, 100},
                              {3 * kLoop, Kind::Drum, SynthTarget::Mono, 1, 100}};
        check(pad1 == want1 && seq->playedStartFine(0) == kLoopFine &&
                  seq->recorder().track().at(0).startFine == kLoopFine - 41,
              "P3 Hit 1 sample before the loop end quantizes to the next loop's start, exactly "
              "once per loop (no double next to the live hit)");
        const Events want23 = {{kLoop, Kind::Drum, SynthTarget::Mono, 2, 100},
                               {kLoop + 1, Kind::Drum, SynthTarget::Mono, 3, 100},
                               {2 * kLoop, Kind::Drum, SynthTarget::Mono, 2, 100},
                               {2 * kLoop, Kind::Drum, SynthTarget::Mono, 3, 100},
                               {3 * kLoop, Kind::Drum, SynthTarget::Mono, 2, 100},
                               {3 * kLoop, Kind::Drum, SynthTarget::Mono, 3, 100}};
        check(pad23 == want23,
              "P4 Hits on and just after the loop start quantize to it; live hits sound in the "
              "capture loop only once");
    }
    {
        // After reset (new epoch), a hit quantized onto the loop end must
        // sound on the very first downbeat (sample 0): the occurrence of the
        // "previous" loop lands exactly on the timeline start.
        auto seq = recorded({hit(6, kLoop - 700), hit(7, 3 * kSixteenth + 200)});
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100));
        seq->reset();
        const Events ev = run(*seq, Script{}, 2 * kLoop, 4096);
        const Events want = {{0, Kind::Drum, SynthTarget::Mono, 6, 100},
                             {3 * kSixteenth, Kind::Drum, SynthTarget::Mono, 7, 100},
                             {kLoop, Kind::Drum, SynthTarget::Mono, 6, 100},
                             {kLoop + 3 * kSixteenth, Kind::Drum, SynthTarget::Mono, 7, 100}};
        check(ev == want && seq->timelineEpoch() == 1,
              "P4b After reset, a hit quantized to the loop end plays on the first downbeat "
              "(sample 0) and every loop start after it");
    }
    {
        // Many loops at an irregular tempo: no cumulative drift.
        auto seq = makeSequencer(123.456);
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(4, 3 * 5832 + 777));  // near 16th 3
        run(*seq, sc, 200000, 4096);
        seq->disarm();
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100));
        const std::int64_t L = seq->pattern().lengthTicks();
        // Run ~1000 loops with big blocks and capture the last few.
        std::int64_t last = -1;
        std::int64_t count = 0;
        bool exact = true;
        const std::int64_t total = seq->clock().sampleOfTick(1000 * L);
        while (seq->clock().samplePosition() < total) {
            const std::int64_t base = seq->clock().samplePosition();
            seq->process(4096, [&](const SequencerEvent& e) {
                const std::int64_t s = base + static_cast<std::int64_t>(e.offset);
                // Which loop? The exact expected sample is the tick of 16th 3.
                const std::int64_t loop = seq->clock().firstTickAtOrAfter(s) / L;
                exact = exact && s == seq->clock().sampleOfTick(loop * L + 3 * 240);
                last = s;
                ++count;
            });
        }
        check(exact && count >= 995 && last == seq->clock().sampleOfTick(999 * L + 720),
              "P5 1,000 loops at 123.456 BPM: every quantized hit lands on the exact tick sample "
              "(no drift)");
    }
}

// ---------------------------------------------------------------------------
void selectionTests() {
    std::cout << "\n-- Event selection and notes --\n";
    // One loose event of each kind: drums on pad 0 and 9, mono, FM, poly notes.
    std::vector<LiveEvent> live = {hit(0, kSixteenth + 900), hit(9, 5 * kSixteenth - 700),
                                   on(SynthTarget::Mono, 40, 2 * kSixteenth + 1100),
                                   off(SynthTarget::Mono, 40, 4 * kSixteenth),
                                   on(SynthTarget::Fm, 60, 6 * kSixteenth - 1300),
                                   off(SynthTarget::Fm, 60, 7 * kSixteenth + 50),
                                   on(SynthTarget::Poly, 64, 9 * kSixteenth + 1400),
                                   off(SynthTarget::Poly, 64, 12 * kSixteenth + 100)};
    {
        const unsigned masks[] = {RecordDrums, RecordMono, RecordFm, RecordPoly, RecordAll};
        bool ok = true;
        for (unsigned mask : masks) {
            auto seq = recorded(live);
            const RecordedTrack& t = seq->recorder().track();
            std::vector<RecordedEvent> raw;
            for (std::size_t i = 0; i < t.size(); ++i) raw.push_back(t.at(i));
            seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100, 0, mask));
            for (std::size_t i = 0; i < t.size(); ++i) {
                const bool sel = (targetBit(t.at(i)) & mask) != 0;
                const std::int64_t q = seq->playedStartFine(i);
                ok = ok && (sel ? q % kSixteenthFine == 0 && q != t.at(i).startFine
                                : q == t.at(i).startFine) &&
                     seq->playedLengthFine(i) == t.at(i).lengthFine &&
                     t.at(i).sameContent(raw[i]);
            }
        }
        check(ok, "N1 Targets: drums / mono / FM / poly / all each move only the selected kind; "
                  "lengths, velocities and the raw events unchanged");
    }
    {
        // Every drum lane individually.
        std::vector<LiveEvent> pads;
        for (int p = 0; p < 27; ++p) pads.push_back(hit(p, p * 3000 + 700, 10 + p * 4));
        auto seq = recorded(pads);
        const RecordedTrack& t = seq->recorder().track();
        bool ok = t.size() == 27;
        for (int lane = 0; lane < 27 && ok; ++lane) {
            QuantizeSettings s = settings(QuantizeGrid::Sixteenth, 100, 0, RecordDrums);
            s.drumLanes = 1u << lane;
            ok = seq->setQuantize(s);
            for (std::size_t i = 0; i < t.size(); ++i) {
                const bool moved = seq->playedStartFine(i) != t.at(i).startFine;
                ok = ok && moved == (t.at(i).value == lane);
            }
        }
        Events ev;
        {
            QuantizeSettings s = settings(QuantizeGrid::Sixteenth, 100, 0, RecordDrums);
            s.drumLanes = (1u << 3) | (1u << 26);
            seq->setQuantize(s);
            ev = run(*seq, Script{}, kLoop, 64);
        }
        bool vel = ev.size() == 27;
        for (const auto& e : ev) vel = vel && e.velocity == 10 + e.value * 4;
        check(ok && vel, "N2 All 27 drum lanes selectable one at a time (only that pad moves); "
                         "velocities kept");
    }
    {
        // Durations preserved by default: note-off moves with the note-on.
        auto seq = recorded(live);
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100));
        const Events ev = run(*seq, Script{}, kLoop, 100);
        const RecordedTrack& t = seq->recorder().track();
        bool ok = true;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const RecordedEvent& e = t.at(i);
            if (e.kind != RecordedEvent::Kind::Note) continue;
            const std::int64_t onFine = kLoopFine + seq->playedStartFine(i);
            const std::int64_t onS = seq->clock().sampleOfFine(onFine);
            const std::int64_t offS = seq->clock().sampleOfFine(onFine + e.lengthFine);
            int found = 0;
            for (const auto& c : ev) {
                if (c.target != e.target || c.value != e.value) continue;
                if (c.kind == Kind::NoteOn && c.sample + kLoop == onS) ++found;
                if (c.kind == Kind::NoteOff && c.sample + kLoop == offS) ++found;
            }
            ok = ok && found == 2 && seq->playedLengthFine(i) == e.lengthFine;
        }
        check(ok && countKind(ev, Kind::NoteOn) == 3 && countKind(ev, Kind::NoteOff) == 3,
              "N3 Note durations preserved by default (note-off follows the moved note-on)");
    }
    {
        // Optional note-off quantization.
        std::vector<LiveEvent> notes = {
            on(SynthTarget::Mono, 40, 2 * kSixteenth + 1100), off(SynthTarget::Mono, 40, 4 * kSixteenth - 900),
            // Short note: start and end quantize to the same point -> keeps its length.
            on(SynthTarget::Fm, 61, 6 * kSixteenth - 400), off(SynthTarget::Fm, 61, 6 * kSixteenth + 300),
            // Crossing the loop end.
            on(SynthTarget::Poly, 50, 15 * kSixteenth + 500), off(SynthTarget::Poly, 50, kLoop + 2 * kSixteenth - 800)};
        auto seq = recorded(notes, 120.0, 2);  // the loop-crossing note ends in loop 1
        QuantizeSettings s = settings(QuantizeGrid::Sixteenth, 100);
        s.quantizeNoteOff = true;
        seq->setQuantize(s);
        const RecordedTrack& t = seq->recorder().track();
        const std::int64_t len0 = seq->playedLengthFine(0);
        const std::int64_t len1 = seq->playedLengthFine(1);
        const std::int64_t len2 = seq->playedLengthFine(2);
        bool ok = seq->playedStartFine(0) == 2 * kSixteenthFine && len0 == 2 * kSixteenthFine &&
                  len1 == t.at(1).lengthFine && seq->playedStartFine(2) == 15 * kSixteenthFine &&
                  len2 == 3 * kSixteenthFine;
        // 50% strength: end and start both move half way.
        s.strength = 50;
        seq->setQuantize(s);
        const RecordedEvent& e0 = t.at(0);
        const std::int64_t qs = refPosition(e0.startFine, kLoopFine, 240, 0, 50);
        const std::int64_t qe = refPosition(e0.startFine + e0.lengthFine, kLoopFine, 240, 0, 50);
        ok = ok && seq->playedLengthFine(0) == qe - qs;
        // Play several loops and stop: every note-on gets its note-off.
        s.strength = 100;
        seq->setQuantize(s);
        // Notes started during recording may still sound when the run starts.
        const std::size_t sounding = seq->activeNoteCount();
        Events ev = run(*seq, Script{}, 3 * kLoop, 77);
        seq->stop();
        seq->process(16, [&](const SequencerEvent& e) {
            ev.push_back({0, e.kind, e.target, e.value, e.velocity});
        });
        bool positive = true;
        for (std::size_t i = 0; i < t.size(); ++i) positive = positive && seq->playedLengthFine(i) >= 1;
        // The quantized note-off is what plays: mono note 2 x 16th -> 4 x 16th
        // in every loop of the run (the run starts on a loop start).
        int monoOffs = 0;
        for (const auto& e : ev) {
            if (e.kind == Kind::NoteOff && e.target == SynthTarget::Mono && e.value == 40) {
                ok = ok && e.sample % kLoop == 4 * kSixteenth;
                ++monoOffs;
            }
        }
        ok = ok && monoOffs == 3;
        check(ok && positive &&
                  countKind(ev, Kind::NoteOn) + sounding == countKind(ev, Kind::NoteOff) &&
                  countKind(ev, Kind::NoteOn) == 9 &&
                  seq->activeNoteCount() == 0,
              "N4 Optional note-off quantization: ends on the grid, partial strength on both "
              "ends, too-short notes keep their length, loop-crossing notes, no stuck notes");
    }
    {
        // Two notes on one synth, different pitches, quantized onto one start.
        auto seq = recorded({on(SynthTarget::Poly, 60, 4 * kSixteenth - 500),
                             on(SynthTarget::Poly, 67, 4 * kSixteenth + 600),
                             off(SynthTarget::Poly, 60, 6 * kSixteenth),
                             off(SynthTarget::Poly, 67, 9 * kSixteenth)});
        seq->setQuantize(settings(QuantizeGrid::Quarter, 100));
        const Events ev = run(*seq, Script{}, kLoop, 32);
        const Events want = {{4 * kSixteenth, Kind::NoteOn, SynthTarget::Poly, 60, 100},
                             {4 * kSixteenth, Kind::NoteOn, SynthTarget::Poly, 67, 100},
                             {4 * kSixteenth + 2 * kSixteenth + 500, Kind::NoteOff, SynthTarget::Poly, 60, 0},
                             {4 * kSixteenth + 5 * kSixteenth - 600, Kind::NoteOff, SynthTarget::Poly, 67, 0}};
        check(ev == want, "N5 Notes quantized onto one start keep their own note-offs");
    }
    {
        // Ties: same pad quantized onto one sample -> capture order;
        // pattern first, then recorded.
        auto seq = makeSequencer();
        seq->pattern().setHit(5, 4 * 4, 7);  // pattern step at a quarter (60-tick steps)
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) { s.arm(); s.start(); }});
        sc.live.push_back(hit(5, 4 * kSixteenth + 900, 30));
        sc.live.push_back(hit(5, 4 * kSixteenth - 800, 20));
        sc.live.push_back(hit(2, 4 * kSixteenth + 100, 50));
        run(*seq, sc, kLoop, 256);
        seq->disarm();
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100));
        const Events ev = run(*seq, Script{}, kLoop, 1);
        const Events at = between(ev, 4 * kSixteenth, 4 * kSixteenth + 1);
        // Capture order is time order: the 23200-sample hit (velocity 20)
        // was captured before the 24900-sample hit (velocity 30).
        check(at.size() == 4 && at[0].value == 2 && at[1].velocity == 7 && at[2].velocity == 20 &&
                  at[3].velocity == 30,
              "N6 Simultaneous quantized events: pad order, then pattern before recorded, then "
              "capture order");
    }
}

// ---------------------------------------------------------------------------
void nonDestructiveTests() {
    std::cout << "\n-- Non-destructive behaviour --\n";
    const auto live = loosePerformance(0);
    auto seq = recorded(live);
    const RecordedTrack& t = seq->recorder().track();
    std::vector<RecordedEvent> raw;
    for (std::size_t i = 0; i < t.size(); ++i) raw.push_back(t.at(i));
    const Events original = run(*seq, Script{}, kLoop, 256);  // loop 1, unquantized

    // Many changes of grid, strength and swing.
    bool same = true;
    std::uint32_t r = 99;
    for (int n = 0; n < 200; ++n) {
        r = r * 1664525u + 1013904223u;
        QuantizeSettings s = settings(kGrids[(r >> 8) % 7], static_cast<int>((r >> 12) % 101),
                                      static_cast<int>((r >> 20) % 101));
        s.quantizeNoteOff = (r & 1) != 0;
        seq->setQuantize(s);
        for (std::size_t i = 0; i < t.size(); ++i) {
            same = same && seq->playedStartFine(i) ==
                               refPosition(raw[i].startFine, kLoopFine,
                                           quantize::gridTicks(s.grid), s.swing, s.strength);
        }
    }
    check(same, "D1 200 random grid/strength/swing changes: each result computed from the raw "
                "timing (matches the reference every time, nothing compounds)");
    bool rawKept = t.size() == raw.size();
    for (std::size_t i = 0; i < t.size() && rawKept; ++i) rawKept = t.at(i).sameContent(raw[i]);
    check(rawKept, "D2 The recorded track is untouched (every field of every event)");

    seq->setQuantize(settings(QuantizeGrid::Eighth, 0, 80));
    bool zero = true;
    for (std::size_t i = 0; i < t.size(); ++i) zero = zero && seq->playedStartFine(i) == raw[i].startFine;
    const Events back = run(*seq, Script{}, kLoop, 256);  // loop 2
    check(zero && back == original,
          "D3 Strength back to 0 restores the original timestamps and playback exactly");

    // Idempotence and A -> B -> A.
    const QuantizeSettings a = settings(QuantizeGrid::Sixteenth, 60, 40);
    const QuantizeSettings b = settings(QuantizeGrid::Quarter, 100, 0);
    seq->setQuantize(a);
    std::vector<std::int64_t> qa;
    for (std::size_t i = 0; i < t.size(); ++i) qa.push_back(seq->playedStartFine(i));
    const Events evA = run(*seq, Script{}, kLoop, 256);
    seq->setQuantize(a);
    seq->setQuantize(b);
    run(*seq, Script{}, kLoop, 256);
    seq->setQuantize(a);
    bool repeat = true;
    for (std::size_t i = 0; i < t.size(); ++i) repeat = repeat && seq->playedStartFine(i) == qa[i];
    const Events evA2 = run(*seq, Script{}, kLoop, 256);
    check(repeat && evA2 == evA,
          "D4 Same settings twice give identical output; A -> B -> A returns exactly to A");
}

// ---------------------------------------------------------------------------
void recordingTests() {
    std::cout << "\n-- Engine 026 regression with quantization --\n";
    {
        // Recording while quantization is on stores raw timing.
        auto seq = makeSequencer();
        Script sc;
        sc.actions.push_back({0, [](Sequencer& s) {
                                  s.setQuantize(settings(QuantizeGrid::Quarter, 100, 50));
                                  s.arm();
                                  s.start();
                              }});
        const std::int64_t times[] = {1234, 30001, 50505};
        for (std::int64_t x : times) sc.live.push_back(hit(1, x));
        const Events ev = run(*seq, sc, 2 * kLoop, 128);
        const RecordedTrack& t = seq->recorder().track();
        bool rawOk = t.size() == 3;
        for (std::size_t i = 0; i < t.size() && rawOk; ++i) {
            rawOk = t.at(i).startFine == exactFine(times[i], 48000, 120000);
        }
        const Events loop0 = between(ev, 0, kLoop);
        check(rawOk && loop0.size() == 3 && loop0[0].sample == 1234 && loop0[1].sample == 30001,
              "R1 Recording with quantization on stores raw timing; live input sounds unquantized");
        check(countKind(between(ev, kLoop, 2 * kLoop), Kind::Drum) == 3,
              "R2 Quantized replay starts in the next loop");
    }
    {
        // Undo / redo with quantization active.
        auto seq = makeSequencer();
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100));
        seq->start();
        RecordedTrack& t = seq->recorder().track();
        for (int pass = 0; pass < 3; ++pass) {
            seq->arm();
            Script sc;
            sc.live.push_back(hit(pass, 1000 + pass * 7000 + 333));  // inside the pass
            run(*seq, sc, 4096 * 8, 256);
            seq->disarm();
        }
        run(*seq, Script{}, kLoop - (4096 * 24) % kLoop, 512);  // to a loop start
        const Events all3 = run(*seq, Script{}, kLoop, 512);
        seq->undo();
        const Events two = run(*seq, Script{}, kLoop, 512);
        seq->redo();
        const Events all3b = run(*seq, Script{}, kLoop, 512);
        bool onGrid = true;
        for (const auto& e : all3) onGrid = onGrid && e.sample % kSixteenth == 0;
        check(all3.size() == 3 && two.size() == 2 && all3b == all3 && onGrid &&
                  t.undoablePassCount() == 3,
              "R3 Undo/redo of recording passes with quantization on: the quantized playback "
              "follows (cache refreshed on every change)");
    }
}

// ---------------------------------------------------------------------------
Script blockScenario() {
    Script sc;
    sc.actions.push_back({0, [](Sequencer& s) {
                              s.pattern().setHit(0, 0, 120);
                              s.arm();
                              s.start();
                          }});
    for (int k = 0; k < 30; ++k) {
        sc.live.push_back(hit((k * 5) % 27, 211 + k * 3089, 30 + (k * 17) % 90));
    }
    sc.live.push_back(on(SynthTarget::Fm, 70, 9999));
    sc.live.push_back(off(SynthTarget::Fm, 70, 31111));
    sc.live.push_back(on(SynthTarget::Poly, 55, 94500));
    sc.live.push_back(off(SynthTarget::Poly, 55, 101000));
    sc.actions.push_back({4096 * 25, [](Sequencer& s) {
                              s.disarm();
                              s.setQuantize(settings(QuantizeGrid::Sixteenth, 75, 33));
                          }});
    sc.actions.push_back({4096 * 50, [](Sequencer& s) {
                              QuantizeSettings q = settings(QuantizeGrid::Eighth, 100, 60);
                              q.quantizeNoteOff = true;
                              s.setQuantize(q);
                              s.arm();
                          }});
    sc.live.push_back(hit(13, 4096 * 52 + 1717));  // recorded while quantized
    sc.actions.push_back({4096 * 75, [](Sequencer& s) {
                              s.disarm();
                              s.setQuantize(settings(QuantizeGrid::Quarter, 50, 0, RecordDrums));
                          }});
    sc.actions.push_back({4096 * 100, [](Sequencer& s) {
                              s.setQuantize(settings(QuantizeGrid::Quarter, 0));
                          }});
    return sc;
}

void blockTests() {
    std::cout << "\n-- Block-size independence --\n";
    for (double bpm : {120.0, 123.456}) {
        Events ref;
        bool haveRef = false;
        bool same = true;
        std::string diff;
        for (std::size_t block : kBlocks) {
            auto seq = makeSequencer(bpm);
            const Events ev = run(*seq, blockScenario(), 4096 * 125, block);
            if (!haveRef) {
                ref = ev;
                haveRef = true;
            } else if (!(ev == ref)) {
                same = false;
                diff += " " + std::to_string(block);
            }
        }
        check(same && ref.size() > 150,
              std::string("K") + (bpm == 120.0 ? "1" : "2") +
                  " Identical events for block sizes 1..4096 with quantize changes during "
                  "playback (" + (bpm == 120.0 ? "120" : "123.456") + " BPM)" + diff);
    }
}

// ---------------------------------------------------------------------------
void controlTests() {
    std::cout << "\n-- Validation, commands, real-time safety --\n";
    {
        auto seq = makeSequencer();
        const QuantizeSettings good = settings(QuantizeGrid::Eighth, 40, 20);
        bool ok = seq->setQuantize(good);
        QuantizeSettings bad[8];
        for (auto& b : bad) b = good;
        bad[0].strength = -1;
        bad[1].strength = 101;
        bad[2].swing = -1;
        bad[3].swing = 101;
        bad[4].grid = static_cast<QuantizeGrid>(7);
        bad[5].targets = 0x10;
        bad[6].drumLanes = 1u << 27;
        bad[7].strength = 1000;
        for (const auto& b : bad) ok = ok && !seq->setQuantize(b) && seq->quantizeSettings() == good;
        check(ok, "V1 Invalid strength, swing, grid, targets or lane mask rejected; settings "
                  "unchanged");
    }
    {
        auto seq = makeSequencer();
        SequencerCommand c;
        c.type = SequencerCommandType::SetQuantize;
        c.quantize = settings(QuantizeGrid::Half, 101);
        seq->postCommand(c);
        c.quantize = settings(QuantizeGrid::Half, 55);
        seq->postCommand(c);
        seq->process(16, [](const SequencerEvent&) {});
        bool ok = seq->invalidCommandCount() == 1 && seq->quantizeSettings() == c.quantize;
        int accepted = 0;
        for (int i = 0; i < 100; ++i) {
            c.quantize.strength = i % 101;
            accepted += seq->postCommand(c) ? 1 : 0;
        }
        seq->process(16, [](const SequencerEvent&) {});
        check(ok && accepted == 63 && seq->droppedCommandCount() == 37 &&
                  seq->quantizeSettings().strength == 62,
              "V2 SetQuantize via the command queue: invalid ones counted, 63 accepted, overflow "
              "counted, the last accepted applies at the next block");
    }
    {
        // No allocation: quantized playback, setting changes (direct and
        // queued), recording with quantization on, undo/redo.
        auto seq = makeSequencer(120.0, 2);
        std::vector<LiveEvent> live;
        for (int k = 0; k < 300; ++k) {
            live.push_back(hit(k % 27, 500 + k * 1601));
            if (k % 9 == 0) live.push_back(on(SynthTarget::Poly, 40 + k % 40, 500 + k * 1601));
            if (k % 9 == 4) live.push_back(off(SynthTarget::Poly, 40 + (k - 4) % 40, 500 + k * 1601));
        }
        SequencerCommand q;
        q.type = SequencerCommandType::SetQuantize;
        q.quantize = settings(QuantizeGrid::ThirtySecond, 80, 45);
        q.quantize.quantizeNoteOff = true;
        auto sink = [](const SequencerEvent&) {};
        const std::size_t before = g_allocations.load();
        seq->setQuantize(settings(QuantizeGrid::Sixteenth, 100, 20));
        seq->arm();
        seq->start();
        std::size_t li = 0;
        for (std::int64_t pos = 0; pos < 4 * 192000; pos += 256) {
            while (li < live.size() && live[li].sampleTime < pos + 256) seq->liveEvent(live[li++]);
            if (pos == 192000) seq->postCommand(q);
            if (pos == 300032) seq->undo();
            if (pos == 400128) seq->redo();
            if (pos == 500224) seq->setQuantize(settings(QuantizeGrid::Whole, 30));
            seq->process(256, sink);
        }
        seq->stop();
        seq->process(256, sink);
        const std::size_t allocations = g_allocations.load() - before;
        check(allocations == 0 && seq->quantizeRefreshCount() > 0,
              "V3 Zero allocations: quantized playback, recording, setting changes, undo/redo (" +
                  std::to_string(allocations) + ")");
    }
    {
        // Strength 0 output is the Engine 026 output (quantization off).
        auto a = recorded(loosePerformance(0));
        auto b = recorded(loosePerformance(0));
        QuantizeSettings zero = settings(QuantizeGrid::Sixteenth, 0, 100);
        zero.quantizeNoteOff = true;
        b->setQuantize(zero);
        check(run(*a, Script{}, 2 * kLoop, 100) == run(*b, Script{}, 2 * kLoop, 100) &&
                  b->quantizeRefreshCount() == 0,
              "V4 Strength 0 (any grid/swing) plays exactly like Engine 026 and computes nothing");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 027 - non-destructive quantization tests\n";
    std::cout << "Host-side tests; not validated on FM-1 hardware.\n";
    mathTests();
    playbackTests();
    selectionTests();
    nonDestructiveTests();
    recordingTests();
    blockTests();
    controlTests();
    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
