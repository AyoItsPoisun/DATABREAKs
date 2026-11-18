
#pragma once

#include "audio_engine.h"

namespace databreaks {

class StereoMixer {
public:
    void setMasterVolume(double volume) {
        masterVolume = std::clamp(volume, 0.0, 2.0);
    }

    double getMasterVolume() const {
        return masterVolume;
    }

    StereoFrame process(
        const StereoFrame& input
    ) const {
        StereoFrame output;

        output.left = clampAudio(
            input.left * masterVolume
        );

        output.right = clampAudio(
            input.right * masterVolume
        );

        return output;
    }

private:
    double masterVolume = 0.8;
};

}
