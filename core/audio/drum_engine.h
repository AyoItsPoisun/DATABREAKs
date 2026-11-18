
#pragma once

// DATABREAKs Engine 019 - 27-pad drum voice integration (host-side C++17).
//
// Connects DrumPadBank<27> (Engine 018: pad settings, velocity, pitch,
// reverse, regions, choke-group decisions) to SampleVoiceManager<N>
// (Engine 017: voice pool and mixing) so that pad hits produce audio.
//
// Responsibilities added here:
//   * pad-to-voice ownership with per-slot generation checks;
//   * real choke groups: choked pads' voices fade out instead of only having
//     a status flag cleared;
//   * short linear anti-click release fades for chokes and retriggers;
//   * an explicit same-pad retrigger policy;
//   * deterministic voice allocation and stealing;
//   * pad activity that follows the actual voices.
//
// Real-time notes: processBlock() performs no heap allocation, takes no
// locks and makes no system calls. All state lives in fixed-size arrays.
// Configuration and trigger calls must come from the same thread as
// processBlock() (there is no internal synchronisation).
//
// This is not FM-1 firmware and has not been run on FM-1 hardware.

#include "audio_engine.h"
#include "drum_pad_bank.h"
#include "sample_voice_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace databreaks {

// What happens to a pad's existing voices when that same pad is hit again.
enum class RetriggerPolicy {
    // Existing voices of the pad start the release fade; the new hit starts
    // a new voice immediately. (Default.)
    FadeAndRetrigger,
    // Existing voices of the pad keep playing; the new hit layers on top.
    Overlap
};

template <std::size_t VoiceCount = 16>
class DrumEngine {
public:
    static constexpr std::size_t PadCount = 27;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // 5 ms at 48 kHz.
    static constexpr std::size_t DefaultReleaseFadeFrames =
        static_cast<std::size_t>(SAMPLE_RATE) / 200;

    // Upper bound for a configurable fade: one second.
    static constexpr std::size_t MaxReleaseFadeFrames =
        static_cast<std::size_t>(SAMPLE_RATE);

    using Bank = DrumPadBank<PadCount>;
    using Voices = SampleVoiceManager<VoiceCount>;

    static constexpr std::size_t padCount() { return PadCount; }
    static constexpr std::size_t voiceCapacity() { return VoiceCount; }

    // ---- Pad configuration (forwards to DrumPadBank) -----------------------
    //
    // A sample vector must stay alive and unresized for as long as any voice
    // may be playing it. Clearing or replacing a pad's sample does not stop
    // voices that are already playing the old one.

    bool assignSample(std::size_t pad,
                      const std::vector<StereoFrame>* sample,
                      std::size_t startFrame = 0,
                      std::size_t endFrame = 0) {
        // DrumPadBank::assignSample clears the pad's active flag even if a
        // voice is still playing; trigger() therefore also chokes by group
        // membership, not only by that flag.
        return bank_.assignSample(pad, sample, startFrame, endFrame);
    }

    void clearSample(std::size_t pad) {
        // Voices already playing the old sample continue (see above).
        bank_.clearSample(pad);
    }

    bool setRegion(std::size_t pad, std::size_t start, std::size_t end) {
        return bank_.setRegion(pad, start, end);
    }

    bool setVolume(std::size_t pad, double volume) {
        return bank_.setVolume(pad, volume);
    }

    bool setPitch(std::size_t pad, double semitones) {
        return bank_.setPitch(pad, semitones);
    }

    bool setVelocitySensitivity(std::size_t pad, double sensitivity) {
        return bank_.setVelocitySensitivity(pad, sensitivity);
    }

    bool setChokeGroup(std::size_t pad, unsigned int group) {
        return bank_.setChokeGroup(pad, group);
    }

    bool setReverse(std::size_t pad, bool reverse) {
        return bank_.setReverse(pad, reverse);
    }

    const typename Bank::Pad* getPad(std::size_t pad) const {
        return bank_.getPad(pad);
    }

    // Read-only access to the underlying components (for inspection/tests).
    const Bank& bank() const { return bank_; }
    const Voices& voices() const { return voices_; }

    // ---- Engine settings ----------------------------------------------------

    // Fade length used for chokes, retriggers and releasePad/releaseAll.
    // Clamped to [1, MaxReleaseFadeFrames].
    void setReleaseFadeFrames(std::size_t frames) {
        releaseFadeFrames_ =
            std::clamp<std::size_t>(frames, 1, MaxReleaseFadeFrames);
    }

    std::size_t releaseFadeFrames() const { return releaseFadeFrames_; }

