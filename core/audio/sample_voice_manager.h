
#pragma once

#include "audio_engine.h"
#include "sample_player.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace databreaks {

template <std::size_t VoiceCount = 16>
class SampleVoiceManager {
public:
    static_assert(VoiceCount > 0,
                  "VoiceCount must be greater than zero");

    struct Voice {
        SamplePlayer player;
        double volume = 1.0;
        unsigned long age = 0;
        bool active = false;
    };

    bool trigger(const std::vector<StereoFrame>* sample,
                 std::size_t startFrame = 0,
                 std::size_t endFrame = 0,
                 double volume = 1.0,
                 double rate = 1.0,
                 bool reverse = false) {

        if (sample == nullptr || sample->empty()) {
            return false;
        }

        if (endFrame == 0) {
            endFrame = sample->size();
        }

        if (startFrame >= endFrame ||
            endFrame > sample->size()) {
            return false;
        }

        std::size_t selected = VoiceCount;

        // Prefer an unused or finished voice.
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            if (!voices[i].active ||
                !voices[i].player.isPlaying()) {
                selected = i;
                break;
            }
        }

        // Steal the oldest voice when the pool is full.
        if (selected == VoiceCount) {
            selected = oldestVoice();
        }

        Voice& voice = voices[selected];

        voice.player.stop();
        voice.player.detachSample();
        voice.player.attachSample(*sample);

        voice.player.setRegion(startFrame, endFrame);
        voice.player.setPlaybackRate(rate);
        voice.player.setReverse(reverse);

        voice.volume = std::clamp(volume, 0.0, 2.0);
        voice.age = ++voiceCounter;

        voice.player.trigger();
        voice.active = voice.player.isPlaying();

        return voice.active;
    }

    void processBlock(StereoFrame* output,
                      std::size_t frameCount) {

        if (output == nullptr) {
            return;
        }

        for (std::size_t frame = 0;
             frame < frameCount;
             ++frame) {

            double left = 0.0;
            double right = 0.0;

            for (std::size_t i = 0;
                 i < VoiceCount;
                 ++i) {

                Voice& voice = voices[i];

                if (!voice.active) {
                    continue;
                }

                StereoFrame rendered{};

                voice.player.processBlock(&rendered, 1);

                // Mix the produced frame even if the player
                // stopped immediately after producing it.
                left += rendered.left * voice.volume;
                right += rendered.right * voice.volume;

                if (!voice.player.isPlaying()) {
                    voice.active = false;
                }
            }

            output[frame].left += left;
            output[frame].right += right;
        }
    }

    void stopAll() {
        for (Voice& voice : voices) {
            voice.player.stop();
            voice.active = false;
        }
    }

    std::size_t activeVoiceCount() const {
        std::size_t count = 0;

        for (const Voice& voice : voices) {
            if (voice.active && voice.player.isPlaying()) {
                ++count;
            }
        }

        return count;
    }

    static constexpr std::size_t capacity() {
        return VoiceCount;
    }

private:
    std::array<Voice, VoiceCount> voices{};
    unsigned long voiceCounter = 0;

    std::size_t oldestVoice() const {
        std::size_t selected = 0;

        for (std::size_t i = 1; i < VoiceCount; ++i) {
            if (voices[i].age < voices[selected].age) {
                selected = i;
            }
        }

        return selected;
    }
};

} // namespace databreaks
