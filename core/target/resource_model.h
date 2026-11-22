#pragma once

// DATABREAKs Engines 031-032 - RAM accounting for a target resource profile.
//
// resourceReport<Profile>() lists the engine's major RAM consumers for a
// profile (target_profile.h), each with HOW its figure was obtained:
//
//   Measured  - sizeof() of the real C++ type this profile instantiates, or
//               an exact array size read from that type. Nothing estimated.
//   Modelled  - capacity x measured element size + a measured fixed part,
//               for a class a profile does not control. (Engine 031 needed
//               this for Pattern, RecordedTrack and the Sequencer buffers;
//               since Engine 032 they are real per-profile types and no row
//               is modelled. The basis is kept for future use.)
//   HostSized - counted at its host size because no profile controls it.
//               (No row since Engine 032.)
//   Excluded  - left out of this profile's build (the member does not
//               exist); `ifIncluded` shows what including it would cost.
//
// Since Engine 032 every profile's AudioPipeline, Sequencer, pattern bank,
// working pattern, recorded track, event buffer, voices and effects are real
// types built from the profile (ProfilePipeline<P>), so the report is a
// decomposition of real objects for EVERY profile, and the static_asserts
// below prove at compile time that:
//   (1) HostProfile is the engine value for value;
//   (2) the FM-1 instantiation uses the FM-1 profile's capacities;
//   (3) both decompositions add up to the real object sizes byte for byte;
//   (4) the FM-1 build's permanent RAM fits its budget.
//
// What this does NOT prove: that a finished FM-1 firmware fits. It leaves
// out the platform layer (USB, BLE, display, RTOS, stacks, heap), samples in
// RAM (none: flash playback is not implemented), code-related data and the
// CPU. The DSP is still double precision at 48 kHz in every profile.

#include "target_profile.h"

#include "../audio/audio_pipeline.h"
#include "../sequencer/pattern_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace databreaks {

// The real types a profile builds (ProfilePatternBank<P> is in pattern_bank.h).
template <typename P>
using ProfileDrumEngine = DrumEngine<P::DrumVoices, P::DrumEventCapacity>;
template <typename P>
using ProfilePipeline = AudioPipeline<P::DrumVoices, P::DrumEventCapacity, P::SynthEventCapacity,
                                      P::FmVoices, P::PolyVoices, P>;
template <typename P>
using ProfileLoadScratch = pattern_io::LoadScratch<ProfilePatternBank<P>>;

enum class Basis : std::uint8_t { Measured, Modelled, HostSized, Excluded };
enum class Lifetime : std::uint8_t { Permanent, WhileLoading };
// Which object the bytes belong to (for the per-object subtotals).
enum class Owner : std::uint8_t { Sequencer, PipelineObject, TapeHeap, Samples, LoadScratch };

inline const char* toString(Basis b) {
    switch (b) {
    case Basis::Measured: return "MEASURED";
    case Basis::Modelled: return "MODELLED";
    case Basis::HostSized: return "HOST-SIZED";
    case Basis::Excluded: return "EXCLUDED";
    }
    return "?";
}

struct ResourceItem {
    const char* name = "";
    std::size_t bytes = 0;
    Basis basis = Basis::Measured;
    Lifetime lifetime = Lifetime::Permanent;
    Owner owner = Owner::Sequencer;
    std::size_t ifIncluded = 0;  // Excluded items: the cost if included
};

namespace resource_detail {

// Sequencer quantize cache: quantStart_ + quantLength_ (int32 per event slot)
constexpr std::size_t quantizeCache(std::size_t events) { return 2 * events * sizeof(std::int32_t); }
// TapeMachine: delay + print-through buffers, two channels, double
constexpr std::size_t tapeHeap(std::size_t delayFrames, std::size_t printFrames) {
    return (2 * delayFrames + 2 * printFrames) * sizeof(double);
}

template <typename P>
constexpr std::size_t eventBufferBytes() {
    return BasicSequencer<P>::eventBufferCapacity() * sizeof(SequencerEvent);
}
template <typename P>
constexpr std::size_t sequencerOther() {
    return sizeof(BasicSequencer<P>) - sizeof(BasicPattern<P>) - sizeof(BasicRecorder<P>) -
           sizeof(ProfilePatternBank<P>) - sizeof(Arrangement) - eventBufferBytes<P>() -
           quantizeCache(P::RecordedEvents);
}
template <typename Pipe>
constexpr std::size_t pipelineOther() {
    return sizeof(Pipe) - sizeof(typename Pipe::Sequencer) - sizeof(typename Pipe::Drums) -
           sizeof(typename Pipe::Mono) - sizeof(typename Pipe::Fm) - sizeof(typename Pipe::Poly) -
           (Pipe::HasReverb ? sizeof(Reverb) : 0) - (Pipe::HasTape ? sizeof(TapeEffect) : 0);
}

} // namespace resource_detail