    void setRetriggerPolicy(RetriggerPolicy policy) {
        retriggerPolicy_ = policy;
    }

    RetriggerPolicy retriggerPolicy() const { return retriggerPolicy_; }

    // ---- Triggering ----------------------------------------------------------

    // Hit a pad. velocity is MIDI 1..127 (values above 127 are clamped to
    // 127 as in DrumPadBank; 0 or negative is "no hit"). Returns the voice
    // slot that was started, or npos if nothing was triggered.
    //
    // Order of operations:
    //   1. validate pad, velocity and sample (via DrumPadBank::trigger);
    //   2. fade out voices of pads named in TriggerEvent::chokedPads (and of
    //      any other playing pad in the same nonzero choke group);
    //   3. apply the same-pad retrigger policy;
    //   4. allocate a slot (see selectSlot) and start the voice;
    //   5. record ownership and mark the pad active.
    std::size_t trigger(std::size_t pad, int velocity) {
        if (pad >= PadCount || velocity <= 0) {
            return npos;
        }

        typename Bank::TriggerEvent event;
        if (!bank_.trigger(pad, velocity, event)) {
            return npos;
        }

        // 2. Real choke: fade every voice owned by a pad that the bank
        //    reported in chokedPads. Also fade any other pad of the same
        //    nonzero group that still has a playing voice: the bank's flag
        //    can be cleared by assignSample/clearSample while a voice plays.
        const unsigned int group = bank_.getPad(pad)->chokeGroup;
        for (std::size_t other = 0; other < PadCount; ++other) {
            if (other == pad) {
                continue;
            }
            const bool sameGroup =
                group != 0 && bank_.getPad(other)->chokeGroup == group;
            if (event.chokedPads[other] ||
                (sameGroup && isPadPlaying(other))) {
                releasePadVoices(other);
            }
        }

        // 3. Same-pad retrigger.
        if (retriggerPolicy_ == RetriggerPolicy::FadeAndRetrigger) {
            releasePadVoices(pad);
        }

        // 4. Allocate and start.
        const std::size_t slot = selectSlot();
        const std::size_t previousOwner = owners_[slot].pad;

        const bool started = voices_.triggerInSlot(
            slot,
            event.sample,
            event.startFrame,
            event.endFrame,
            event.volume,
            event.playbackRate,
            event.reverse
        );

        // Whatever was in the slot is gone now (stolen or replaced).
        owners_[slot] = {};
        if (previousOwner != npos && previousOwner != pad) {
            syncBankFlag(previousOwner);
        }

        if (!started) {
            // Defensive: DrumPadBank already validated the region, so this
            // should not happen. Keep state consistent if it does.
            syncBankFlag(pad);
            return npos;
        }

        // 5. Ownership.
        owners_[slot].pad = pad;
        owners_[slot].generation = voices_.voiceGeneration(slot);
        syncBankFlag(pad);

        return slot;
    }

    // ---- Stopping -------------------------------------------------------------

    // Fade out all voices of a pad (anti-click).
    void releasePad(std::size_t pad) {
        if (pad >= PadCount) {
            return;
        }
        releasePadVoices(pad);
        syncBankFlag(pad);
    }

    // Fade out every voice.
    void releaseAll() {
        for (std::size_t pad = 0; pad < PadCount; ++pad) {
            releasePad(pad);
        }
    }

