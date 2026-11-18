
#pragma once

#include <algorithm>
#include <cmath>

namespace databreaks {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr double PI = 3.14159265358979323846;

struct StereoFrame {
    double left = 0.0;
    double right = 0.0;
};

inline double midiToHz(int note) {
    return 440.0 * std::pow(
        2.0, (note - 69) / 12.0
    );
}

inline double clampAudio(double value) {
    return std::clamp(value, -1.0, 1.0);
}

}
