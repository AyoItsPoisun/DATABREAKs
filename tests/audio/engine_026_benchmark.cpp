// DATABREAKs Engine 026 - performance recording HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose CPU and
// memory budget are not documented or verified.
//
// Audio-thread rows time Sequencer::process() (events go to a counting
// sink) and report the mean and the worst block. Control-thread rows time
// the cross-thread entry points (postLive / postCommand) alone.

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
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
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t sink = 0;

double us(Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

std::unique_ptr<Sequencer> makeSequencer() {
    auto s = std::make_unique<Sequencer>(48000);
    s->clock().setTempo(120.0);
    s->pattern().configure({4, 4}, 1, 240);  // loop 96000 samples
    return s;
}

// Fills the recorded track with `count` events (drums and notes, spread over
// the loop) in passes of up to 512, using the normal capture path.
void fillTrack(Sequencer& s, int count) {
    s.start();
    int made = 0;
    while (made < count) {
        s.arm();
        const std::int64_t base = s.clock().samplePosition();
        const int n = std::min(500, count - made);
        for (int k = 0; k < n; ++k) {
            const std::int64_t at = base + (k * 191) % 90000;
            if (k % 8 == 0) {
                s.liveEvent({LiveEventType::NoteOn, static_cast<SynthTarget>(k % 3), 40 + k % 40,
                             90, at});
                s.liveEvent({LiveEventType::NoteOff, static_cast<SynthTarget>(k % 3), 40 + k % 40,
                             0, at + 2000});
            } else {
                s.liveEvent({LiveEventType::DrumHit, SynthTarget::Mono, k % 27, 100, at});
            }
        }
        for (int b = 0; b < 375; ++b) s.process(256, [](const SequencerEvent&) {});  // one loop
        s.disarm();
        made += n;
    }
}

struct Row {
    double meanUs = 0.0;
    double worstUs = 0.0;
    std::uint64_t events = 0;
    std::size_t allocs = 0;
};

// Times `blocks` process() calls; `feed(pos)` may add live input before each.
template <typename Feed>
Row timeBlocks(Sequencer& s, std::size_t block, std::size_t blocks, Feed feed) {
    Row r;
    const std::size_t a0 = g_allocations.load();
    double total = 0.0;
    for (std::size_t b = 0; b < blocks; ++b) {
        feed(s.clock().samplePosition());
        const auto t0 = Clock::now();
        s.process(block, [&](const SequencerEvent& e) {
            ++r.events;
            sink += e.offset;
        });
        const double t = us(Clock::now() - t0);
        total += t;
        r.worstUs = std::max(r.worstUs, t);
    }
    r.meanUs = total / static_cast<double>(blocks);
    r.allocs = g_allocations.load() - a0;
    return r;
}

void print(const char* name, std::size_t block, const Row& r) {
    const double blockUs = 1e6 * static_cast<double>(block) / 48000.0;
    std::printf("  %-44s block %4zu  mean %8.3f us  worst %8.2f us  (%5.1f%% / %5.1f%% of block)  "
                "events %7llu  allocs %zu\n",
                name, block, r.meanUs, r.worstUs, 100.0 * r.meanUs / blockUs,
                100.0 * r.worstUs / blockUs, static_cast<unsigned long long>(r.events), r.allocs);
}

auto noFeed = [](std::int64_t) {};

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 026 - recording benchmark (HOST ONLY, not FM-1)\n");
    std::printf("Times are wall-clock per process() call on this host.\n\n");

    std::printf("Audio thread (Sequencer::process):\n");
    for (std::size_t block : {64u, 256u}) {
        const std::size_t blocks = 8 * 96000 / block;  // 8 loops
        {
            auto s = makeSequencer();
            s->arm();
            s->start();
            Row r = timeBlocks(*s, block, blocks, noFeed);
            print("idle: armed, nothing played", block, r);
        }
        {
            // Capture: one hit per 100 samples on rotating pads (480 / s),
            // a note every 1000 samples. Per-pass capacity limits the store.
            auto s = makeSequencer();
            s->recorder().setRecordMode(RecordMode::Loop);
            s->arm();
            s->start();
            std::int64_t next = 0;
            Row r = timeBlocks(*s, block, blocks, [&](std::int64_t pos) {
                for (; next < pos + static_cast<std::int64_t>(block); next += 100) {
                    s->liveEvent({LiveEventType::DrumHit, SynthTarget::Mono,
                                  static_cast<int>(next / 100) % 27, 100, next});
                    if (next % 1000 == 0) {
                        s->liveEvent({LiveEventType::NoteOn, SynthTarget::Poly,
                                      static_cast<int>(next / 1000) % 128, 80, next});
                        s->liveEvent({LiveEventType::NoteOff, SynthTarget::Poly,
                                      static_cast<int>(next / 1000) % 128, 0, next + 500});
                    }
                }
            });
            print("capture: loop record, 480 hits/s + notes", block, r);
        }
        {
            auto s = makeSequencer();
            fillTrack(*s, 2048);
            Row r = timeBlocks(*s, block, blocks, noFeed);
            print("playback: full track (2048 events)", block, r);
        }
        {
            auto s = makeSequencer();
            fillTrack(*s, 1536);
            s->arm();
            std::int64_t next = s->clock().samplePosition();
            Row r = timeBlocks(*s, block, blocks / 2, [&](std::int64_t pos) {
                for (; next < pos + static_cast<std::int64_t>(block); next += 450) {
                    s->liveEvent({LiveEventType::DrumHit, SynthTarget::Mono,
                                  static_cast<int>(next) % 27, 90, next});
                }
            });
            print("overdub: 1536-event track + live input", block, r);
        }
        {
            auto s = makeSequencer();
            fillTrack(*s, 1536);
            s->recorder().setWriteMode(WriteMode::Replace);
            s->arm();
            Row r = timeBlocks(*s, block, blocks / 2, noFeed);
            print("replace: erasing a 1536-event track", block, r);
        }
        {
            // Dense: full-ish track + dense pattern + capture.
            auto s = makeSequencer();
            for (std::size_t pad = 0; pad < 27; ++pad) {
                for (std::size_t k = 0; k < 16; ++k) s->pattern().setHit(pad, k, 100);
            }
            for (int i = 0; i < 48; ++i) {
                s->pattern().addNote({true, static_cast<SynthTarget>(i % 3), 40 + i, 90, 100,
                                      static_cast<std::int64_t>(i) * 80, 200});
            }
            fillTrack(*s, 1536);
            s->arm();
            std::int64_t next = s->clock().samplePosition();
            Row r = timeBlocks(*s, block, blocks / 2, [&](std::int64_t pos) {
                for (; next < pos + static_cast<std::int64_t>(block); next += 300) {
                    s->liveEvent({LiveEventType::DrumHit, SynthTarget::Mono,
                                  static_cast<int>(next) % 27, 90, next});
                }
            });
            print("dense: 27x16 pattern + 48 notes + track + input", block, r);
        }
    }

    {
        // Loop boundary: worst block among blocks that contain a loop wrap
        // in loop-record mode with input on both sides of the wrap.
        auto s = makeSequencer();
        fillTrack(*s, 1024);
        s->recorder().setRecordMode(RecordMode::Loop);
        s->recorder().setWriteMode(WriteMode::Replace);
        s->arm();
        Row worst;
        for (int loop = 0; loop < 50; ++loop) {
            const std::int64_t pos = s->clock().samplePosition();
            const std::int64_t wrap = (pos / 96000 + 1) * 96000;
            while (s->clock().samplePosition() + 256 <= wrap - 128) {
                s->process(256, [](const SequencerEvent&) {});
            }
            for (int k = 0; k < 8; ++k) {
                s->liveEvent({LiveEventType::DrumHit, SynthTarget::Mono, k, 100, wrap - 4 + k});
            }
            const Row r = timeBlocks(*s, 256, 1, noFeed);
            worst.worstUs = std::max(worst.worstUs, r.worstUs);
            worst.meanUs += r.meanUs / 50.0;
            worst.events += r.events;
            worst.allocs += r.allocs;
        }
        print("loop wrap block (replace, loop record)", 256, worst);
    }

    {
        // Commit / undo / redo on a full track (each followed by one block,
        // which includes the visibility refresh).
        auto s = makeSequencer();
        fillTrack(*s, 2048);
        double undoWorst = 0.0;
        double undoTotal = 0.0;
        const std::size_t a0 = g_allocations.load();
        for (int i = 0; i < 200; ++i) {
            const auto t0 = Clock::now();
            if (i % 2 == 0) {
                s->undo();
            } else {
                s->redo();
            }
            s->process(256, [](const SequencerEvent&) {});
            const double t = us(Clock::now() - t0);
            undoTotal += t;
            undoWorst = std::max(undoWorst, t);
        }
        std::printf("  %-44s            mean %8.3f us  worst %8.2f us  allocs %zu\n",
                    "undo/redo + next block (2048 events)", undoTotal / 200.0, undoWorst,
                    g_allocations.load() - a0);
        // Commit: opening a pass when the history is full (flattens the
        // oldest pass: compaction of the 2048-event track).
        double commitWorst = 0.0;
        const std::size_t a1 = g_allocations.load();
        for (int i = 0; i < 40; ++i) {
            s->arm();
            const std::int64_t at = s->clock().samplePosition();
            s->liveEvent({LiveEventType::DrumHit, SynthTarget::Mono, 1, 100, at});
            const auto t0 = Clock::now();
            s->process(64, [](const SequencerEvent&) {});
            commitWorst = std::max(commitWorst, us(Clock::now() - t0));
            s->disarm();
        }
        std::printf("  %-44s            worst %8.2f us  allocs %zu\n",
                    "commit: new pass on full history (block 64)", commitWorst,
                    g_allocations.load() - a1);
    }

    std::printf("\nControl thread (cross-thread entry points, no consumer running):\n");
    {
        auto s = makeSequencer();
        const int n = 200000;
        const auto t0 = Clock::now();
        int accepted = 0;
        for (int i = 0; i < n; ++i) {
            if (s->postLive({LiveEventType::DrumHit, SynthTarget::Mono, i % 27, 100, i})) {
                ++accepted;
            } else {
                // Drain on the same thread to keep the ring moving (not timed
                // separately; this row is an upper bound).
                s->process(1, [](const SequencerEvent&) {});
            }
        }
        const double t = us(Clock::now() - t0);
        std::printf("  postLive (+ occasional drain)                 %8.1f ns/call  accepted %d\n",
                    1000.0 * t / n, accepted);
        SequencerCommand c;
        c.type = SequencerCommandType::SetTargets;
        const auto t1 = Clock::now();
        for (int i = 0; i < 63; ++i) s->postCommand(c);
        const double tc = us(Clock::now() - t1);
        std::printf("  postCommand (63 into an empty queue)          %8.1f ns/call\n",
                    1000.0 * tc / 63);
    }
    std::printf("\nsink %llu\n", static_cast<unsigned long long>(sink % 10));
    return 0;
}
