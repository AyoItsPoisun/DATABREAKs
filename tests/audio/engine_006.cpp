
#include "../../core/audio/audio_engine.h"
#include "../../core/audio/tape_machine.h"
#include "../../core/audio/tape_transport.h"

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

std::vector<StereoFrame> createRecording() {
    constexpr double duration = 2.0;

    const std::size_t count =
        static_cast<std::size_t>(SAMPLE_RATE * duration);

    std::vector<StereoFrame> recording;
    recording.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const double time =
            static_cast<double>(i) / SAMPLE_RATE;

        const double fadeIn = std::min(time / 0.1, 1.0);
        const double fadeOut =
            std::min((duration - time) / 0.2, 1.0);

        const double envelope =
            std::max(0.0, std::min(fadeIn, fadeOut));

        const double bass =
            std::sin(2.0 * PI * 55.0 * time) * 0.35;

        const double mid =
            std::sin(2.0 * PI * 220.0 * time) * 0.22;

        const double high =
            std::sin(2.0 * PI * 880.0 * time) * 0.12;

        const double signal =
            (bass + mid + high) * envelope;

        recording.push_back({
            signal,
            signal * 0.92
        });
    }

    return recording;
}

void renderSpeed(
    const std::vector<StereoFrame>& recording,
    TapeSpeed speed,
    const char* filename,
    double wowAmount,
    double flutterAmount
) {
    TapeTransport transport;

    transport.setRecording(recording);
    transport.setSpeed(speed);
    transport.setWow(wowAmount);
    transport.setFlutter(flutterAmount);

    std::vector<StereoFrame> playback =
        transport.render();

    TapeMachine coloration;

    coloration.setSpeed(speed);
    coloration.setWow(0.0);
    coloration.setFlutter(0.0);
    coloration.setHiss(0.25);
    coloration.setSaturation(0.45);
    coloration.setHeadBump(0.4);

    for (auto& frame : playback) {
        frame = coloration.process(frame);
    }

    writeWav(filename, playback);

    std::cout
        << filename
        << " | "
        << playback.size() / static_cast<double>(SAMPLE_RATE)
        << " seconds\n";
}

int main() {
    const auto recording = createRecording();

    std::cout << "DATABREAKs Engine 006\n";
    std::cout << "Virtual tape transport test\n\n";

    renderSpeed(
        recording,
        TapeSpeed::IPS_7_5,
        "databreaks_tape_7_5ips.wav",
        0.65,
        0.45
    );

    renderSpeed(
        recording,
        TapeSpeed::IPS_15,
        "databreaks_tape_15ips.wav",
        0.65,
        0.45
    );

    renderSpeed(
        recording,
        TapeSpeed::IPS_30,
        "databreaks_tape_30ips.wav",
        0.65,
        0.45
    );

    std::cout << "\nAll tape-speed renders completed.\n";

    return 0;
}
