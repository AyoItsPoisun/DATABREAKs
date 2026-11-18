
#pragma once

#include <array>

#include "../../core/audio/audio_engine.h"
#include "synth_parameters.h"

namespace databreaks {

class PolySynth {
public:
    void setParameters(const PolyParameters& parameters) {
        params = parameters;
    }

    const PolyParameters& getParameters() const {
        return params;
    }

    StereoFrame process(
        const std::array<double, 4>& frequencies,
        double time,
        double gate
    ) {
        StereoFrame output;

        double env = calculateEnvelope(
            time, gate, params.envelope
        );

        for (int voice = 0; voice < 4; ++voice) {
            double detuneAmount =
                (voice % 2 == 0)
                ? -params.detune
                : params.detune;

            double frequency =
                frequencies[voice] * (1.0 + detuneAmount);

            double phase =
                2.0 * PI * frequency * time;

            double sample = std::sin(phase);

            double pan = voice / 3.0;

            pan = (pan - 0.5) *
                 params.stereoWidth + 0.5;

            pan = std::clamp(pan, 0.0, 1.0);

            double voiceSample =
                sample * env *
                params.voiceVolume *
                params.volume;

            output.left += voiceSample * (1.0 - pan);
            output.right += voiceSample * pan;
        }

        return output;
    }

private:
    PolyParameters params;
};

}
