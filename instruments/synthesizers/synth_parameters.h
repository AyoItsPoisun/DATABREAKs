
#pragma once

#include <algorithm>

namespace databreaks {

struct ADSRParameters {
    double attack = 0.01;
    double decay = 0.15;
    double sustain = 0.65;
    double release = 0.20;
};

struct MonoParameters {
    double oscillatorMix = 0.65;
    double subMix = 0.25;
    double filterCutoff = 350.0;
    double filterEnvelopeAmount = 2200.0;
    double volume = 0.65;

    ADSRParameters envelope;
};

struct FMParameters {
    double modulationRatio = 2.0;
    double modulationDepth = 4.0;
    double volume = 0.42;

    ADSRParameters envelope;
};

struct PolyParameters {
    double detune = 0.003;
    double voiceVolume = 0.12;
    double stereoWidth = 1.0;
    double volume = 1.0;

    ADSRParameters envelope;
};

inline double calculateEnvelope(
    double time,
    double gate,
    const ADSRParameters& adsr
) {
    const double attack = std::max(adsr.attack, 0.001);
    const double decay = std::max(adsr.decay, 0.001);
    const double release = std::max(adsr.release, 0.001);
    const double sustain = std::clamp(
        adsr.sustain, 0.0, 1.0
    );

    if (time < 0.0) return 0.0;

    if (time < attack) {
        return time / attack;
    }

    if (time < attack + decay) {
        double progress = (time - attack) / decay;

        return 1.0 - progress * (1.0 - sustain);
    }

    if (time < gate) {
        return sustain;
    }

    double releaseTime = time - gate;

    if (releaseTime >= release) {
        return 0.0;
    }

    return sustain * (1.0 - releaseTime / release);
}

}