    // Stop all voices of a pad immediately (hard stop, no fade).
    void stopPad(std::size_t pad) {
        if (pad >= PadCount) {
            return;
        }
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsLiveVoice(slot, pad)) {
                voices_.stopVoice(slot);
            }
            if (owners_[slot].pad == pad) {
                owners_[slot] = {};
            }
        }
        bank_.stopPad(pad);
    }

    // Stop every voice immediately and clear all ownership and activity.
    void stopAll() {
        voices_.stopAll();
        owners_.fill({});
        bank_.stopAll();
    }

    // ---- Audio ------------------------------------------------------------------

    // Mixes frameCount stereo frames of all voices INTO output (adds to the
    // existing contents, like SampleVoiceManager::processBlock). The caller
    // clears the buffer when a fresh mix is wanted. A null output or a
    // frameCount of 0 does nothing. Pad activity is updated at the end of
    // the block.
    void processBlock(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }

        voices_.processBlock(output, frameCount);
        syncOwnership();
    }

    // ---- Queries ------------------------------------------------------------------

    // Pad that owns a slot's current sound, or npos.
    std::size_t voiceOwner(std::size_t slot) const {
        if (slot >= VoiceCount) {
            return npos;
        }
        const std::size_t pad = owners_[slot].pad;
        return (pad != npos && ownsLiveVoice(slot, pad)) ? pad : npos;
    }

    // Number of live voices (playing or releasing) owned by a pad.
    std::size_t padVoiceCount(std::size_t pad) const {
        return countVoices(pad, false);
    }

    // Number of a pad's voices that are in their release fade.
    std::size_t padReleasingVoiceCount(std::size_t pad) const {
        return countVoices(pad, true);
    }

    // Fills `slots` with the pad's live voice slots (lowest slot first) and
    // returns how many were written (at most maxSlots).
    std::size_t padVoices(std::size_t pad,
                          std::size_t* slots,
                          std::size_t maxSlots) const {
        std::size_t written = 0;
        if (slots == nullptr) {
            return 0;
        }
        for (std::size_t slot = 0;
             slot < VoiceCount && written < maxSlots;
             ++slot) {
            if (ownsLiveVoice(slot, pad)) {
                slots[written++] = slot;
            }
        }
        return written;
    }

    // Active = at least one voice of this pad is playing or releasing.
    bool isPadActive(std::size_t pad) const {
        return padVoiceCount(pad) > 0;
    }

    // At least one of the pad's voices is playing and not releasing.
    bool isPadPlaying(std::size_t pad) const {
        return padVoiceCount(pad) > padReleasingVoiceCount(pad);
    }

    // The pad still sounds, but only through releasing voices.
    bool isPadReleasing(std::size_t pad) const {
        const std::size_t total = padVoiceCount(pad);
        return total > 0 && total == padReleasingVoiceCount(pad);
    }

    std::size_t activeVoiceCount() const {
        return voices_.activeVoiceCount();
    }

private:
    struct Ownership {
        std::size_t pad = npos;
        std::uint32_t generation = 0;
    };

    Bank bank_;
    Voices voices_;
    std::array<Ownership, VoiceCount> owners_{};

    std::size_t releaseFadeFrames_ = DefaultReleaseFadeFrames;
    RetriggerPolicy retriggerPolicy_ = RetriggerPolicy::FadeAndRetrigger;

    // True if `slot` currently plays the sound that `pad` started there.
    bool ownsLiveVoice(std::size_t slot, std::size_t pad) const {
        return slot < VoiceCount &&
               pad < PadCount &&
               owners_[slot].pad == pad &&
               owners_[slot].generation == voices_.voiceGeneration(slot) &&
               voices_.isVoiceActive(slot);
    }

    std::size_t countVoices(std::size_t pad, bool releasingOnly) const {
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsLiveVoice(slot, pad) &&
                (!releasingOnly || voices_.isVoiceReleasing(slot))) {
                ++count;
            }
        }
        return count;
    }

    void releasePadVoices(std::size_t pad) {
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsLiveVoice(slot, pad)) {
                voices_.releaseVoice(slot, releaseFadeFrames_);
            }
        }
    }

    // DrumPadBank's `active` flag decides which pads a new hit chokes. Keep
    // it equal to "this pad has a playing (non-releasing) voice": releasing
    // voices are already fading, and finished pads must not stay active.
    void syncBankFlag(std::size_t pad) {
        if (pad >= PadCount) {
            return;
        }
        if (!isPadPlaying(pad)) {
            bank_.stopPad(pad);
        }
        // The bank only sets the flag itself (inside trigger), so a pad that
        // is playing already has it set.
    }

    // Drop ownership records whose voice has finished or been replaced, then
    // bring the bank's flags in line.
    void syncOwnership() {
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            const std::size_t pad = owners_[slot].pad;
            if (pad != npos && !ownsLiveVoice(slot, pad)) {
                owners_[slot] = {};
            }
        }
        for (std::size_t pad = 0; pad < PadCount; ++pad) {
            syncBankFlag(pad);
        }
    }

    // Deterministic slot choice:
    //   1. the lowest-numbered free slot;
    //   2. otherwise the oldest voice that is already releasing;
    //   3. otherwise the oldest voice overall (hard steal: it stops at once,
    //      because a slot cannot hold the old fade and the new sound).
    std::size_t selectSlot() const {
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (!voices_.isVoiceActive(slot)) {
                return slot;
            }
        }

        std::size_t best = npos;
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (voices_.isVoiceReleasing(slot) &&
                (best == npos ||
                 voices_.voiceAge(slot) < voices_.voiceAge(best))) {
                best = slot;
            }
        }
        if (best != npos) {
            return best;
        }

        best = 0;
        for (std::size_t slot = 1; slot < VoiceCount; ++slot) {
            if (voices_.voiceAge(slot) < voices_.voiceAge(best)) {
                best = slot;
            }
        }
        return best;
    }
};

} // namespace databreaks
