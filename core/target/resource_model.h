#pragma once

// DATABREAKs Engine 031 - RAM accounting for a target resource profile.
//
// resourceReport<Profile>() lists the engine's major RAM consumers for a
// profile (target_profile.h), each with HOW its figure was obtained:
//
//   Measured  - sizeof() of a real C++ type built with this profile's
//               capacities (or an unchanged type), or an exact array size
//               read from the code. Nothing estimated.
//   Modelled  - the class is NOT yet parameterised by a profile, so its
//               size for this profile is computed: the profile's capacity x
//               the measured element size + the class's fixed part as
//               measured on the host. Not a compiled object.
//   HostSized - a part that no profile value controls yet, counted at its
//               measured host size (an upper bound for a smaller target).
//   Excluded  - left out of this profile by configuration; `ifIncluded`
//               shows what including it would cost.
//
// All of it is constexpr: the static_asserts at the end prove at compile
// time that (1) HostProfile matches the engine value for value, (2) the host
// decomposition adds up to the real object sizes byte for byte, and (3) the
// FM-1 profile's configured model fits its RAM budget.
//
// What this does NOT prove: that a finished FM-1 firmware fits. The model
// leaves out the platform layer (USB, BLE, display, RTOS, stacks, heap),
// samples in RAM (none: flash playback is not implemented), code-related
// data, and the Modelled/Excluded classes are not built for the FM-1 yet.
// See docs/PROGRESS_REPORT.md (Engine 031) for the list.

#include "target_profile.h"

#include "../audio/audio_pipeline.h"
#include "../sequencer/pattern_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace databreaks {

// Types a profile's capacities instantiate today.
template <typename P>
using ProfilePatternBank = PatternBank<P::PatternBanks, P::PatternsPerBank, P::BankStepPool,
                                       P::BankNotePool, P::BankEventPool>;
template <typename P>
using ProfileDrumEngine = DrumEngine<P::DrumVoices, P::DrumEventCapacity>;
template <typename P>
using ProfilePipeline = AudioPipeline<P::DrumVoices, P::DrumEventCapacity, P::SynthEventCapacity,
                                      P::FmVoices, P::PolyVoices>;

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

// ---- Element-scaled storage, with the element sizes taken from the code ----
constexpr std::size_t patternArrays(std::size_t steps, std::size_t notes) {
    return Pattern::Lanes * steps * sizeof(DrumStep) + notes * sizeof(NoteEvent);
}
// events_ (RecordedEvent) + visible_ (bool) per event
constexpr std::size_t trackArrays(std::size_t events) {
    return events * (sizeof(RecordedEvent) + sizeof(bool));
}
// Sequencer::events_: 2 x per-block + active notes + pending live inputs
constexpr std::size_t eventBufferEntries(std::size_t perBlock, std::size_t active, std::size_t pending) {
    return 2 * perBlock + active + pending;
}
// Sequencer quantize cache: quantStart_ + quantLength_ (int32 per event)
constexpr std::size_t quantizeCache(std::size_t events) { return 2 * events * sizeof(std::int32_t); }
// TapeMachine: delay + print-through buffers, two channels, double
constexpr std::size_t tapeHeap(std::size_t delayFrames, std::size_t printFrames) {
    return (2 * delayFrames + 2 * printFrames) * sizeof(double);
}

// ---- Fixed (non-scaled) parts, measured on the host ----
constexpr std::size_t PatternFixed = sizeof(Pattern) - patternArrays(Pattern::MaxSteps, Pattern::MaxNotes);
constexpr std::size_t TrackFixed = sizeof(RecordedTrack) - trackArrays(RecordedTrack::MaxEvents);
constexpr std::size_t RecorderOther = sizeof(Recorder) - sizeof(RecordedTrack);
constexpr std::size_t HostEventBuffer = Sequencer::eventBufferCapacity() * sizeof(SequencerEvent);
constexpr std::size_t HostQuantizeCache = quantizeCache(RecordedTrack::MaxEvents);
constexpr std::size_t SequencerOther = sizeof(Sequencer) - sizeof(Pattern) - sizeof(Recorder) -
                                       sizeof(DefaultPatternBank) - sizeof(Arrangement) -
                                       HostEventBuffer - HostQuantizeCache;