template <typename P>
struct ResourceReport {
    static constexpr std::size_t MaxItems = 16;
    const char* profile = P::Name;
    std::array<ResourceItem, MaxItems> items{};
    std::size_t count = 0;
    std::size_t budget = P::RamBudgetBytes;  // 0 = no budget

    constexpr void add(const ResourceItem& item) { items[count++] = item; }

    constexpr std::size_t sum(Owner owner) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i) n += items[i].owner == owner ? items[i].bytes : 0;
        return n;
    }
    constexpr std::size_t sumBasis(Basis basis, Lifetime lifetime) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i) {
            n += (items[i].basis == basis && items[i].lifetime == lifetime) ? items[i].bytes : 0;
        }
        return n;
    }
    constexpr std::size_t countBasis(Basis basis) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i) n += items[i].basis == basis ? 1 : 0;
        return n;
    }
    constexpr std::size_t sequencerBytes() const { return sum(Owner::Sequencer); }
    // The AudioPipeline object itself, including its Sequencer.
    constexpr std::size_t pipelineBytes() const { return sum(Owner::Sequencer) + sum(Owner::PipelineObject); }
    constexpr std::size_t tapeHeapBytes() const { return sum(Owner::TapeHeap); }
    constexpr std::size_t sampleBytes() const { return sum(Owner::Samples); }
    constexpr std::size_t loadScratchBytes() const { return sum(Owner::LoadScratch); }
    // Everything allocated for as long as the engine runs.
    constexpr std::size_t permanentBytes() const {
        return pipelineBytes() + tapeHeapBytes() + sampleBytes();
    }
    // Peak while a bank file is being loaded (scratch coexists with the engine).
    constexpr std::size_t loadPeakBytes() const { return permanentBytes() + loadScratchBytes(); }
    constexpr bool hasBudget() const { return budget != 0; }
    constexpr bool withinBudget() const { return !hasBudget() || permanentBytes() <= budget; }
    constexpr bool loadWithinBudget() const { return !hasBudget() || loadPeakBytes() <= budget; }
    constexpr long long headroom() const {
        return static_cast<long long>(budget) - static_cast<long long>(permanentBytes());
    }
    constexpr long long loadHeadroom() const {
        return static_cast<long long>(budget) - static_cast<long long>(loadPeakBytes());
    }
};

