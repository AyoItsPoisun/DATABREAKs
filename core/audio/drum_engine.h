
#pragma once

// DATABREAKs Engine 019/020 - 27-pad drum voice engine (host-side C++17).
//
// Engine 019 connected DrumPadBank<27> (Engine 018: pad settings, velocity,
// pitch, reverse, regions, choke-group decisions) to SampleVoiceManager<N>
// (Engine 017: voice pool and mixing) so that pad hits produce audio:
//   * pad-to-voice ownership;
//   * real choke groups (choked pads' voices fade out);
//   * short linear anti-click release fades for chokes and retriggers;
//   * an explicit same-pad retrigger policy;
//   * pad activity that follows the actual voices.
//
// Engine 020 (performance and reliability):
//   * click-free voice stealing: a stolen voice fades out over a short steal
//     fade and the new sound starts on the very next frame in the same slot,
//     so capacity is never exceeded (bounded delay, see selectSlot());
//   * ownership by unique sound id instead of per-slot generation;
//   * a fixed-capacity lock-free SPSC event queue (postTrigger etc.) whose
//     events are applied at their frame offset inside processBlock();
//   * statistics for stolen voices, dropped queued sounds and queue use.
//
// Threading:
//   * processBlock() and everything except the post*() functions and
//     droppedEventCount() must run on ONE thread (the audio thread), or only
//     while that thread is not running;
//   * post*() may be called from ONE other thread (single producer);
//   * there is no other internal synchronisation.
//
// Real-time notes: trigger(), processBlock(), post*() and the release/stop
// functions perform no heap allocation, take no locks and make no system
// calls. All state lives in fixed-size arrays.
//
// This is not FM-1 firmware and has not been run on FM-1 hardware.

#include "audio_engine.h"
#include "drum_event_queue.h"
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

