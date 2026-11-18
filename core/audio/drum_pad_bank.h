
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "audio_engine.h"

namespace databreaks {

template <std::size_t PadCount = 27>
class DrumPadBank {
public:
    struct Pad {
        const std::vector<StereoFrame>* sample = nullptr;
        std::size_t startFrame = 0;
        std::size_t endFrame = 0;

        double volume = 1.0;
        double pitchSemitones = 0.0;
        double velocitySensitivity = 1.0;

        unsigned int chokeGroup = 0;
        bool reverse = false;
        bool active = false;
    };

    struct TriggerEvent {
        std::size_t padIndex = 0;
        const std::vector<StereoFrame>* sample = nullptr;
        std::size_t startFrame = 0;
        std::size_t endFrame = 0;

        double playbackRate = 1.0;
        double volume = 0.0;
        double normalizedVelocity = 0.0;

        bool reverse = false;
        std::array<bool, PadCount> chokedPads{};
    };

    static constexpr std::size_t padCount() {
        return PadCount;
    }

    bool assignSample(
        std::size_t padIndex,
        const std::vector<StereoFrame>* sample,
        std::size_t startFrame = 0,
        std::size_t endFrame = 0
    ) {
        if (!validPad(padIndex) || sample == nullptr || sample->empty()) {
            return false;
        }

        if (endFrame == 0) {
            endFrame = sample->size();
        }

        if (startFrame >= endFrame || endFrame > sample->size()) {
            return false;
        }

        Pad& pad = pads_[padIndex];
        pad.sample = sample;
        pad.startFrame = startFrame;
        pad.endFrame = endFrame;
        pad.active = false;
        return true;
    }

    void clearSample(std::size_t padIndex) {
        if (!validPad(padIndex)) {
            return;
        }

        pads_[padIndex].sample = nullptr;
        pads_[padIndex].startFrame = 0;
        pads_[padIndex].endFrame = 0;
        pads_[padIndex].active = false;
    }

    bool setRegion(
        std::size_t padIndex,
        std::size_t startFrame,
        std::size_t endFrame
    ) {
        if (!validPad(padIndex)) {
            return false;
        }

        const Pad& pad = pads_[padIndex];

        if (pad.sample == nullptr ||
            startFrame >= endFrame ||
            endFrame > pad.sample->size()) {
            return false;
        }

        pads_[padIndex].startFrame = startFrame;
        pads_[padIndex].endFrame = endFrame;
        return true;
    }

    bool setVolume(std::size_t padIndex, double volume) {
        if (!validPad(padIndex) || !std::isfinite(volume)) {
            return false;
        }

        pads_[padIndex].volume = std::clamp(volume, 0.0, 2.0);
        return true;
    }

    bool setPitch(std::size_t padIndex, double semitones) {
        if (!validPad(padIndex) || !std::isfinite(semitones)) {
            return false;
        }

        pads_[padIndex].pitchSemitones =
            std::clamp(semitones, -48.0, 48.0);
        return true;
    }

    bool setVelocitySensitivity(std::size_t padIndex, double sensitivity) {
        if (!validPad(padIndex) || !std::isfinite(sensitivity)) {
            return false;
        }

        pads_[padIndex].velocitySensitivity =
            std::clamp(sensitivity, 0.0, 1.0);
        return true;
    }

    bool setChokeGroup(std::size_t padIndex, unsigned int group) {
        if (!validPad(padIndex)) {
            return false;
        }

        pads_[padIndex].chokeGroup = group;
        return true;
    }

    bool setReverse(std::size_t padIndex, bool reverse) {
        if (!validPad(padIndex)) {
            return false;
        }

        pads_[padIndex].reverse = reverse;
        return true;
    }

    bool trigger(std::size_t padIndex, int velocity, TriggerEvent& event) {
        if (!validPad(padIndex)) {
            return false;
        }

        const Pad& pad = pads_[padIndex];

        if (pad.sample == nullptr ||
            pad.startFrame >= pad.endFrame ||
            pad.endFrame > pad.sample->size()) {
            return false;
        }

        velocity = std::clamp(velocity, 0, 127);

        // MIDI velocity zero is treated as no trigger.
        if (velocity == 0) {
            return false;
        }

        TriggerEvent nextEvent;
        nextEvent.padIndex = padIndex;
        nextEvent.sample = pad.sample;
        nextEvent.startFrame = pad.startFrame;
        nextEvent.endFrame = pad.endFrame;
        nextEvent.normalizedVelocity =
            static_cast<double>(velocity) / 127.0;
        nextEvent.playbackRate =
            std::clamp(
                std::pow(2.0, pad.pitchSemitones / 12.0),
                0.125,
                8.0
            );
        nextEvent.volume =
            pad.volume *
            (1.0 - pad.velocitySensitivity +
             pad.velocitySensitivity * nextEvent.normalizedVelocity);
        nextEvent.reverse = pad.reverse;

        // A nonzero choke group silences other active pads in that group.
        if (pad.chokeGroup != 0) {
            for (std::size_t i = 0; i < PadCount; ++i) {
                if (i == padIndex) {
                    continue;
                }

                if (pads_[i].active &&
                    pads_[i].chokeGroup == pad.chokeGroup) {
                    nextEvent.chokedPads[i] = true;
                    pads_[i].active = false;
                }
            }
        }

        pads_[padIndex].active = true;
        event = nextEvent;
        return true;
    }

    void stopPad(std::size_t padIndex) {
        if (validPad(padIndex)) {
            pads_[padIndex].active = false;
        }
    }

    void stopAll() {
        for (Pad& pad : pads_) {
            pad.active = false;
        }
    }

    bool isActive(std::size_t padIndex) const {
        return validPad(padIndex) && pads_[padIndex].active;
    }

    const Pad* getPad(std::size_t padIndex) const {
        return validPad(padIndex) ? &pads_[padIndex] : nullptr;
    }

private:
    std::array<Pad, PadCount> pads_{};

    static bool validPad(std::size_t padIndex) {
        return padIndex < PadCount;
    }
};

} // namespace databreaks
