
#pragma once

// DATABREAKs Engine 022 - polyphonic / monophonic synth engine (host-side).
//
// SynthEngine<Voice, VoiceCount, Mono> owns a fixed array of stateful voices
// (synth_voices.h) and provides MIDI-style performance control:
//   noteOn(note, velocity), noteOff(note), allNotesOff(), pitch bend,
//   and render(output, frames) which ADDS level x (sum of voices) into the
//   output, one frame at a time, so any block size gives identical audio.
//
// Mono = true (MonoSynthEngine): one voice, last-note priority. Held notes
// are kept in a fixed-size stack; releasing the sounding note returns to the
// most recent still-held note without retriggering; releasing the last held
// note starts the release.
//
// Mono = false (FmSynthEngine, PolySynthEngine): voice allocation is
//   1. a voice already playing this note (retriggered),
//   2. the lowest idle voice,
//   3. the oldest releasing voice,
//   4. the oldest held voice.
// Every retrigger or steal restarts the attack from the voice's current
// level with continuous phase, so the waveform never jumps.
//
// Velocity: gain = velocity / 127 (linear); 127 = the original synth level.
// noteOn with velocity <= 0 is treated as noteOff (MIDI convention);
// velocity above 127 is clamped. Notes outside 0..127 are rejected.
//
// Threading: everything here runs on ONE thread (the audio thread). Use
// AudioPipeline's post*() functions to send notes from another thread.
// Real-time: no allocation, locks or I/O.

#include "../../core/audio/audio_engine.h"
#include "synth_voices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace databreaks {

template <typename Voice, std::size_t VoiceCount, bool Mono>
class SynthEngine {
public:
    static_assert(VoiceCount > 0, "VoiceCount must be greater than zero");
    static_assert(!Mono || VoiceCount == 1, "Mono engines have one voice");

    using Params = typename Voice::Params;

    static constexpr std::size_t voiceCapacity() { return VoiceCount; }
    static constexpr std::size_t HeldNoteCapacity = 16;
    static constexpr double MaxPitchBendSemitones = 24.0;

    SynthEngine() {
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            voices_[i].core.slot = i;
            voices_[i].core.slotCount = VoiceCount;
            voices_[i].setParameters(params_);
        }
    }

    // ---- Parameters ---------------------------------------------------------

    // Non-finite fields fall back to defaults; values are clamped (see
    // sanitized() in synth_voices.h). Applies to all voices immediately.
    void setParameters(const Params& parameters) {
        params_ = sanitized(parameters);
        for (Voice& voice : voices_) {
            voice.setParameters(params_);
        }
    }

    const Params& parameters() const { return params_; }

    // Source level, applied once to the summed voices. [0, 2]; non-finite
    // values are ignored.
    void setLevel(double level) {
        if (std::isfinite(level)) {
            level_ = std::clamp(level, 0.0, 2.0);
        }
    }

    double level() const { return level_; }

    // ---- Pitch bend -----------------------------------------------------------

    // Bend in semitones, clamped to +/-24; non-finite values are ignored.
    // Applies to all sounding voices from the next sample.
    void setPitchBend(double semitones) {
        if (!std::isfinite(semitones)) {
            return;
        }
        bendSemitones_ = std::clamp(semitones, -MaxPitchBendSemitones,
                                    MaxPitchBendSemitones);
        pitchRatio_ = std::pow(2.0, bendSemitones_ / 12.0);
    }

    // MIDI 14-bit pitch bend: 0..16383, centre 8192, scaled to +/- range.
    void setPitchBendMidi(int value, double rangeSemitones = 2.0) {
        if (!std::isfinite(rangeSemitones)) {
            return;
        }
        value = std::clamp(value, 0, 16383);
        const double normalised = (static_cast<double>(value) - 8192.0) /
                                  (value >= 8192 ? 8191.0 : 8192.0);
        setPitchBend(normalised * std::clamp(rangeSemitones, 0.0, 24.0));
    }

    double pitchBend() const { return bendSemitones_; }

    // ---- Notes ------------------------------------------------------------------

    // Returns true if a voice was started or retriggered.
    bool noteOn(int note, int velocity) {
        if (note < 0 || note > 127) {
            return false;
        }
        if (velocity <= 0) {
            noteOff(note);
            return false;
        }
        const double gain =
            static_cast<double>(std::min(velocity, 127)) / 127.0;

        if constexpr (Mono) {
            removeHeld(note);
            pushHeld(note);
            startVoice(0, note, gain);
            return true;
        } else {
            const std::size_t slot = selectVoice(note);
            startVoice(slot, note, gain);
            return true;
        }
    }

    void noteOff(int note) {
        if (note < 0 || note > 127) {
            return;
        }

        if constexpr (Mono) {
            removeHeld(note);
            Voice& voice = voices_[0];
            if (!gate_[0] || notes_[0] != note) {
                return;  // not the sounding note: just forget it
            }
            if (heldCount_ > 0) {
                // Return to the most recent held note without retrigger.
                notes_[0] = held_[heldCount_ - 1];
                voice.core.frequency = midiToHz(notes_[0]);
            } else {
                voice.core.envelope.noteOff();
                gate_[0] = false;
            }
        } else {
            for (std::size_t i = 0; i < VoiceCount; ++i) {
                if (gate_[i] && notes_[i] == note) {
                    voices_[i].core.envelope.noteOff();
                    gate_[i] = false;
                }
            }
        }
    }

    // Release every voice (normal release fades).
    void allNotesOff() {
        heldCount_ = 0;
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            voices_[i].core.envelope.noteOff();
            gate_[i] = false;
        }
    }

    // Stop everything immediately and clear all state (not a fade).
    void reset() {
        heldCount_ = 0;
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            voices_[i].reset();
            gate_[i] = false;
            notes_[i] = -1;
        }
    }

    // ---- Audio ------------------------------------------------------------------

    // Adds level x (sum of active voices) into output. Null output, zero
    // frames or no active voice: nothing is touched.
    void render(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0 || activeVoiceCount() == 0) {
            return;
        }
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            double left = 0.0;
            double right = 0.0;
            for (Voice& voice : voices_) {
                if (!voice.core.envelope.active()) {
                    continue;
                }
                const StereoFrame v = voice.tick(pitchRatio_);
                left += v.left;
                right += v.right;
            }
            output[frame].left += left * level_;
            output[frame].right += right * level_;
        }
    }

    // ---- Queries ------------------------------------------------------------------

    std::size_t activeVoiceCount() const {
        std::size_t count = 0;
        for (const Voice& voice : voices_) {
            count += voice.core.envelope.active() ? 1 : 0;
        }
        return count;
    }

    // Note is held (gate on) in some voice.
    bool isNoteHeld(int note) const {
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            if (gate_[i] && notes_[i] == note) {
                return true;
            }
        }
        return false;
    }

    const Voice& voice(std::size_t slot) const {
        return voices_[std::min(slot, VoiceCount - 1)];
    }

    int voiceNote(std::size_t slot) const {
        return slot < VoiceCount ? notes_[slot] : -1;
    }

