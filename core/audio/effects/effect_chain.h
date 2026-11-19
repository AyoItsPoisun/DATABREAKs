
#pragma once

// DATABREAKs Engine 023 - ordered effect chain (host-side C++17).
//
// A fixed-capacity, ordered list of AudioEffect pointers. process() runs
// each effect over the whole block in order. Every effect is causal and
// per-sample, so this equals running the chain sample by sample, for any
// block size.
//
// The chain does not own the effects (AudioPipeline owns them). insert()
// at a position is the integration point for later stages, for example the
// Engine 024 tape processor wrapped as an AudioEffect.
//
// Editing the chain (append/insert/remove) is configuration: do it on the
// audio thread between blocks, or while audio is stopped. No allocation.

#include "audio_effect.h"

#include <array>
#include <cstddef>

namespace databreaks {

template <std::size_t MaxSlots = 8>
class EffectChain {
public:
    static constexpr std::size_t capacity() { return MaxSlots; }

    bool append(AudioEffect* effect) { return insert(count_, effect); }

    // Insert before `position` (0 = first; size() = last).
    bool insert(std::size_t position, AudioEffect* effect) {
        if (effect == nullptr || count_ == MaxSlots || position > count_) {
            return false;
        }
        for (std::size_t i = count_; i > position; --i) {
            slots_[i] = slots_[i - 1];
        }
        slots_[position] = effect;
        ++count_;
        return true;
    }

    bool remove(std::size_t position) {
        if (position >= count_) {
            return false;
        }
        for (std::size_t i = position; i + 1 < count_; ++i) {
            slots_[i] = slots_[i + 1];
        }
        slots_[--count_] = nullptr;
        return true;
    }

    // Index of an effect, or capacity() if it is not in the chain.
    std::size_t indexOf(const AudioEffect* effect) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (slots_[i] == effect) {
                return i;
            }
        }
        return MaxSlots;
    }

    std::size_t size() const { return count_; }
    AudioEffect* slot(std::size_t i) const {
        return i < count_ ? slots_[i] : nullptr;
    }

    void process(StereoFrame* buffer, std::size_t frames) {
        if (buffer == nullptr || frames == 0) {
            return;
        }
        for (std::size_t i = 0; i < count_; ++i) {
            slots_[i]->process(buffer, frames);
        }
    }

    void resetAll() {
        for (std::size_t i = 0; i < count_; ++i) {
            slots_[i]->reset();
        }
    }

private:
    std::array<AudioEffect*, MaxSlots> slots_{};
    std::size_t count_ = 0;
};

} // namespace databreaks
