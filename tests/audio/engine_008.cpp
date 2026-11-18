
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

        const double left =
            (std::sin(2.0 * PI * 110.0 * time) * 0.30 +
             std::sin(2.0 * PI * 700.0 * time) * 0.20 +
             std::sin(2.0 * PI * 5000.0 * time) * 0.12)
            * envelope;

        const double right =
            (std::sin(2.0 * PI * 165.0 * time) * 0.30 +
             std::sin(2.0 * PI * 900.0 * time) * 0.20 +
             std::sin(2.0 * PI * 6500.0 * time) * 0.12)
            * envelope;

        source.push_back({left, right});
    }

    return source;
}

void renderVariant(
    const std::vector<StereoFrame>& source,
    const char* filename,
    double crosstalk,
    double printThrough,
    double age
) {
    TapeMachine tape;

    tape.setSpeed(TapeSpeed::IPS_15);
    tape.setTapeType(TapeType::Ferric);
    tape.setWow(0.25);
    tape.setFlutter(0.15);
    tape.setHiss(0.25);
    tape.setSaturation(0.35);
    tape.setHeadBump(0.25);
    tape.setBias(0.5);
    tape.setCompression(0.25);

    tape.setCrosstalk(crosstalk);
    tape.setPrintThrough(printThrough);
    tape.setAge(age);

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

    std::cout << "DATABREAKs Engine 008\n";
    std::cout << "Advanced tape character comparison\n\n";

    renderVariant(
        source,
        "databreaks_tape_clean_reference.wav",
        0.0, 0.0, 0.0
    );

    renderVariant(
        source,
        "databreaks_tape_crosstalk.wav",
        0.85, 0.0, 0.0
    );

    renderVariant(
        source,
        "databreaks_tape_printthrough.wav",
        0.0, 0.9, 0.0
    );

    renderVariant(
        source,
        "databreaks_tape_aged.wav",
        0.25, 0.35, 0.9
    );

    std::cout << "\nAll advanced tape renders completed.\n";

    return 0;
}
