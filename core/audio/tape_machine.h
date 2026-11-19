
#pragma once

#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace databreaks {

enum class TapeSpeed {
    IPS_7_5,
    IPS_15,
    IPS_30
};

enum class TapeType {
    Ferric,
    Chrome,
    Metal
};

class TapeMachine {
public:
    explicit TapeMachine(int sampleRate = SAMPLE_RATE)
        : sampleRate(sampleRate),
          delayBufferLeft(static_cast<std::size_t>(sampleRate / 10), 0.0),
          delayBufferRight(static_cast<std::size_t>(sampleRate / 10), 0.0),
          printBufferLeft(static_cast<std::size_t>(sampleRate / 4), 0.0),
          printBufferRight(static_cast<std::size_t>(sampleRate / 4), 0.0) {}

    void setSpeed(TapeSpeed value) {
        speed = value;
    }

    void setTapeType(TapeType value) {
        tapeType = value;
    }

    void setWow(double value) {
        wow = clamp01(value);
    }

    void setFlutter(double value) {
        flutter = clamp01(value);
    }

    void setHiss(double value) {
        hiss = clamp01(value);
    }

    void setSaturation(double value) {
        saturation = clamp01(value);
    }

    void setHeadBump(double value) {
        headBump = clamp01(value);
    }

    void setBias(double value) {
        bias = clamp01(value);
    }

    void setCompression(double value) {
        compression = clamp01(value);
    }

    void setCrosstalk(double value) {
        crosstalk = clamp01(value);
    }

    void setPrintThrough(double value) {
        printThrough = clamp01(value);
    }

    void setAge(double value) {
        age = clamp01(value);
    }

    void setEnabled(bool value) {
        enabled = value;
    }

    StereoFrame process(const StereoFrame& input) {
        if (!enabled) {
            return input;
        }

        // Read the quiet magnetic print-through from an earlier pass.
        const std::size_t printDelay = std::min(
            static_cast<std::size_t>(sampleRate * 0.18),
            printBufferLeft.size() - 1
        );

        const std::size_t printReadIndex =
            (printWriteIndex + printBufferLeft.size() - printDelay)
            % printBufferLeft.size();

        const double printLeft =
            printBufferLeft[printReadIndex];

        const double printRight =
            printBufferRight[printReadIndex];

        printBufferLeft[printWriteIndex] = input.left;
        printBufferRight[printWriteIndex] = input.right;

        printWriteIndex =
            (printWriteIndex + 1) % printBufferLeft.size();

        const double wowSignal =
            std::sin(wowPhase) * wow * 0.0015;

        const double flutterSignal =
            std::sin(flutterPhase) * flutter * 0.00025;

        const double delaySeconds =
            0.012 + wowSignal + flutterSignal;

        const double delaySamples = std::clamp(
            delaySeconds * sampleRate,
            1.0,
            static_cast<double>(delayBufferLeft.size() - 2)
        );

        const double delayedLeft =
            readDelay(delayBufferLeft, input.left, delaySamples);

        const double delayedRight =
            readDelay(delayBufferRight, input.right, delaySamples);

        advancePhases();

        // Aging gradually reduces the available high-frequency response.
        const double cutoff =
            speedCutoff() *
            tapeTypeCutoff() *
            biasCutoff() *
            (1.0 - age * 0.45);

        const double filterCoefficient =
            1.0 - std::exp(
                -2.0 * PI * cutoff / sampleRate
            );

        lowpassLeft +=
            filterCoefficient * (delayedLeft - lowpassLeft);

        lowpassRight +=
            filterCoefficient * (delayedRight - lowpassRight);

        const double bumpCoefficient =
            1.0 - std::exp(
                -2.0 * PI * 110.0 / sampleRate
            );

        bumpLeft +=
            bumpCoefficient * (delayedLeft - bumpLeft);

        bumpRight +=
            bumpCoefficient * (delayedRight - bumpRight);

        double left =
            lowpassLeft + bumpLeft * headBump * 0.18;

        double right =
            lowpassRight + bumpRight * headBump * 0.18;

        // Tape compression uses a shared stereo envelope detector.
        const double detector =
            std::max(std::abs(left), std::abs(right));

        const double coefficient =
            detector > envelope
                ? attackCoefficient()
                : releaseCoefficient();

        envelope += coefficient * (detector - envelope);

        double gain = 1.0;

        if (compression > 0.0 && envelope > 0.45) {
            const double ratio = 1.0 + compression * 3.0;

            const double compressedLevel =
                0.45 + (envelope - 0.45) / ratio;

            gain = compressedLevel / envelope;
        }

        left *= gain;
        right *= gain;

        // Formulation, bias and age influence saturation.
        const double drive =
            1.0 + saturation *
            tapeTypeDrive() *
            biasDrive() *
            (1.0 + age * 0.5);

        const double normalization = std::tanh(drive);

        left = std::tanh(left * drive) / normalization;
        right = std::tanh(right * drive) / normalization;

        // Tape hiss increases as the formulation ages.
        const double noiseLevel =
            hiss *
            speedHissLevel() *
            tapeTypeNoise() *
            (1.0 + age * 4.0);

        left += nextNoise() * noiseLevel;
        right += nextNoise() * noiseLevel;

        // Print-through is intentionally quieter than the main signal.
        left += printLeft * printThrough * 0.12;
        right += printRight * printThrough * 0.12;

        // Crosstalk blends a small amount of the opposite channel.
        const double crossAmount = crosstalk * 0.20;

        const double originalLeft = left;
        const double originalRight = right;

        left =
            originalLeft * (1.0 - crossAmount) +
            originalRight * crossAmount;

        right =
            originalRight * (1.0 - crossAmount) +
            originalLeft * crossAmount;

        return {
            clampAudio(left),
            clampAudio(right)
        };
    }

