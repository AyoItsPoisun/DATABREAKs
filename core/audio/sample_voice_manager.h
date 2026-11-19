
#pragma once

#include "audio_engine.h"
#include "sample_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace databreaks {

// Engine 017: fixed-capacity pool of sample voices.
//
// Engine 019 added slot-addressed control (triggerInSlot, releaseVoice,
// stopVoice and per-slot queries) so that a higher layer can own voices and
// fade them out. Engine 020 added queued ("pending") triggers: queueInSlot
// fades out the slot's current sound and starts the queued sound on the
// frame right after the fade ends, so a full pool can be stolen from without
// a click and without exceeding VoiceCount. Every started or queued sound
// gets a unique, never-reused sound id. Engine 021 added an output gain
// (setOutputGain) applied once to the summed voices of each frame; at its
// default of 1.0 the output is bit-identical to before.
//
// The original API (trigger, processBlock, stopAll, activeVoiceCount,
// capacity) behaves exactly as before: a voice that is never released is
// mixed with a gain of exactly 1.0.
//
// Sample lifetime: voices keep a pointer to the caller's sample vector. The
// vector must outlive every voice that references it and must not be resized
// while such a voice is active.
template <std::size_t VoiceCount = 16>
class SampleVoiceManager {
public:
    static_assert(VoiceCount > 0,
                  "VoiceCount must be greater than zero");

    // Returned by slot-selecting functions when no slot applies.
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    struct Voice {
        SamplePlayer player;
        double volume = 1.0;
        unsigned long age = 0;
        bool active = false;

        // Release fade (Engine 019). Linear ramp: releaseGain falls by
        // releaseStep after every rendered frame and never increases.
        double releaseGain = 1.0;
        double releaseStep = 0.0;
        bool releasing = false;

        // Incremented every time this slot starts a new sound, so stale
        // references to an earlier sound in the same slot can be detected.
        std::uint32_t generation = 0;

        // Engine 020: unique id of the sound in this slot (0 = none).
        std::uint64_t soundId = 0;

        // Engine 020: sound waiting for this slot's fade to finish.
        struct Pending {
            const std::vector<StereoFrame>* sample = nullptr;
            std::size_t startFrame = 0;
            std::size_t endFrame = 0;
            double volume = 1.0;
            double rate = 1.0;
            bool reverse = false;
            std::uint64_t soundId = 0;  // 0 = nothing pending
        } pending;
    };

