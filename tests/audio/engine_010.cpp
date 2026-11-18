
#include "audio_engine.h"
#include "tape_machine.h"
#include "tape_transport.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

using namespace databreaks;

int main() {
    constexpr double durationSeconds = 10.0;
    const std::size_t frameCount =
        static_cast<std::size_t>(SAMPLE_RATE * durationSeconds);

    using Clock = std::chrono::steady_clock;

    std::cout << "DATABREAKs Engine 010 - Tape Performance Benchmark\n";
    std::cout << "Sample rate: " << SAMPLE_RATE << " Hz\n";
    std::cout << "Duration: " << durationSeconds << " seconds\n";
    std::cout << "Frames: " << frameCount << "\n\n";

    const auto totalStart = Clock::now();

    // Generate a repeatable stereo test signal.
    const auto sourceStart = Clock::now();

    std::vector<StereoFrame> source;
    source.reserve(frameCount);

    for (std::size_t i = 0; i < frameCount; ++i) {
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

    const auto sourceEnd = Clock::now();

    // Benchmark tape transport at its default 15 IPS speed.
    const auto transportStart = Clock::now();

    TapeTransport transport;
    transport.setRecording(source);
    transport.setWow(0.25);
    transport.setFlutter(0.15);

    std::vector<StereoFrame> transported = transport.render();

    const auto transportEnd = Clock::now();

    // Benchmark tape coloration.
    const auto colorationStart = Clock::now();

    TapeMachine tape;
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

    std::vector<StereoFrame> processed;
    processed.reserve(transported.size());

    for (const StereoFrame& frame : transported) {
        processed.push_back(tape.process(frame));
    }

    const auto colorationEnd = Clock::now();
    const auto totalEnd = Clock::now();

    const auto milliseconds = [](auto start, auto end) {
        return std::chrono::duration<double, std::milli>(
            end - start
        ).count();
    };

    const double sourceMs = milliseconds(sourceStart, sourceEnd);
    const double transportMs =
        milliseconds(transportStart, transportEnd);
    const double colorationMs =
        milliseconds(colorationStart, colorationEnd);
    const double totalMs = milliseconds(totalStart, totalEnd);

    const double transportDuration =
        static_cast<double>(transported.size()) / SAMPLE_RATE;

    const double colorationRealtime =
        colorationMs > 0.0
            ? transportDuration * 1000.0 / colorationMs
            : 0.0;

    const double combinedRealtime =
        totalMs > 0.0
            ? durationSeconds * 1000.0 / totalMs
            : 0.0;

    std::cout << "TIMINGS\n";
    std::cout << "Source generation: " << sourceMs << " ms\n";
    std::cout << "Tape transport:    " << transportMs << " ms\n";
    std::cout << "Tape coloration:   " << colorationMs << " ms\n";
    std::cout << "Total:             " << totalMs << " ms\n\n";

    std::cout << "THROUGHPUT\n";
    std::cout << "Transport output:  "
              << transported.size() << " frames\n";
    std::cout << "Processed output:  "
              << processed.size() << " frames\n";
    std::cout << "Coloration speed:  "
              << colorationRealtime << "x real time\n";
    std::cout << "Combined speed:    "
              << combinedRealtime << "x real time\n\n";

    std::cout << "BASIC MEMORY INFORMATION\n";
    std::cout << "StereoFrame size:  "
              << sizeof(StereoFrame) << " bytes\n";
    std::cout << "Source capacity:   "
              << source.capacity() * sizeof(StereoFrame)
              << " bytes\n";
    std::cout << "Transport capacity: "
              << transported.capacity() * sizeof(StereoFrame)
              << " bytes\n";
    std::cout << "Output capacity:   "
              << processed.capacity() * sizeof(StereoFrame)
              << " bytes\n";
    std::cout << "TapeMachine object: "
              << sizeof(TapeMachine) << " bytes"
              << " (excludes dynamic buffers)\n";

    std::cout << "\nBenchmark complete.\n";

    return 0;
}