template <std::size_t VoiceCount = 16, std::size_t EventCapacity = 256>
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

    // Engine 020: steal fade, 1 ms at 48 kHz by default, at most 10 ms.
    // This is also the longest a new hit can be delayed by a steal.
    static constexpr std::size_t DefaultStealFadeFrames =
        static_cast<std::size_t>(SAMPLE_RATE) / 1000;
    static constexpr std::size_t MaxStealFadeFrames =
        static_cast<std::size_t>(SAMPLE_RATE) / 100;

    using Bank = DrumPadBank<PadCount>;
    using Voices = SampleVoiceManager<VoiceCount>;
    using EventQueue = DrumEventQueue<EventCapacity>;

    static constexpr std::size_t padCount() { return PadCount; }
    static constexpr std::size_t voiceCapacity() { return VoiceCount; }
    static constexpr std::size_t eventCapacity() {
        return EventQueue::capacity();
    }

    // How trigger() obtained its slot (for tests and statistics).
    enum class Allocation {
        None,             // nothing triggered
        FreeSlot,         // an idle slot; sound starts now
        StoleReleasing,   // queued behind a voice that was already fading
        StolePlaying,     // queued behind a playing voice (steal fade)
        ReplacedPending   // every slot was mid-steal: replaced a queued sound
    };

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

    // Fade length for a stolen voice. Clamped to [1, MaxStealFadeFrames].
    void setStealFadeFrames(std::size_t frames) {
        stealFadeFrames_ =
            std::clamp<std::size_t>(frames, 1, MaxStealFadeFrames);
    }

    std::size_t stealFadeFrames() const { return stealFadeFrames_; }

    void setRetriggerPolicy(RetriggerPolicy policy) {
        retriggerPolicy_ = policy;
    }

    RetriggerPolicy retriggerPolicy() const { return retriggerPolicy_; }

    // ---- Triggering (audio thread) -------------------------------------------

    // Hit a pad now. velocity is MIDI 1..127 (values above 127 are clamped to
    // 127 as in DrumPadBank; 0 or negative is "no hit"). Returns the voice
    // slot used, or npos if nothing was triggered. lastAllocation() tells
    // whether the sound starts now (FreeSlot) or after a steal fade.
    //
    // Order of operations:
    //   1. validate pad, velocity and sample (via DrumPadBank::trigger);
    //   2. fade out voices of pads named in TriggerEvent::chokedPads (and of
    //      any other playing pad in the same nonzero choke group);
    //   3. apply the same-pad retrigger policy;
    //   4. choose a slot (see selectSlot) and start or queue the sound;
    //   5. record ownership and mark the pad active.
    std::size_t trigger(std::size_t pad, int velocity) {
        lastAllocation_ = Allocation::None;

        if (pad >= PadCount || velocity <= 0) {
            return npos;
        }

        typename Bank::TriggerEvent event;
        if (!bank_.trigger(pad, velocity, event)) {
            return npos;
        }

        // 2. Real choke.
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

        // 4. Allocate.
        Allocation kind = Allocation::None;
        const std::size_t slot = selectSlot(kind);
        if (slot >= VoiceCount) {
            // Unreachable: selectSlot() always returns a valid slot. The
            // explicit guard keeps every owners_[slot] access provably in
            // bounds (and silences a GCC -O3 -Warray-bounds false positive).
            syncBankFlag(pad);
            return npos;
        }
        const std::size_t previousOwner = owners_[slot].pad;

        const std::uint64_t id = voices_.queueInSlot(
            slot,
            stealFadeFrames_,
            event.sample,
            event.startFrame,
            event.endFrame,
            event.volume,
            event.playbackRate,
            event.reverse
        );

        // Whatever sound the slot's record referred to is no longer owned:
        // a stolen voice must not stay associated with its previous pad.
        owners_[slot] = {};
        if (previousOwner != npos && previousOwner != pad) {
            syncBankFlag(previousOwner);
        }

        if (id == 0) {
            // Defensive: DrumPadBank already validated the region.
            syncBankFlag(pad);
            return npos;
        }

        if (kind == Allocation::StoleReleasing ||
            kind == Allocation::StolePlaying) {
            ++stolenVoices_;
        } else if (kind == Allocation::ReplacedPending) {
            ++droppedPendingSounds_;
        }

        // 5. Ownership.
        owners_[slot].pad = pad;
        owners_[slot].soundId = id;
        lastAllocation_ = kind;
        syncBankFlag(pad);

        return slot;
    }

    Allocation lastAllocation() const { return lastAllocation_; }

    // ---- Stopping (audio thread) ----------------------------------------------

    // Fade out all voices of a pad (anti-click). Queued sounds of the pad
    // are cancelled before they start.
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

    // Stop all voices of a pad immediately (hard stop, no fade). Queued
    // sounds of the pad are cancelled; another pad's fading tail in the same
    // slot is left to finish.
    void stopPad(std::size_t pad) {
        if (pad >= PadCount) {
            return;
        }
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsPending(slot, pad)) {
                voices_.cancelPending(slot);
            } else if (ownsSounding(slot, pad)) {
                voices_.stopVoice(slot);
            }
            if (owners_[slot].pad == pad) {
                owners_[slot] = {};
            }
        }
        bank_.stopPad(pad);
    }

    // Stop every voice (and every queued sound) immediately and clear all
    // ownership and activity. Events still in the queue are not discarded.
    void stopAll() {
        voices_.stopAll();
        owners_.fill({});
        bank_.stopAll();
    }

    // ---- Event queue (producer thread) -------------------------------------
    //
    // Each post*() call adds one event for the audio thread. frameOffset is
    // the frame inside the NEXT processBlock() call at which it applies; it
    // is clamped to that block (see processBlock). Returns false if the
    // event is invalid (bad pad, velocity <= 0) or the queue is full; a full
    // queue drops the event and increments droppedEventCount().

    bool postTrigger(std::size_t pad, int velocity,
                     std::size_t frameOffset = 0) {
        if (pad >= PadCount || velocity <= 0) {
            return false;
        }
        return queue_.push({DrumEventType::Trigger, pad, velocity,
                            frameOffset});
    }

    bool postReleasePad(std::size_t pad, std::size_t frameOffset = 0) {
        if (pad >= PadCount) {
            return false;
        }
        return queue_.push({DrumEventType::ReleasePad, pad, 0, frameOffset});
    }

    bool postStopPad(std::size_t pad, std::size_t frameOffset = 0) {
        if (pad >= PadCount) {
            return false;
        }
        return queue_.push({DrumEventType::StopPad, pad, 0, frameOffset});
    }

    bool postStopAll(std::size_t frameOffset = 0) {
        return queue_.push({DrumEventType::StopAll, 0, 0, frameOffset});
    }

    // Safe to read from either thread.
    std::uint32_t droppedEventCount() const { return queue_.droppedCount(); }

    // ---- Audio (audio thread) ---------------------------------------------------

    // Mixes frameCount stereo frames of all voices INTO output (adds to the
    // existing contents, like SampleVoiceManager::processBlock). The caller
    // clears the buffer when a fresh mix is wanted. A null output or a
    // frameCount of 0 does nothing (queued events wait for the next block).
    //
    // Queued events are applied in FIFO order at frame
    // min(max(frameOffset, previous event's frame), frameCount - 1).
    // At most eventCapacity() events are applied per call; the rest wait.
    // Pad activity is updated at the end of the block.
    void processBlock(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }

        std::size_t position = 0;
        DrumEvent event;

        for (std::size_t applied = 0;
             applied < EventQueue::capacity() && queue_.pop(event);
             ++applied) {
            const std::size_t at = std::min(
                std::max(event.frameOffset, position), frameCount - 1);

            if (at > position) {
                voices_.processBlock(output + position, at - position);
                position = at;
            }

            applyEvent(event);
            ++processedEvents_;
        }

        voices_.processBlock(output + position, frameCount - position);
        syncOwnership();
    }

    // ---- Queries (audio thread) ---------------------------------------------------

    // Pad that owns a slot's current or queued sound, or npos.
    std::size_t voiceOwner(std::size_t slot) const {
        if (slot >= VoiceCount) {
            return npos;
        }
        const std::size_t pad = owners_[slot].pad;
        return (pad != npos && ownsLiveVoice(slot, pad)) ? pad : npos;
    }

    // Number of live voices owned by a pad: sounding (playing or releasing)
    // plus queued behind a steal fade.
    std::size_t padVoiceCount(std::size_t pad) const {
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsLiveVoice(slot, pad)) {
                ++count;
            }
        }
        return count;
    }

    // Number of a pad's voices that are in their release fade.
    std::size_t padReleasingVoiceCount(std::size_t pad) const {
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsSounding(slot, pad) && voices_.isVoiceReleasing(slot)) {
                ++count;
            }
        }
        return count;
    }

    // Number of a pad's sounds that are queued behind a steal fade.
    std::size_t padPendingVoiceCount(std::size_t pad) const {
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsPending(slot, pad)) {
                ++count;
            }
        }
        return count;
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

    // Active = the pad has a voice that is playing, releasing or queued.
    bool isPadActive(std::size_t pad) const {
        return padVoiceCount(pad) > 0;
    }

    // At least one of the pad's voices is playing (not releasing) or queued
    // to start after a steal fade.
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

    // ---- Statistics (audio thread) ------------------------------------------------

    std::uint64_t stolenVoiceCount() const { return stolenVoices_; }
    std::uint64_t droppedPendingSoundCount() const {
        return droppedPendingSounds_;
    }
    std::uint64_t processedEventCount() const { return processedEvents_; }

