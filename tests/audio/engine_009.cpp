
#include "audio_engine.h"
#include "tape_machine.h"
#include "tape_transport.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

using namespace databreaks;

namespace {

void writeU16(std::ofstream& file, std::uint16_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void writeU32(std::ofstream& file, std::uint32_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
    file.put(static_cast<char>((value >> 16) & 0xFF));
    file.put(static_cast<char>((value >> 24) & 0xFF));
}

void writeWav(
    const char* filename,
    const std::vector<StereoFrame>& audio
) {
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint16_t channels = 2;

    const std::uint32_t dataSize =
        static_cast<std::uint32_t>(audio.size() * channels * 2);

    std::ofstream file(filename, std::ios::binary);

    if (!file) {
        std::cerr << "Could not create " << filename << '\n';
        return;
    }

    file.write("RIFF", 4);
    writeU32(file, 36 + dataSize);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    writeU32(file, 16);
    writeU16(file, 1);
    writeU16(file, channels);
    writeU32(file, SAMPLE_RATE);
    writeU32(file, SAMPLE_RATE * channels * (bitsPerSample / 8));
    writeU16(file, channels * (bitsPerSample / 8));
    writeU16(file, bitsPerSample);

    file.write("data", 4);
    writeU32(file, dataSize);

    for (const StereoFrame& frame : audio) {
        const double left = clampAudio(frame.left);
        const double right = clampAudio(frame.right);

        const auto leftSample = static_cast<std::int16_t>(
            left * 32767.0
        );

        const auto rightSample = static_cast<std::int16_t>(
            right * 32767.0
        );

        writeU16(file, static_cast<std::uint16_t>(leftSample));
        writeU16(file, static_cast<std::uint16_t>(rightSample));
    }

    std::cout << "Wrote " << filename
              << " (" << audio.size() << " frames)\n";
}

std::vector<StereoFrame> makeSource() {
    constexpr double durationSeconds = 8.0;
    constexpr std::size_t frameCount =
        static_cast<std::size_t>(SAMPLE_RATE * durationSeconds);

    std::vector<StereoFrame> audio;
    audio.reserve(frameCount);

    for (std::size_t i = 0; i < frameCount; ++i) {
        const double time =
            static_cast<double>(i) / SAMPLE_RATE;

        const double bass =
            std::sin(2.0 * PI * 55.0 * time) * 0.24;

        const double mid =
            std::sin(2.0 * PI * 220.0 * time) * 0.13;

        const double high =
            std::sin(2.0 * PI * 1760.0 * time) * 0.07;

        const double pulsePhase = std::fmod(time, 0.5);
        const double pulseEnvelope = std::exp(-pulsePhase * 28.0);
        const double pulse =
            std::sin(2.0 * PI * 95.0 * time) *
            pulseEnvelope * 0.35;

        const double left =
            bass + mid + high + pulse;

        const double right =
            bass + mid * 0.85 + high * 0.65 + pulse * 0.9;

        audio.push_back({left, right});
    }

    return audio;
}

} // namespace

int main() {
    std::cout << "DATABREAKs Engine 009\n";
    std::cout << "Integrated tape transport and coloration\n\n";

    const std::vector<StereoFrame> source = makeSource();

    writeWav("databreaks_tape_009_source.wav", source);

    // Stage 1: tape transport.
    TapeTransport transport;

    transport.setRecording(source);
    transport.setWow(0.25);
    transport.setFlutter(0.15);

    const std::vector<StereoFrame> transported =
        transport.render();

    // Stage 2: tape coloration.
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

    writeWav(
        "databreaks_tape_009_processed.wav",
        processed
    );

    std::cout << "\nEngine 009 finished.\n";
    std::cout << "Source frames: " << source.size() << '\n';
    std::cout << "Transport frames: " << transported.size() << '\n';
    std::cout << "Processed frames: " << processed.size() << '\n';

    return 0;
}
