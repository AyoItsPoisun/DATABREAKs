// DATABREAKs Engine 029 - portability boundary HOST benchmark.
//
// Reports measurements only (no pass/fail thresholds). The numbers describe
// THIS development host and compiler settings, NOT the FM-1, whose CPU and
// memory budget are not documented or verified.

#include "audio_pipeline.h"
#include "../../core/sequencer/pattern_io.h"
#include "../../platform/audio_io.h"
#include "../../platform/input.h"
#include "../../platform/host/file_storage.h"
#include "../../platform/host/memory_storage.h"
#include "../../platform/host/offline_audio_device.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace databreaks;
using Clock = std::chrono::steady_clock;

namespace {

volatile std::int64_t g_sink = 0;

double ns(Clock::duration d, double n) {
    return std::chrono::duration<double, std::nano>(d).count() / n;
}

template <typename Int>
void clockRow(const char* name) {
    BasicMusicalClock<Int> c(48000);
    c.setTempoMilli(123456);
    c.start();
    c.advance(5'000'000'000LL);  // ~29 h: 64-bit intermediate values overflow
    const int n = 2'000'000;
    std::int64_t acc = 0;
    const std::int64_t base = c.tickPosition();
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) acc += c.sampleOfTick(base + i);
    const double tick = ns(Clock::now() - t0, n);
    t0 = Clock::now();
    for (int i = 0; i < n; ++i) acc += c.firstTickAtOrAfter(c.samplePosition() + i);
    const double first = ns(Clock::now() - t0, n);
    t0 = Clock::now();
    for (int i = 0; i < n; ++i) acc += c.fineAtSample(c.samplePosition() + i);
    const double fine = ns(Clock::now() - t0, n);
    BasicMusicalClock<Int> s(48000);  // short session: fits the 64-bit fast path
    s.setTempoMilli(123456);
    t0 = Clock::now();
    for (int i = 0; i < n; ++i) acc += s.sampleOfTick(i);
    const double shortTick = ns(Clock::now() - t0, n);
    g_sink = acc;
    std::printf("  %-28s sampleOfTick %6.1f ns (short session %5.1f ns), firstTickAtOrAfter %6.1f ns, fineAtSample %6.1f ns\n",
                name, tick, shortTick, first, fine);
}

struct NullSink final : ByteSink {
    std::size_t bytes = 0;
    bool write(const std::uint8_t*, std::size_t n) override {
        bytes += n;
        return true;
    }
};

struct DropBlocks final : BlockSink {
    void consume(const void* data, std::size_t frames, SampleFormat) override {
        g_sink = g_sink + static_cast<const std::int16_t*>(data)[frames - 1];
    }
};

}  // namespace

