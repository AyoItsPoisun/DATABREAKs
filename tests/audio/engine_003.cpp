
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../core/audio/audio_engine.h"
#include "../../core/audio/mixer.h"

#include "../../instruments/synthesizers/mono_synth.h"
#include "../../instruments/synthesizers/fm_synth.h"
#include "../../instruments/synthesizers/poly_synth.h"

using namespace databreaks;

void write16(std::ofstream& file, uint16_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void write32(std::ofstream& file, uint32_t value) {
    write16(file, static_cast<uint16_t>(value & 0xFFFF));
    write16(file, static_cast<uint16_t>((value >> 16) & 0xFFFF));
}

void writeWav(
    const std::string& filename,
    const std::vector<StereoFrame>& audio
) {
    std::ofstream file(filename, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Cannot create WAV file");
    }

    uint32_t dataSize =
        static_cast<uint32_t>(audio.size() * 4);

    file.write("RIFF", 4);
    write32(file, 36 + dataSize);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    write32(file, 16);
    write16(file, 1);
    write16(file, 2);
    write32(file, SAMPLE_RATE);
    write32(file, SAMPLE_RATE * 4);
    write16(file, 4);
    write16(file, 16);

    file.write("data", 4);
    write32(file, dataSize);

    for (const auto& frame : audio) {
        int16_t left = static_cast<int16_t>(
            clampAudio(frame.left) * 32767.0
        );

        int16_t right = static_cast<int16_t>(
            clampAudio(frame.right) * 32767.0
        );

        write16(file, static_cast<uint16_t>(left));
        write16(file, static_cast<uint16_t>(right));
    }
}

int main() {
    constexpr double duration = 12.0;

    const int totalFrames =
        static_cast<int>(SAMPLE_RATE * duration);

    MonoSynth mono;
    FMSynth fm;
    PolySynth poly;
    StereoMixer mixer;

    // MONO parameters
    MonoParameters monoSettings;

    monoSettings.filterCutoff = 350.0;
    monoSettings.filterEnvelopeAmount = 2200.0;
    monoSettings.volume = 0.65;

    monoSettings.envelope = {
        0.015, 0.15, 0.55, 0.16
    };

    mono.setParameters(monoSettings);

    // FM parameters
    FMParameters fmSettings;

    fmSettings.modulationRatio = 2.0;
    fmSettings.modulationDepth = 4.0;
    fmSettings.volume = 0.42;

    fmSettings.envelope = {
        0.005, 0.12, 0.35, 0.08
    };

    fm.setParameters(fmSettings);

    // POLY parameters
    PolyParameters polySettings;

    polySettings.detune = 0.003;
    polySettings.voiceVolume = 0.12;
    polySettings.stereoWidth = 1.0;
    polySettings.volume = 1.0;

    polySettings.envelope = {
        0.08, 0.22, 0.65, 0.25
    };

    poly.setParameters(polySettings);

    mixer.setMasterVolume(0.8);

    const int bassNotes[] = {36, 36, 43, 31};

    const int fmNotes[] = {
        60, 64, 67, 72, 67, 64, 62, 60
    };

    const std::array<std::array<int, 4>, 4> chords = {{
        {{60, 64, 67, 71}},
        {{57, 60, 64, 67}},
        {{53, 57, 60, 64}},
        {{55, 59, 62, 67}}
    }};

    std::vector<StereoFrame> audio(totalFrames);

    std::cout << "\n";
    std::cout << "DATABREAKs AUDIO ENGINE 003\n";
    std::cout << "Independent Synthesizer Architecture\n\n";

    for (int i = 0; i < totalFrames; ++i) {
        double time =
            static_cast<double>(i) / SAMPLE_RATE;

        StereoFrame signal;

        if (time < 4.0) {
            // MONO ENGINE
            int noteIndex = std::min(
                3, static_cast<int>(time)
            );

            double noteTime = time - noteIndex;

            signal.left = mono.process(
                midiToHz(bassNotes[noteIndex]),
                noteTime,
                0.82
            );

            signal.right = signal.left;

        } else if (time < 8.0) {
            // FM ENGINE
            double localTime = time - 4.0;

            int noteIndex = std::min(
                7, static_cast<int>(localTime / 0.5)
            );

            double noteTime =
                std::fmod(localTime, 0.5);

            double sample = fm.process(
                midiToHz(fmNotes[noteIndex]),
                noteTime,
                0.40
            );

            signal.left = sample;
            signal.right = sample;

        } else {
            // POLY ENGINE
            double localTime = time - 8.0;

            int chordIndex = std::min(
                3, static_cast<int>(localTime)
            );

            double noteTime = localTime - chordIndex;

            std::array<double, 4> frequencies;

            for (int voice = 0; voice < 4; ++voice) {
                frequencies[voice] = midiToHz(
                    chords[chordIndex][voice]
                );
            }

            signal = poly.process(
                frequencies,
                noteTime,
                0.88
            );
        }

        // Short beginning and ending fades.
        double fade = std::min(
            1.0,
            std::min(
                time / 0.02,
                (duration - time) / 0.02
            )
        );

        signal.left *= fade;
        signal.right *= fade;

        audio[i] = mixer.process(signal);
    }

    writeWav("databreaks_engine_003.wav", audio);

    std::cout << "MONO engine: initialized\n";
    std::cout << "FM engine: initialized\n";
    std::cout << "POLY engine: initialized\n";
    std::cout << "Stereo mixer: initialized\n\n";

    std::cout << "Sample rate: " << SAMPLE_RATE << " Hz\n";
    std::cout << "Duration: " << duration << " seconds\n";
    std::cout << "Output: Stereo 16-bit WAV\n\n";

    std::cout << "Audio Engine 003 render complete!\n";

    return 0;
}
