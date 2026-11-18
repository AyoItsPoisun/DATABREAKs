
#pragma once

#include "audio_engine.h"
#include "tape_machine.h"

#include <cstddef>

namespace databreaks {

class TapeProcessor {
public:
    static constexpr std::size_t BLOCK_SIZE = 64;

    TapeMachine& machine() {
        return tape;
    }

    const TapeMachine& machine() const {
        return tape;
    }

    // Processes an arbitrary number of frames without allocating memory.
    // Input and output may point to the same buffer for in-place processing.
    void processBlock(
        const StereoFrame* input,
        StereoFrame* output,
        std::size_t frameCount
    ) {
        if (input == nullptr || output == nullptr) {
            return;
        }

        for (std::size_t i = 0; i < frameCount; ++i) {
            const StereoFrame frame = input[i];
            output[i] = tape.process(frame);
        }
    }

    void reset() {
        tape.reset();
    }

private:
    TapeMachine tape;
};

} // namespace databreaks
