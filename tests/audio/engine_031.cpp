// DATABREAKs Engine 031 - target resource profiles, RAM accounting and the
// zero-initialised pattern bank.
//
// Host-side assertion test. It checks:
//   P   the HOST and FM1 profiles (core/target/target_profile.h): HOST equals
//       the engine; FM1 values carry the evidence the Engine 030 report gives;
//   A   the RAM accounting (core/target/resource_model.h): exact for the host,
//       real types where a profile already controls them, the FM-1 budget
//       check, the load-time peak and the remaining gap - all printed;
//   Z   the zero-initialised PatternBank: a default bank is all zero bytes
//       (placed in .bss), and every observable state is identical to Engine
//       030's (fingerprints recorded from commit 418bc65).
// It measures the host build only. It does not simulate FM-1 hardware and
// does not show that a finished firmware fits: see the "not accounted" list
// it prints. Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"
#include "engine_031_bank_state.h"
#include "../../core/sequencer/pattern_io.h"
#include "../../core/target/resource_model.h"
#include "../../core/target/target_profile.h"
#include "../../platform/audio_io.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

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

#if defined(__linux__) && defined(__GNUC__)
// GNU ld symbols bounding .bss in the executable.
extern "C" char __bss_start;
extern "C" char _end;
#define DATABREAKS_HAVE_BSS_BOUNDS 1
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

// Engine 030 (commit 418bc65) fingerprints of the bank script in
// engine_031_bank_state.h, recorded by running the same script against the
// unmodified pattern_bank.h.
constexpr std::uint64_t kStage[5] = {0xdd8f92040d6779e5ull, 0xd49a1c8534c3e1f1ull, 0x8e7b12b401556764ull,
                                     0xf085b03ef55d9562ull, 0x48435c867078b428ull};
constexpr std::uint64_t kDbpb = 0xcf9b5ee6e302bf8full;
constexpr std::size_t kDbpbSize = 14702;
// Same script on PatternBank<2, 8, 300, 40, 40> (small pools: stores fail
// and are counted).
constexpr std::uint64_t kSmallStage[5] = {0xdd8f92040d6779e5ull, 0xd49a1c8534c3e1f1ull,
                                          0xaa2aae58ccdc722eull, 0xdea457c18d8f8fb0ull,
                                          0x61dad670bdbf70b2ull};
constexpr std::uint64_t kSmallDbpb = 0xb053fe3109cda5b0ull;
constexpr std::size_t kSmallDbpbSize = 1391;
// A new Sequencer's bank (slot 0 holds the default working pattern).
constexpr std::uint64_t kSequencerBank = 0x1b8b0c3aefedd5a5ull;

using SmallBank = PatternBank<2, 8, 300, 40, 40>;
using Fm1Bank = ProfilePatternBank<Fm1Profile>;
using Fm1Pipeline = ProfilePipeline<Fm1Profile>;

DefaultPatternBank g_staticBank;  // static storage: must be all zero, in .bss
Fm1Bank g_staticFm1Bank;

bool matches(const e031::ScriptResult& r, const std::uint64_t (&stage)[5], std::uint64_t dbpb,
             std::size_t size) {
    for (int i = 0; i < 5; ++i) {
        if (r.stage[i] != stage[i]) return false;
    }
    return r.dbpb == dbpb && r.dbpbSize == size;
}

template <typename T>
std::size_t nonZeroBytes(const T& object) {
    const auto* b = reinterpret_cast<const unsigned char*>(&object);
    std::size_t n = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) n += b[i] != 0 ? 1 : 0;
    return n;
}

template <typename T>
bool inBss(const T& object) {
#ifdef DATABREAKS_HAVE_BSS_BOUNDS
    const char* p = reinterpret_cast<const char*>(&object);
    return p >= &__bss_start && p + sizeof(T) <= &_end;
#else
    (void)object;
    return true;
#endif
}