template <typename P>
constexpr ResourceReport<P> resourceReport() {
    using namespace resource_detail;
    using Pipe = ProfilePipeline<P>;
    constexpr Basis M = Basis::Measured;
    constexpr Lifetime Perm = Lifetime::Permanent;

    ResourceReport<P> r;
    // ---- Sequencer (inside the AudioPipeline) ----
    r.add({"Sequencer: working Pattern", sizeof(BasicPattern<P>), M, Perm, Owner::Sequencer, 0});
    r.add({"Sequencer: RecordedTrack (working recording)", sizeof(BasicRecordedTrack<P>), M, Perm,
           Owner::Sequencer, 0});
    r.add({"Sequencer: Recorder state besides the track",
           sizeof(BasicRecorder<P>) - sizeof(BasicRecordedTrack<P>), M, Perm, Owner::Sequencer, 0});
    r.add({"Sequencer: PatternBank (persisted patterns)", sizeof(ProfilePatternBank<P>), M, Perm,
           Owner::Sequencer, 0});
    r.add({"Sequencer: Arrangement", sizeof(Arrangement), M, Perm, Owner::Sequencer, 0});
    r.add({"Sequencer: per-block event buffer", eventBufferBytes<P>(), M, Perm, Owner::Sequencer, 0});
    r.add({"Sequencer: quantize cache", quantizeCache(P::RecordedEvents), M, Perm, Owner::Sequencer, 0});
    r.add({"Sequencer: clock, queues, active notes, other", sequencerOther<P>(), M, Perm,
           Owner::Sequencer, 0});
    // ---- Rest of the AudioPipeline object ----
    r.add({"Drum engine (voices, 27 pads, event queues)", sizeof(typename Pipe::Drums), M, Perm,
           Owner::PipelineObject, 0});
    r.add({"Synth engines (mono + FM + poly voices)",
           sizeof(typename Pipe::Mono) + sizeof(typename Pipe::Fm) + sizeof(typename Pipe::Poly), M, Perm,
           Owner::PipelineObject, 0});
    if (Pipe::HasReverb) {
        r.add({"Reverb (double delay network)", sizeof(Reverb), M, Perm, Owner::PipelineObject, 0});
    } else {
        r.add({"Reverb (double delay network)", 0, Basis::Excluded, Perm, Owner::PipelineObject,
               sizeof(Reverb)});
    }
    if (Pipe::HasTape) {
        r.add({"Tape effect object (dry line, controls)", sizeof(TapeEffect), M, Perm, Owner::PipelineObject,
               0});
        r.add({"Tape machine heap buffers (allocated at construction)",
               tapeHeap(P::TapeDelayFrames, P::TapePrintFrames), M, Perm, Owner::TapeHeap, 0});
    } else {
        r.add({"Tape effect object (dry line, controls)", 0, Basis::Excluded, Perm, Owner::PipelineObject,
               sizeof(TapeEffect)});
        // What TapeMachine would allocate at the core's fixed rate.
        r.add({"Tape machine heap buffers (allocated at construction)", 0, Basis::Excluded, Perm,
               Owner::TapeHeap, TapeEffect::machineBufferBytes()});
    }
    r.add({"Pipeline: synth/drum queues, scheduled events, EQ, compressor, clipper, distortion, master",
           pipelineOther<Pipe>(), M, Perm, Owner::PipelineObject, 0});
    // ---- Samples ----
    r.add({"Sample audio in RAM", P::SampleStorageBytes, Basis::Excluded, Perm, Owner::Samples, 0});
    // ---- Loading ----
    r.add({"pattern_io::LoadScratch (staged bank + Pattern + RecordedTrack)", sizeof(ProfileLoadScratch<P>),
           M, Lifetime::WhileLoading, Owner::LoadScratch, 0});
    return r;
}

// What constructing ProfilePipeline<P> allocates in total: the object plus
// the tape heap (if the profile has tape). Since Engine 032 this is the same
// as resourceReport<P>().permanentBytes().
template <typename P>
constexpr std::size_t buildableTodayBytes() {
    return sizeof(ProfilePipeline<P>) + (P::TapeIncluded ? TapeEffect::machineBufferBytes() : 0);
}

// ---- Compile-time proofs ------------------------------------------------------

// (1) HostProfile is the engine, value for value (and the defaults are host).
static_assert(HostProfile::SampleRate == SAMPLE_RATE, "HostProfile: sample rate");
static_assert(std::is_same<ProfilePipeline<HostProfile>, AudioPipeline<>>::value &&
                  std::is_same<AudioPipeline<>, AudioPipeline<16>>::value,
              "HostProfile: AudioPipeline voice/event counts");
static_assert(std::is_same<ProfilePatternBank<HostProfile>, DefaultPatternBank>::value,
              "HostProfile: DefaultPatternBank");
static_assert(std::is_same<BasicSequencer<HostProfile>, Sequencer>::value &&
                  std::is_same<AudioPipeline<16>::Sequencer, Sequencer>::value &&
                  std::is_same<Sequencer::Bank, DefaultPatternBank>::value,
              "HostProfile: the host pipeline owns the host Sequencer and bank");
static_assert(HostProfile::MonoVoices == 1, "HostProfile: MonoSynthEngine is one voice");
static_assert(Pattern::MaxSteps == 256 && Pattern::MaxNotes == 512 && RecordedTrack::MaxEvents == 2048 &&
                  RecordedTrack::MaxEventsPerPass == 512 && RecordedTrack::MaxPasses == 16 &&
                  Recorder::MaxPendingLive == 512 && Sequencer::MaxEventsPerBlock == 1024 &&
                  Sequencer::MaxActiveNotes == 256 && Sequencer::eventBufferCapacity() == 2816 &&
                  Arrangement::MaxEntries == HostProfile::ArrangementEntries,
              "HostProfile: host capacities unchanged since Engine 031");
static_assert(AudioPipeline<16>::HasReverb && AudioPipeline<16>::HasTape &&
                  std::is_same<AudioPipeline<16>::ReverbSlot, Reverb>::value &&
                  std::is_same<AudioPipeline<16>::TapeSlot, TapeEffect>::value,
              "HostProfile: reverb and tape present");
