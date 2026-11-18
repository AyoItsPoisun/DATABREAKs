
#pragma once

#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace databreaks {

class SamplePlayer {
public:
    void attachSample(const std::vector<StereoFrame>& source) {
        sample = &source;
        startFrame = 0;
        endFrame = source.size();
        stop();
    }

    void detachSample() {
        sample = nullptr;
        startFrame = 0;
        endFrame = 0;
        stop();
    }

    void setRegion(std::size_t start, std::size_t end) {
        if (sample == nullptr || sample->empty()) {
            startFrame = 0;
            endFrame = 0;
            stop();
            return;
        }

        startFrame = std::min(start, sample->size());
        endFrame = std::clamp(end, startFrame, sample->size());
        stop();
    }

    void setPitchSemitones(double semitones) {
        if (!std::isfinite(semitones)) {
            return;
        }

        semitones = std::clamp(semitones, -48.0, 48.0);
        pitchRatio = std::pow(2.0, semitones / 12.0);
    }

    void setPlaybackRate(double rate) {
        if (!std::isfinite(rate)) {
            return;
        }

        playbackRate = std::clamp(rate, 0.125, 8.0);
    }

    void setReverse(bool enabled) {
        reverse = enabled;
    }

    void trigger() {
        if (sample == nullptr ||
            sample->empty() ||
            startFrame >= endFrame) {
            stop();
            return;
        }

        position = reverse
            ? static_cast<double>(endFrame - 1)
            : static_cast<double>(startFrame);

        playing = true;
    }

    void stop() {
        playing = false;
        position = 0.0;
    }

    bool isPlaying() const {
        return playing;
    }

    double playbackPosition() const {
        return position;
    }

    std::size_t start() const {
        return startFrame;
    }

    std::size_t end() const {
        return endFrame;
    }

    std::size_t processBlock(
        StereoFrame* output,
        std::size_t frameCount
    ) {
        if (output == nullptr) {
            return 0;
        }

        std::size_t produced = 0;

        for (; produced < frameCount; ++produced) {
            if (!playing || sample == nullptr ||
                position < static_cast<double>(startFrame) ||
                position >= static_cast<double>(endFrame)) {
                playing = false;
                output[produced] = {};
                continue;
            }

            const std::size_t indexA =
                static_cast<std::size_t>(position);

            const std::size_t indexB =
                std::min(indexA + 1, endFrame - 1);

            const double fraction =
                position - static_cast<double>(indexA);

            const StereoFrame& a = (*sample)[indexA];
            const StereoFrame& b = (*sample)[indexB];

            output[produced].left =
                a.left + (b.left - a.left) * fraction;

            output[produced].right =
                a.right + (b.right - a.right) * fraction;

            const double rate = pitchRatio * playbackRate;

            position += reverse ? -rate : rate;

            // Stop immediately after the final valid sample.
            if ((!reverse &&
                 position >= static_cast<double>(endFrame)) ||
                (reverse &&
                 position < static_cast<double>(startFrame))) {
                playing = false;
            }
        }

        return produced;
    }

private:
    const std::vector<StereoFrame>* sample = nullptr;

    std::size_t startFrame = 0;
    std::size_t endFrame = 0;

    double position = 0.0;
    double pitchRatio = 1.0;
    double playbackRate = 1.0;

    bool reverse = false;
    bool playing = false;
};

} // namespace databreaks
