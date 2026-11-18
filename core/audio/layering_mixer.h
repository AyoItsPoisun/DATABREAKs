
#pragma once

#include "audio_engine.h"

namespace databreaks {

class LayeringMixer {
public:

    // Individual instrument volume controls.
    void setMonoVolume(double volume) {
        monoVolume = clampVolume(volume);
    }

    void setFMVolume(double volume) {
        fmVolume = clampVolume(volume);
    }

    void setPolyVolume(double volume) {
        polyVolume = clampVolume(volume);
    }

    // Overall output volume.
    void setMasterVolume(double volume) {
        masterVolume = clampVolume(volume);
    }

    double getMonoVolume() const {
        return monoVolume;
    }

    double getFMVolume() const {
        return fmVolume;
    }

    double getPolyVolume() const {
        return polyVolume;
    }

    double getMasterVolume() const {
        return masterVolume;
    }

    // Combine all three independent instruments.
    StereoFrame process(
        double monoInput,
        double fmInput,
        const StereoFrame& polyInput
    ) const {

        StereoFrame output;

        // Center the mono and FM signals.
        output.left =
            monoInput * monoVolume +
            fmInput * fmVolume;

        output.right =
            monoInput * monoVolume +
            fmInput * fmVolume;

        // Add the stereo polyphonic instrument.
        output.left +=
            polyInput.left * polyVolume;

        output.right +=
            polyInput.right * polyVolume;

        // Apply master volume.
        output.left *= masterVolume;
        output.right *= masterVolume;

        return output;
    }

private:

    double monoVolume = 1.0;
    double fmVolume = 1.0;
    double polyVolume = 1.0;
    double masterVolume = 0.8;

    static double clampVolume(double volume) {
        return std::clamp(volume, 0.0, 2.0);
    }
};

}
