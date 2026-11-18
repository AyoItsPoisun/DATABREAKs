
#pragma once

#include "../../core/audio/audio_engine.h"
#include "synth_parameters.h"

namespace databreaks {

class MonoSynth {
public:
    void setParameters(const MonoParameters& parameters) {
        params = parameters;
    }

    const MonoParameters& getParameters() const {
        return params;
    }

    void reset() {
        filterState = 0.0;
    }

    double process(
        double frequency,
        double time,
        double gate
    ) {
        double env = calculateEnvelope(
            time, gate, params.envelope
        );

        double phase = 2.0 * PI * frequency * time;

        double fundamental = std::sin(phase);
        double sub = std::sin(phase * 0.5);

        double input =
            fundamental * params.oscillatorMix +
            sub * params.subMix;

        double cutoff =
            params.filterCutoff +
            env * params.filterEnvelopeAmount;

        cutoff = std::clamp(
            cutoff, 20.0, SAMPLE_RATE * 0.45
        );

        double coefficient =
            1.0 - std::exp(
                -2.0 * PI * cutoff / SAMPLE_RATE
            );

        filterState +=
            coefficient * (input - filterState);

        return filterState * env * params.volume;
    }

private:
    MonoParameters params;
    double filterState = 0.0;
};

}
