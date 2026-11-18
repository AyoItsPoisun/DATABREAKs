
#pragma once

#include "audio_engine.h"
#include "tape_machine.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace databreaks {

class TapeTransport {
public:
    explicit TapeTransport(int sampleRate = SAMPLE_RATE)
        : sampleRate(sampleRate) {}

    void setRecording(const std::vector<StereoFrame>& audio) {
        tape = audio;
        readPosition = 0.0;
        wowPhase = 0.0;
        flutterPhase = 0.0;
    }

    void setSpeed(TapeSpeed value) {
        speed = value;
    }

    void setWow(double value) {
        wow = clamp01(value);
    }

    void setFlutter(double value) {
        flutter = clamp01(value);
    }

    void rewind() {
        readPosition = 0.0;
        wowPhase = 0.0;
        flutterPhase = 0.0;
    }

    std::vector<StereoFrame> render() {
        std::vector<StereoFrame> output;

        if (tape.empty()) {
            return output;
        }

        const double ratio = speedRatio();

        output.reserve(
            static_cast<std::size_t>(
                tape.size() / ratio
            ) + 2
        );

        readPosition = 0.0;
        wowPhase = 0.0;
        flutterPhase = 0.0;

        while (readPosition < static_cast<double>(tape.size())) {
            const std::size_t indexA =
                static_cast<std::size_t>(readPosition);

            const std::size_t indexB =
                std::min(indexA + 1, tape.size() - 1);

            const double fraction =
                readPosition - static_cast<double>(indexA);

            StereoFrame frame;

            frame.left =
                tape[indexA].left * (1.0 - fraction) +
                tape[indexB].left * fraction;

            frame.right =
                tape[indexA].right * (1.0 - fraction) +
                tape[indexB].right * fraction;

            output.push_back(frame);

            const double wowVariation =
                std::sin(wowPhase) * wow * 0.0015;

            const double flutterVariation =
                std::sin(flutterPhase) * flutter * 0.00025;

            const double modulation =
                1.0 + wowVariation + flutterVariation;

            readPosition += ratio * modulation;

            wowPhase += 2.0 * PI * 0.35 / sampleRate;
            flutterPhase += 2.0 * PI * 6.3 / sampleRate;

            if (wowPhase >= 2.0 * PI) {
                wowPhase -= 2.0 * PI;
            }

            if (flutterPhase >= 2.0 * PI) {
                flutterPhase -= 2.0 * PI;
            }
        }

        return output;
    }

private:
    int sampleRate;
    TapeSpeed speed = TapeSpeed::IPS_15;

    std::vector<StereoFrame> tape;

    double readPosition = 0.0;
    double wow = 0.0;
    double flutter = 0.0;

    double wowPhase = 0.0;
    double flutterPhase = 0.0;

    static double clamp01(double value) {
        return std::clamp(value, 0.0, 1.0);
    }

    double speedRatio() const {
        switch (speed) {
            case TapeSpeed::IPS_7_5:
                return 0.5;

            case TapeSpeed::IPS_15:
                return 1.0;

            case TapeSpeed::IPS_30:
                return 2.0;
        }

        return 1.0;
    }
};

}
