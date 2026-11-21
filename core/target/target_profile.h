#pragma once

// DATABREAKs Engine 031 - target resource profiles.
//
// A profile is the single place where a build target's capacities and
// memory budget are written down. Two profiles exist:
//
//   HostProfile - the existing host engine, value for value. Every value is
//                 checked against the code that really uses it
//                 (core/target/resource_model.h static_asserts), so the
//                 profile cannot silently drift from the engine.
//   Fm1Profile  - the capacities DATABREAKs intends to use on the M-VAVE
//                 FM-1, chosen from docs/FM1_HARDWARE_REPORT.md (Engine 030).
//
// Each value carries an Evidence tag:
//   Verified     - a VERIFIED FM-1 fact in docs/FM1_HARDWARE_REPORT.md
//                  (quoted from M-VAVE's source / JieLi's datasheet; not
//                  measured by us);
//   Configured   - a DATABREAKs design choice for the target (not a hardware
//                  fact; changeable);
//   Conservative - a choice made deliberately low because the hardware value
//                  it depends on is UNKNOWN;
//   HostCurrent  - the existing host engine's value.
//
// IMPORTANT: Fm1Profile does NOT make the engine run on an FM-1. Only some
// classes take their capacities from a profile today (PatternBank,
// DrumEngine, the synth engines, AudioPipeline's voice counts); the others
// (Pattern, RecordedTrack, Sequencer buffers, Reverb, TapeEffect) are still
// fixed host-sized classes. resource_model.h reports which figures are real
// C++ objects and which are models. Nothing here has run on hardware.
//
// Plain constants only: no allocation, no platform headers.

#include <cstddef>
#include <cstdint>

