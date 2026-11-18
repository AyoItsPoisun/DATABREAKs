
#pragma once

#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace databreaks {

class TapeStreamTransport {
public:
    explicit TapeStreamTransport(int rate = SAMPLE_RATE)
        : sampleRate(rate > 0 ? rate : SAMPLE_RATE) {}

    // Attach an existing recording. The caller owns its lifetime.
    void attachTape(const std::vector<StereoFrame>& audio) {
        tape = &audio;
        rewind();
    }

    void detachTape() {
        tape = nullptr;
        rewind();
    }

    void setSpeedRatio(double ratio) {
        speedRatio = std::clamp(ratio, 0.125, 8.0);
    }

    void setWow(double amount) {
        wow = clamp01(amount);
    }

    void setFlutter(double amount) {
        flutter = clamp01(amount);
    }

    void rewind() {
        readPosition = 0.0;
        wowPhase = 0.0;
        flutterPhase = 0.0;
    }

    bool finished() const {
        return tape == nullptr ||
               tape->empty() ||
               readPosition >= static_cast<double>(tape->size());
    }

    double position() const {
        return readPosition;
    }

    // Returns the number of frames actually read.
    // Any remaining output frames are filled with silence.
    // No dynamic memory allocation occurs in this function.
    std::size_t processBlock(
        StereoFrame* output,
        std::size_t frameCount
    ) {
        if (output == nullptr) {
            return 0;
        }

        std::size_t produced = 0;

        for (std::size_t i = 0; i < frameCount; ++i) {
            if (finished()) {
                output[i] = {};
                continue;
            }

            const std::size_t indexA =
                static_cast<std::size_t>(readPosition);

            const std::size_t indexB =
                std::min(indexA + 1, tape->size() - 1);

            const double fraction =
                readPosition - static_cast<double>(indexA);

            const StereoFrame& a = (*tape)[indexA];
            const StereoFrame& b = (*tape)[indexB];

            output[i].left =
                a.left + (b.left - a.left) * fraction;

            output[i].right =
                a.right + (b.right - a.right) * fraction;

            const double wowVariation =
                std::sin(wowPhase) * wow * 0.0015;

            const double flutterVariation =
                std::sin(flutterPhase) * flutter * 0.00025;

            const double modulation =
                1.0 + wowVariation + flutterVariation;

            readPosition += speedRatio * modulation;

            wowPhase += 2.0 * PI * 0.35 / sampleRate;
            flutterPhase += 2.0 * PI * 6.3 / sampleRate;

            if (wowPhase >= 2.0 * PI) {
                wowPhase -= 2.0 * PI;
            }

            if (flutterPhase >= 2.0 * PI) {
                flutterPhase -= 2.0 * PI;
            }

            ++produced;
        }

        return produced;
    }

private:
    const std::vector<StereoFrame>* tape = nullptr;

    int sampleRate;

    double readPosition = 0.0;
    double speedRatio = 1.0;

    double wow = 0.0;
    double flutter = 0.0;

    double wowPhase = 0.0;
    double flutterPhase = 0.0;

    static double clamp01(double value) {
        return std::clamp(value, 0.0, 1.0);
    }
};

} // namespace databreaks