static_assert(resource_detail::tapeHeap(HostProfile::TapeDelayFrames, HostProfile::TapePrintFrames) ==
                  TapeEffect::machineBufferBytes(),
              "tape heap formula matches TapeEffect");

// (2) The FM-1 instantiation uses the FM-1 profile's capacities.
namespace fm1_check {
using Pipe = ProfilePipeline<Fm1Profile>;
using Seq = BasicSequencer<Fm1Profile>;
static_assert(std::is_same<Pipe::Sequencer, Seq>::value && std::is_same<Seq::Bank, ProfilePatternBank<Fm1Profile>>::value,
              "FM1: the pipeline owns the FM-1 sequencer and bank");
static_assert(Seq::Pattern::MaxSteps == 64 && Seq::Pattern::MaxNotes == 128, "FM1: working pattern 64 steps, 128 notes");
static_assert(Seq::RecordedTrack::MaxEvents == 512 && Seq::RecordedTrack::MaxPasses == 16 &&
                  std::is_same<Seq::Recorder::Track, Seq::RecordedTrack>::value,
              "FM1: 512 recorded events, 16 undo passes");
static_assert(Seq::Bank::StepPool == 2048 && Seq::Bank::NotePool == 256 && Seq::Bank::EventPool == 512 &&
                  Seq::Bank::Capacity == 16 && std::is_same<Seq::Bank::PatternType, Seq::Pattern>::value,
              "FM1: bank pools 2048/256/512, 16 slots");
static_assert(Seq::MaxEventsPerBlock == 256 && Seq::MaxActiveNotes == 64 &&
                  Seq::eventBufferCapacity() == 2 * 256 + 64 + 512,
              "FM1: 256 events per block");
static_assert(std::is_same<Pipe::Drums, DrumEngine<8, 256>>::value, "FM1: 8 drum voices");
static_assert(Pipe::Mono::voiceCapacity() + Pipe::Fm::voiceCapacity() + Pipe::Poly::voiceCapacity() == 7 &&
                  Pipe::Fm::voiceCapacity() == 2 && Pipe::Poly::voiceCapacity() == 4,
              "FM1: 7 synth voices (mono 1, FM 2, poly 4)");
static_assert(!Pipe::HasReverb && !Pipe::HasTape && std::is_same<Pipe::ReverbSlot, AbsentEffect>::value &&
                  std::is_same<Pipe::TapeSlot, AbsentEffect>::value,
              "FM1: reverb and tape are not built");
} // namespace fm1_check

// (3) Both decompositions add up to the real objects, byte for byte.
static_assert(resourceReport<HostProfile>().sequencerBytes() == sizeof(Sequencer) &&
                  resourceReport<HostProfile>().pipelineBytes() == sizeof(AudioPipeline<>) &&
                  resourceReport<HostProfile>().loadScratchBytes() ==
                      sizeof(pattern_io::LoadScratch<DefaultPatternBank>),
              "host decomposition is exact");
static_assert(resourceReport<Fm1Profile>().sequencerBytes() == sizeof(BasicSequencer<Fm1Profile>) &&
                  resourceReport<Fm1Profile>().pipelineBytes() == sizeof(ProfilePipeline<Fm1Profile>) &&
                  resourceReport<Fm1Profile>().loadScratchBytes() == sizeof(ProfileLoadScratch<Fm1Profile>) &&
                  resourceReport<Fm1Profile>().permanentBytes() == buildableTodayBytes<Fm1Profile>(),
              "FM-1 decomposition is exact and equals the real FM-1 pipeline");
static_assert(resourceReport<HostProfile>().countBasis(Basis::Modelled) == 0 &&
                  resourceReport<HostProfile>().countBasis(Basis::HostSized) == 0 &&
                  resourceReport<Fm1Profile>().countBasis(Basis::Modelled) == 0 &&
                  resourceReport<Fm1Profile>().countBasis(Basis::HostSized) == 0,
              "no modelled or host-sized rows remain");

// (4) The FM-1 build fits its budget. Raising an FM-1 capacity past the
// budget stops every build that includes this header.
static_assert(resourceReport<Fm1Profile>().withinBudget(),
              "Fm1Profile: the FM-1 build's permanent RAM exceeds Fm1Profile::RamBudgetBytes");
static_assert(Fm1Profile::RamBudgetBytes <= Fm1Profile::MappedRamBytes,
              "Fm1Profile: the budget must fit the VERIFIED mapped RAM");

} // namespace databreaks
