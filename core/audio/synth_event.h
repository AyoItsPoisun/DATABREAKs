
#pragma once

// DATABREAKs - synth event types shared by AudioPipeline (Engine 022) and
// the sequencer (Engine 025). Moved here unchanged from audio_pipeline.h in
// Engine 025 so that the sequencer headers do not include the pipeline;
// audio_pipeline.h includes this file, so existing code is unaffected.

#include <cstddef>
#include <cstdint>

namespace databreaks {

// Engine 022: which synth a queued note event addresses.
enum class SynthTarget : std::uint8_t { Mono, Fm, Poly };

enum class SynthEventType : std::uint8_t {
    NoteOn,       // note + velocity (velocity 0 = note-off)
    NoteOff,      // note
    PitchBend,    // bendSemitones
    AllNotesOff   // release every voice of the target
};

struct SynthEvent {
    SynthEventType type = SynthEventType::NoteOn;
    SynthTarget target = SynthTarget::Mono;
    int note = 0;
    int velocity = 0;
    double bendSemitones = 0.0;
    // Frame within the next render()/mixSources() call.
    std::size_t frameOffset = 0;
};

} // namespace databreaks
