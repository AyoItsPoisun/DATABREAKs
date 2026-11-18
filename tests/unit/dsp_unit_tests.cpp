// DATABREAKs host-side DSP unit tests.
//
// Assertion-based checks for the existing core/audio and
// instruments/synthesizers headers. These run on the development
// host only; they say nothing about behaviour on FM-1 hardware.
//
// Known bugs (documented in docs/PROGRESS_REPORT.md) are NOT asserted
// here so that this suite stays green until they are deliberately fixed.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../../core/audio/audio_engine.h"
#include "../../core/audio/mixer.h"
#include "../../instruments/synthesizers/fm_synth.h"
#include "../../instruments/synthesizers/mono_synth.h"
#include "../../instruments/synthesizers/poly_synth.h"
#include "../../instruments/synthesizers/synth_parameters.h"

using namespace databreaks;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

bool near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

void testMidiToHz() {
    CHECK(near(midiToHz(69), 440.0));
    CHECK(near(midiToHz(81), 880.0));
    CHECK(near(midiToHz(57), 220.0));
    CHECK(near(midiToHz(60), 261.6255653005986, 1e-9));
}

void testClampAudio() {
    CHECK(clampAudio(2.0) == 1.0);
    CHECK(clampAudio(-2.0) == -1.0);
    CHECK(clampAudio(0.25) == 0.25);
}

void testEnvelopeStages() {
    const ADSRParameters adsr{0.1, 0.2, 0.5, 0.4};
    const double gate = 1.0;

    CHECK(calculateEnvelope(-0.01, gate, adsr) == 0.0);
    CHECK(near(calculateEnvelope(0.0, gate, adsr), 0.0));
    CHECK(near(calculateEnvelope(0.05, gate, adsr), 0.5));   // mid attack
    CHECK(near(calculateEnvelope(0.2, gate, adsr), 0.75));   // mid decay
    CHECK(near(calculateEnvelope(0.6, gate, adsr), 0.5));    // sustain
    CHECK(near(calculateEnvelope(1.2, gate, adsr), 0.25));   // mid release
    CHECK(calculateEnvelope(1.401, gate, adsr) == 0.0);      // released
    CHECK(calculateEnvelope(5.0, gate, adsr) == 0.0);

    // Envelope stays inside [0, 1] for a sweep of times.
    bool bounded = true;
    for (int i = 0; i < 2000; ++i) {
        double e = calculateEnvelope(i * 0.001, gate, adsr);
        if (e < 0.0 || e > 1.0) bounded = false;
    }
    CHECK(bounded);
}

void testEnvelopeGuardsInvalidParameters() {
    // Zero-length stages and out-of-range sustain must not divide by
    // zero or escape [0, 1].
    const ADSRParameters bad{0.0, 0.0, 3.0, 0.0};
    bool finiteAndBounded = true;
    for (int i = 0; i < 100; ++i) {
        double e = calculateEnvelope(i * 0.0005, 0.02, bad);
        if (!std::isfinite(e) || e < 0.0 || e > 1.0) {
            finiteAndBounded = false;
        }
    }
    CHECK(finiteAndBounded);
}

void testMixer() {
    StereoMixer mixer;
    CHECK(near(mixer.getMasterVolume(), 0.8));

    mixer.setMasterVolume(5.0);
    CHECK(near(mixer.getMasterVolume(), 2.0));
    mixer.setMasterVolume(-1.0);
    CHECK(near(mixer.getMasterVolume(), 0.0));

    mixer.setMasterVolume(0.5);
    StereoFrame out = mixer.process({0.4, -0.6});
    CHECK(near(out.left, 0.2));
    CHECK(near(out.right, -0.3));

    mixer.setMasterVolume(2.0);
    out = mixer.process({0.9, -0.9});
    CHECK(out.left == 1.0);
    CHECK(out.right == -1.0);
}

void testFmSynth() {
    FMSynth fm;
    CHECK(fm.process(440.0, 0.0, 0.4) == 0.0);     // env starts at 0
    CHECK(fm.process(440.0, 10.0, 0.4) == 0.0);    // fully released

    bool bounded = true;
    for (int i = 0; i < SAMPLE_RATE; ++i) {
        double s = fm.process(440.0, static_cast<double>(i) / SAMPLE_RATE, 0.4);
        if (!std::isfinite(s) || std::fabs(s) > fm.getParameters().volume + 1e-12) {
            bounded = false;
        }
    }
    CHECK(bounded);
}

void testMonoSynth() {
    MonoSynth mono;
    bool bounded = true;
    double peak = 0.0;
    for (int i = 0; i < SAMPLE_RATE; ++i) {
        double s = mono.process(110.0, static_cast<double>(i) / SAMPLE_RATE, 0.8);
        peak = std::fmax(peak, std::fabs(s));
        if (!std::isfinite(s) || std::fabs(s) > 1.0) bounded = false;
    }
    CHECK(bounded);
    CHECK(peak > 0.01);  // produces audible output

    // Filter state persists between calls; reset() clears it so the
    // first post-reset sample at time 0 is silent (env == 0).
    mono.reset();
    CHECK(mono.process(110.0, 0.0, 0.8) == 0.0);
}

void testPolySynth() {
    PolySynth poly;
    const std::array<double, 4> chord = {
        midiToHz(60), midiToHz(64), midiToHz(67), midiToHz(71)
    };

    bool bounded = true;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (int i = 0; i < SAMPLE_RATE; ++i) {
        StereoFrame f = poly.process(chord, static_cast<double>(i) / SAMPLE_RATE, 0.88);
        if (!std::isfinite(f.left) || !std::isfinite(f.right) ||
            std::fabs(f.left) > 1.0 || std::fabs(f.right) > 1.0) {
            bounded = false;
        }
        leftEnergy += f.left * f.left;
        rightEnergy += f.right * f.right;
    }
    CHECK(bounded);
    CHECK(leftEnergy > 0.0);
    CHECK(rightEnergy > 0.0);

    // Released voices are silent.
    StereoFrame silent = poly.process(chord, 10.0, 0.88);
    CHECK(silent.left == 0.0);
    CHECK(silent.right == 0.0);
}

}  // namespace

int main() {
    testMidiToHz();
    testClampAudio();
    testEnvelopeStages();
    testEnvelopeGuardsInvalidParameters();
    testMixer();
    testFmSynth();
    testMonoSynth();
    testPolySynth();

    std::printf("dsp_unit_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
