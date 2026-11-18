
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr double PI = 3.14159265358979323846;
constexpr double DURATION = 12.0;

struct StereoSample {
    double left = 0.0;
    double right = 0.0;
};

// Convert MIDI note number to frequency.
double midiToHz(int note) {
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

// Basic ADSR envelope.
double envelope(
    double time,
    double gate,
    double attack,
    double decay,
    double sustain,
    double release
) {
    if (time < 0.0) return 0.0;

    if (time < attack) {
        return time / attack;
    }

    if (time < attack + decay) {
        double progress = (time - attack) / decay;
        return 1.0 - progress * (1.0 - sustain);
    }

    if (time < gate) {
        return sustain;
    }

    double releaseTime = time - gate;

    if (releaseTime >= release) {
        return 0.0;
    }

    return sustain * (1.0 - releaseTime / release);
}

// Simple one-pole low-pass filter.
class LowPass {
public:
    double process(double input, double cutoff) {
        cutoff = std::clamp(
            cutoff, 20.0, SAMPLE_RATE * 0.45
        );

        double coefficient =
            1.0 - std::exp(
                -2.0 * PI * cutoff / SAMPLE_RATE
            );

        state += coefficient * (input - state);
        return state;
    }

private:
    double state = 0.0;
};

// ENGINE 1: MONO ANALOG-INSPIRED SYNTH
class MonoSynth {
public:
    double process(double time) {
        const int notes[] = {36, 36, 43, 31};

        double beat = 1.0;
        int index = std::min(
            3, static_cast<int>(time / beat)
        );

        double noteTime = std::fmod(time, beat);
        double frequency = midiToHz(notes[index]);

        double env = envelope(
            noteTime, 0.82, 0.015, 0.15, 0.55, 0.16
        );

        double phase = 2.0 * PI * frequency * noteTime;

        double oscillator =
            0.65 * std::sin(phase) +
            0.25 * std::sin(phase * 2.0) +
            0.10 * (2.0 * (phase / (2.0 * PI) -
                std::floor(phase / (2.0 * PI) + 0.5)));

        double cutoff = 350.0 + env * 2200.0;

        double filtered = filter.process(
            oscillator * env, cutoff
        );

        return filtered * 0.65;
    }

private:
    LowPass filter;
};

// ENGINE 2: TWO-OPERATOR FM SYNTH
class FMSynth {
public:
    double process(double time) {
        const int notes[] = {
            60, 64, 67, 72, 67, 64, 62, 60
        };

        const double beat = 0.5;

        int index = std::min(
            7, static_cast<int>(time / beat)
        );

        double noteTime = std::fmod(time, beat);
        double carrier = midiToHz(notes[index]);
        double modulator = carrier * 2.0;

        double env = envelope(
            noteTime, 0.40, 0.005, 0.12, 0.35, 0.08
        );

        double modulationIndex = 4.0 * env;

        double mod =
            std::sin(2.0 * PI * modulator * noteTime);

        double phase =
            2.0 * PI * carrier * noteTime +
            modulationIndex * mod;

        return 0.42 * std::sin(phase) * env;
    }
};

// ENGINE 3: POLYPHONIC ANALOG-INSPIRED SYNTH
class PolySynth {
public:
    double process(double time) {
        const int chords[4][4] = {
            {60, 64, 67, 71},
            {57, 60, 64, 67},
            {53, 57, 60, 64},
            {55, 59, 62, 67}
        };

        const double beat = 1.0;

        int chordIndex = std::min(
            3, static_cast<int>(time / beat)
        );

        double noteTime = std::fmod(time, beat);

        double env = envelope(
            noteTime, 0.88, 0.08, 0.22, 0.65, 0.25
        );

        double output = 0.0;

        for (int voice = 0; voice < 4; ++voice) {
            double frequency =
                midiToHz(chords[chordIndex][voice]);

            double detune = (voice % 2 == 0)
                ? 0.997 : 1.003;

            double phase =
                2.0 * PI * frequency * noteTime;

            double oscillatorA =
                std::sin(phase);

            double oscillatorB =
                std::sin(phase * detune);

            double pan = voice / 3.0;

            double voiceSample =
                (oscillatorA + oscillatorB) * 0.5;

            output += voiceSample * env * 0.12;

            // Small stereo contribution per voice.
            stereo.left += voiceSample * env *
                0.12 * (1.0 - pan);

            stereo.right += voiceSample * env *
                0.12 * pan;
        }

        return output;
    }

    StereoSample stereo;

    void reset() {
        stereo = {};
    }
};

// Write little-endian WAV data.
void write16(std::ofstream& file, uint16_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void write32(std::ofstream& file, uint32_t value) {
    write16(file, static_cast<uint16_t>(value & 0xFFFF));
    write16(file, static_cast<uint16_t>(value >> 16));
}

int main() {
    std::cout << "\n";
    std::cout << "==============================\n";
    std::cout << " DATABREAKs AUDIO ENGINE 002\n";
    std::cout << " THREE VOICES / ONE INSTRUMENT\n";
    std::cout << "==============================\n\n";

    const int frames =
        static_cast<int>(SAMPLE_RATE * DURATION);

    std::vector<StereoSample> audio(frames);

    MonoSynth mono;
    FMSynth fm;
    PolySynth poly;

    double peak = 0.0;

    for (int i = 0; i < frames; ++i) {
        double time = static_cast<double>(i) /
                      SAMPLE_RATE;

        StereoSample output;

        if (time < 4.0) {
            output.left = mono.process(time);
            output.right = output.left;

        } else if (time < 8.0) {
            double localTime = time - 4.0;

            output.left = fm.process(localTime);
            output.right = output.left;

        } else {
            double localTime = time - 8.0;

            poly.reset();
            poly.process(localTime);

            output = poly.stereo;
        }

        // Short fade at the beginning and end.
        double fade = std::min(
            1.0,
            std::min(time / 0.02,
                     (DURATION - time) / 0.02)
        );

        output.left *= fade;
        output.right *= fade;

        output.left = std::clamp(
            output.left, -0.95, 0.95
        );

        output.right = std::clamp(
            output.right, -0.95, 0.95
        );

        peak = std::max(
            peak,
            std::max(
                std::abs(output.left),
                std::abs(output.right)
            )
        );

        audio[i] = output;
    }

    std::ofstream file(
        "databreaks_engine_002.wav",
        std::ios::binary
    );

    if (!file) {
        std::cerr << "ERROR: Cannot create WAV file.\n";
        return 1;
    }

    const uint32_t dataSize =
        frames * CHANNELS * 2;

    file.write("RIFF", 4);
    write32(file, 36 + dataSize);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    write32(file, 16);
    write16(file, 1);
    write16(file, CHANNELS);
    write32(file, SAMPLE_RATE);
    write32(file, SAMPLE_RATE * CHANNELS * 2);
    write16(file, CHANNELS * 2);
    write16(file, 16);

    file.write("data", 4);
    write32(file, dataSize);

    for (const auto& sample : audio) {
        int16_t left = static_cast<int16_t>(
            sample.left * 32767.0
        );

        int16_t right = static_cast<int16_t>(
            sample.right * 32767.0
        );

        write16(file, static_cast<uint16_t>(left));
        write16(file, static_cast<uint16_t>(right));
    }

    file.close();

    std::cout << "MONO ENGINE: Complete\n";
    std::cout << "FM ENGINE: Complete\n";
    std::cout << "POLY ENGINE: Complete\n\n";

    std::cout << "Sample rate: " << SAMPLE_RATE << " Hz\n";
    std::cout << "Duration: " << DURATION << " seconds\n";
    std::cout << "Channels: Stereo\n";
    std::cout << "Peak amplitude: " << peak << "\n";

    std::cout << "\nAudio successfully rendered!\n";
    std::cout << "File: databreaks_engine_002.wav\n";

    return 0;
}
