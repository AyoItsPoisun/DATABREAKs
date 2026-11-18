
#pragma once

#include "../../core/audio/audio_engine.h"
#include "synth_parameters.h"

namespace databreaks {

class FMSynth {
public:
    void setParameters(const FMParameters& parameters) {
        params = parameters;
    }

    const FMParameters& getParameters() const {
        return params;
    }

    double process(
        double frequency,
        double time,
        double gate
    ) {
        double env = calculateEnvelope(
            time, gate, params.envelope
        );

        double modulatorFrequency =
            frequency * params.modulationRatio;

        double modulator = std::sin(
            2.0 * PI * modulatorFrequency * time
        );

        double phase =
            2.0 * PI * frequency * time +
            params.modulationDepth * env * modulator;

        return std::sin(phase) * env * params.volume;
    }

private:
    FMParameters params;
};

}