namespace databreaks {

enum class Evidence : std::uint8_t { Verified, Configured, Conservative, HostCurrent };

inline const char* toString(Evidence e) {
    switch (e) {
    case Evidence::Verified: return "VERIFIED FM-1 fact";
    case Evidence::Configured: return "configured choice";
    case Evidence::Conservative: return "conservative (hardware value UNKNOWN)";
    case Evidence::HostCurrent: return "host engine value";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// HOST: today's engine. Changing a value here does not change the engine; the
// static_asserts in resource_model.h fail instead.
// ---------------------------------------------------------------------------
struct HostProfile {
    static constexpr const char* Name = "HOST";

    // Audio
    static constexpr int SampleRate = 48000;           // SAMPLE_RATE
    static constexpr std::size_t BlockFrames = 256;    // AudioStreamConfig default / PipelineRenderer chunk
    static constexpr std::size_t DrumVoices = 16;      // AudioPipeline<16>
    static constexpr std::size_t DrumEventCapacity = 256;
    static constexpr std::size_t SynthEventCapacity = 256;
    static constexpr std::size_t MonoVoices = 1;
    static constexpr std::size_t FmVoices = 4;
    static constexpr std::size_t PolyVoices = 8;
    static constexpr bool ReverbIncluded = true;
    static constexpr bool TapeIncluded = true;
    static constexpr std::size_t TapeDelayFrames = SampleRate / 10;  // TapeMachine: 0.1 s per channel
    static constexpr std::size_t TapePrintFrames = SampleRate / 4;   // TapeMachine: 0.25 s per channel

    // Sequencer working state
    static constexpr std::size_t PatternMaxSteps = 256;  // Pattern::MaxSteps (per lane, dense)
    static constexpr std::size_t PatternMaxNotes = 512;  // Pattern::MaxNotes
    static constexpr std::size_t RecordedEvents = 2048;  // RecordedTrack::MaxEvents
    static constexpr std::size_t RecordedPasses = 16;    // RecordedTrack::MaxPasses
    static constexpr std::size_t MaxEventsPerBlock = 1024;  // Sequencer
    static constexpr std::size_t MaxActiveNotes = 256;      // Sequencer
    static constexpr std::size_t MaxPendingLive = 512;      // Recorder

    // Pattern bank (persisted)
    static constexpr std::size_t PatternBanks = 2;
    static constexpr std::size_t PatternsPerBank = 8;
    static constexpr std::size_t BankStepPool = 8192;
    static constexpr std::size_t BankNotePool = 1024;
    static constexpr std::size_t BankEventPool = 2048;
    static constexpr std::size_t ArrangementEntries = 64;  // Arrangement::MaxEntries

    // Samples and memory: the host has no fixed budget. Samples are
    // caller-owned std::vector<StereoFrame> (16 B per stereo frame).
    static constexpr std::size_t SampleStorageBytes = 0;  // 0 = unbounded (heap)
    static constexpr std::size_t RamBudgetBytes = 0;      // 0 = no budget
};

// ---------------------------------------------------------------------------
// FM-1: target capacities (docs/FM1_HARDWARE_REPORT.md, Engine 030).
// ---------------------------------------------------------------------------
struct Fm1Profile {
    static constexpr const char* Name = "FM1";

    // ---- Audio ----
    // Report A1: FM1_SAMPLE_RATE 44100 (nominal); the hardware runs 44,118 Hz.
    static constexpr int SampleRate = 44100;
    static constexpr Evidence SampleRateEvidence = Evidence::Verified;
    static constexpr int HardwareSampleRate = 44118;
    // Report A2: M-VAVE open SDK, FM1_BLOCK_FRAMES 128, stereo.
    static constexpr std::size_t BlockFrames = 128;
    static constexpr Evidence BlockFramesEvidence = Evidence::Verified;

    // Voices. Report X10/X11: M-VAVE runs 12 integer voices (stock) and 8
    // (open engine) on this chip. The per-voice CPU cost of DATABREAKs on
    // pi32v2 is UNKNOWN (report section 9), so these are deliberately low.
    static constexpr std::size_t DrumVoices = 8;
    static constexpr std::size_t DrumEventCapacity = 256;   // as host (per-block queue)
    static constexpr std::size_t SynthEventCapacity = 256;  // as host
    static constexpr std::size_t MonoVoices = 1;            // MonoSynthEngine is one voice
    static constexpr std::size_t FmVoices = 2;
    static constexpr std::size_t PolyVoices = 4;
    static constexpr Evidence VoiceEvidence = Evidence::Conservative;

    // Effects. In double precision the reverb's delay network is 162,808 B
    // and the tape machine's buffers 246,960 B at 44.1 kHz; together with
    // the rest they do not fit the budget below. Excluded from the FM-1
    // profile until a float path (a later engine) halves them.
    static constexpr bool ReverbIncluded = false;
    static constexpr bool TapeIncluded = false;
    static constexpr std::size_t TapeDelayFrames = 0;
    static constexpr std::size_t TapePrintFrames = 0;
    static constexpr Evidence EffectsEvidence = Evidence::Configured;

    // ---- Sequencer working state ----
    // 64 steps per lane = 4 bars of 16ths (or one bar of 64ths), dense.
    static constexpr std::size_t PatternMaxSteps = 64;
    static constexpr std::size_t PatternMaxNotes = 128;
    // One recording of up to 512 events (the host's per-pass limit).
    static constexpr std::size_t RecordedEvents = 512;
    static constexpr std::size_t RecordedPasses = 16;  // undo depth, as host
    // Per block of 128 frames (host: 1,024 for any block size).
    static constexpr std::size_t MaxEventsPerBlock = 256;
    static constexpr std::size_t MaxActiveNotes = 64;
    static constexpr std::size_t MaxPendingLive = 512;  // as host (Recorder not parameterised)
    static constexpr Evidence SequencerEvidence = Evidence::Configured;

    // ---- Pattern bank (persisted) ----
    // 16 slots, as host, so pattern ids and the DBPB format are unchanged.
    // (M-VAVE's own firmware also has 16 project slots: report X11.) Pools
    // are a quarter of the host's: a host bank larger than this loads on an
    // FM-1 build as LoadStatus::CapacityExceeded, never corrupted.
    static constexpr std::size_t PatternBanks = 2;
    static constexpr std::size_t PatternsPerBank = 8;
    static constexpr std::size_t BankStepPool = 2048;
    static constexpr std::size_t BankNotePool = 256;
    static constexpr std::size_t BankEventPool = 512;
    static constexpr std::size_t ArrangementEntries = 64;  // as host (272 B)
    static constexpr Evidence BankEvidence = Evidence::Configured;

    // ---- Samples ----
    // Report M7: M-VAVE's user-flash layout reserves a 456 KiB shared sample
    // bank in the 1 MiB NOR flash. This is FLASH, not RAM. The RAM the engine
    // may spend on samples is 0: samples must be played from flash, which
    // is NOT implemented (the engine holds samples in RAM as double frames).
    static constexpr std::size_t SampleFlashBytes = 456 * 1024;
    static constexpr Evidence SampleFlashEvidence = Evidence::Verified;
    static constexpr std::size_t SampleStorageBytes = 0;  // RAM for samples
    static constexpr Evidence SampleStorageEvidence = Evidence::Configured;

    // ---- Memory ----
    // Report M1: RAM0 as M-VAVE's linker configures it. Total SRAM in the
    // chip is UNKNOWN (M3); the 28,672 B of cache-way RAM (M2) is not counted.
    static constexpr std::size_t MappedRamBytes = 523596;
    static constexpr Evidence MappedRamEvidence = Evidence::Verified;
    // The engine's budget: 384 KiB of RAM0, leaving 130,380 B for the
    // platform layer (USB, BLE, display strips, RTOS, stacks, heap). What the
    // platform really needs is UNKNOWN (report section 2; M-VAVE's build
    // requires a heap of at least 49,152 B).
    static constexpr std::size_t RamBudgetBytes = 384 * 1024;
    static constexpr Evidence RamBudgetEvidence = Evidence::Conservative;
};

} // namespace databreaks