template <typename Pipe>
constexpr std::size_t pipelineOther() {
    return sizeof(Pipe) - sizeof(Sequencer) - sizeof(typename Pipe::Drums) - sizeof(typename Pipe::Mono) -
           sizeof(typename Pipe::Fm) - sizeof(typename Pipe::Poly) - sizeof(Reverb) - sizeof(TapeEffect);
}
constexpr std::size_t PipelineOther = pipelineOther<AudioPipeline<>>();

template <typename P>
constexpr bool isHost() { return std::is_same<P, HostProfile>::value; }

template <typename P>
constexpr std::size_t patternBytes() {
    return PatternFixed + patternArrays(P::PatternMaxSteps, P::PatternMaxNotes);
}
template <typename P>
constexpr std::size_t trackBytes() {
    return TrackFixed + trackArrays(P::RecordedEvents);
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
    constexpr bool host = isHost<P>();
    // Host: the classes ARE these sizes. Other profiles: modelled.
    constexpr Basis scaled = host ? Basis::Measured : Basis::Modelled;
    constexpr Basis fixed = host ? Basis::Measured : Basis::HostSized;
    using Pipe = ProfilePipeline<P>;

    ResourceReport<P> r;
    // ---- Sequencer (inside the AudioPipeline) ----
    r.add({"Sequencer: working Pattern", patternBytes<P>(), scaled, Lifetime::Permanent, Owner::Sequencer, 0});
    r.add({"Sequencer: RecordedTrack (working recording)", trackBytes<P>(), scaled, Lifetime::Permanent,
           Owner::Sequencer, 0});
    r.add({"Sequencer: Recorder state besides the track", RecorderOther, fixed, Lifetime::Permanent,
           Owner::Sequencer, 0});
    r.add({"Sequencer: PatternBank (persisted patterns)", sizeof(ProfilePatternBank<P>), Basis::Measured,
           Lifetime::Permanent, Owner::Sequencer, 0});
    r.add({"Sequencer: Arrangement", sizeof(Arrangement), Basis::Measured, Lifetime::Permanent,
           Owner::Sequencer, 0});
    r.add({"Sequencer: per-block event buffer",
           eventBufferEntries(P::MaxEventsPerBlock, P::MaxActiveNotes, P::MaxPendingLive) *
               sizeof(SequencerEvent),
           scaled, Lifetime::Permanent, Owner::Sequencer, 0});
    r.add({"Sequencer: quantize cache", quantizeCache(P::RecordedEvents), scaled, Lifetime::Permanent,
           Owner::Sequencer, 0});
    r.add({"Sequencer: clock, queues, active notes, other", SequencerOther, fixed, Lifetime::Permanent,
           Owner::Sequencer, 0});
    // ---- Rest of the AudioPipeline object ----
    r.add({"Drum engine (voices, 27 pads, event queues)", sizeof(typename Pipe::Drums), Basis::Measured,
           Lifetime::Permanent, Owner::PipelineObject, 0});
    r.add({"Synth engines (mono + FM + poly voices)",
           sizeof(typename Pipe::Mono) + sizeof(typename Pipe::Fm) + sizeof(typename Pipe::Poly),
           Basis::Measured, Lifetime::Permanent, Owner::PipelineObject, 0});
    if (P::ReverbIncluded) {
        r.add({"Reverb (double delay network)", sizeof(Reverb), Basis::Measured, Lifetime::Permanent,
               Owner::PipelineObject, 0});
    } else {
        r.add({"Reverb (double delay network)", 0, Basis::Excluded, Lifetime::Permanent,
               Owner::PipelineObject, sizeof(Reverb)});
    }
    if (P::TapeIncluded) {
        r.add({"Tape effect object (dry line, controls)", sizeof(TapeEffect), Basis::Measured,
               Lifetime::Permanent, Owner::PipelineObject, 0});
        r.add({"Tape machine heap buffers (allocated at construction)",
               tapeHeap(P::TapeDelayFrames, P::TapePrintFrames), Basis::Measured, Lifetime::Permanent,
               Owner::TapeHeap, 0});
    } else {
        r.add({"Tape effect object (dry line, controls)", 0, Basis::Excluded, Lifetime::Permanent,
               Owner::PipelineObject, sizeof(TapeEffect)});
        r.add({"Tape machine heap buffers (allocated at construction)", 0, Basis::Excluded,
               Lifetime::Permanent, Owner::TapeHeap,
               tapeHeap(static_cast<std::size_t>(P::SampleRate / 10),
                        static_cast<std::size_t>(P::SampleRate / 4))});
    }
    r.add({"Pipeline: synth/drum queues, scheduled events, EQ, compressor, clipper, distortion, master",
           host ? PipelineOther : pipelineOther<Pipe>(), Basis::Measured, Lifetime::Permanent,
           Owner::PipelineObject, 0});
    // ---- Samples ----
    r.add({"Sample audio in RAM", P::SampleStorageBytes, Basis::Excluded, Lifetime::Permanent, Owner::Samples, 0});
    // ---- Loading ----
    r.add({"pattern_io::LoadScratch (staged bank + Pattern + RecordedTrack)",
           sizeof(ProfilePatternBank<P>) + patternBytes<P>() + trackBytes<P>(), scaled,
           Lifetime::WhileLoading, Owner::LoadScratch, 0});
    return r;
}

