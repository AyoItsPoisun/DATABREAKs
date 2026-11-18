#include "../../core/audio/audio_engine.h"
#include "../../core/audio/tape_machine.h"

#include <cmath>
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

    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file.write("WAVE", 4);

    file.write("fmt ", 4);

    const std::uint32_t formatSize = 16;
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

int main() {
    TapeMachine tape;

    tape.setSpeed(TapeSpeed::IPS_15);
    tape.setWow(0.65);
    tape.setFlutter(0.45);
    tape.setHiss(0.35);
    tape.setSaturation(0.55);
    tape.setHeadBump(0.4);

    constexpr double duration = 12.0;

    const std::size_t totalSamples =
        static_cast<std::size_t>(SAMPLE_RATE * duration);

    std::vector<StereoFrame> audio;
    audio.reserve(totalSamples);

    for (std::size_t i = 0; i < totalSamples; ++i) {
        const double time =
            static_cast<double>(i) / SAMPLE_RATE;

        const double fadeIn =
            std::min(time / 0.5, 1.0);

        const double fadeOut =
            std::min((duration - time) / 0.8, 1.0);

        const double envelope =
            std::max(0.0, std::min(fadeIn, fadeOut));

        const double bass =
            std::sin(2.0 * PI * 55.0 * time) * 0.35;

        const double mid =
            std::sin(2.0 * PI * 220.0 * time) * 0.20;

        const double high =
            std::sin(2.0 * PI * 880.0 * time) * 0.12;

        StereoFrame dry {
            (bass + mid + high) * envelope,
            (bass + mid * 0.95 + high * 0.8) * envelope
        };

        audio.push_back(tape.process(dry));
    }

    writeWav("databreaks_engine_005.wav", audio);

    std::cout
        << "Engine 005 tape-machine render complete.\n"
        << "Output: databreaks_engine_005.wav\n"
        << "Duration: " << duration << " seconds\n";

    return 0;
}