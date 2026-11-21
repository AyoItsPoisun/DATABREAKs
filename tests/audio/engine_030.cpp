// DATABREAKs Engine 030 - resource-assumption tripwire.
//
// docs/FM1_HARDWARE_REPORT.md compares the FM-1 hardware model against the
// engine's CURRENT resource requirements. This program pins those
// requirements (object sizes, construction-time heap, worst-case DBPB
// size, numeric format, sample rate, voice counts) so that any change to
// them fails here and forces the report to be updated.
//
// It measures the HOST build only. It makes no claim about FM-1 hardware
// and simulates none: there are no hardware tests here. Object sizes are
// checked only on an LP64 host (8-byte pointers), where they were
// recorded; elsewhere they are printed and skipped.
// Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"
#include "../../core/sequencer/pattern_io.h"
#include "../../platform/audio_io.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

namespace {
std::atomic<std::size_t> g_bytes{0};
std::atomic<bool> g_counting{false};
}

__attribute__((noinline)) void* operator new(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) g_bytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }

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

struct CountingSink final : ByteSink {
    std::size_t bytes = 0;
    bool write(const std::uint8_t*, std::size_t n) override {
        bytes += n;
        return true;
    }
};

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 030 - resource-assumption tripwire (host build)\n";
    std::cout << "Documents the engine's current requirements; not a hardware test.\n\n";

    // ---- R1: object sizes quoted in docs/FM1_HARDWARE_REPORT.md ------------------
    struct Row {
        const char* name;
        std::size_t actual;
        std::size_t documented;  // native __int128 build
        std::size_t portable;    // -DDATABREAKS_PORTABLE_INT128 build (8-byte-aligned clock)
    };
    const Row rows[] = {
        {"AudioPipeline<16>", sizeof(AudioPipeline<16>), 919840, 919832},
        {"Sequencer", sizeof(Sequencer), 655264, 655256},
        {"Pattern (working)", sizeof(Pattern), 127024, 127024},
        {"RecordedTrack (working)", sizeof(RecordedTrack), 182584, 182584},
        {"DefaultPatternBank", sizeof(DefaultPatternBank), 200480, 200480},
        {"Arrangement", sizeof(Arrangement), 272, 272},
        {"LoadScratch<DefaultPatternBank>", sizeof(pattern_io::LoadScratch<DefaultPatternBank>), 510088, 510088},
        {"Reverb", sizeof(Reverb), 163560, 163560},
        {"DrumEngine<16>", sizeof(DrumEngine<16>), 45544, 45544},
        {"TapeEffect", sizeof(TapeEffect), 10080, 10080},
        {"SequencerEvent", sizeof(SequencerEvent), 32, 32},
        {"StereoFrame", sizeof(StereoFrame), 16, 16},
    };
    for (const Row& r : rows) std::printf("  %-34s %9zu bytes\n", r.name, r.actual);
    if (sizeof(void*) == 8) {
        bool ok = true;
        for (const Row& r : rows) ok = ok && r.actual == (ExactInt128IsNative ? r.documented : r.portable);
        check(ok, "R1 Object sizes equal the figures in docs/FM1_HARDWARE_REPORT.md (LP64 host)");
    } else {
        check(true, "R1 (object sizes recorded on an LP64 host; skipped on this host)");
    }

    // ---- R2: heap allocated when the pipeline is constructed -------------------------
    {
        g_bytes = 0;
        g_counting = true;
        auto* p = new AudioPipeline<16>();
        g_counting = false;
        const std::size_t heap = g_bytes.load() - sizeof(AudioPipeline<16>);
        std::printf("\n  AudioPipeline<16> construction heap (besides the object): %zu bytes\n", heap);
        check(heap == 268800, "R2 Pipeline construction allocates 268,800 heap bytes (TapeMachine "
                              "delay lines), as documented");
        delete p;
    }

    // ---- R3: worst-case saved bank (every pool full) ---------------------------------
    {
        static DefaultPatternBank bank;
        auto d = std::make_unique<PatternData>();
        d->pattern.configure({4, 4}, 16, 240);  // 256 steps
        for (std::size_t pad = 0; pad < 2; ++pad) {
            for (std::size_t k = 0; k < 256; ++k) d->pattern.setHit(pad, k, 90);
        }
        for (int i = 0; i < 64; ++i) {
            d->pattern.addNote({true, SynthTarget::Poly, 40 + i, 90, 100, i * 100, 200});
        }
        for (int pass = 0; pass < 16; ++pass) {
            const std::uint32_t id = d->track.openPass();
            for (int e = 0; e < 8; ++e) {
                RecordedEvent r;
                r.kind = RecordedEvent::Kind::Drum;
                r.value = e;
                r.velocity = 100;
                r.startFine = pass * 1000 + e;
                r.sequence = static_cast<std::uint64_t>(pass * 8 + e + 1);
                d->track.add(r, id);
            }
        }
        bool stored = true;
        for (int id = 0; id < 16; ++id) stored = stored && bank.store(id, d->pattern, d->track, d->quantize);
        Arrangement arr;
        for (int i = 0; i < 64; ++i) arr.append(i % 16, 1);
        CountingSink sink;
        pattern_io::save(bank, arr, 0, sink);
        std::printf("  Worst-case DBPB (all pools full, 16 x 16 passes, 64 entries): %zu bytes\n",
                    pattern_io::encodedSize(bank, arr));
        check(stored && bank.stepsUsed() == DefaultPatternBank::StepPool &&
                  bank.notesUsed() == DefaultPatternBank::NotePool &&
                  bank.eventsUsed() == DefaultPatternBank::EventPool &&
                  pattern_io::encodedSize(bank, arr) == 206026 && sink.bytes == 206026,
              "R3 A completely full bank serialises to 206,026 bytes (the documented worst case)");
    }

    // ---- R4: numeric format, rate and voice assumptions -------------------------------
    {
        AudioStreamConfig cfg;
        cfg.sampleRate = 44100;
        const bool rejects441 = !cfg.valid();
        check(std::is_same<decltype(StereoFrame{}.left), double>::value && SAMPLE_RATE == 48000 &&
                  CHANNELS == 2 && rejects441,
              "R4 The audio engine is double precision, stereo, fixed at 48 kHz; the stream "
              "boundary rejects 44.1 kHz (the FM-1 open SDK's rate) - a documented incompatibility");
        const std::size_t drumVoices = 16;  // AudioPipeline<16>
        const std::size_t voices = drumVoices + 1 /*mono*/ + 4 /*FM*/ + 8 /*poly*/;
        check(voices == 29 && std::is_same<decltype(std::declval<AudioPipeline<16>&>().drums()),
                                           DrumEngine<16, 256>&>::value,
              "R4 The default pipeline runs 29 voices: 16 drum sample voices, 1 mono, 4 FM, 8 poly");
    }

    // ---- R5: sample memory in the engine's format ------------------------------------
    {
        const std::size_t perSecond = sizeof(StereoFrame) * SAMPLE_RATE;
        std::printf("  Sample memory in the engine's format: %zu bytes per second of stereo audio\n",
                    perSecond);
        check(perSecond == 768000, "R5 Samples are held as double stereo frames: 768,000 bytes "
                                   "per second at 48 kHz");
    }

    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