private:
    std::array<Voice, VoiceCount> voices_{};
    std::array<int, VoiceCount> notes_ = initialNotes();
    std::array<bool, VoiceCount> gate_{};
    std::array<unsigned long, VoiceCount> age_{};
    unsigned long ageCounter_ = 0;

    std::array<int, HeldNoteCapacity> held_{};
    std::size_t heldCount_ = 0;

    Params params_ = sanitized(Params{});
    double level_ = 1.0;
    double bendSemitones_ = 0.0;
    double pitchRatio_ = 1.0;

    static std::array<int, VoiceCount> initialNotes() {
        std::array<int, VoiceCount> notes{};
        notes.fill(-1);
        return notes;
    }

    void startVoice(std::size_t slot, int note, double gain) {
        Voice& voice = voices_[slot];
        voice.core.frequency = midiToHz(note);
        voice.core.velocityGain = gain;
        voice.core.envelope.noteOn();  // from the current level
        notes_[slot] = note;
        gate_[slot] = true;
        age_[slot] = ++ageCounter_;
    }

    std::size_t selectVoice(int note) const {
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            if (voices_[i].core.envelope.active() && notes_[i] == note) {
                return i;
            }
        }
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            if (!voices_[i].core.envelope.active()) {
                return i;
            }
        }
        std::size_t best = VoiceCount;
        for (std::size_t i = 0; i < VoiceCount; ++i) {
            if (!gate_[i] && (best == VoiceCount || age_[i] < age_[best])) {
                best = i;
            }
        }
        if (best != VoiceCount) {
            return best;
        }
        best = 0;
        for (std::size_t i = 1; i < VoiceCount; ++i) {
            if (age_[i] < age_[best]) {
                best = i;
            }
        }
        return best;
    }

    void removeHeld(int note) {
        std::size_t out = 0;
        for (std::size_t i = 0; i < heldCount_; ++i) {
            if (held_[i] != note) {
                held_[out++] = held_[i];
            }
        }
        heldCount_ = out;
    }

    void pushHeld(int note) {
        if (heldCount_ == HeldNoteCapacity) {
            // Full: forget the oldest held note.
            for (std::size_t i = 1; i < HeldNoteCapacity; ++i) {
                held_[i - 1] = held_[i];
            }
            --heldCount_;
        }
        held_[heldCount_++] = note;
    }
};

using MonoSynthEngine = SynthEngine<MonoVoice, 1, true>;

template <std::size_t VoiceCount = 4>
using FmSynthEngine = SynthEngine<FmVoice, VoiceCount, false>;

template <std::size_t VoiceCount = 8>
using PolySynthEngine = SynthEngine<PolyVoice, VoiceCount, false>;

} // namespace databreaks