int main() {
    std::printf("DATABREAKs Engine 029 - portability boundary benchmark (HOST ONLY, not FM-1)\n\n");
    std::printf("Musical clock, 48 kHz, 123.456 BPM, at a position of 5e9 samples:\n");
#if defined(__SIZEOF_INT128__)
    __extension__ typedef __int128 Native;
    clockRow<Native>("native __int128");
#endif
    clockRow<PortableInt128>("PortableInt128");

    // Sequencer block cost with the build's ExactInt128 (native, or the
    // portable type in a -DDATABREAKS_PORTABLE_INT128 build), early in a
    // session and ~29 h in (where fine-tick values exceed 64 bits).
    std::printf("\nSequencer (ExactInt128 = %s), 256-frame blocks, 27 pads x 16 steps:\n",
                ExactInt128IsNative ? "native" : "portable");
    for (std::int64_t start : {std::int64_t(0), std::int64_t(5'000'000'000LL)}) {
        auto s = std::make_unique<Sequencer>(48000);
        for (std::size_t pad = 0; pad < 27; ++pad) {
            for (std::size_t k = 0; k < 16; ++k) s->pattern().setHit(pad, k, 100);
        }
        s->start();
        s->clock().advance(start);
        const int blocks = 20000;
        std::uint64_t events = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < blocks; ++i) s->process(256, [&](const SequencerEvent&) { ++events; });
        std::printf("  position %-12lld %7.3f us per block (%llu events)\n", static_cast<long long>(start),
                    ns(Clock::now() - t0, blocks) / 1000.0, static_cast<unsigned long long>(events));
    }

    // ---- Storage ---------------------------------------------------------------
    std::printf("\nStorage (16 typical patterns: 96 steps, 8 notes, 64 events each):\n");
    static DefaultPatternBank bank;
    {
        auto d = std::make_unique<PatternData>();
        d->pattern.configure({4, 4}, 1, 240);
        for (std::size_t pad = 0; pad < 6; ++pad) {
            for (std::size_t k = 0; k < 16; ++k) d->pattern.setHit(pad, k, 90);
        }
        for (int i = 0; i < 8; ++i) d->pattern.addNote({true, SynthTarget::Poly, 50 + i, 90, 100, i * 400, 200});
        const std::uint32_t pass = d->track.openPass();
        for (int i = 0; i < 64; ++i) {
            RecordedEvent e;
            e.kind = RecordedEvent::Kind::Drum;
            e.value = i % 27;
            e.velocity = 100;
            e.startFine = i * 40000;
            e.sequence = static_cast<std::uint64_t>(i + 1);
            d->track.add(e, pass);
        }
        for (int id = 0; id < 16; ++id) bank.store(id, d->pattern, d->track, d->quantize);
    }
    Arrangement arr;
    for (int i = 0; i < 16; ++i) arr.append(i, 2);
    const int reps = 50;
    std::vector<std::uint8_t> bytes;
    auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pattern_io::save(bank, arr, 0, bytes);
    std::printf("  save -> std::vector (Engine 028 API)          %8.0f us  (%zu bytes)\n",
                ns(Clock::now() - t0, reps) / 1000.0, bytes.size());
    NullSink nullSink;
    t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pattern_io::save(bank, arr, 0, nullSink);
    std::printf("  save -> ByteSink (streaming)                  %8.0f us\n", ns(Clock::now() - t0, reps) / 1000.0);
    static MemoryStorage<120000, 2> store;
    t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pattern_io::saveTo(store, "bank", bank, arr, 0);
    std::printf("  saveTo MemoryStorage (atomic replace)         %8.0f us\n", ns(Clock::now() - t0, reps) / 1000.0);

    static DefaultPatternBank dest;
    Arrangement a2;
    int active = 0;
    t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pattern_io::load(bytes.data(), bytes.size(), dest, a2, active);
    std::printf("  load <- pointer (heap scratch, Engine 028 API)%8.0f us\n", ns(Clock::now() - t0, reps) / 1000.0);
    static pattern_io::LoadScratch<DefaultPatternBank> scratch;
    const MemoryByteSource src(bytes.data(), bytes.size());
    t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pattern_io::load(src, scratch, dest, a2, active);
    std::printf("  load <- ByteSource (caller scratch)           %8.0f us\n", ns(Clock::now() - t0, reps) / 1000.0);
    t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pattern_io::loadFrom(store, "bank", scratch, dest, a2, active);
    std::printf("  loadFrom MemoryStorage                        %8.0f us\n", ns(Clock::now() - t0, reps) / 1000.0);
    {
        char tmpl[] = "/tmp/databreaks029bXXXXXX";
        const char* dir = mkdtemp(tmpl);
        if (dir != nullptr) {
            FileStorage files(dir);
            t0 = Clock::now();
            for (int i = 0; i < 10; ++i) pattern_io::saveTo(files, "bank.dbpb", bank, arr, 0);
            const double save = ns(Clock::now() - t0, 10) / 1000.0;
            t0 = Clock::now();
            for (int i = 0; i < 10; ++i) pattern_io::loadFrom(files, "bank.dbpb", scratch, dest, a2, active);
            const double load = ns(Clock::now() - t0, 10) / 1000.0;
            std::printf("  FileStorage saveTo / loadFrom                 %8.0f / %.0f us\n", save, load);
            char p[512];
            std::snprintf(p, sizeof p, "%s/bank.dbpb", dir);
            std::remove(p);
            std::remove(dir);
        }
    }
    std::printf("  LoadScratch<DefaultPatternBank>: %zu bytes (caller-provided)\n",
                sizeof(pattern_io::LoadScratch<DefaultPatternBank>));

    // ---- Audio I/O -------------------------------------------------------------
    std::printf("\nAudio I/O (pipeline with the sequencer playing, effects bypassed):\n");
    static std::vector<StereoFrame> kick(4800, StereoFrame{0.3, 0.3});
    auto make = [&] {
        auto p = std::make_unique<AudioPipeline<16>>();
        p->drums().assignSample(0, &kick);
        for (int k = 0; k < 16; ++k) p->sequencer().pattern().setHit(0, static_cast<std::size_t>(k), 100);
        p->sequencer().start();
        return p;
    };
    const std::size_t frames = 48000 * 10;
    for (std::size_t block : {std::size_t(64), std::size_t(256)}) {
        auto p = make();
        std::vector<StereoFrame> buf(block);
        std::vector<std::int16_t> out(block * 2);
        t0 = Clock::now();
        for (std::size_t pos = 0; pos < frames; pos += block) {
            std::fill(buf.begin(), buf.end(), StereoFrame{});
            p->render(buf.data(), block);
            for (std::size_t i = 0; i < block; ++i) {
                out[2 * i] = static_cast<std::int16_t>(std::lround(clampAudio(buf[i].left) * 32767.0));
                out[2 * i + 1] = static_cast<std::int16_t>(std::lround(clampAudio(buf[i].right) * 32767.0));
            }
            g_sink = g_sink + out[0];
        }
        const double direct = ns(Clock::now() - t0, frames);
        auto q = make();
        PipelineRenderer<AudioPipeline<16>> renderer(*q);
        AudioStreamConfig cfg;
        cfg.blockFrames = block;
        auto device = std::make_unique<OfflineAudioDevice<>>(cfg);
        device->start(renderer);
        DropBlocks drop;
        t0 = Clock::now();
        device->run(frames, &drop);
        const double viaDevice = ns(Clock::now() - t0, frames);
        std::printf("  block %4zu: direct render + Int16 conversion %6.1f ns/frame; PipelineRenderer via AudioDevice %6.1f ns/frame\n",
                    block, direct, viaDevice);
    }

    // ---- Input ---------------------------------------------------------------------
    std::printf("\nInput:\n");
    {
        auto seq = std::make_unique<Sequencer>(48000);
        Sequencer& s = *seq;
        s.start();
        static QueueInputSource<1024> queue;
        InputRouter router([&s](const LiveEvent& e) { return s.liveEvent(e); });
        const int n = 200000;
        t0 = Clock::now();
        for (int i = 0; i < n; ++i) {
            queue.push({InputEventType::PadDown, static_cast<std::uint16_t>(i % 27), 100, 1000000000});
            if (i % 256 == 255) {
                router.pump(queue, 256);
                s.process(32, [](const SequencerEvent&) {});  // drains the sequencer's pending input
            }
        }
        std::printf("  queue push + pump + route + sequencer intake: %.1f ns per pad event\n",
                    ns(Clock::now() - t0, n));
    }
    std::printf("\nsink %lld\n", static_cast<long long>(g_sink % 10));
    return 0;
}
