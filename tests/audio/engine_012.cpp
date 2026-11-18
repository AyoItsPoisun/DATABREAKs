
#include "audio_engine.h"
#include "tape_stream_transport.h"
#include "tape_transport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

using namespace databreaks;

namespace {

bool nearlyEqual(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance;
}

} // namespace

int main() {
    constexpr std::size_t sourceFrames = SAMPLE_RATE;
    constexpr std::size_t blockSize = 127;

    std::vector<StereoFrame> source;
    source.reserve(sourceFrames);

    for (std::size_t i = 0; i < sourceFrames; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;

        source.push_back({
            0.4 * std::sin(2.0 * PI * 110.0 * t),
            0.3 * std::sin(2.0 * PI * 220.0 * t)
        });
    }

    // Generate the offline reference.
    TapeTransport offline;
    offline.setRecording(source);
    offline.setWow(0.25);
    offline.setFlutter(0.15);

    const std::vector<StereoFrame> reference = offline.render();

    // Read the same recording incrementally.
    TapeStreamTransport streaming;
    streaming.attachTape(source);
    streaming.setWow(0.25);
    streaming.setFlutter(0.15);

    std::vector<StereoFrame> streamed;
    streamed.reserve(reference.size());

    std::array<StereoFrame, blockSize> block{};

    while (!streaming.finished()) {
        const std::size_t produced =
            streaming.processBlock(block.data(), block.size());

        if (produced == 0) {
            break;
        }

        streamed.insert(
            streamed.end(),
            block.begin(),
            block.begin() + produced
        );
    }

    if (streamed.size() != reference.size()) {
        std::cerr << "FAIL: Streaming output length differs from offline output.\n";
        std::cerr << "Streaming: " << streamed.size()
                  << ", offline: " << reference.size() << '\n';
        return 1;
    }

    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (!nearlyEqual(streamed[i].left, reference[i].left) ||
            !nearlyEqual(streamed[i].right, reference[i].right)) {
            std::cerr << "FAIL: Output mismatch at frame " << i << '\n';
            return 1;
        }
    }

    std::cout << "PASS: Streaming output matches offline transport.\n";
    std::cout << "PASS: Partial final block handled correctly.\n";

    // After reaching the end, a further block must contain silence.
    std::array<StereoFrame, 16> silence{};
    silence.fill({1.0, 1.0});

    const std::size_t afterEnd =
        streaming.processBlock(silence.data(), silence.size());

    if (afterEnd != 0) {
        std::cerr << "FAIL: Transport produced frames after end of tape.\n";
        return 1;
    }

    for (const StereoFrame& frame : silence) {
        if (frame.left != 0.0 || frame.right != 0.0) {
            std::cerr << "FAIL: Output was not silent after end of tape.\n";
            return 1;
        }
    }

    std::cout << "PASS: End-of-tape silence.\n";

    // Rewind should restore the first source frame.
    streaming.rewind();

    std::array<StereoFrame, 1> firstFrame{};
    const std::size_t rewound =
        streaming.processBlock(firstFrame.data(), firstFrame.size());

    if (rewound != 1 ||
        !nearlyEqual(firstFrame[0].left, source[0].left) ||
        !nearlyEqual(firstFrame[0].right, source[0].right)) {
        std::cerr << "FAIL: Rewind did not return to the first frame.\n";
        return 1;
    }

    std::cout << "PASS: Rewind.\n";

    // Double speed should advance approximately twice as fast.
    streaming.rewind();
    streaming.setSpeedRatio(2.0);

    std::array<StereoFrame, 100> fastBlock{};
    streaming.processBlock(fastBlock.data(), fastBlock.size());

    if (streaming.position() < 199.0 ||
        streaming.position() > 201.0) {
        std::cerr << "FAIL: Speed ratio was not applied correctly.\n";
        return 1;
    }

    std::cout << "PASS: Adjustable playback speed.\n";

    // Null output must be safely rejected.
    if (streaming.processBlock(nullptr, 10) != 0) {
        std::cerr << "FAIL: Null output handling.\n";
        return 1;
    }

    std::cout << "PASS: Null output handling.\n";

    // Detaching the tape must leave the transport finished.
    streaming.detachTape();

    if (!streaming.finished()) {
        std::cerr << "FAIL: Detached transport is not finished.\n";
        return 1;
    }

    std::cout << "PASS: Tape detach.\n";
    std::cout << "Engine 012 tests complete.\n";

    return 0;
}
