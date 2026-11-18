
#pragma once

#include "audio_engine.h"
#include "tape_machine.h"
#include "tape_stream_transport.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace databreaks {

class TapeStreamingEngine {
public:
    static constexpr std::size_t BLOCK_SIZE = 64;

    explicit TapeStreamingEngine(int sampleRate = SAMPLE_RATE)
        : transport(sampleRate) {}

    // The recording must remain alive while attached.
    void attachTape(const std::vector<StereoFrame>& audio) {
        transport.attachTape(audio);
    }

    void detachTape() {
        transport.detachTape();
    }

    void setSpeedRatio(double ratio) {
        transport.setSpeedRatio(ratio);
    }

    void setWow(double amount) {
        transport.setWow(amount);
    }

    void setFlutter(double amount) {
        transport.setFlutter(amount);
    }

    TapeMachine& machine() {
        return tape;
    }

    const TapeMachine& machine() const {
        return tape;
    }

    void rewind() {
        transport.rewind();
        tape.reset();
    }

    bool finished() const {
        return transport.finished();
    }

    double position() const {
        return transport.position();
    }

    // Processes any requested frame count in fixed-size internal blocks.
    // Returns the number of source frames consumed.
    // Remaining output frames are set to silence at end of tape.
    // No dynamic allocation occurs in this function.
    std::size_t processBlock(
        StereoFrame* output,
        std::size_t frameCount
    ) {
        if (output == nullptr) {
            return 0;
        }

        std::size_t totalProduced = 0;
        std::size_t offset = 0;

        while (offset < frameCount) {
            const std::size_t count = std::min(
                BLOCK_SIZE,
                frameCount - offset
            );

            const std::size_t produced =
                transport.processBlock(scratch.data(), count);

            if (produced > 0) {
                tapeProcessor(scratch.data(), output + offset, produced);
                totalProduced += produced;
            }

            for (std::size_t i = produced; i < count; ++i) {
                output[offset + i] = {};
            }

            offset += count;
        }

        return totalProduced;
    }

    void reset() {
        rewind();
    }

private:
    TapeStreamTransport transport;
    TapeMachine tape;
    std::array<StereoFrame, BLOCK_SIZE> scratch{};

    void tapeProcessor(
        const StereoFrame* input,
        StereoFrame* output,
        std::size_t frameCount
    ) {
        for (std::size_t i = 0; i < frameCount; ++i) {
            const StereoFrame frame = input[i];
            output[i] = tape.process(frame);
        }
    }
};

} // namespace databreaks
