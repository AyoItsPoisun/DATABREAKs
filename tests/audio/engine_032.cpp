// DATABREAKs Engine 032 - the FM-1 profile as real types.
//
// Host-side assertion test. Since Engine 032 the Sequencer, its working
// Pattern / RecordedTrack / Recorder, the pattern bank, the per-block event
// buffer and the pipeline's voices and effects are built from a target
// profile (core/target/target_profile.h). This checks:
//   H  the HOST build is unchanged: the same types, sizes, capacities, effect
//      chain, and rendered output (fingerprints recorded at Engine 031,
//      commit 1d36980, from engine_032_scene.h);
//   F  the FM-1 build really applies the FM-1 capacities: limits enforced by
//      behaviour, reverb and tape absent, the same output as the host for a
//      scene inside the FM-1 limits, no allocation while rendering;
//   S  DBPB is unchanged: a host file within the FM-1 capacities loads into
//      the FM-1 sequencer; a valid file beyond them is CapacityExceeded;
//   R  the RAM figures are measured from the real FM-1 objects.
// It runs on the development host; nothing here is FM-1 firmware, and the
// DSP is still double precision at 48 kHz in both profiles.
// Exits non-zero if any check fails.

#include "audio_pipeline.h"
#include "engine_032_scene.h"
#include "../../core/sequencer/pattern_io.h"
#include "../../core/target/resource_model.h"
#include "../../platform/storage.h"

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
std::atomic<std::size_t> g_count{0};
std::atomic<bool> g_counting{false};
}

__attribute__((noinline)) void* operator new(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_bytes.fetch_add(size, std::memory_order_relaxed);
        g_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace databreaks;

// Force every member of the FM-1 types to compile (a class template only
// compiles the members that are used; this compiles all of them).
template class databreaks::BasicPattern<Fm1Profile>;
template class databreaks::BasicRecordedTrack<Fm1Profile>;
template class databreaks::BasicRecorder<Fm1Profile>;
template class databreaks::BasicSequencer<Fm1Profile>;
template class databreaks::PatternBank<2, 8, 2048, 256, 512, Fm1Profile>;
template class databreaks::AudioPipeline<8, 256, 256, 2, 4, Fm1Profile>;

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

using HostPipe = AudioPipeline<16>;
using Fm1Pipe = ProfilePipeline<Fm1Profile>;
using Fm1Seq = BasicSequencer<Fm1Profile>;
using Fm1Bank = ProfilePatternBank<Fm1Profile>;

// Engine 031 (commit 1d36980) fingerprints of engine_032_scene.h: 96,000
// frames; identical for blocks of 128, 256 and 37 frames.
constexpr std::uint64_t kHostBypassed = 0x3f3b54eefa561161ull;
constexpr std::uint64_t kHostEffects = 0x9264b9af75fd30eeull;

// Detects whether pipeline type P has reverb() / tape() (SFINAE).
template <typename P, typename = void>
struct HasReverbAccessor : std::false_type {};
template <typename P>
struct HasReverbAccessor<P, std::void_t<decltype(std::declval<P&>().reverb())>> : std::true_type {};
template <typename P, typename = void>
struct HasTapeAccessor : std::false_type {};
template <typename P>
struct HasTapeAccessor<P, std::void_t<decltype(std::declval<P&>().tape())>> : std::true_type {};

// A cheap fingerprint of a bank's observable counters and slot table.
template <typename Bank>
std::uint64_t bankSummary(const Bank& b) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(b.usedCount());
    mix(b.stepsUsed());
    mix(b.notesUsed());
    mix(b.eventsUsed());
    for (int id = 0; id < static_cast<int>(Bank::Capacity); ++id) {
        const auto i = b.info(id);
        mix(i.used);
        mix(static_cast<std::uint64_t>(i.bars));
        mix(i.steps);
        mix(i.notes);
        mix(i.events);
    }
    return h;
}

template <typename Pipe>
std::vector<std::string> chainNames(Pipe& p) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < p.effects().size(); ++i) names.emplace_back(p.effects().slot(i)->name());
    return names;
}

