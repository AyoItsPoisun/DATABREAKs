
#include "audio_engine.h"
#include "tape_machine.h"
#include "tape_processor.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>

using namespace databreaks;

namespace {

bool nearlyEqual(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance;
}

void configure(TapeMachine& tape) {
    tape.setTapeType(TapeType::Ferric);
    tape.setWow(0.0);
    tape.setFlutter(0.0);
    tape.setHiss(0.25);
    tape.setSaturation(0.35);
    tape.setHeadBump(0.25);
    tape.setBias(0.5);
    tape.setCompression(0.25);
    tape.setCrosstalk(0.20);
    tape.setPrintThrough(0.15);
    tape.setAge(0.10);
}

} // namespace

int main() {
    constexpr std::size_t blockSize = TapeProcessor::BLOCK_SIZE;
    constexpr std::size_t testFrames = blockSize * 2 + 17;

    std::array<StereoFrame, testFrames> input{};
    std::array<StereoFrame, testFrames> blockOutput{};
    std::array<StereoFrame, testFrames> directOutput{};

    // Build a repeatable stereo test signal.
    for (std::size_t i = 0; i < testFrames; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;

        input[i] = {
            0.3 * std::sin(2.0 * PI * 110.0 * t),
            0.25 * std::sin(2.0 * PI * 220.0 * t)
        };
    }

    // Reference: direct frame-by-frame processing.
    TapeMachine reference;
    configure(reference);

    for (std::size_t i = 0; i < testFrames; ++i) {
        directOutput[i] = reference.process(input[i]);
    }

    // Test: process the same signal in fixed-size blocks.
    TapeProcessor processor;
    configure(processor.machine());

    for (std::size_t offset = 0; offset < testFrames; offset += blockSize) {
        const std::size_t remaining = testFrames - offset;
        const std::size_t count =
            remaining < blockSize ? remaining : blockSize;

        processor.processBlock(
            input.data() + offset,
            blockOutput.data() + offset,
            count
        );
    }

    // Compare both processing methods.
    for (std::size_t i = 0; i < testFrames; ++i) {
        if (!nearlyEqual(blockOutput[i].left, directOutput[i].left) ||
            !nearlyEqual(blockOutput[i].right, directOutput[i].right)) {
            std::cerr << "FAIL: Block mismatch at frame " << i << '\n';
            return 1;
        }
    }

    std::cout << "PASS: Full and partial block processing matches reference.\n";

    // Test in-place processing.
    auto inPlace = input;

    TapeProcessor inPlaceProcessor;
    configure(inPlaceProcessor.machine());

    inPlaceProcessor.processBlock(
        inPlace.data(),
        inPlace.data(),
        inPlace.size()
    );

    for (std::size_t i = 0; i < testFrames; ++i) {
        if (!nearlyEqual(inPlace[i].left, directOutput[i].left) ||
            !nearlyEqual(inPlace[i].right, directOutput[i].right)) {
            std::cerr << "FAIL: In-place mismatch at frame " << i << '\n';
            return 1;
        }
    }

    std::cout << "PASS: In-place processing.\n";

    // Test reset and null-pointer handling.
    processor.reset();
    processor.processBlock(nullptr, blockOutput.data(), blockSize);
    processor.processBlock(input.data(), nullptr, blockSize);

    std::cout << "PASS: Reset and null-pointer handling.\n";
    std::cout << "Engine 011 tests complete.\n";

    return 0;
}