template <typename P>
void printReport(const ResourceReport<P>& r) {
    std::printf("\n  Resource report, profile %s\n", r.profile);
    for (std::size_t i = 0; i < r.count; ++i) {
        const ResourceItem& it = r.items[i];
        std::printf("    %-86s %9zu  %-10s", it.name, it.bytes, toString(it.basis));
        if (it.basis == Basis::Excluded && it.ifIncluded != 0) std::printf("  (would be %zu)", it.ifIncluded);
        if (it.lifetime == Lifetime::WhileLoading) std::printf("  [while loading]");
        std::printf("\n");
    }
    std::printf("    Sequencer %zu | AudioPipeline object %zu | tape heap %zu | samples %zu\n",
                r.sequencerBytes(), r.pipelineBytes(), r.tapeHeapBytes(), r.sampleBytes());
    std::printf("    PERMANENT %zu (measured %zu, modelled %zu, host-sized %zu) | load scratch %zu | load peak %zu\n",
                r.permanentBytes(), r.sumBasis(Basis::Measured, Lifetime::Permanent),
                r.sumBasis(Basis::Modelled, Lifetime::Permanent),
                r.sumBasis(Basis::HostSized, Lifetime::Permanent), r.loadScratchBytes(), r.loadPeakBytes());
    if (r.hasBudget()) {
        std::printf("    budget %zu | headroom %lld | headroom while loading %lld\n", r.budget, r.headroom(),
                    r.loadHeadroom());
    } else {
        std::printf("    no RAM budget (host)\n");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 031 - target resource profiles (host build)\n";
    std::cout << "Accounting and regression only; not a hardware test.\n\n";
    const bool lp64 = sizeof(void*) == 8;

    // ==== P: profiles =================================================================
    {
        AudioStreamConfig cfg;
        check(HostProfile::SampleRate == SAMPLE_RATE && HostProfile::BlockFrames == cfg.blockFrames &&
                  std::is_same<ProfilePipeline<HostProfile>, AudioPipeline<16>>::value &&
                  std::is_same<ProfilePatternBank<HostProfile>, DefaultPatternBank>::value &&
                  HostProfile::PatternMaxSteps == Pattern::MaxSteps &&
                  HostProfile::RecordedEvents == RecordedTrack::MaxEvents &&
                  HostProfile::MaxEventsPerBlock == Sequencer::MaxEventsPerBlock &&
                  HostProfile::ArrangementEntries == Arrangement::MaxEntries &&
                  HostProfile::RamBudgetBytes == 0,
              "P1 HOST profile is the existing engine value for value (also static_asserted), no budget");

        check(Fm1Profile::SampleRate == 44100 && Fm1Profile::HardwareSampleRate == 44118 &&
                  Fm1Profile::SampleRateEvidence == Evidence::Verified && Fm1Profile::BlockFrames == 128 &&
                  Fm1Profile::BlockFramesEvidence == Evidence::Verified &&
                  Fm1Profile::MappedRamBytes == 523596 && Fm1Profile::MappedRamEvidence == Evidence::Verified &&
                  Fm1Profile::SampleFlashBytes == 466944 &&
                  Fm1Profile::SampleFlashEvidence == Evidence::Verified,
              "P2 FM1 values taken from VERIFIED report facts: 44,100 Hz (44,118 Hz hardware), 128-frame "
              "blocks, 523,596 B mapped RAM0, 456 KiB flash sample bank");

        check(Fm1Profile::RamBudgetBytes == 393216 && Fm1Profile::RamBudgetBytes < Fm1Profile::MappedRamBytes &&
                  Fm1Profile::RamBudgetEvidence == Evidence::Conservative &&
                  Fm1Profile::VoiceEvidence == Evidence::Conservative &&
                  Fm1Profile::BankEvidence == Evidence::Configured &&
                  Fm1Profile::SequencerEvidence == Evidence::Configured &&
                  Fm1Profile::SampleStorageBytes == 0 && !Fm1Profile::ReverbIncluded &&
                  !Fm1Profile::TapeIncluded,
              "P3 FM1 choices are labelled as choices: 384 KiB budget and voice counts conservative "
              "(hardware UNKNOWN); pools/sequencer configured; no RAM samples, reverb or tape");
    }

    // ==== A: accounting ===============================================================
    constexpr ResourceReport<HostProfile> host = resourceReport<HostProfile>();
    constexpr ResourceReport<Fm1Profile> fm1 = resourceReport<Fm1Profile>();
    printReport(host);
    printReport(fm1);

    {
        // The real host objects, with construction-time heap counted.
        g_bytes = 0;
        g_counting = true;
        auto* p = new AudioPipeline<16>();
        g_counting = false;
        const std::size_t heap = g_bytes.load() - sizeof(AudioPipeline<16>);
        delete p;
        check(host.sequencerBytes() == sizeof(Sequencer) && host.pipelineBytes() == sizeof(AudioPipeline<16>) &&
                  host.tapeHeapBytes() == heap &&
                  host.permanentBytes() == sizeof(AudioPipeline<16>) + heap &&
                  host.loadScratchBytes() == sizeof(pattern_io::LoadScratch<DefaultPatternBank>) &&
                  host.sumBasis(Basis::Modelled, Lifetime::Permanent) == 0 &&
                  host.sumBasis(Basis::HostSized, Lifetime::Permanent) == 0,
              "A1 HOST accounting is exact: Sequencer, AudioPipeline, construction heap (" +
                  std::to_string(heap) + " B) and LoadScratch equal the real objects; nothing modelled");
    }
    {
        g_bytes = 0;
        g_counting = true;
        auto* p = new Fm1Pipeline();
        g_counting = false;
        const std::size_t total = g_bytes.load();
        const bool voices = std::is_same<Fm1Pipeline::Drums, DrumEngine<8, 256>>::value &&
                            Fm1Pipeline::Fm::voiceCapacity() == 2 && Fm1Pipeline::Poly::voiceCapacity() == 4 &&
                            Fm1Pipeline::Mono::voiceCapacity() == Fm1Profile::MonoVoices;
        delete p;
        bool rows = true;
        for (std::size_t i = 0; i < fm1.count; ++i) {
            const std::string name = fm1.items[i].name;
            if (name.find("PatternBank") != std::string::npos) rows = rows && fm1.items[i].bytes == sizeof(Fm1Bank);
            if (name.find("Drum engine") != std::string::npos) {
                rows = rows && fm1.items[i].bytes == sizeof(Fm1Pipeline::Drums);
            }
        }
        check(voices && rows && total == buildableTodayBytes<Fm1Profile>(),
              "A2 FM1 MEASURED rows are real C++ types built from the profile: PatternBank<2,8,2048,256,512>, "
              "DrumEngine<8>, FmSynthEngine<2>, PolySynthEngine<4>; the FM1-voice pipeline constructs (" +
                  std::to_string(total) + " B with its heap)");
        std::printf("  'Other' pipeline state: host %zu B, FM1-voice pipeline %zu B (alignment only)\n",
                    resource_detail::pipelineOther<AudioPipeline<16>>(),
                    resource_detail::pipelineOther<Fm1Pipeline>());
        check(resource_detail::pipelineOther<Fm1Pipeline>() >= resource_detail::pipelineOther<AudioPipeline<16>>() &&
                  resource_detail::pipelineOther<Fm1Pipeline>() - resource_detail::pipelineOther<AudioPipeline<16>>() <= 16,
              "A3 Pipeline state not controlled by a profile is the same size in both (<= 16 B of padding), "
              "so counting it at its measured size is sound");
    }
    {
        static_assert(resourceReport<Fm1Profile>().withinBudget(), "FM1 budget");
        check(fm1.withinBudget() && fm1.headroom() > 0,
              "A4 FM1 configured model fits the budget: permanent " + std::to_string(fm1.permanentBytes()) +
                  " B <= " + std::to_string(fm1.budget) + " B (headroom " + std::to_string(fm1.headroom()) +
                  " B); also a static_assert in resource_model.h");
        // Tripwire: these FM1 figures are quoted in docs/PROGRESS_REPORT.md (LP64
        // host). With the portable 128-bit clock (8- instead of 16-byte
        // aligned) the Sequencer's fixed part is 8 B smaller.
        if (lp64) {
            const std::size_t d = ExactInt128IsNative ? 0 : 8;
            check(fm1.permanentBytes() == 296040 - 2 * d && fm1.sequencerBytes() == 207648 - d &&
                      fm1.loadScratchBytes() == 130056 && host.permanentBytes() == 1188000 - d,
                  "A5 Report figures unchanged: FM1 permanent 296,040 B, FM1 Sequencer 207,648 B, FM1 "
                  "LoadScratch 130,056 B, host permanent 1,188,000 B (portable int128 layout: -16/-8/0/-8)");
        } else {
            check(true, "A5 (report figures recorded on an LP64 host; skipped)");
        }
        // Known, documented gap: the load scratch (kept as it is in Engine
        // 031) does not fit next to the engine within the budget.
        check(!fm1.loadWithinBudget() && fm1.loadHeadroom() < 0,
              "A6 KNOWN GAP reported, not hidden: while loading, FM1 needs " +
                  std::to_string(fm1.loadPeakBytes()) + " B, " + std::to_string(-fm1.loadHeadroom()) +
                  " B over budget (LoadScratch is left for the storage engine)");
        const long long gap =
            static_cast<long long>(buildableTodayBytes<Fm1Profile>()) - static_cast<long long>(Fm1Profile::RamBudgetBytes);
        std::printf("  Buildable today with FM1 voices (Sequencer, reverb and tape still host-sized): %zu B; "
                    "gap to budget %lld B\n",
                    buildableTodayBytes<Fm1Profile>(), gap);
        check(gap > 0 && buildableTodayBytes<Fm1Profile>() > fm1.permanentBytes(),
              "A7 The configured model is NOT what compiles today: the FM1-voice pipeline is still " +
                  std::to_string(gap) + " B over budget until Pattern, RecordedTrack, the Sequencer "
                  "buffers, reverb and tape take their capacities from the profile");
    }
    {
        // The FM1 bank works and the format is shared: a small host bank
        // loads into it; a bank beyond its pools is rejected without change.
        static DefaultPatternBank hostBank;
        static Fm1Bank fm1Bank;
        static pattern_io::LoadScratch<Fm1Bank> scratch;
        auto d = e031::richPattern(0);
        hostBank.store(2, d->pattern, d->track, d->quantize);
        Arrangement arr;
        arr.append(2, 1);
        std::vector<std::uint8_t> bytes;
        pattern_io::save(hostBank, arr, 2, bytes);
        Arrangement loadedArr;
        int active = -1;
        const MemoryByteSource src(bytes.data(), bytes.size());
        const auto ok = pattern_io::load(src, scratch, fm1Bank, loadedArr, active);
        std::vector<std::uint8_t> again;
        pattern_io::save(fm1Bank, loadedArr, active, again);

        auto dense = e031::densePattern();  // 1,728 steps
        for (int id = 3; id < 6; ++id) hostBank.store(id, dense->pattern, dense->track, dense->quantize);
        std::vector<std::uint8_t> big;
        pattern_io::save(hostBank, arr, 2, big);
        const std::uint64_t before = e031::bankFingerprint(fm1Bank);
        const MemoryByteSource bigSrc(big.data(), big.size());
        const auto tooBig = pattern_io::load(bigSrc, scratch, fm1Bank, loadedArr, active);
        check(ok == pattern_io::LoadStatus::Ok && again == bytes &&
                  tooBig == pattern_io::LoadStatus::CapacityExceeded && e031::bankFingerprint(fm1Bank) == before,
              "A8 The FM1 bank uses the same DBPB format: a host file round-trips byte for byte; a file "
              "beyond its pools is CapacityExceeded and changes nothing");
    }

    // ==== Z: zero-initialised pattern bank ============================================
    {
        check(nonZeroBytes(g_staticBank) == 0 && nonZeroBytes(g_staticFm1Bank) == 0,
              "Z1 A default-constructed bank is all zero bytes (Engine 030: 22,960 non-zero bytes)");
#ifdef DATABREAKS_HAVE_BSS_BOUNDS
        check(inBss(g_staticBank) && inBss(g_staticFm1Bank),
              "Z2 Static banks are placed in .bss, not initialised data (GNU ld bounds)");
#else
        check(true, "Z2 (.bss placement not checkable on this toolchain; skipped)");
#endif
        auto heapBank = std::unique_ptr<DefaultPatternBank>(new DefaultPatternBank);
        auto valueBank = std::make_unique<DefaultPatternBank>();
        check(e031::bankFingerprint(g_staticBank) == kStage[0] && e031::bankFingerprint(*heapBank) == kStage[0] &&
                  e031::bankFingerprint(*valueBank) == kStage[0],
              "Z3 Default state (static, new, value-initialised) is identical to Engine 030's, including "
              "every pristine slot's info, pass table and loaded working state");

        // A pristine slot loads exactly the default working pattern.
        auto loaded = std::make_unique<PatternData>();
        auto fresh = std::make_unique<PatternData>();
        e031::Fnv a;
        e031::Fnv b;
        g_staticBank.load(11, loaded->pattern, loaded->track, loaded->quantize);
        e031::hashWorking(a, loaded->pattern, loaded->track, loaded->quantize);
        e031::hashWorking(b, fresh->pattern, fresh->track, fresh->quantize);
        const auto info = g_staticBank.info(11);
        check(a.h == b.h && !info.used && info.timeSignature.numerator == 4 &&
                  info.timeSignature.denominator == 4 && info.bars == 1 && info.stepTicks == 240 &&
                  info.quantize == QuantizeSettings{} && g_staticBank.nextPassId(11) == 1 &&
                  g_staticBank.passState(11, 5).active && g_staticBank.passState(11, 5).id == 0,
              "Z4 A zero slot reads as the old default: unused, 4/4, 1 bar, 240-tick steps, default "
              "quantize, next pass id 1, pass entries {0, active}; it loads a default Pattern exactly");

        const auto r1 = e031::runScript(g_staticBank);
        const auto r2 = e031::runScript(*heapBank);
        static SmallBank small;
        const auto r3 = e031::runScript(small);
        check(matches(r1, kStage, kDbpb, kDbpbSize) && matches(r2, kStage, kDbpb, kDbpbSize),
              "Z5 Every bank operation (shape/quantize/clear on pristine slots, store, copy, duplicate, erase, "
              "copy-from-empty, full-pool store, clearAll) gives Engine 030's states and DBPB bytes");
        check(matches(r3, kSmallStage, kSmallDbpb, kSmallDbpbSize),
              "Z6 Same on a small bank (stores rejected for full pools and counted, as before)");

        auto seq = std::make_unique<Sequencer>(48000);
        check(e031::bankFingerprint(seq->bank()) == kSequencerBank,
              "Z7 A new Sequencer's bank is identical to Engine 030's");
    }

    std::printf("\n  NOT ACCOUNTED (outside this model; must be measured on hardware):\n"
                "    - the platform layer: USB, BLE, display strip buffers, RTOS, stacks, heap\n"
                "      (the budget leaves %zu B of RAM0 for it; its real need is UNKNOWN);\n"
                "    - sample audio: 0 B budgeted because samples must play from flash, which is\n"
                "      not implemented (the engine holds samples in RAM as double frames);\n"
                "    - the MODELLED rows are not compiled objects for the FM-1 yet;\n"
                "    - code, constants and other initialised data (flash) - not RAM;\n"
                "    - double-precision CPU cost (Engine 030 section 5.2).\n",
                Fm1Profile::MappedRamBytes - Fm1Profile::RamBudgetBytes);

    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