// The FM-1 voice configuration built TODAY: real types, with the classes
// that no profile controls yet (Sequencer, Reverb, TapeEffect) at their host
// sizes and the tape heap at the core's fixed rate. The distance from this to
// the budget is the remaining RAM gap.
template <typename P>
constexpr std::size_t buildableTodayBytes() {
    return sizeof(ProfilePipeline<P>) + TapeEffect::machineBufferBytes();
}

// ---- Compile-time proofs ------------------------------------------------------

// (1) HostProfile is the engine, value for value.
static_assert(HostProfile::SampleRate == SAMPLE_RATE, "HostProfile: sample rate");
static_assert(std::is_same<ProfilePipeline<HostProfile>, AudioPipeline<>>::value &&
                  std::is_same<AudioPipeline<>, AudioPipeline<16>>::value,
              "HostProfile: AudioPipeline voice/event counts");
static_assert(std::is_same<ProfilePatternBank<HostProfile>, DefaultPatternBank>::value,
              "HostProfile: DefaultPatternBank");
static_assert(HostProfile::MonoVoices == 1, "HostProfile: MonoSynthEngine is one voice");
static_assert(HostProfile::PatternMaxSteps == Pattern::MaxSteps &&
                  HostProfile::PatternMaxNotes == Pattern::MaxNotes,
              "HostProfile: Pattern capacities");
static_assert(HostProfile::RecordedEvents == RecordedTrack::MaxEvents &&
                  HostProfile::RecordedPasses == RecordedTrack::MaxPasses,
              "HostProfile: RecordedTrack capacities");
static_assert(HostProfile::MaxEventsPerBlock == Sequencer::MaxEventsPerBlock &&
                  HostProfile::MaxActiveNotes == Sequencer::MaxActiveNotes &&
                  HostProfile::MaxPendingLive == Recorder::MaxPendingLive,
              "HostProfile: Sequencer limits");
static_assert(resource_detail::eventBufferEntries(HostProfile::MaxEventsPerBlock, HostProfile::MaxActiveNotes,
                                                  HostProfile::MaxPendingLive) ==
                  Sequencer::eventBufferCapacity(),
              "event-buffer formula matches the Sequencer");
static_assert(HostProfile::ArrangementEntries == Arrangement::MaxEntries, "HostProfile: Arrangement");
static_assert(resource_detail::tapeHeap(HostProfile::TapeDelayFrames, HostProfile::TapePrintFrames) ==
                  TapeEffect::machineBufferBytes(),
              "tape heap formula matches TapeEffect");

// (2) The host decomposition adds up to the real objects, byte for byte.
static_assert(resourceReport<HostProfile>().sequencerBytes() == sizeof(Sequencer),
              "host Sequencer decomposition is exact");
static_assert(resourceReport<HostProfile>().pipelineBytes() == sizeof(AudioPipeline<>),
              "host AudioPipeline decomposition is exact");
static_assert(resourceReport<HostProfile>().loadScratchBytes() ==
                  sizeof(pattern_io::LoadScratch<DefaultPatternBank>),
              "host LoadScratch decomposition is exact");

// (3) The FM-1 profile's configured model fits its budget. Raising an FM-1
// capacity past the budget stops every build that includes this header.
static_assert(resourceReport<Fm1Profile>().withinBudget(),
              "Fm1Profile: the configured RAM model exceeds Fm1Profile::RamBudgetBytes");
static_assert(Fm1Profile::RamBudgetBytes <= Fm1Profile::MappedRamBytes,
              "Fm1Profile: the budget must fit the VERIFIED mapped RAM");

} // namespace databreaks