// Every pad on every step of a 16-bar, 1/64-note pattern of the given type.
template <typename Seq>
std::uint64_t dropsWithDensePattern(Seq& s, std::size_t frames) {
    auto& pat = s.pattern();
    pat.configure({4, 4}, 1, 60);  // 64 steps of 1/64
    for (std::size_t pad = 0; pad < PatternLanes; ++pad) {
        for (std::size_t k = 0; k < 64; ++k) pat.setHit(pad, k, 100);
    }
    s.clock().setTempoMilli(300000);
    s.start();
    std::uint64_t delivered = 0;
    s.process(frames, [&delivered](const SequencerEvent&) { ++delivered; });
    return s.droppedEventCount();
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 032 - FM-1 profile as real types (host build)\n";
    std::cout << "Not a hardware test; the DSP is double precision at 48 kHz in both profiles.\n\n";
    const bool lp64 = sizeof(void*) == 8;

    // ==== H: host unchanged ===========================================================
    {
        check(std::is_same<HostPipe::Sequencer, Sequencer>::value &&
                  std::is_same<Sequencer, BasicSequencer<HostProfile>>::value &&
                  std::is_same<Sequencer::Bank, DefaultPatternBank>::value &&
                  std::is_same<Sequencer::Pattern, Pattern>::value &&
                  std::is_same<Sequencer::RecordedTrack, RecordedTrack>::value &&
                  std::is_same<Sequencer::Recorder, Recorder>::value &&
                  std::is_same<Sequencer::PatternData, PatternData>::value &&
                  std::is_same<Sequencer::SequencerCommand, SequencerCommand>::value &&
                  std::is_same<HostPipe::ReverbSlot, Reverb>::value &&
                  std::is_same<HostPipe::TapeSlot, TapeEffect>::value,
              "H1 Host defaults: AudioPipeline<16> owns Sequencer = BasicSequencer<HostProfile> with the "
              "host Pattern, RecordedTrack, Recorder, bank, PatternData, command type, reverb and tape");
        const std::size_t d = ExactInt128IsNative ? 0 : 8;
        check(!lp64 || (sizeof(HostPipe) == 919200 - d && sizeof(Sequencer) == 654624 - d &&
                        sizeof(Pattern) == 127024 && sizeof(RecordedTrack) == 182584 &&
                        sizeof(Recorder) == 201152 && sizeof(DefaultPatternBank) == 199840 &&
                        sizeof(pattern_io::LoadScratch<DefaultPatternBank>) == 509448),
              "H2 Host object sizes unchanged since Engine 031 (pipeline 919,200 B, Sequencer 654,624 B, "
              "Pattern 127,024 B, RecordedTrack 182,584 B, bank 199,840 B, LoadScratch 509,448 B)");
        check(Pattern::MaxSteps == 256 && Pattern::MaxNotes == 512 && RecordedTrack::MaxEvents == 2048 &&
                  RecordedTrack::MaxEventsPerPass == 512 && RecordedTrack::MaxPasses == 16 &&
                  Recorder::MaxPendingLive == 512 && Sequencer::MaxEventsPerBlock == 1024 &&
                  Sequencer::MaxActiveNotes == 256 && Sequencer::eventBufferCapacity() == 2816 &&
                  DefaultPatternBank::StepPool == 8192 && DefaultPatternBank::NotePool == 1024 &&
                  DefaultPatternBank::EventPool == 2048 && DefaultPatternBank::Capacity == 16,
              "H3 Host capacities unchanged: 256 steps, 512 notes, 2,048 events (512 per pass), 16 passes, "
              "1,024 events per block, bank 8,192/1,024/2,048, 16 slots");
        auto p = std::make_unique<HostPipe>();
        const std::vector<std::string> expected = {"Equalizer", "Compressor", "SoftClipper", "Tape", "Distortion", "Reverb"};
        const auto names = chainNames(*p);
        check(HasReverbAccessor<HostPipe>::value && HasTapeAccessor<HostPipe>::value && names == expected &&
                  p->effects().slot(3) == &p->tape() && p->effects().slot(5) == &p->reverb(),
              "H4 Host effect chain unchanged: EQ > Compressor > SoftClipper > Tape > Distortion > Reverb");
    }
    {
        std::uint64_t bypassed[3];
        std::uint64_t effects[3];
        const std::size_t blocks[3] = {128, 256, 37};
        for (int i = 0; i < 3; ++i) {
            auto p = std::make_unique<HostPipe>();
            e032::setUp(*p);
            bypassed[i] = e032::render(*p, 96000, blocks[i]);
            effects[i] = e032::hostWithEffects(96000, blocks[i]);
        }
        check(bypassed[0] == kHostBypassed && bypassed[1] == kHostBypassed && bypassed[2] == kHostBypassed,
              "H5 Host render (drums, mono, FM, poly via the sequencer) is identical to Engine 031 at blocks "
              "of 128, 256 and 37 frames");
        check(effects[0] == kHostEffects && effects[1] == kHostEffects && effects[2] == kHostEffects,
              "H6 Host render with compressor, tape and reverb enabled is identical to Engine 031");
    }

    // ==== F: FM-1 capacities applied ==================================================
    {
        check(Fm1Seq::Pattern::MaxSteps == 64 && Fm1Seq::Pattern::MaxNotes == 128 &&
                  Fm1Seq::RecordedTrack::MaxEvents == 512 && Fm1Bank::StepPool == 2048 &&
                  Fm1Bank::NotePool == 256 && Fm1Bank::EventPool == 512 && Fm1Seq::MaxEventsPerBlock == 256 &&
                  Fm1Bank::Capacity == 16 && std::is_same<Fm1Pipe::Drums, DrumEngine<8, 256>>::value &&
                  Fm1Pipe::Mono::voiceCapacity() + Fm1Pipe::Fm::voiceCapacity() + Fm1Pipe::Poly::voiceCapacity() == 7,
              "F1 FM1 types carry the FM1 capacities (also static_asserted): 64 steps, 128 notes, 512 events, "
              "bank 2,048/256/512, 256 events per block, 16 slots, 8 drum + 7 synth voices");
        auto p = std::make_unique<Fm1Pipe>();
        const auto names = chainNames(*p);
        check(!HasReverbAccessor<Fm1Pipe>::value && !HasTapeAccessor<Fm1Pipe>::value &&
                  names == std::vector<std::string>{"Equalizer", "Compressor", "SoftClipper", "Distortion"} &&
                  std::is_same<Fm1Pipe::ReverbSlot, AbsentEffect>::value &&
                  std::is_same<Fm1Pipe::TapeSlot, AbsentEffect>::value,
              "F2 FM1 pipeline has no reverb or tape: no member storage, no accessor, chain EQ > Compressor > "
              "SoftClipper > Distortion");
    }
    {
        // Limits enforced by behaviour, against the host for contrast.
        auto fs = std::make_unique<Fm1Seq>(48000);
        auto hs = std::make_unique<Sequencer>(48000);
        const bool shapes = fs->pattern().configure({4, 4}, 4, 240) && !fs->pattern().configure({4, 4}, 5, 240) &&
                            hs->pattern().configure({4, 4}, 5, 240);
        int lastOk = 0;
        int overflow = 0;
        for (int i = 0; i < 129; ++i) {
            const int r = fs->pattern().addNote({true, SynthTarget::Poly, 60, 100, 100, i, 10});
            if (i < 128) lastOk = r;
            else overflow = r;
        }
        check(shapes && lastOk == 127 && overflow == -1 && fs->pattern().noteCount() == 128,
              "F3 FM1 working pattern: 4 bars of 16ths (64 steps) accepted, 5 bars (80) rejected (host accepts "
              "it); note 129 rejected");
        auto track = std::make_unique<Fm1Seq::RecordedTrack>();
        for (int pass = 0; pass < 2; ++pass) {
            const std::uint32_t id = track->openPass();
            for (int e = 0; e < 300; ++e) {
                RecordedEvent r;
                r.value = e % 27;
                r.velocity = 100;
                r.startFine = e * 100;
                r.sequence = static_cast<std::uint64_t>(pass * 300 + e + 1);
                track->add(r, id);
            }
        }
        check(track->size() == 512 && track->trackFullCount() == 88,
              "F4 FM1 recorded track holds 512 events; the next 88 are rejected and counted");
        auto fs2 = std::make_unique<Fm1Seq>(48000);
        const std::uint64_t fmDrops = dropsWithDensePattern(*fs2, 48000);
        auto hs2 = std::make_unique<Sequencer>(48000);
        const std::uint64_t hostDrops = dropsWithDensePattern(*hs2, 48000);
        std::printf("  dense 27 x 64-step pattern, one 48,000-frame block: dropped FM1 %llu, host %llu\n",
                    static_cast<unsigned long long>(fmDrops), static_cast<unsigned long long>(hostDrops));
        check(fmDrops == 2160 - 256 && hostDrops == 2160 - 1024,
              "F5 FM1 sequencer delivers at most 256 triggers per block (host 1,024): of 2,160 triggers in "
              "one 48,000-frame block, 1,904 are dropped and counted (host 1,136)");
        auto dense = std::make_unique<Fm1Seq::PatternData>();
        dense->pattern.configure({4, 4}, 4, 240);
        for (std::size_t pad = 0; pad < PatternLanes; ++pad) {
            for (std::size_t k = 0; k < 64; ++k) dense->pattern.setHit(pad, k, 90);
        }
        static Fm1Bank bank;
        static DefaultPatternBank hostBank;
        auto half = std::make_unique<Fm1Seq::PatternData>();
        half->pattern.configure({4, 4}, 4, 240);
        for (std::size_t pad = 0; pad < 20; ++pad) {
            for (std::size_t k = 0; k < 64; ++k) half->pattern.setHit(pad, k, 90);
        }
        auto hostDense = std::make_unique<PatternData>();
        auto hostHalf = std::make_unique<PatternData>();
        hostDense->pattern.configure({4, 4}, 4, 240);
        hostHalf->pattern.configure({4, 4}, 4, 240);
        for (std::size_t pad = 0; pad < PatternLanes; ++pad) {
            for (std::size_t k = 0; k < 64; ++k) {
                hostDense->pattern.setHit(pad, k, 90);
                if (pad < 20) hostHalf->pattern.setHit(pad, k, 90);
            }
        }
        const bool fmDense = bank.store(1, dense->pattern, dense->track, dense->quantize);  // 1,728 steps
        const bool fmHalf = bank.store(2, half->pattern, half->track, half->quantize);     // +1,280 > 2,048
        const bool hostOk = hostBank.store(1, hostDense->pattern, hostDense->track, hostDense->quantize) &&
                            hostBank.store(2, hostHalf->pattern, hostHalf->track, hostHalf->quantize);
        check(fmDense && !fmHalf && bank.stepsUsed() == 1728 && bank.fullCount() == 1 && !bank.used(2) &&
                  hostOk && hostBank.stepsUsed() == 3008,
              "F6 FM1 bank pool of 2,048 steps: a 1,728-step pattern fits, a further 1,280 is rejected and "
              "counted with nothing changed (the host's 8,192-step pool takes both)");
    }
    {
        // Same scene in both profiles (inside the FM-1 limits). PolyVoice
        // pans each voice by its slot (slot / (voices - 1), Engine 022 DSP,
        // unchanged), so a 4-voice poly is spread differently from an
        // 8-voice one. Hence: without poly the FM-1 build must equal the
        // default host pipeline; with poly it must equal a host-profile
        // pipeline with the same synth voice counts.
        std::vector<std::int16_t> host;
        std::vector<std::int16_t> fm1;
        std::vector<std::int16_t> hostSameSynths;
        std::vector<std::int16_t> fm1Poly;
        auto hp = std::make_unique<HostPipe>();
        e032::setUp(*hp, false);
        e032::render(*hp, 96000, 128, &host);
        auto fp = std::make_unique<Fm1Pipe>();
        e032::setUp(*fp, false);
        e032::render(*fp, 96000, 128, &fm1);
        auto hs = std::make_unique<AudioPipeline<16, 256, 256, 2, 4, HostProfile>>();
        e032::setUp(*hs);
        e032::render(*hs, 96000, 128, &hostSameSynths);
        auto fq = std::make_unique<Fm1Pipe>();
        e032::setUp(*fq);
        e032::render(*fq, 96000, 128, &fm1Poly);
        int peak = 0;
        for (std::int16_t s : fm1Poly) peak = std::max(peak, std::abs(static_cast<int>(s)));
        check(fm1 == host && fp->sequencer().droppedEventCount() == 0 && fp->undeliveredSequencerEventCount() == 0,
              "F7 Drums, mono and FM through the FM1 pipeline are bit-identical to the default host pipeline "
              "(effects bypassed, within FM1 limits)");
        check(fm1Poly == hostSameSynths && peak > 1000 && fq->sequencer().droppedEventCount() == 0,
              "F8 With the poly chord, the FM1 pipeline equals a host-profile pipeline with the same 2 FM / 4 "
              "poly voices: the FM1 sequencer, 8 drum voices and absent reverb/tape change nothing here");

        auto fp2 = std::make_unique<Fm1Pipe>();
        e032::setUp(*fp2);
        std::vector<StereoFrame> buf(128);
        g_count = 0;
        g_counting = true;
        for (int b = 0; b < 750; ++b) {
            std::fill(buf.begin(), buf.end(), StereoFrame{});
            fp2->render(buf.data(), buf.size());
        }
        g_counting = false;
        check(g_count.load() == 0, "F9 FM1 pipeline: 0 allocations while rendering 750 blocks of 128 frames");
    }

    // ==== S: storage format unchanged ================================================
    {
        static DefaultPatternBank hostBank;
        auto small = std::make_unique<PatternData>();
        small->pattern.configure({4, 4}, 2, 240);
        for (std::size_t k = 0; k < 32; k += 2) small->pattern.setHit(5, k, 77);
        small->pattern.addNote({true, SynthTarget::Fm, 62, 90, 100, 120, 480});
        hostBank.store(0, small->pattern, small->track, small->quantize);
        Arrangement arr;
        arr.append(0, 2);
        std::vector<std::uint8_t> bytes;
        pattern_io::save(hostBank, arr, 0, bytes);

        auto fs = std::make_unique<Fm1Seq>(48000);
        static Fm1Bank staged;
        static pattern_io::LoadScratch<Fm1Bank> scratch;
        Arrangement a2;
        int active = -1;
        const MemoryByteSource src(bytes.data(), bytes.size());
        const auto ok = pattern_io::load(src, scratch, staged, a2, active);
        const bool applied = ok == pattern_io::LoadStatus::Ok && fs->loadBankState(staged, a2, active);
        std::vector<std::uint8_t> again;
        fs->commitActivePattern();
        pattern_io::save(fs->bank(), fs->arrangement(), fs->activePattern(), again);
        check(applied && again == bytes && fs->pattern().stepCount() == 32 && fs->pattern().noteCount() == 1,
              "S1 A host DBPB file within the FM1 capacities loads into the FM1 sequencer and saves back "
              "byte-identical (format and pattern ids unchanged)");

        auto wide = std::make_unique<PatternData>();
        wide->pattern.configure({4, 4}, 16, 240);  // 256 steps: valid DBPB, beyond FM1's 64
        wide->pattern.setHit(1, 200, 90);
        hostBank.store(4, wide->pattern, wide->track, wide->quantize);
        std::vector<std::uint8_t> wideBytes;
        pattern_io::save(hostBank, arr, 0, wideBytes);
        const MemoryByteSource wideSrc(wideBytes.data(), wideBytes.size());
        const auto fmStatus = pattern_io::load(wideSrc, scratch, staged, a2, active);
        static DefaultPatternBank hostDest;
        static pattern_io::LoadScratch<DefaultPatternBank> hostScratch;
        const auto hostStatus = pattern_io::load(wideSrc, hostScratch, hostDest, a2, active);
        check(fmStatus == pattern_io::LoadStatus::CapacityExceeded && hostStatus == pattern_io::LoadStatus::Ok,
              "S2 A valid file with a 256-step pattern loads on the host and is CapacityExceeded on FM1 "
              "(not OutOfRange: the file is valid)");

        // A 1-bar pattern that still holds data on step 200 (kept from a
        // longer shape; the host keeps step data beyond the current length):
        // its shape fits FM1, the step does not.
        static DefaultPatternBank keptBank;
        auto kept = std::make_unique<PatternData>();
        kept->pattern.configure({4, 4}, 16, 240);
        kept->pattern.setHit(2, 200, 90);
        kept->pattern.configure({4, 4}, 1, 240);
        keptBank.store(0, kept->pattern, kept->track, kept->quantize);
        std::vector<std::uint8_t> keptBytes;
        pattern_io::save(keptBank, arr, 0, keptBytes);
        const MemoryByteSource keptSrc(keptBytes.data(), keptBytes.size());
        const std::uint64_t before = bankSummary(staged);
        const auto keptFm = pattern_io::load(keptSrc, scratch, staged, a2, active);
        const auto keptHost = pattern_io::load(keptSrc, hostScratch, hostDest, a2, active);
        check(keptFm == pattern_io::LoadStatus::CapacityExceeded && keptHost == pattern_io::LoadStatus::Ok &&
                  bankSummary(staged) == before,
              "S3 A 1-bar pattern holding data on step 200 loads on the host and is CapacityExceeded on FM1, "
              "leaving the FM1 bank unchanged");

        std::vector<std::uint8_t> bad = bytes;
        // Corrupt a step's velocity to 0 (out of range for the format) and fix the CRC.
        bool corrupted = false;
        for (std::size_t i = 16; i + 4 < bad.size() && !corrupted; ++i) {
            if (bad[i] == 5 && bad[i + 1] == 0 && bad[i + 2] == 1 && bad[i + 3] == 77) {
                bad[i + 3] = 0;
                corrupted = true;
            }
        }
        const std::uint32_t crc = pattern_io::crc32(bad.data(), bad.size() - 4);
        for (int i = 0; i < 4; ++i) bad[bad.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
        const MemoryByteSource badSrc(bad.data(), bad.size());
        const auto badFm = pattern_io::load(badSrc, scratch, staged, a2, active);
        const auto badHost = pattern_io::load(badSrc, hostScratch, hostDest, a2, active);
        check(corrupted && badFm == pattern_io::LoadStatus::OutOfRange && badHost == pattern_io::LoadStatus::OutOfRange,
              "S4 A malformed field is OutOfRange in both profiles (format validation unchanged)");
    }

    // ==== R: RAM figures from the real FM-1 objects ===================================
    {
        constexpr auto fm1 = resourceReport<Fm1Profile>();
        g_bytes = 0;
        g_counting = true;
        auto* p = new Fm1Pipe();
        g_counting = false;
        const std::size_t total = g_bytes.load();
        delete p;
        std::printf("\n  FM1 build: pipeline object %zu B (Sequencer %zu B), construction heap %zu B, "
                    "load scratch %zu B, budget %zu B, headroom %lld B, while loading %lld B\n",
                    sizeof(Fm1Pipe), sizeof(Fm1Seq), total - sizeof(Fm1Pipe), sizeof(ProfileLoadScratch<Fm1Profile>),
                    fm1.budget, fm1.headroom(), fm1.loadHeadroom());
        check(total == sizeof(Fm1Pipe) && fm1.permanentBytes() == total && fm1.sequencerBytes() == sizeof(Fm1Seq) &&
                  fm1.countBasis(Basis::Modelled) == 0 && fm1.countBasis(Basis::HostSized) == 0 &&
                  fm1.countBasis(Basis::Excluded) == 4,
              "R1 FM1 report is measured: constructing the FM1 pipeline allocates exactly its object (no tape "
              "heap) = the report's permanent total; no modelled or host-sized rows; 4 excluded rows "
              "(reverb, tape object, tape heap, RAM samples)");
        const std::size_t d = ExactInt128IsNative ? 0 : 8;
        check(!lp64 || (fm1.permanentBytes() == 292976 - 2 * d && sizeof(Fm1Seq) == 204576 - d &&
                        sizeof(ProfileLoadScratch<Fm1Profile>) == 130056),
              "R2 FM1 figures quoted in the report: permanent 292,976 B, Sequencer 204,576 B, LoadScratch "
              "130,056 B (LP64; portable int128 layout -16/-8/0)");
        check(fm1.withinBudget() && fm1.headroom() > 0 && buildableTodayBytes<Fm1Profile>() == fm1.permanentBytes() &&
                  !fm1.loadWithinBudget(),
              "R3 FM1 build fits the 384 KiB budget (headroom " + std::to_string(fm1.headroom()) +
                  " B); the Engine 031 gap (792,240 B over) is closed; while loading it is still " +
                  std::to_string(-fm1.loadHeadroom()) + " B over (LoadScratch kept)");
    }

    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