    bool trigger(const std::vector<StereoFrame>* sample,
                 std::size_t startFrame = 0,
                 std::size_t endFrame = 0,
                 double volume = 1.0,
                 double rate = 1.0,
                 bool reverse = false) {

        if (!validRequest(sample, startFrame, endFrame)) {
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

        return startVoice(selected, sample, startFrame, endFrame,
                          volume, rate, reverse);
    }

    // Engine 019: start a sound in a specific slot chosen by the caller.
    // Whatever was playing in that slot is replaced immediately. Returns
    // false (and leaves the slot untouched) for an invalid slot or request.
    bool triggerInSlot(std::size_t slot,
                       const std::vector<StereoFrame>* sample,
                       std::size_t startFrame = 0,
                       std::size_t endFrame = 0,
                       double volume = 1.0,
                       double rate = 1.0,
                       bool reverse = false) {
        if (slot >= VoiceCount ||
            !validRequest(sample, startFrame, endFrame)) {
            return false;
        }

        return startVoice(slot, sample, startFrame, endFrame,
                          volume, rate, reverse);
    }

    // Engine 020: start a sound in `slot` without cutting what is there.
    //
    // * Slot idle: the sound starts immediately (like triggerInSlot).
    // * Slot busy: its current sound starts (or speeds up) a linear fade of
    //   at most fadeFrames frames, and the new sound starts on the frame
    //   after that fade ends (or after the old sound ends by itself, if that
    //   is sooner). A sound already queued on this slot is replaced.
    //
    // Returns the new sound's id, or 0 if the slot or request is invalid.
    std::uint64_t queueInSlot(std::size_t slot,
                              std::size_t fadeFrames,
                              const std::vector<StereoFrame>* sample,
                              std::size_t startFrame = 0,
                              std::size_t endFrame = 0,
                              double volume = 1.0,
                              double rate = 1.0,
                              bool reverse = false) {
        if (slot >= VoiceCount ||
            !validRequest(sample, startFrame, endFrame)) {
            return 0;
        }

        if (!isVoiceActive(slot)) {
            return startVoice(slot, sample, startFrame, endFrame,
                              volume, rate, reverse)
                ? voices[slot].soundId
                : 0;
        }

        releaseVoice(slot, std::max<std::size_t>(fadeFrames, 1));

        typename Voice::Pending& pending = voices[slot].pending;
        pending.sample = sample;
        pending.startFrame = startFrame;
        pending.endFrame = endFrame;
        pending.volume = volume;
        pending.rate = rate;
        pending.reverse = reverse;
        pending.soundId = ++soundCounter;
        return pending.soundId;
    }

    // Engine 020: drop a queued sound without touching the slot's current
    // sound (which keeps fading).
    void cancelPending(std::size_t slot) {
        if (slot < VoiceCount) {
            voices[slot].pending = {};
        }
    }

    bool hasPending(std::size_t slot) const {
        return slot < VoiceCount && voices[slot].pending.soundId != 0;
    }

    std::uint64_t pendingSoundId(std::size_t slot) const {
        return slot < VoiceCount ? voices[slot].pending.soundId : 0;
    }

    // Id of the sound currently in the slot (0 if never used).
    std::uint64_t voiceSoundId(std::size_t slot) const {
        return slot < VoiceCount ? voices[slot].soundId : 0;
    }

    // Engine 019: begin a linear fade to silence lasting fadeFrames frames
    // from the voice's current gain. A fadeFrames of 0 stops immediately.
    //
    // Calling this on a voice that is already releasing never makes it
    // louder and never slows the fade: the steeper of the current and the
    // newly requested slopes is kept.
    bool releaseVoice(std::size_t slot, std::size_t fadeFrames) {
        if (!isVoiceActive(slot)) {
            return false;
        }

        if (fadeFrames == 0) {
            stopVoice(slot);
            return true;
        }

        Voice& voice = voices[slot];
        const double step =
            voice.releaseGain / static_cast<double>(fadeFrames);

        if (!voice.releasing) {
            voice.releasing = true;
            voice.releaseStep = step;
        } else {
            voice.releaseStep = std::max(voice.releaseStep, step);
        }

        return true;
    }

    // Engine 019: stop one voice immediately (no fade). Also discards a
    // sound queued on the slot (Engine 020).
    void stopVoice(std::size_t slot) {
        if (slot >= VoiceCount) {
            return;
        }

        silenceVoice(voices[slot]);
        voices[slot].pending = {};
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

                const double gain = voice.volume * voice.releaseGain;

                // Mix the produced frame even if the player
                // stopped immediately after producing it.
                left += rendered.left * gain;
                right += rendered.right * gain;

                bool finished = !voice.player.isPlaying();

                if (voice.releasing) {
                    voice.releaseGain -= voice.releaseStep;

                    // Tolerance absorbs floating-point residue after the
                    // final step of the ramp.
                    if (voice.releaseGain <= 1e-12) {
                        finished = true;
                    }
                }

                if (finished) {
                    silenceVoice(voice);

                    // A queued sound takes over the slot and renders from
                    // the next frame on.
                    if (voice.pending.soundId != 0) {
                        startPending(i);
                    }
                }
            }

            output[frame].left += left * outputGain;
            output[frame].right += right * outputGain;
        }
    }

    void stopAll() {
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            stopVoice(i);
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

    // ---- Engine 019 per-slot queries --------------------------------------

    bool isVoiceActive(std::size_t slot) const {
        return slot < VoiceCount &&
               voices[slot].active &&
               voices[slot].player.isPlaying();
    }

    bool isVoiceReleasing(std::size_t slot) const {
        return isVoiceActive(slot) && voices[slot].releasing;
    }

    // Current release gain in [0, 1]; 1.0 when not releasing.
    double voiceReleaseGain(std::size_t slot) const {
        return slot < VoiceCount ? voices[slot].releaseGain : 0.0;
    }

    std::uint32_t voiceGeneration(std::size_t slot) const {
        return slot < VoiceCount ? voices[slot].generation : 0;
    }

    // Trigger order stamp; larger means more recently triggered.
    unsigned long voiceAge(std::size_t slot) const {
        return slot < VoiceCount ? voices[slot].age : 0;
    }

    static constexpr std::size_t capacity() {
        return VoiceCount;
    }

    // Engine 021: gain applied to the summed voices before they are added to
    // the output buffer (a bus level). Clamped to [0, 2]; non-finite values
    // are ignored. Takes effect from the next processed frame.
    void setOutputGain(double gain) {
        if (std::isfinite(gain)) {
            outputGain = std::clamp(gain, 0.0, 2.0);
        }
    }

    double getOutputGain() const {
        return outputGain;
    }

private:
    std::array<Voice, VoiceCount> voices{};
    unsigned long voiceCounter = 0;
    std::uint64_t soundCounter = 0;
    double outputGain = 1.0;

    static void silenceVoice(Voice& voice) {
        voice.player.stop();
        voice.active = false;
        voice.releasing = false;
        voice.releaseStep = 0.0;
        voice.releaseGain = 1.0;
    }

    void startPending(std::size_t slot) {
        const typename Voice::Pending pending = voices[slot].pending;
        voices[slot].pending = {};
        startVoice(slot, pending.sample, pending.startFrame,
                   pending.endFrame, pending.volume, pending.rate,
                   pending.reverse, pending.soundId);
    }

    static bool validRequest(const std::vector<StereoFrame>* sample,
                             std::size_t startFrame,
                             std::size_t& endFrame) {
        if (sample == nullptr || sample->empty()) {
            return false;
        }

        if (endFrame == 0) {
            endFrame = sample->size();
        }

        return startFrame < endFrame && endFrame <= sample->size();
    }

    bool startVoice(std::size_t slot,
                    const std::vector<StereoFrame>* sample,
                    std::size_t startFrame,
                    std::size_t endFrame,
                    double volume,
                    double rate,
                    bool reverse,
                    std::uint64_t soundId = 0) {
        Voice& voice = voices[slot];
        voice.pending = {};
        voice.soundId = soundId != 0 ? soundId : ++soundCounter;

        voice.player.stop();
        voice.player.detachSample();
        voice.player.attachSample(*sample);

        voice.player.setRegion(startFrame, endFrame);
        voice.player.setPlaybackRate(rate);
        voice.player.setReverse(reverse);

        voice.volume = std::isfinite(volume)
            ? std::clamp(volume, 0.0, 2.0)
            : 0.0;
        voice.age = ++voiceCounter;
        voice.releaseGain = 1.0;
        voice.releaseStep = 0.0;
        voice.releasing = false;
        ++voice.generation;

        voice.player.trigger();
        voice.active = voice.player.isPlaying();

        return voice.active;
    }

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