private:
    struct Ownership {
        std::size_t pad = npos;
        std::uint64_t soundId = 0;
    };

    Bank bank_;
    Voices voices_;
    EventQueue queue_;
    std::array<Ownership, VoiceCount> owners_{};

    std::size_t releaseFadeFrames_ = DefaultReleaseFadeFrames;
    std::size_t stealFadeFrames_ = DefaultStealFadeFrames;
    RetriggerPolicy retriggerPolicy_ = RetriggerPolicy::FadeAndRetrigger;
    Allocation lastAllocation_ = Allocation::None;

    std::uint64_t stolenVoices_ = 0;
    std::uint64_t droppedPendingSounds_ = 0;
    std::uint64_t processedEvents_ = 0;

    void applyEvent(const DrumEvent& event) {
        switch (event.type) {
        case DrumEventType::Trigger:
            trigger(event.pad, event.velocity);
            break;
        case DrumEventType::ReleasePad:
            releasePad(event.pad);
            break;
        case DrumEventType::StopPad:
            stopPad(event.pad);
            break;
        case DrumEventType::StopAll:
            stopAll();
            break;
        }
    }

    // `pad`'s sound is the one currently audible in `slot`.
    bool ownsSounding(std::size_t slot, std::size_t pad) const {
        return slot < VoiceCount &&
               pad < PadCount &&
               owners_[slot].pad == pad &&
               owners_[slot].soundId == voices_.voiceSoundId(slot) &&
               voices_.isVoiceActive(slot);
    }

    // `pad`'s sound is queued in `slot` behind a steal fade.
    bool ownsPending(std::size_t slot, std::size_t pad) const {
        return slot < VoiceCount &&
               pad < PadCount &&
               owners_[slot].pad == pad &&
               voices_.hasPending(slot) &&
               owners_[slot].soundId == voices_.pendingSoundId(slot);
    }

    bool ownsLiveVoice(std::size_t slot, std::size_t pad) const {
        return ownsSounding(slot, pad) || ownsPending(slot, pad);
    }

    void releasePadVoices(std::size_t pad) {
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (ownsPending(slot, pad)) {
                // Never started: cancel instead of fading. The slot's
                // current (unowned) tail keeps fading.
                voices_.cancelPending(slot);
                owners_[slot] = {};
            } else if (ownsSounding(slot, pad)) {
                voices_.releaseVoice(slot, releaseFadeFrames_);
            }
        }
    }

    // DrumPadBank's `active` flag decides which pads a new hit chokes. Keep
    // it equal to "this pad has a playing (or queued) voice": releasing
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

    // Drop ownership records whose sound has finished, been replaced or
    // been cancelled, then bring the bank's flags in line.
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

    // Deterministic slot choice (capacity is never exceeded):
    //   1. the lowest-numbered idle slot -> sound starts now;
    //   2. otherwise a voice that is already releasing and has nothing
    //      queued: the quietest one (lowest release gain), ties -> oldest.
    //      Its fade is shortened to at most the steal fade; the new sound
    //      starts right after it;
    //   3. otherwise the oldest playing voice: it gets the steal fade and
    //      the new sound starts right after it;
    //   4. otherwise every slot is already mid-steal: the slot whose queued
    //      sound was queued earliest has that sound replaced (it never
    //      sounds; counted in droppedPendingSoundCount()).
    std::size_t selectSlot(Allocation& kind) const {
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (!voices_.isVoiceActive(slot)) {
                kind = Allocation::FreeSlot;
                return slot;
            }
        }

        std::size_t best = npos;
        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (!voices_.isVoiceReleasing(slot) || voices_.hasPending(slot)) {
                continue;
            }
            if (best == npos) {
                best = slot;
                continue;
            }
            const double gain = voices_.voiceReleaseGain(slot);
            const double bestGain = voices_.voiceReleaseGain(best);
            if (gain < bestGain ||
                (gain == bestGain &&
                 voices_.voiceAge(slot) < voices_.voiceAge(best))) {
                best = slot;
            }
        }
        if (best != npos) {
            kind = Allocation::StoleReleasing;
            return best;
        }

        for (std::size_t slot = 0; slot < VoiceCount; ++slot) {
            if (voices_.isVoiceReleasing(slot) || voices_.hasPending(slot)) {
                continue;
            }
            if (best == npos ||
                voices_.voiceAge(slot) < voices_.voiceAge(best)) {
                best = slot;
            }
        }
        if (best != npos) {
            kind = Allocation::StolePlaying;
            return best;
        }

        best = 0;
        for (std::size_t slot = 1; slot < VoiceCount; ++slot) {
            if (voices_.pendingSoundId(slot) <
                voices_.pendingSoundId(best)) {
                best = slot;
            }
        }
        kind = Allocation::ReplacedPending;
        return best;
    }
};

} // namespace databreaks
