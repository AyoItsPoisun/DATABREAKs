
#include "audio_engine.h"
#include "tape_machine.h"
#include "tape_transport.h"
#include "tape_stream_transport.h"
#include "tape_streaming_engine.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

using namespace databreaks;

namespace {

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

bool nearlyEqual(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance;
}

} // namespace

int main() {
    constexpr std::size_t sourceFrames = SAMPLE_RATE;
    constexpr std::size_t outputBlockSize = 113;

    std::vector<StereoFrame> source;
    source.reserve(sourceFrames);

    for (std::size_t i = 0; i < sourceFrames; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;

        source.push_back({
            0.35 * std::sin(2.0 * PI * 110.0 * t),
            0.30 * std::sin(2.0 * PI * 220.0 * t)
        });
    }

    // Reference: offline transport followed by tape coloration.
    TapeTransport referenceTransport;
    referenceTransport.setRecording(source);
    referenceTransport.setWow(0.25);
    referenceTransport.setFlutter(0.15);

    const std::vector<StereoFrame> transported =
        referenceTransport.render();

    TapeMachine referenceTape;
    configure(referenceTape);

    std::vector<StereoFrame> reference;
    reference.reserve(transported.size());

    for (const StereoFrame& frame : transported) {
        reference.push_back(referenceTape.process(frame));
    }

    // Integrated streaming engine.
    TapeStreamingEngine engine;
    engine.attachTape(source);
    engine.setWow(0.25);
    engine.setFlutter(0.15);
    configure(engine.machine());

    std::vector<StereoFrame> actual;
    actual.reserve(reference.size());

    std::vector<StereoFrame> block(outputBlockSize);

    while (!engine.finished()) {
        const std::size_t produced =
            engine.processBlock(block.data(), block.size());

        if (produced == 0) {
            break;
        }

        actual.insert(
            actual.end(),
            block.begin(),
            block.begin() + produced
        );
    }

    if (actual.size() != reference.size()) {
        std::cerr << "FAIL: Integrated output length mismatch.\n";
        std::cerr << "Actual: " << actual.size()
                  << ", expected: " << reference.size() << '\n';
        return 1;
    }

    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (!nearlyEqual(actual[i].left, reference[i].left) ||
            !nearlyEqual(actual[i].right, reference[i].right)) {
            std::cerr << "FAIL: Integrated output mismatch at frame "
                      << i << '\n';
            return 1;
        }
    }

    std::cout << "PASS: Integrated output matches separate components.\n";
    std::cout << "PASS: Streaming blocks and final partial block.\n";

    // End-of-tape silence.
    std::vector<StereoFrame> silence(16, {1.0, 1.0});

    const std::size_t afterEnd =
        engine.processBlock(silence.data(), silence.size());

    if (afterEnd != 0) {
        std::cerr << "FAIL: Output continued after end of tape.\n";
        return 1;
    }

    for (const StereoFrame& frame : silence) {
        if (frame.left != 0.0 || frame.right != 0.0) {
            std::cerr << "FAIL: End-of-tape output was not silent.\n";
            return 1;
        }
    }

    std::cout << "PASS: End-of-tape silence.\n";

    // Rewind.
    engine.rewind();

    if (engine.finished() || engine.position() != 0.0) {
        std::cerr << "FAIL: Rewind did not reset transport position.\n";
        return 1;
    }

    std::cout << "PASS: Rewind.\n";

    // Multi-block processing.
    std::vector<StereoFrame> largeBlock(
        TapeStreamingEngine::BLOCK_SIZE * 3 + 7
    );

    const std::size_t largeProduced =
        engine.processBlock(largeBlock.data(), largeBlock.size());

    if (largeProduced != largeBlock.size()) {
        std::cerr << "FAIL: Multi-block processing did not fill request.\n";
        return 1;
    }

    std::cout << "PASS: Multi-block processing.\n";

    // Detach.
    engine.detachTape();

    if (!engine.finished()) {
        std::cerr << "FAIL: Detached engine is not finished.\n";
        return 1;
    }

    std::cout << "PASS: Detach.\n";
    std::cout << "Engine 013 tests complete.\n";

    return 0;
}
