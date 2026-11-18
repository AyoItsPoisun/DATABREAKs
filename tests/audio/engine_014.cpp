
#include "audio_engine.h"
#include "tape_machine.h"
#include "tape_streaming_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace databreaks;

namespace {

constexpr double BENCHMARK_SECONDS = 10.0;
constexpr double SOURCE_SECONDS = 12.0;

void configure(TapeMachine& tape) {
    tape.setTapeType(TapeType::Ferric);
    tape.setHiss(0.25);
    tape.setSaturation(0.35);
    tape.setHeadBump(0.25);
    tape.setBias(0.5);
    tape.setCompression(0.25);
    tape.setCrosstalk(0.20);
    tape.setPrintThrough(0.15);
    tape.setAge(0.10);
}

bool runBenchmark(
    const std::vector<StereoFrame>& source,
    std::size_t outputFrames,
    std::size_t blockSize
) {
    using Clock = std::chrono::steady_clock;

    TapeStreamingEngine engine;
    engine.attachTape(source);
    engine.setWow(0.25);
    engine.setFlutter(0.15);
    configure(engine.machine());

    std::vector<StereoFrame> output(blockSize);

    std::size_t totalFrames = 0;

    const auto start = Clock::now();

    while (totalFrames < outputFrames) {
        const std::size_t count = std::min(
            blockSize,
            outputFrames - totalFrames
        );

        const std::size_t produced =
            engine.processBlock(output.data(), count);

        if (produced != count) {
            std::cerr
                << "ERROR: Unexpected end of tape. Block size: "
                << blockSize
                << ", processed: "
                << totalFrames + produced
                << " frames.\n";

            return false;
        }

        totalFrames += produced;
    }

    const auto end = Clock::now();

    const double elapsedSeconds =
        std::chrono::duration<double>(end - start).count();

    const double audioSeconds =
        static_cast<double>(totalFrames) / SAMPLE_RATE;

    const double realtimeFactor =
        elapsedSeconds > 0.0
            ? audioSeconds / elapsedSeconds
            : 0.0;

    std::cout << std::setw(8) << blockSize
              << std::setw(16) << totalFrames
              << std::setw(18) << std::fixed
              << std::setprecision(3)
              << elapsedSeconds * 1000.0
              << std::setw(17) << std::setprecision(2)
              << realtimeFactor << "x\n";

    return true;
}

} // namespace

int main() {
    const std::size_t sourceFrames =
        static_cast<std::size_t>(SAMPLE_RATE * SOURCE_SECONDS);

    const std::size_t benchmarkFrames =
        static_cast<std::size_t>(SAMPLE_RATE * BENCHMARK_SECONDS);

    std::cout << "DATABREAKs Engine 014\n";
    std::cout << "Integrated streaming tape benchmark\n\n";

    std::cout << "Generating " << SOURCE_SECONDS
              << " seconds of stereo test audio...\n";

    std::vector<StereoFrame> source;
    source.reserve(sourceFrames);

    for (std::size_t i = 0; i < sourceFrames; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;

        const double left =
            0.25 * std::sin(2.0 * PI * 55.0 * t) +
            0.15 * std::sin(2.0 * PI * 220.0 * t) +
            0.08 * std::sin(2.0 * PI * 1760.0 * t);

        const double right =
            0.25 * std::sin(2.0 * PI * 55.0 * t) +
            0.15 * std::sin(2.0 * PI * 220.0 * t + 0.2) +
            0.08 * std::sin(2.0 * PI * 1760.0 * t + 0.4);

        source.push_back({left, right});
    }

    std::cout << "\nBenchmark configuration\n";
    std::cout << "Sample rate: " << SAMPLE_RATE << " Hz\n";
    std::cout << "Channels: " << CHANNELS << "\n";
    std::cout << "Source frames: " << source.size() << "\n";
    std::cout << "Measured output: " << benchmarkFrames << " frames\n";
    std::cout << "StereoFrame size: "
              << sizeof(StereoFrame) << " bytes\n";
    std::cout << "Internal 64-frame scratch buffer: "
              << TapeStreamingEngine::BLOCK_SIZE *
                 sizeof(StereoFrame)
              << " bytes\n";

    std::cout << "\nRESULTS\n";
    std::cout << std::setw(8) << "Block"
              << std::setw(16) << "Frames"
              << std::setw(18) << "Time (ms)"
              << std::setw(18) << "Realtime\n";

    std::cout << "----------------------------------------------------------\n";

    bool success = true;

    success &= runBenchmark(source, benchmarkFrames, 32);
    success &= runBenchmark(source, benchmarkFrames, 64);
    success &= runBenchmark(source, benchmarkFrames, 128);
    success &= runBenchmark(source, benchmarkFrames, 256);

    if (!success) {
        std::cerr << "\nBenchmark failed.\n";
        return 1;
    }

    std::cout << "\nBenchmark complete.\n";
    std::cout << "Higher realtime factors indicate faster processing.\n";

    return 0;
}