    void reset() {
        std::fill(
            delayBufferLeft.begin(),
            delayBufferLeft.end(),
            0.0
        );

        std::fill(
            delayBufferRight.begin(),
            delayBufferRight.end(),
            0.0
        );

        std::fill(
            printBufferLeft.begin(),
            printBufferLeft.end(),
            0.0
        );

        std::fill(
            printBufferRight.begin(),
            printBufferRight.end(),
            0.0
        );

        lowpassLeft = 0.0;
        lowpassRight = 0.0;
        bumpLeft = 0.0;
        bumpRight = 0.0;
        envelope = 0.0;

        writeIndex = 0;
        printWriteIndex = 0;
        wowPhase = 0.0;
        flutterPhase = 0.0;

        // Engine 024: also restart the hiss generator, so that a reset
        // machine behaves exactly like a newly constructed one.
        noiseState = InitialNoiseState;
    }

private:
    int sampleRate;

    TapeSpeed speed = TapeSpeed::IPS_15;
    TapeType tapeType = TapeType::Ferric;

    double wow = 0.0;
    double flutter = 0.0;
    double hiss = 0.0;
    double saturation = 0.0;
    double headBump = 0.0;
    double bias = 0.5;
    double compression = 0.0;

    double crosstalk = 0.0;
    double printThrough = 0.0;
    double age = 0.0;

    bool enabled = true;

    std::vector<double> delayBufferLeft;
    std::vector<double> delayBufferRight;

    std::vector<double> printBufferLeft;
    std::vector<double> printBufferRight;

    std::size_t writeIndex = 0;
    std::size_t printWriteIndex = 0;

    double wowPhase = 0.0;
    double flutterPhase = 0.0;

    double lowpassLeft = 0.0;
    double lowpassRight = 0.0;

    double bumpLeft = 0.0;
    double bumpRight = 0.0;

    double envelope = 0.0;

    static constexpr std::uint32_t InitialNoiseState = 0x12345678;
    std::uint32_t noiseState = InitialNoiseState;

    static double clamp01(double value) {
        return std::clamp(value, 0.0, 1.0);
    }

    double speedCutoff() const {
        switch (speed) {
            case TapeSpeed::IPS_7_5:
                return 7000.0;
            case TapeSpeed::IPS_15:
                return 12000.0;
            case TapeSpeed::IPS_30:
                return 18000.0;
        }

        return 12000.0;
    }

    double speedHissLevel() const {
        switch (speed) {
            case TapeSpeed::IPS_7_5:
                return 0.0009;
            case TapeSpeed::IPS_15:
                return 0.00065;
            case TapeSpeed::IPS_30:
                return 0.0004;
        }

        return 0.00065;
    }

    double tapeTypeCutoff() const {
        switch (tapeType) {
            case TapeType::Ferric:
                return 0.78;
            case TapeType::Chrome:
                return 1.0;
            case TapeType::Metal:
                return 1.12;
        }

        return 1.0;
    }

    double tapeTypeNoise() const {
        switch (tapeType) {
            case TapeType::Ferric:
                return 1.35;
            case TapeType::Chrome:
                return 0.85;
            case TapeType::Metal:
                return 0.65;
        }

        return 1.0;
    }

    double tapeTypeDrive() const {
        switch (tapeType) {
            case TapeType::Ferric:
                return 6.0;
            case TapeType::Chrome:
                return 4.5;
            case TapeType::Metal:
                return 3.5;
        }

        return 4.5;
    }

    double biasCutoff() const {
        return 0.85 + bias * 0.15;
    }

    double biasDrive() const {
        return 1.15 - bias * 0.30;
    }

    double attackCoefficient() const {
        return 1.0 - std::exp(
            -1.0 / (0.005 * sampleRate)
        );
    }

    double releaseCoefficient() const {
        return 1.0 - std::exp(
            -1.0 / (0.12 * sampleRate)
        );
    }

    double readDelay(
        std::vector<double>& buffer,
        double input,
        double delaySamples
    ) {
        buffer[writeIndex] = input;

        double readPosition =
            static_cast<double>(writeIndex) - delaySamples;

        while (readPosition < 0.0) {
            readPosition += static_cast<double>(buffer.size());
        }

        const auto indexA =
            static_cast<std::size_t>(readPosition);

        const auto indexB =
            (indexA + 1) % buffer.size();

        const double fraction =
            readPosition - static_cast<double>(indexA);

        return buffer[indexA] * (1.0 - fraction) +
               buffer[indexB] * fraction;
    }

    void advancePhases() {
        wowPhase += 2.0 * PI * 0.35 / sampleRate;
        flutterPhase += 2.0 * PI * 6.3 / sampleRate;

        if (wowPhase >= 2.0 * PI) {
            wowPhase -= 2.0 * PI;
        }

        if (flutterPhase >= 2.0 * PI) {
            flutterPhase -= 2.0 * PI;
        }

        writeIndex =
            (writeIndex + 1) % delayBufferLeft.size();
    }

    double nextNoise() {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;

        const double normalized =
            static_cast<double>(noiseState) /
            4294967295.0;

        return normalized * 2.0 - 1.0;
    }
};

}
