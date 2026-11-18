
#include "../../core/audio/audio_engine.h"
#include "../../core/audio/tape_machine.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

using namespace databreaks;

void writeWav(
    const char* filename,
    const std::vector<StereoFrame>& audio
) {
    std::ofstream file(filename, std::ios::binary);

    const std::uint32_t dataSize =
        static_cast<std::uint32_t>(audio.size() * 4);

    const std::uint32_t fileSize = 36 + dataSize;
    const std::uint16_t format = 1;
    const std::uint16_t channels = 2;
    const std::uint32_t sampleRate = SAMPLE_RATE;
    const std::uint32_t byteRate = SAMPLE_RATE * 4;
    const std::uint16_t blockAlign = 4;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t formatSize = 16;

    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&formatSize), 4);
    file.write(reinterpret_cast<const char*>(&format), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    for (const auto& frame : audio) {
        const auto left = static_cast<std::int16_t>(
            clampAudio(frame.left) * 32767.0
        );

        const auto right = static_cast<std::int16_t>(
            clampAudio(frame.right) * 32767.0
        );

        file.write(reinterpret_cast<const char*>(&left), 2);
        file.write(reinterpret_cast<const char*>(&right), 2);
    }
}

std::vector<StereoFrame> createSource() {
    constexpr double duration = 5.0;

    const std::size_t count =
        static_cast<std::size_t>(SAMPLE_RATE * duration);

    std::vector<StereoFrame> source;
    source.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const double time =
            static_cast<double>(i) / SAMPLE_RATE;

        const double envelope =
            std::min(1.0, time / 0.2) *
            std::min(1.0, (duration - time) / 0.3);

        const double bass =
            std::sin(2.0 * PI * 55.0 * time) * 0.38;

        const double mid =
            std::sin(2.0 * PI * 330.0 * time) * 0.22;

        const double high =
            std::sin(2.0 * PI * 4000.0 * time) * 0.10;

        const double signal =
            (bass + mid + high) * envelope;

        source.push_back({signal, signal * 0.94});
    }

    return source;
}

void renderTapeType(
    const std::vector<StereoFrame>& source,
    TapeType type,
    const char* filename,
    double bias,
    double compression
) {
    TapeMachine tape;

    tape.setSpeed(TapeSpeed::IPS_15);
    tape.setTapeType(type);
    tape.setBias(bias);
    tape.setCompression(compression);
    tape.setHiss(0.4);
    tape.setSaturation(0.55);
    tape.setHeadBump(0.4);

    std::vector<StereoFrame> output;
    output.reserve(source.size());

    for (const auto& frame : source) {
        output.push_back(tape.process(frame));
    }

    writeWav(filename, output);

    std::cout << "Created " << filename << '\n';
}

int main() {
    const auto source = createSource();

    std::cout << "DATABREAKs Engine 007\n";
    std::cout << "Tape formulation comparison\n\n";

    renderTapeType(
        source,
        TapeType::Ferric,
        "databreaks_tape_ferric.wav",
        0.5,
        0.5
    );

    renderTapeType(
        source,
        TapeType::Chrome,
        "databreaks_tape_chrome.wav",
        0.5,
        0.5
    );

    renderTapeType(
        source,
        TapeType::Metal,
        "databreaks_tape_metal.wav",
        0.5,
        0.5
    );

    std::cout << "\nAll three tape formulations rendered.\n";

    return 0;
}
