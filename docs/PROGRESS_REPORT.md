# DATABREAKs progress report

Updated 2026-11-19 for Engine 026 on branch `claude/engine-019-prep-transfer`.
Engine 026 starts from `f94fcef` (Engine 025). Engine 025 started from `3bdd248` (Engine 024). Engine 024 started from `7a1ced6` (Engine 023). Engine 023 started from `d326959` (Engine 022). Engine 022 started from `e4d796c` (the Engine 021 demo WAV, on top of `100a8e1`,
Engine 021). Engine 021 started from `dfd3164` (Engine 020); Engine 020 started from
`ce44e70` (Engine 019). Earlier: `e54db9b` (CTest/docs
transfer), whose parent is `e75b325` on `recovery/codespaces-engine-018`
("Recover audio engine development through Engine 018").

History of this report:

- **`eb93203`:** written against `90e8dfb`, when only the synth prototypes existed.
- **`e54db9b`:** updated after the sample player, slice map, voice manager and 27-pad drum bank were recovered in `e75b325`.
- **`ce44e70`:** updated for Engine 019 (drum voice integration).
- **`dfd3164`:** updated for Engine 020 (drum engine performance and reliability).
- **`100a8e1`:** updated for Engine 021 (unified audio pipeline).
- **`d326959`:** updated for Engine 022 (synthesizer integration); the outcomes of findings B1–B4 were added in §4.1.
- **`7a1ced6`:** updated for Engine 023 (effects engine).
- **`3bdd248`:** updated for Engine 024 (tape in the effects chain).
- **`f94fcef`:** updated for Engine 025 (sequencer foundation).
- **Engine 026 commit:** updated for Engine 026 (recording and performance capture).

## 1. Summary

The repository contains host-side C++17 prototypes through **Engine 026**:

- synths (mono, FM, poly) and two mixers;
- a tape-emulation chain (Engines 005–014);
- a sample player (015);
- a sample slice map (016);
- a multi-voice sample manager (017);
- a 27-pad drum bank (018);
- drum voice integration (019);
- drum engine performance and reliability (020);
- a unified audio pipeline (021): drums → drum level → `StereoMixer` master;
- synthesizer integration (022): stateful mono, FM and poly synths with note-on/off, velocity and pitch bend, as pipeline sources alongside the drums. Findings B1–B4 are fixed in the new voices;
- an effects engine (023): EQ, compressor, soft clipper, distortion and reverb in an ordered chain on the combined bus, between the sources and the master;
- tape in the effects chain (024): the existing `TapeMachine` runs as a chain effect (`TapeEffect`) between the soft clipper and distortion, with all twelve tape controls;
- a sequencer foundation (025): an exact musical clock, 27-pad drum and synth-note patterns, and sample-accurate scheduling into the existing instruments, identical for any block size;
- **performance recording** (026): free and loop recording of drum hits and synth notes (musical events, not audio) with overdub, replace, punch in/out and 16-step undo/redo. Raw timing is kept at 1/1024-tick resolution and replays on the captured sample. Cross-thread control uses lock-free command and live-input queues.

With Engine 019, pad hits produce audio through managed sample voices, and
choke groups fade the voices that are actually sounding.

Engine 020 adds:

- voice stealing without a hard cut;
- a fixed-capacity lock-free event queue, so a second thread can post pad events, each applied at an exact frame;
- stress and determinism tests;
- a host benchmark.

There is **still no FM-1 firmware**: no target toolchain, HAL, drivers, display,
pad scanning, storage or boot code. Engines 019–026 have been run only on a
Linux development host. They have **not** been run or validated on an FM-1, and
the benchmark numbers describe that host, not the FM-1.

## 2. What was verified

### 2.1 Engine 026 commit

Same environment as §2.7.

| Check | Result |
|---|---|
| Default build | 40 executables (35 registered tests + the Engine 020, 023, 024, 025 and 026 benchmarks), **0 warnings** |
| `ctest --test-dir build --output-on-failure` | **35/35 pass** (the 33 existing tests plus `engine_026` and `render_engine_026_render`) |
| `-L unit` / `-L render` | **19/19** / **16/16 pass** |
| `engine_026` | **55/55 checks**. `engine_025` 38 and all earlier engine tests: passing and unchanged |
| Release `-O3 -DNDEBUG` | **0 warnings, 35/35 pass** |
| ASan + UBSan (Debug) | **35/35 pass**, no reports (after the test-harness fix in §13.6) |
| ThreadSanitizer | Engines 020–026, no reports (`engine_026` R1 run three times) |
| Allocation | `engine_026` R2 and every benchmark row: 0 allocations. Earlier allocation checks still pass |
| Existing renders | The 16 reference WAVs are byte-identical to `e54db9b`'s; the Engine 021 demo matches the committed WAV; the Engine 022–025 demos match their own renders |
| New demo | `databreaks_engine_026_demo.wav`: valid, bit-identical for blocks 1/64/256/1000/4096 and across three build types (§13.7) |
| Mutation spot checks | 10 deliberate defects, all caught (§13.6) |
| `git diff --check` | Clean |
| Listening / FM-1 hardware | **Not done** / **not done, deliberately** |

### 2.2 Engine 025 commit (`f94fcef`)

Same environment as §2.7.

| Check | Result |
|---|---|
| Default build | 37 executables (33 registered tests + the Engine 020, 023, 024 and 025 benchmarks), **0 warnings** (`-Wall -Wextra -Wpedantic`; `__int128` is marked `__extension__`) |
| `ctest --test-dir build --output-on-failure` | **33/33 pass** (the 31 existing tests plus `engine_025` and `render_engine_025_render`) |
| `-L unit` / `-L render` | **18/18** / **15/15 pass** |
| `engine_025` | **38/38 checks**. `engine_017` 22, `engine_019` 50, `engine_020` 29, `engine_021` 17, `engine_022` 29, `engine_023` 48 and `engine_024` 35: all passing and unchanged |
| Release `-O3 -DNDEBUG` | **0 warnings, 33/33 pass** |
| ASan + UBSan (Debug, leak detection on) | **0 warnings, 33/33 pass**, no reports |
| ThreadSanitizer | Engines 020–025 (including the transport producer-thread test), no reports |
| Allocation | `engine_025` RT1 and every benchmark row: 0 allocations. Earlier allocation checks still pass |
| Existing renders | The 16 reference WAVs are byte-identical to `e54db9b`'s; the Engine 021 demo matches the committed WAV; the Engine 022–024 demos match their own renders |
| New demo | `databreaks_engine_025_demo.wav`: valid, bit-identical for blocks 1/64/256/1000/4096 and across three build types (§12.6) |
| `git diff --check` | Clean |
| Listening / FM-1 hardware | **Not done** / **not done, deliberately** |

### 2.3 Engine 024 commit (`3bdd248`)

Same environment as §2.7.

| Check | Result |
|---|---|
| Default build | 34 executables (31 registered tests + the Engine 020, 023 and 024 benchmarks), **0 warnings** |
| `ctest --test-dir build --output-on-failure` | **31/31 pass** (the 29 existing tests plus `engine_024` and `render_engine_024_render`) |
| `-L unit` / `-L render` | **17/17** / **14/14 pass** |
| `engine_024` | **35/35 checks**. `engine_017` 22, `engine_019` 50, `engine_020` 29, `engine_021` 17, `engine_022` 29 and `engine_023` 48: all passing |
| Existing tape tests | `tape_test`, `transport_test`, `tape_character_test`, Engines 011–013, and the renders of Engines 005–010 and 014: all pass |
| Release `-O3 -DNDEBUG` | **0 warnings, 31/31 pass** |
| ASan + UBSan (Debug, leak detection on) | **0 warnings, 31/31 pass**, no reports |
| ThreadSanitizer | Engines 020–024, no reports |
| Allocation | `engine_024` P7 and every benchmark row: 0 allocations. Earlier allocation checks still pass |
| Existing renders | The 16 reference WAVs (including the tape renders of Engines 005–009) are byte-identical to `e54db9b`'s; the Engine 021 demo matches the committed WAV; the Engine 022 and 023 demos match their own renders (MD5 `0cfa5643…`, `9cbaecbb…`) |
| New demo | `databreaks_engine_024_demo.wav`: valid, and byte-identical across runs and three build types (§11.7) |
| `git diff --check` | Clean |
| Listening / FM-1 hardware | **Not done** / **not done, deliberately** |

### 2.4 Engine 023 commit (`7a1ced6`)

Same environment as §2.7.

| Check | Result |
|---|---|
| Default build | 31 executables (29 registered tests + the Engine 020 and 023 benchmarks), **0 warnings** |
| `ctest --test-dir build --output-on-failure` | **29/29 pass** (the 27 existing tests plus `engine_023` and `render_engine_023_render`) |
| `-L unit` / `-L render` | **16/16** / **13/13 pass** |
| `engine_023` | **48/48 checks**. `engine_017` 22, `engine_019` 50, `engine_020` 29, `engine_021` 17 and `engine_022` 29: all unchanged and passing |
| Release `-O3 -DNDEBUG` | **0 warnings, 29/29 pass** |
| ASan + UBSan (Debug, leak detection on) | **0 warnings, 29/29 pass**, no reports |
| ThreadSanitizer | `engine_020` 29/29, `engine_021` 17/17, `engine_022` 29/29, `engine_023` 48/48, no reports |
| Allocation | `engine_023` RT1 and every benchmark row: 0 allocations. Earlier allocation checks still pass |
| Existing renders | The 16 reference WAVs are byte-identical to `e54db9b`'s; the Engine 021 demo is byte-identical to the committed WAV; the Engine 022 demo is byte-identical to the Engine 022 render (MD5 `0cfa5643…`) |
| New demo | `databreaks_engine_023_demo.wav`: valid, and byte-identical across runs and three build types (§10.5) |
| Benchmark | §10.6 (host measurements) |
| `git diff --check` | Clean |
| Listening / FM-1 hardware | **Not done** / **not done, deliberately** |

### 2.5 Engine 022 commit (`d326959`)

Same environment as §2.7.

| Check | Result |
|---|---|
| Default build | 28 executables (27 registered tests + the benchmark), **0 warnings** |
| `ctest --test-dir build --output-on-failure` | **27/27 pass** (the 25 existing tests plus `engine_022` and `render_engine_022_render`) |
| `-L unit` / `-L render` | **15/15** / **12/12 pass** |
| `engine_022` | **29/29 checks**. `engine_017` 22/22, `engine_019` 50/50, `engine_020` 29/29 and `engine_021` 17/17 are unchanged |
| Release `-O3 -DNDEBUG` | **0 warnings, 27/27 pass** |
| ASan + UBSan (Debug, leak detection on) | **0 warnings, 27/27 pass**, no reports |
| ThreadSanitizer | `engine_020` 29/29, `engine_021` 17/17, `engine_022` 29/29 (including the T2 producer-thread test), no reports |
| Allocation | `engine_022` R1: 0 allocations. `engine_019` check 41, `engine_020` check 14 and `engine_021` check 11 still pass |
| Existing render WAVs | All 16 reference WAVs byte-identical to `e54db9b`'s. The `engine_021` demo is byte-identical to the committed `databreaks_engine_021_demo.wav` |
| New demo | `databreaks_engine_022_demo.wav`: valid, and byte-identical across four runs and three build types (§9.5) |
| `git diff --check` | Clean |
| Listening / FM-1 hardware | **Not done** / **not done, deliberately** |

### 2.6 Engine 021 commit (`100a8e1`)

Same environment as below.

| Check | Result |
|---|---|
| Default build (`cmake -S . -B build`, `cmake --build build -j2`) | 26 executables (25 registered tests + the benchmark), **0 warnings** |
| `ctest --test-dir build --output-on-failure` | **25/25 pass** (the 23 existing tests plus `engine_021` and `render_engine_021_render`) |
| `-L unit` / `-L render` | **14/14** / **11/11 pass** |
| `engine_021` | **17/17 checks**. `engine_017` 22/22, `engine_019` 50/50 and `engine_020` 29/29 are unchanged |
| Release `-O3 -DNDEBUG` | **0 warnings, 25/25 pass** |
| ASan + UBSan (Debug, leak detection on) | **0 warnings, 25/25 pass**, no reports |
| ThreadSanitizer | `engine_020` 29/29 and `engine_021` 17/17, no reports |
| Allocation | `engine_021` check 11: 0 allocations in the integrated path. `engine_019` check 41 and `engine_020` check 14 still pass |
| Existing render WAVs | All 16 are **byte-identical** to `e54db9b`'s |
| Demo render | `databreaks_engine_021_demo.wav` written, self-checks pass, byte-identical across runs (§8.5) |
| `git diff --check` | Clean |
| Listening / FM-1 hardware | **Not done** / **not done, deliberately** |

### 2.7 Engine 020 commit (`dfd3164`)

Environment: Linux virtual machine, 4 vCPU (Intel Xeon @ 2.10 GHz), GCC 13.3.0,
CMake 3.28.3.

| Check | Result |
|---|---|
| `cmake -S . -B build && cmake --build build -j2` (default, unoptimised) | 24 executables (23 tests + the benchmark), **0 warnings** (`-Wall -Wextra -Wpedantic`; build log searched for "warning" and "error") |
| `ctest --test-dir build --output-on-failure` | **23/23 pass** |
| `ctest --test-dir build -L unit` | **13/13 pass**: adds **`engine_020` (29 checks)**; `engine_019` still passes 50/50 |
| `ctest --test-dir build -L render` | **10/10 pass** |
| Release build (`-O3 -DNDEBUG`), separate directory | **0 warnings**, **23/23 pass**. A GCC `-O3` `-Warray-bounds` false positive on `owners_[slot]` was removed by adding an explicit bounds guard (see §7.6) |
| AddressSanitizer + UndefinedBehaviorSanitizer | Separate Debug build, leak detection on: **23/23 pass**, no reports, 0 warnings |
| ThreadSanitizer (`-fsanitize=thread`) | `engine_020`, including the two-thread queue test: **29/29 pass, no reports**. Control: weakening the queue's acquire/release ordering to relaxed made TSan report data races, so the clean result is meaningful |
| Render output unchanged | All 16 render WAVs are **byte-identical** to those from `e54db9b` |
| Mutation checks on `engine_020` | Five deliberate faults were each caught (§7.7). Three were not caught; each was investigated, and all three are explained in §7.7 |
| Benchmark | Run in the default and Release builds; results in §7.8 |
| Listening test | **Not done** |
| FM-1 hardware | **Not done, deliberately**: nothing flashed or connected |

### 2.8 Engine 019 commit (`ce44e70`, for reference)

22/22 CTest; `engine_019` 50 checks; ASan + UBSan 22/22; 0 warnings; render
WAVs byte-identical to `e54db9b`.

## 3. Component status

| Component | Engine | Status | Notes |
|---|---|---|---|
| `audio_engine.h`, `StereoMixer` | 001–003 | Working prototype | Unit-tested. Since Engine 021, `StereoMixer` is the pipeline's master stage |
| Original synths + stateless ADSR (`mono/fm/poly_synth.h`, `synth_parameters.h`) | 002–004 | Prototype with known bugs, kept as reference | B1–B4 still apply to these classes; **unchanged** so the 002–004 reference renders stay byte-identical |
| Stateful synth voices and engines (`synth_voices.h`, `synth_engine.h`) | 022 | Working prototype, tested | B1–B4 fixed; reproduce the originals for normal notes (§9) |
| `LayeringMixer` | 004 | Working prototype | No output clamp (the demo's WAV writer clamps). No unit test |
| Tape machine / transport / streaming | 005–014 | Working prototype | Assertion tests in 011–013 and the three `tape_*_test` programs. 005–010 and 014 are renders or benchmarks. **Unchanged** |
| `SamplePlayer` | 015 | Working prototype, tested | Linear interpolation, reverse, pitch/rate. **Unchanged** |
| `SampleSliceMap` | 016 | Working prototype, tested | Uses `std::vector`. **Unchanged** |
| `SampleVoiceManager<N>` | 017 (+019, 020) | Working prototype, tested | Fixed pool, oldest-voice stealing. `processBlock` **adds** into the output. Engine 019 added slot-level control (§6.2); Engine 020 added queued sounds and unique sound ids (§7.2). The original API's results are unchanged |
| `DrumPadBank<27>` | 018 | Working prototype, tested | **Unchanged**. Its `active` flag is now kept in sync by `DrumEngine` |
| `DrumEngine<N, E>` | 019, 020 | Working prototype, tested | See §6 and §7 |
| `DrumEventQueue<N>` | 020 | Working prototype, tested | Lock-free single-producer/single-consumer ring; see §7.4 |
| `AudioPipeline<V, E, S>` | 021–025 | Working prototype, tested | Unified host render path: drums + mono/FM/poly synths → effects chain → master, driven by the sequencer; see §8–§12 |
| Sequencer (`core/sequencer/`) | 025, 026 | Working prototype, tested | Musical clock, patterns, sample-accurate scheduling (§12); performance recording, command and live-input queues (§13) |
| Effects (`core/audio/effects/`) | 023, 024 | Working prototype, tested | EQ, compressor, soft clipper, tape (`TapeEffect`, 024), distortion, reverb, `EffectChain`; see §10 and §11 |
| `SpscRing<T, N>` | 020, 022 | Working prototype, tested | Generalised drum queue; carries drum and synth events |
| MIDI byte input, quantisation, pattern banks, effect automation, tape transport as a source | — | Not started | Live input enters through `Sequencer::postLive` (Engine 026); new sources go into `AudioPipeline::mixSources` |
| Firmware (HAL, drivers, display, pads, storage, boot) | — | Not started | Depends on hardware facts that are not yet documented |

## 4. DSP findings (synth/envelope code, carried over unchanged)

These apply to `instruments/synthesizers/` and are **not fixed** on this branch,
by request. They were first reported in `eb93203`.

- **B1: hard cut on Poly chord changes (audible click).** Gate 0.88 s plus release 0.25 s is longer than the 1 s chord, so the release is cut off.
  - On `90e8dfb`: the left channel was at about 0.05 when it jumped to 0 at t = 9 s.
  - On `e75b325`'s layered Engine 003/004: about 0.03 at t = 9 s and 10 s.
  - More generally, the time-based design restarts every note at `t = 0`, so a new note always cuts the old one off.
- **B2: early release is ignored.** In `calculateEnvelope`, the attack and decay branches don't check `gate`. If the note is released before attack + decay completes, the envelope still finishes the full attack and decay, and only then releases from the sustain level. With `gate = 0`, a note still plays a full attack and decay.
- **B3: release starts from the sustain level, not the current level.** Even with B2 fixed, releasing during the attack or decay would jump to the sustain level, causing a click.
- **B4: phase is computed from absolute time.** `sin(2πf·t)` restarts at every note. That means no continuous phase, no retriggering without a click, and poor precision as `t` grows. On a target without double-precision floating point it is also expensive (`sin`/`exp`/`pow` per sample).

### 4.1 Outcomes (Engine 022)

The wording above is kept exactly as first reported. Before Engine 022 each
finding was re-tested against the unchanged code: `instruments/` had not
changed since `90e8dfb`.

**B1: hard cut on Poly chord changes.**

- *Original code:* still present in `PolySynth` and the Engine 002–004 demos, which are deliberately unchanged.
- *Engine 022:* resolved in the new engines. Chords are separate voices with their own envelopes. A new chord takes over voices that are still releasing, and the attack restarts from each voice's current level with continuous phase, so nothing is cut to 0.
- *Measured (check B1):* the 003/004 progression (gate 0.88 s, release 0.25 s, chord change at 1.0 s) has a largest sample step of 1.6e-3 at the change, below the 8.8e-3 of steady playing.

**B2: early release is ignored.**

- *Re-test:* with attack 0.1 s, decay 0.2 s and gate 0.05 s, `calculateEnvelope` keeps rising to 0.8 at 0.08 s and finishes the decay.
- *Refinement:* the release time is counted from the gate. When release finally begins at 0.3 s, it has therefore already "run". The level jumps from the sustain level straight to the value of a release that began at the gate: 0.5 → 0.0 in one sample with a 0.1 s release, or 0.5 → 0.1875 with a 0.4 s release. B2 and B3 together therefore produce a discontinuity.
- *Engine 022:* resolved in `AdsrEnvelope`. `noteOff()` starts the release immediately in any stage. Check B2 shows the level only falls after a mid-attack note-off, reaching 0 exactly 4,800 frames (0.1 s) later.

**B3: release starts from the sustain level, not the current level.**

- *Original code:* still present in `calculateEnvelope`.
- *Engine 022:* resolved. The release starts from the current level: the first release sample equals the last attack sample (check B3). A note-off mid-decay and a retrigger mid-release never step by more than the steepest legal slope, 1/480 per frame for a 10 ms attack (check B2/B3).

**B4: phase is computed from absolute time.**

- *Original code:* still present in the original classes. Re-test: restarting an FM note at t = 0 mid-cycle steps the output by 0.21 in one sample.
- *Engine 022:* resolved with phase accumulators that carry across samples, blocks and notes.
- *Measured:* a 10 s note rendered in blocks of 1/7/64/333/4096 is bit-identical to one block (B4a), and its phase is still exact to 1.6e-12 after 10 s (B4b). Retriggers keep the phase (B1).
- *Cost:* still one `sin` per oscillator per sample in `double`, as noted in B4's last sentence. That part (target cost) remains open until the hardware is known.

## 5. Other issues found in the recovered code

- **R1: Engines 016–018 were missing from the build.** Fixed in `e54db9b` (targets plus CTest).
- **R2: there was no CTest registration.** Fixed in `e54db9b`. Engine 019 brings the total to 22 tests.
- **R3: Engine 003 and Engine 004 are identical. Investigated; left unchanged.**
  - Evidence:
    - In `e75b325`, `tests/audio/engine_003.cpp` and `engine_004.cpp` differ only in the output WAV filename, and they render byte-identical audio.
    - `90e8dfb:tests/audio/engine_003.cpp` (blob `41a9700`) is different code: synths in sequence (0–4 s mono, 4–8 s FM, 8–12 s poly) through `StereoMixer`, with the banner "Independent Synthesizer Architecture".
    - The current files (blobs `5778358` / `69e935e`) layer all three synths through `LayeringMixer`, with the banner "Layered Synthesizer Architecture". **Both** current files print "DATABREAKs AUDIO ENGINE 003".
    - `git log --follow` traces `engine_004.cpp` back to `90e8dfb`'s `engine_003.cpp`.
  - Interpretation: the layered version was probably made by editing `engine_003.cpp` in place in Codespaces, and then copied to `engine_004.cpp`. The original Engine 003 is recoverable, but it is **not clear** whether the in-place change to 003 was intended. Restoring it would also change Engine 003's rendered audio.
  - Decision: following the rule not to guess, both files are left unchanged.
  - To restore the original deliberately (as its own commit), run `git show 90e8dfb:tests/audio/engine_003.cpp > tests/audio/engine_003.cpp`. It still compiles against the current headers, because `StereoMixer` still exists. Engine 004 keeps the layered version.
- **R4: the pad bank's `active` flag never cleared by itself.** **Resolved by `DrumEngine`**, which clears it when the pad no longer has a playing voice (§6.6). `DrumPadBank` itself is unchanged, so code that uses the bank directly still gets the old behaviour.
- **R5: retriggering the same pad stacked voices.** **Resolved by `DrumEngine`'s explicit retrigger policy** (§6.5).
- **R6: housekeeping (partly resolved).** `.gitignore` (`build/`, `*.wav`) was added in Engine 021. Still open:
  - There is a stray empty file at `.devcontainer/tests/audio/audio_test.cpp`.
  - There is no LICENSE, although the project is described as open source.
  - There is no README.

## 6. Engine 019: 27-pad drum voice integration

Files:

- `core/audio/drum_engine.h` (new);
- `core/audio/sample_voice_manager.h` (extended);
- `tests/audio/engine_019.cpp` (new);
- `CMakeLists.txt` (one target, plus one entry each in the warning loop and the `unit` test list).

### 6.1 Architecture and public API

`template <std::size_t VoiceCount = 16> class DrumEngine` owns:

- a `DrumPadBank<27>`;
- a `SampleVoiceManager<VoiceCount>`;
- a fixed `std::array` of ownership records, one per voice slot.

Pad count (27, physical) and voice count (polyphony, a compile-time
parameter) are independent. The default of **16 voices** matches
`SampleVoiceManager`'s existing default. It is a starting value, not tuned for
any target.

| Group | Members |
|---|---|
| Constants | `PadCount` (27), `npos`, `DefaultReleaseFadeFrames` (240), `MaxReleaseFadeFrames` (48000), `padCount()`, `voiceCapacity()` |
| Pad setup (forwarded to the bank) | `assignSample`, `clearSample`, `setRegion`, `setVolume`, `setPitch`, `setVelocitySensitivity`, `setChokeGroup`, `setReverse`, `getPad` |
| Engine settings | `setReleaseFadeFrames` / `releaseFadeFrames` (clamped to 1…48000), `setRetriggerPolicy` / `retriggerPolicy` |
| Playing | `trigger(pad, velocity)` → slot index, or `npos` if nothing played |
| Stopping | `releasePad(pad)`, `releaseAll()` (fade); `stopPad(pad)`, `stopAll()` (immediate) |
| Audio | `processBlock(StereoFrame* out, std::size_t n)`: **adds** into `out`; ignores a null `out` or `n == 0` |
| Queries | `voiceOwner(slot)`, `padVoiceCount`, `padReleasingVoiceCount`, `padVoices(pad, slots, max)`, `isPadActive`, `isPadPlaying`, `isPadReleasing`, `activeVoiceCount`, `bank()` / `voices()` (read-only) |

The trigger flow inside `trigger(pad, velocity)`:

1. Reject a pad index of 27 or more, and a velocity of 0 or below. Velocity above 127 is clamped to 127, as in `DrumPadBank`.
2. Call `DrumPadBank::trigger`. This validates the sample and region, and computes volume (velocity sensitivity), playback rate (pitch), reverse, and `chokedPads`.
3. Choke (§6.4).
4. Apply the retrigger policy (§6.5).
5. Pick a slot (§6.3).
6. Call `SampleVoiceManager::triggerInSlot` with the event's sample, region, volume, rate and reverse flag.
7. Record ownership and sync pad activity.

The existing pad behaviour (velocity, pitch, volume, reverse, regions, choke
decisions) still comes from `DrumPadBank`, and is not reimplemented.

### 6.2 Changes to `SampleVoiceManager` (Engine 017)

These are additions only. The original `trigger`, `processBlock`, `stopAll`,
`activeVoiceCount` and `capacity` give the same results as before: a voice
that is never released is mixed with a gain of exactly `volume * 1.0`.
Engine 017 still passes all 22 checks, and all render WAVs are byte-identical.

New members:

- `triggerInSlot`;
- `releaseVoice(slot, fadeFrames)`;
- `stopVoice`;
- `isVoiceActive`, `isVoiceReleasing`, `voiceReleaseGain`, `voiceGeneration`, `voiceAge`;
- `npos`.

Each voice gained `releaseGain`, `releaseStep`, `releasing` and `generation`.

One deliberate behaviour change: a non-finite volume (NaN or infinity) passed
to `trigger` is now treated as 0. Before, `std::clamp` passed NaN through,
which produced NaN audio.

### 6.3 Voice capacity and stealing (deterministic)

When `trigger` needs a slot, it takes:

1. the lowest-numbered free slot;
2. otherwise the **oldest voice that is already releasing**;
3. otherwise the **oldest voice overall**.

Options 2 and 3 replace the old sound at once (a hard steal, which can
click), because one slot cannot hold both a fade and a new sound. The
previous owner's ownership record is cleared and its activity updated in the
same call. Check 27 verifies the whole order with a 2-voice pool.

> **Superseded in Engine 020** (§7.3): stealing no longer cuts. The stolen
> voice fades out over the steal fade, and the new sound follows it in the
> same slot.

### 6.4 Choke behaviour

When a pad in nonzero group G is hit, every voice owned by a pad named in
`TriggerEvent::chokedPads` starts its release fade. So does every voice of any
other pad in G that still has a playing voice.

The second rule exists because `DrumPadBank::assignSample` and `clearSample`
clear a pad's `active` flag even while it is sounding (check 22b). Without it,
such a pad would escape the next choke.

Voices outside G, and pads in group 0, are never choked (checks 20 and 21).
The new hit starts at once; it doesn't wait for the fades.

### 6.5 Same-pad retrigger policy

`RetriggerPolicy::FadeAndRetrigger` is the default. When a pad is hit again:

- its existing voices start the release fade, keeping their ownership until the fade ends;
- a new voice starts immediately, with its own ownership record (checks 23–26);
- other pads are unaffected.

`RetriggerPolicy::Overlap` layers voices without fading, and is selectable with
`setRetriggerPolicy`. The enum leaves room for more modes, such as "mono hard
cut" or "ignore while playing".

### 6.6 Release fade

The fade is **linear**:

- It starts at the voice's current gain *g* and lowers it by *g* / `fadeFrames` after each rendered frame.
- Both channels are scaled by the same gain.
- When the gain reaches 0 (within 1e-12), the voice is stopped and its slot freed.

The default is **240 frames = 5 ms at 48 kHz**. With *g* = 1, the frames
output during the fade are 1, 1 − 1/240, …, 1/240, and then silence. The
largest step is 1/240 of full scale (checks 35 and 36).

A second release request on a voice that is already fading keeps the steeper
of the two slopes. So the fade can only speed up: it never slows down, restarts
or gets louder (check 35b).

The fade is applied inside `SampleVoiceManager::processBlock`, so sample
playback code is not duplicated.

### 6.7 Pad activity synchronisation

- `isPadActive(p)`: pad *p* owns at least one live voice, playing or releasing.
- `isPadPlaying(p)`: at least one of those voices is not releasing.
- `isPadReleasing(p)`: the pad still sounds, but only through fading voices.
- `DrumPadBank::isActive(p)` (through `bank()`): kept equal to "has a playing voice". This is what the bank uses to decide which pads to choke.

Activity is updated:

- immediately on `trigger`, steal, `releasePad`, `stopPad` and `stopAll`;
- at the **end** of each `processBlock`, for voices that finished naturally or finished fading.

Within a block, a voice that ends partway through is still reported as active
until the block returns.

### 6.8 Ownership model

Each slot has `{pad, generation}`. `SampleVoiceManager` increments a slot's
generation every time the slot starts a new sound. An ownership record counts
only if:

- its generation matches the slot's current generation; and
- the voice is still active.

Records that no longer count are cleared after each block, and whenever their
slot is reused.

Ownership is never inferred from voice order. The generation check is a
second safeguard: the voice pool is private to `DrumEngine`, and `trigger`
always clears a slot's record before reusing the slot. That means no public
sequence of calls can produce a stale record for the generation check to
catch. A mutation that removes it is therefore not detected by
`engine_019`. Check 14 verifies the counter at the voice-manager level instead.

> **Superseded in Engine 020** (§7.2): records now hold a unique sound id
> instead of the slot generation. The generation counter still exists and
> still increments.

### 6.9 Real-time safety

`trigger`, `processBlock`, `releasePad`, `releaseAll`, `stopPad` and `stopAll`:

- use only fixed-size `std::array` state;
- allocate no heap memory (check 41 counts global `operator new` calls over 50 rounds of triggers, chokes and blocks, plus a release and a stop);
- take no locks, perform no I/O and throw no exceptions.

There is no internal synchronisation, so configuration, triggers and
`processBlock` must be called from the same thread. Hand-off to a separate
audio thread (for example with a lock-free event queue) is future work.

> **Updated in Engine 020** (§7.4): the `post*` methods may now be called
> from one other thread. Everything else is still single-threaded.

Sample vectors are borrowed by pointer. They must outlive every voice that
plays them and must not be resized while one is playing. `clearSample` and
`assignSample` don't stop voices that are already playing the old sample.

### 6.10 Test coverage: `tests/audio/engine_019.cpp`, 50 checks, all passing

All checks use deterministic synthetic samples, with exact or 1e-9 tolerance
assertions, and the program exits non-zero on any failure.

- Checks 01–40 follow the Engine 019 test plan:
  - basic integration (01–09);
  - ownership (10–15);
  - choke (16–22);
  - retrigger and pool exhaustion (23–27);
  - activity and stopping (28–33);
  - audio correctness and safety (34–40).
- Extra checks:
  - 07b: velocity above 127 is clamped;
  - 22b: a choke still works after `assignSample` while the pad is sounding;
  - the default retrigger policy;
  - the `Overlap` policy;
  - `releaseAll`;
  - the default fade length of 240;
  - 35b: re-release never raises the gain;
  - 37b: `processBlock` adds into the buffer;
  - 41: no heap allocation;
  - the fade length is clamped to at least 1.
- Check 38 renders the same choke scenario with block sizes 1, 3, 7, 16, 63 and 64, and requires bit-identical output.

### 6.11 Known limitations and remaining work

- ~~A hard steal (pool exhausted, nothing releasing) still cuts a voice without a fade.~~ Resolved in Engine 020 (§7.3).
- Activity changes caused by audio are only seen at block boundaries. (Still true.)
- ~~There is no event queue or thread hand-off, and no sample-accurate trigger timing inside a block.~~ Resolved in Engine 020 for posted events (§7.4). A direct `trigger()` call still takes effect at the start of the next block.
- Per-pad pan, output routing and a pad mixer are not implemented. Voice gain is `pad volume × velocity curve`, as defined by `DrumPadBank`.
- Voices use `double` and linear interpolation, the same as `SamplePlayer`. Suitability for the eventual target is unknown until the hardware is documented.
- Nothing has been listened to, and nothing has been run on an FM-1.

## 7. Engine 020: drum engine performance and reliability

Files:

- `core/audio/drum_event_queue.h` (new);
- `core/audio/drum_engine.h` (extended);
- `core/audio/sample_voice_manager.h` (extended);
- `tests/audio/engine_020.cpp` (new, CTest `unit`);
- `tests/audio/engine_020_benchmark.cpp` (new, build target only);
- `CMakeLists.txt`.

Every Engine 019 public method keeps its name, signature and meaning, and
`engine_019` passes unchanged (50/50).

`DrumEngine` gained a second template parameter,
`EventCapacity = 256`. Existing `DrumEngine<N>` code still compiles.

### 7.1 New public API

| Group | Members |
|---|---|
| Steal fade | `setStealFadeFrames` / `stealFadeFrames` (clamped to 1…`MaxStealFadeFrames` = 480), `DefaultStealFadeFrames` (48) |
| Allocation report | `lastAllocation()` → `Allocation::{None, FreeSlot, StoleReleasing, StolePlaying, ReplacedPending}` |
| Queued sounds | `padPendingVoiceCount(pad)` |
| Event queue (producer thread) | `postTrigger(pad, velocity, frameOffset)`, `postReleasePad`, `postStopPad`, `postStopAll`, `droppedEventCount()`, `eventCapacity()` |
| Statistics (audio thread) | `stolenVoiceCount()`, `droppedPendingSoundCount()`, `processedEventCount()` |

New `SampleVoiceManager` methods:

- `queueInSlot`;
- `cancelPending`;
- `hasPending`;
- `pendingSoundId`;
- `voiceSoundId`.

### 7.2 Ownership by unique sound id

Every sound that is started or queued gets a unique, increasing 64-bit id from
`SampleVoiceManager`. An ownership record `{pad, soundId}` counts only while:

- that id is the sound currently audible in the slot (and the voice is active); or
- that id is the sound queued in the slot.

This means:

- a stolen voice stops belonging to its old pad **at the moment of the steal**, even though its fade is still audible;
- a reused slot can never be mistaken for an earlier sound.

Check 10 performs 10,000 triggers on a 4-voice pool. It checks that ids keep
increasing, and that the slot each trigger returns is owned by that pad.

### 7.3 Click-free voice stealing (bounded design)

A slot can hold one audible sound. A new sound therefore cannot start in a
busy slot without either cutting the old sound or going over `VoiceCount`.

Engine 020 does neither. The stolen voice gets a short **linear steal fade**,
and the new sound is **queued in the same slot**. It starts on the frame
immediately after the fade ends, or sooner if the old sound ends by itself
first.

The trade-off is that when the pool is full, the new hit is delayed by at
most `stealFadeFrames()`: 48 frames = **1 ms** by default, never more than
10 ms. The delay is applied inside the per-frame loop, so it is identical for
any block size.

Slot choice is deterministic:

1. The lowest idle slot. The sound starts now, with no delay.
2. Otherwise a **releasing** voice with nothing queued, choosing the quietest (lowest current gain), then the oldest. Its fade is sped up to finish within the steal fade, and never slowed down.
3. Otherwise the **oldest playing** voice, which gets the steal fade.
4. Otherwise every slot is already fading towards a queued sound (more than `VoiceCount` hits within about one steal fade). The slot whose queued sound was queued **earliest** has that sound replaced. The replaced sound never plays, nothing audible is cut, and it is counted in `droppedPendingSoundCount()`.

Queued sounds count as "playing" for pad activity and choke decisions. A
choke or a same-pad retrigger **cancels** a queued sound before it starts,
rather than fading it (checks S5 and S6).

Waveform checks, using constant test samples:

- **S1** (all voices playing, steal fade 4): left channel 2, 1.75, 1.5, 1.25, then 1; the new sound appears on frame 4 on the right channel.
- **S2** (a releasing voice at gain 0.8 is stolen): 1.8, 1.6, 1.4, 1.2, then 1.
- **S4**: a queued sound is replaced, and the replaced pad never sounds.
- **12** (stress): over 400 rounds with stealing on 8 voices, the largest per-frame drop is no more than 8 voices × 1/48. A hard cut would drop by 1.0.

### 7.4 Trigger handling, event queue and threading model

`DrumEventQueue<N>` is a ring buffer:

- a power-of-two `std::array` holding N − 1 events;
- two atomic indices with acquire/release ordering;
- no locks and no allocation.

It is lock-free by `static_assert`, which requires that `std::atomic<size_t>`
is always lock-free.

`DrumEngine::post*` validates each event and pushes it. When the queue is full,
`post*` returns **false** and the event is dropped and counted. It never
blocks, overwrites or allocates.

`processBlock` applies up to `eventCapacity()` events per call:

- in FIFO order;
- each at frame `min(max(frameOffset, previous event's frame), n − 1)`;
- by rendering up to that frame first, so posted triggers are frame-accurate (check Q1: silent through frame 9, sounding from frame 10).

An offset past the end of the block is clamped to the last frame (Q2). A
smaller offset after a larger one waits for the larger (Q3). Release, stop-pad
and stop-all events also act at their frame (Q4).

**Threading guarantees (exactly these, and no more):**

| Caller | May use |
|---|---|
| One audio thread | `processBlock` and every other method |
| One producer thread, concurrently | `postTrigger`, `postReleasePad`, `postStopPad`, `postStopAll`, `droppedEventCount` |

- More than one producer, or more than one consumer, is **not** supported.
- Configuration (`assignSample`, `set*`) is **not** thread-safe and must not run while the audio thread is running.
- Direct `trigger()` takes effect at the start of the next rendered frame range, as in Engine 019.

The two-thread check **T** has a real producer thread post 20,000 triggers into
a 63-event queue while the audio thread renders. It checks that
processed + dropped = posted, and that every invariant holds after every block.
It passes normally and under ThreadSanitizer.

### 7.5 Real-time safety review

Reviewed `DrumEngine`, `SampleVoiceManager`, `SamplePlayer` and `DrumEventQueue`:

- **Heap allocation:** none in `trigger`, `post*`, `processBlock`, `releasePad`/`releaseAll`, `stopPad` or `stopAll`.
  - Check 14 counts global `operator new` over 2,000 mixed rounds: 0 allocations.
  - The benchmark also reports 0 allocations for every measurement.
  - The check catches a planted allocation in the default (unoptimised) build. An optimiser may remove a matching new/delete pair, so the default build is the authoritative one.
- **Locks, blocking, I/O:** none in those paths. The queue uses only atomic loads and stores.
- **Sample bounds:** `SamplePlayer` reads `indexA < endFrame`, and `indexB = min(indexA + 1, endFrame − 1)`. Regions are validated in `DrumPadBank` and `SampleVoiceManager`. The final frame and reverse playback at the fastest rate are tested (Engine 019 check 40). ASan reports nothing across all 23 tests.
- **Slot bounds:**
  - every slot-taking `SampleVoiceManager` method checks `slot < VoiceCount`;
  - `DrumEngine` guards the slot returned by `selectSlot`, so the access is provably in bounds;
  - pads are checked against 27.
- **Fade completion:** a fading voice stops once its gain is at or below 1e-12. Every voice therefore ends, at most `MaxReleaseFadeFrames` frames after release (check 13, and check 07 after 60 s).
- **Ownership cleanup:** after every block, the ownership/activity rules (`invariantsHold`) are checked in tests 01–04, 07, 09, 10, Q6 and T.

### 7.6 Compiler warning found and fixed

The Release (`-O3`) build reported `-Warray-bounds` on `owners_[slot]` in
`trigger()`. Every path in `selectSlot()` returns an index below `VoiceCount`,
so this is a GCC false positive.

Rather than suppress it, `trigger()` now checks `slot >= VoiceCount`
explicitly. That path is unreachable, and returns `npos` if ever reached. All
builds now have 0 warnings.

### 7.7 Test coverage: `tests/audio/engine_020.cpp`, 29 checks, all passing

All checks use deterministic synthetic samples: constants, and per-pad sines of
different lengths. The program exits non-zero on any failure.

| Requested item | Checks |
|---|---|
| 1. Repeated triggering across all 27 pads | 01 (3,000 triggers, invariants every block) |
| 2. More triggers than voices | 02 |
| 3. Repeated same-pad triggering | 03 (1,000 hits) |
| 4. Multiple simultaneous choke groups | 04 (three groups, at most one pad playing per group) |
| 5. Stealing while voices are fading | S2, S3, 05 |
| 6. Stealing while all voices are active | S1, 06 |
| 7. Long-running playback | 07 (60 s of audio, block 128) |
| 8. Rapid trigger sequences | 08/Q6 (one trigger per frame), S4–S6 |
| 9. Repeated stopPad / stopAll | 09 |
| 10. Ownership after extensive slot reuse | 10 (10,000 triggers) |
| 11. Stereo correctness | 11 (channel isolation and exact mirroring) |
| 12. Fade behaviour under stress | 12 |
| 13. Silence after all voices finish | 13 |
| 14. No allocations | 14 |
| 15. Determinism | 15 (same script, bit-identical across block sizes 1, 7, 64, 256 and 1000, with identical statistics) |
| Queue behaviour | Q1–Q5 |
| Threading | T |

Mutation results:

- **Caught:**
  - hard steal instead of steal fade (7 checks fail);
  - oldest instead of quietest releasing voice (S3);
  - queued sounds not cancelled by choke or retrigger (03, 04, Q6);
  - frame offsets ignored (Q1–Q4, 15);
  - newest instead of earliest queued sound replaced (S4);
  - allocation in `push()` (14, in the default build);
  - TSan control: relaxed queue ordering is reported as a data race.
- **Not caught, investigated:**
  - Removing `max(frameOffset, position)` behaves identically: an earlier offset already lands at `position` through the `at > position` guard.
  - Skipping the clear of the stolen slot's owner record behaves identically: the record is overwritten a few lines later. The clear only matters on the unreachable failure path.
  - Planting an allocation in `push()` is not caught under `-O1`, where GCC is allowed to remove the paired new/delete.

### 7.8 Benchmark: `databreaks_engine_020_benchmark` (host only)

Method:

- Each row renders **10.0 s** of 48 kHz stereo audio through `processBlock`. Real-time factor = 10.0 s ÷ process CPU time (`std::clock`).
- The voices play 12-second sine tones, so every requested voice stays active (the `active` column confirms it).
- `trigger()` timing uses the steady clock over bursts of 64 calls, with a 64-frame block between bursts.
- Allocations are counted with a global `operator new` counter.
- There are **no pass/fail thresholds**. The benchmark is not a CTest test.

Host: shared virtual machine, 4 vCPU Intel Xeon @ 2.10 GHz, GCC 13.3. Repeated
runs varied by up to about 20%.

**Release build (`-O3 -DNDEBUG`):**

| Block | 16 | 32 | 64 | 128 | 256 | 512 | 1024 |
|---|---|---|---|---|---|---|---|
| 16 voices: CPU s per 10 s of audio | 0.0493 | 0.0422 | 0.0384 | 0.0357 | 0.0347 | 0.0345 | 0.0336 |
| Real-time factor | 203× | 237× | 260× | 280× | 288× | 290× | 297× |

| Active voices (block 128) | 0 | 1 | 4 | 8 | 16 | 32* | 64* |
|---|---|---|---|---|---|---|---|
| CPU s per 10 s of audio | 0.0052 | 0.0076 | 0.0123 | 0.0192 | 0.0344 | 0.0792 | 0.1440 |
| Real-time factor | 1930× | 1321× | 810× | 522× | 291× | 126× | 69× |

\* Engine built with a 64-voice capacity; the other columns use 16 voices.

- **`trigger()`:** 200,000 calls, about **259 ns per call**, 0 allocations. 199,984 calls stole a voice; 0 queued sounds were replaced.
- **`postTrigger()` + `processBlock(64)`:** 200,000 events plus 8.33 s of audio in 0.0774 CPU s, 0 allocations, 0 dropped.

**Default unoptimised build (for comparison only):**

- 16 voices: real-time factor 28× at block 16 and 64× at block 1024.
- Block 128: 55× with 16 voices and 15× with 64 voices.
- `trigger()`: about 2.3 µs per call.

What these numbers show on this host:

- Cost per frame grows roughly linearly with active voices.
- Small blocks cost more, because each block repeats the per-voice and per-slot bookkeeping (activity sync loops over 27 pads × voices).
- `trigger()` cost is dominated by the choke scan, which calls `isPadPlaying` for each of 27 pads, and each call loops over the voices.

None of this predicts FM-1 performance. The FM-1's processor, clock, FPU and
memory are not documented, and these voices use `double` arithmetic.

### 7.9 Remaining risks and limitations

- When the pool is full, a new hit is delayed by up to the steal fade (default 1 ms). With more than `VoiceCount` hits inside one steal fade, the earliest queued sound is dropped (counted).
- A stolen voice's pad becomes inactive immediately, although its tail is audible for up to the steal fade.
- Only single-producer/single-consumer use is supported. Configuration is not thread-safe. Direct `trigger()` is not frame-accurate; posted events are.
- Events beyond `eventCapacity()` per block wait for the next block. A full queue drops new events (counted), so the producer must check the return value.
- Pad activity caused by audio (voices ending) is still updated only at block boundaries.
- `trigger()` is O(27 × VoiceCount). This is fine on the host (about 259 ns at `-O3`), but unmeasured on target hardware.
- The allocation checks rely on replacing global `operator new`. That doesn't see `malloc` called directly; none of the audited code does.
- Nothing has been listened to, and nothing has been run on an FM-1.

## 8. Engine 021: unified audio pipeline

Files:

- `core/audio/audio_pipeline.h` (new);
- `core/audio/sample_voice_manager.h` (output gain);
- `core/audio/drum_engine.h` (`setOutputLevel` / `outputLevel`);
- `tests/audio/engine_021.cpp` (new, `unit`);
- `tests/audio/engine_021_render.cpp` (new, `render`);
- `CMakeLists.txt`;
- `.gitignore` (new: `build/`, `*.wav`).

No existing public API changed. Engines 017, 019 and 020 pass unchanged, and
the 16 existing render WAVs are byte-identical.

### 8.1 Architecture and routing decisions

Existing pieces reviewed:

- `StereoMixer` (master gain clamped to 0–2, hard clip at ±1, per frame; previously used only in unit tests);
- `LayeringMixer` (per-frame mono/FM/poly layer volumes plus its own master; used by the synth demos 003/004);
- `DrumEngine` (block-based, adds into the caller's buffer, frame-accurate queued events within each `processBlock` call).

Decisions:

- **One pipeline, no new mixer type.** `AudioPipeline` owns the `DrumEngine` and a `StereoMixer`, and has three methods:
  - `mixSources(out, n)`: adds every source into the bus, each at its level;
  - `applyMaster(out, n)`: the master stage, in place;
  - `render(out, n)`: `mixSources` then `applyMaster`.
- **The drum level is applied where drum voices are summed.** `SampleVoiceManager` multiplies each frame's summed voices by `outputGain` once, before adding them to the buffer. This means:
  - no scratch buffer;
  - no splitting of the caller's block, so any block size works and **queued event offsets keep their exact frame** (splitting would have broken them, because offsets are relative to each `processBlock` call);
  - audio already in the buffer is never scaled by the drum level.
- **The existing `StereoMixer` is the master stage.** Before it, `applyMaster` replaces NaN/inf samples with 0 and counts them. It also counts samples whose |x × master| exceeds 1 (`clippedSampleCount()`). The pipeline's master default is 1.0, chosen for predictable gain staging; `StereoMixer`'s own default stays 0.8.
- **`LayeringMixer` is left alone.** The synths are still stateless functions of note time (finding B4), so they cannot render arbitrary blocks yet. A future synth source belongs in `mixSources()` with its own level, not in a second mixer.

Signal flow and gain staging, each gain applied exactly once:

```
pad sample ─► × pad volume × velocity curve (DrumPadBank)
           ─► × release/steal fade (per voice)
           ─► Σ voices × drum level (SampleVoiceManager summing) ─► + bus
bus (any audio already there + drums) ─► NaN/inf→0 ─► × master ─► clip ±1
```

Left and right always receive identical gains. There is no pan stage yet.

### 8.2 API

`AudioPipeline<DrumVoices = 16, DrumEvents = 256>` provides:

- `drums()`: the full `DrumEngine` API, including `post*`;
- `setDrumLevel` / `drumLevel`: 0–2, non-finite values ignored;
- `setMasterLevel` / `masterLevel`: 0–2, non-finite values ignored;
- `render`, `mixSources`, `applyMaster`;
- `clippedSampleCount`, `invalidSampleCount`, `resetDiagnostics`.

All of them are null-safe and treat a frame count of 0 as a no-op.

### 8.3 Threading model and real-time safety

The threading model is unchanged from Engine 020:

- `render`, `mixSources`, `applyMaster`, level changes and configuration run on **one** audio thread;
- `drums().post*()` may be called from **one** other thread.

ThreadSanitizer runs of `engine_020` and `engine_021` report nothing.

Real-time behaviour of the integrated path:

- no heap allocation (check 11 counts `operator new` over 2,000 rounds of render, mixSources, applyMaster, post, trigger and level changes: 0);
- no locks and no I/O;
- no resizing;
- the queue is fixed-capacity.

Buffer access stays within `[output, output + n)`. ASan and UBSan report
nothing across all 25 tests.

### 8.4 Tests: `tests/audio/engine_021.cpp`, 17 checks, all passing

All checks use synthetic samples and exact (1e-12) or bit-exact assertions.

| Requested item | Check |
|---|---|
| 1. Drum audio reaches the unified output | 01 |
| 2. Multiple pads mix correctly | 02 (pads 0, 1 and 26 sum exactly) |
| 3. Stereo channels remain correct | 03 (left-only stays left; equal channels stay bit-equal) |
| 4. Drum level | 04 (0.5 halves, 0 mutes, clamp to 2, NaN ignored) |
| 5. Master level | 05 (drum 0.5 × master 0.5 = 0.25: each applied once), 05b (full gain staging with velocity) |
| 6. Existing output accumulation | 06 (bus 0.1 + drums: mastered by `render`, unmastered by `mixSources`, kept when no voices) |
| 7. Queued events at the expected frame | 07 (silent at frame 36, sounding at frame 37) |
| 8. Variable block sizes | 08 (block sizes 1, 5, 64, 333, 1024 and 4096 bit-identical, 0 events dropped), 08b (an over-full queue drops 45 of 300 and counts them) |
| 9. Silence | 09 |
| 10. Choke and retrigger | 10a (open hat fades 0.25 → 0 through both gains), 10b |
| 11. No allocations | 11 |
| 12. Deterministic rendering | 12 |
| Clipping | 07b (only above full scale: 2.5 → 1.0, counted; nothing clipped at 0.5) |
| Invalid input | 07c (NaN/inf → 0 and counted; null and empty calls are safe) |

A first version of check 08 also used a 24,000-frame block. Posting one event
every 29 frames ahead of that single block puts 828 events into the
255-event queue, so 573 were dropped (confirmed separately). That is the
documented overflow behaviour, not a pipeline fault. The check now stays
within the queue's limit and asserts that nothing was dropped, and 08b tests
the overflow case explicitly.

Mutation checks, all caught:

- master applied twice (6 checks fail);
- drum level ignored (7 checks);
- `render` clearing the bus instead of adding to it (2 checks);
- drum level also scaling the existing bus audio (check 06).

### 8.5 Render demonstration: `render_engine_021_render`

`databreaks_engine_021_render` synthesises a small original kit from
deterministic formulas: kick, snare, two hi-hats, clap and tom. No external
samples are used.

It then:

- sequences two bars at 120 BPM: 28 events with velocities 55–127, posted through the queue at exact frame offsets;
- puts the closed and open hats in choke group 1; the closed hat chokes the open hat twice, and the last open hat rings out;
- pitches the tom down by 3 semitones;
- renders through `AudioPipeline` in 256-frame blocks, with drum level 0.8 and master 0.9;
- writes `databreaks_engine_021_demo.wav` into the working directory (the build directory under CTest). It is never committed (`.gitignore`).

The program checks its own result and exits non-zero on failure. The checks:

- the file is written;
- all samples are finite;
- the peak is between 0.05 and 1;
- 0 samples were clipped;
- both open-hat chokes happened;
- both channels carry signal;
- all 28 events were applied and none dropped.

Measured result:

- 240,000 frames (5.00 s), stereo, 48 kHz, 16-bit;
- peak 0.7294 (as written: 0.697 left, 0.729 right);
- RMS 0.1064 left, 0.1101 right;
- 0 clipped samples;
- 2 chokes;
- the file is byte-identical across runs (MD5 `8bd8fc8c65ccfa642e764b3371fce1f5`).

This is a software render on the development host, not an FM-1 recording, and
nobody has listened to it yet.

### 8.6 Remaining limitations

- Level changes (drum and master) apply instantly at the next frame, without smoothing, so a large change can produce a step (a "zipper" click). A per-frame ramp is a candidate for a later engine.
- The master stage is a hard clip, not a limiter. It engages only above full scale, and it is counted.
- There is no pan, no per-pad output routing, no effects and no sequencer. Synth layers are not routed through the pipeline yet (B4).
- At most 255 posted events per block are applied; extra events are dropped and counted, so producers must check `post*`'s return value.
- `render()` masters the whole buffer, so audio added by the caller before `render()` is mastered too. That is intended for a final bus; use `mixSources()` when mastering happens elsewhere.
- Everything is host-side `double` processing, and nothing has been run on an FM-1.

## 9. Engine 022: synthesizer integration

Files:

- new: `instruments/synthesizers/synth_voices.h`, `instruments/synthesizers/synth_engine.h`, `core/audio/spsc_ring.h`, `tests/audio/engine_022.cpp` (`unit`), `tests/audio/engine_022_render.cpp` (`render`);
- changed: `core/audio/audio_pipeline.h` (synth sources and synth event queue), `core/audio/drum_event_queue.h` (now an alias of the generic ring), `CMakeLists.txt`.

The original synth classes, `calculateEnvelope`, the mixers, tape, sampler,
drum bank and drum engine are **unchanged**. All earlier milestone tests pass
unchanged, the 16 reference WAVs are byte-identical, and the Engine 021 demo
still matches the committed WAV.

### 9.1 Why new stateful voices instead of editing the originals

`MonoSynth`, `FMSynth`, `PolySynth` and `calculateEnvelope` are what the
Engine 002–004 render demos and their reference WAVs depend on. Fixing them
in place would change those reference renders.

Engine 022 therefore adds stateful voices (`synth_voices.h`) that:

- reuse the same parameter structs (`MonoParameters`, `FMParameters`, `PolyParameters`, `ADSRParameters`);
- reuse the same formulas: sine plus sub-octave sine through the envelope-tracking one-pole low-pass (mono); two-operator FM with envelope-scaled depth; detuned sines with the original pan law (poly).

They replace two things:

- time-based phase with **phase accumulators** in cycles;
- the stateless envelope with a **stateful ADSR** (`AdsrEnvelope`).

For a note held past attack + decay and then released, the new voices
reproduce the originals sample for sample:

| Check | Comparison | Largest difference over 52,000 samples |
|---|---|---|
| E1 | `FmVoice` vs `FMSynth` | 2.6e-13 |
| E2 | `MonoVoice` vs `MonoSynth` | 9.5e-13 |
| E3 | `PolySynthEngine<4>` vs `PolySynth`, stereo | 3.6e-13 |

These differences are floating-point rounding between accumulated phase and
`f·t`.

### 9.2 Synth engines and performance controls

`SynthEngine<Voice, N, Mono>` (`synth_engine.h`) provides three engines:

| Engine | Voices | Notes |
|---|---|---|
| `MonoSynthEngine` | 1 | Last-note priority over a 16-note held stack. Releasing the sounding note returns to the most recent held note without retrigger; releasing the last held note starts the release |
| `FmSynthEngine<N = 4>` | 4 | Polyphonic. The original `FMSynth` was one voice; each FM voice sounds exactly like it |
| `PolySynthEngine<N = 8>` | 8 | Slot *i* of *N* panned to (*i*/(*N*−1) − 0.5) × width + 0.5, which is the original law for *N* = 4 |

Controls:

- `noteOn(note, velocity)`:
  - notes outside 0–127 are rejected;
  - velocity ≤ 0 means note-off (MIDI convention);
  - velocity above 127 is clamped;
  - gain = velocity / 127, so 127 is the original level.
- `noteOff`, `allNotesOff` (normal release) and `reset` (hard stop).
- `setPitchBend(semitones)`: ±24, non-finite values ignored.
- `setPitchBendMidi(0..16383, range)`: 8192 is centre.
- `setParameters`: non-finite values fall back to defaults, and ranges are clamped (`sanitized()`).
- `setLevel` (0–2).

Polyphonic voice allocation, in order:

1. the voice already playing this note;
2. the lowest idle voice;
3. the oldest releasing voice;
4. the oldest held voice.

Every retrigger or steal restarts the attack **from the current level** with
continuous phase.

### 9.3 Pipeline integration (no new mixer)

`AudioPipeline` gained three sources: `mono()`, `fm()` and `poly()`.
`mixSources()` now:

1. renders the drums over the whole block (`DrumEngine` applies its own queued events at their frames; unchanged);
2. renders the synths in segments between their queued events (`SynthEvent` in a fixed-capacity `SpscRing`, FIFO, offsets clamped to the block, 255 per block, overflow dropped and counted).

Each synth adds level × (sum of its voices) into the bus. Then `applyMaster()`
runs once, exactly as in Engine 021.

Each gain is applied once:

```
oscillator × params.volume (× voiceVolume for poly) × velocity/127 × envelope
           × source level (once, on the summed voices) ─► + bus ─► × master ─► clip
```

`LayeringMixer` stays as the per-frame mixer of the 003/004 demos only. The
drum queue's ring was generalised to `SpscRing<T, N>` so synth events reuse
the same tested code; `DrumEventQueue<N>` is now an alias.

Threading:

- direct synth calls are audio-thread only;
- synth `post*` may come from one producer thread;
- check T2 runs a real producer thread posting 20,000 synth events while the audio thread renders: every event is applied or counted, and ThreadSanitizer reports nothing.

### 9.4 Tests: `tests/audio/engine_022.cpp`, 29 checks, all passing

| Requested | Checks |
|---|---|
| FM / mono / poly rendering | F1, M1, P1, P2 (and E1–E3: identical to the originals) |
| Arbitrary block sizes | B4a (1/7/64/333/4096 = one 10 s block), A1 (drums + 3 synths through the pipeline: block sizes 1, 7, 64, 333, 4096 and 30,000 bit-identical) |
| Phase continuity | B4a, B4b (exact phase after 10 s: 1.6e-12) |
| Note-on / note-off | N1, M2, P3 |
| Velocity | V1 (64 gives exactly 64/127) |
| Drums + synths together | G1 (equals the sum of the separate renders, bit-exact) |
| Gain staging | G2 (level 0.5 × master 0.5 = 0.25), G3 (bus audio not rescaled) |
| Stereo | P1 (slot 0 hard left), F1/M1 (centred) |
| Silence afterwards | N1, S1 |
| Invalid parameters | I1 (NaN/inf params sanitised, bad notes rejected), I2 (invalid posts, queue overflow counted) |
| No audio-thread allocation | R1 (2,000 rounds of synth posts, notes, bends, drums and renders: 0) |
| Deterministic output | D1, A1 |
| Frame accuracy | T1 (drum at frame 37 and synth note at frame 100 in the same block) |
| Findings | B1, B2, B3, B2/B3, B4a, B4b (§4.1) |
| Pitch bend | B (+12 = one octave; MIDI 0/8192/16383 = −2/0/+2; clamping) |
| Threading | T2 |

Mutation checks. Every one was caught:

- B2 bug reintroduced (release ignored before sustain): 5 checks fail;
- B3 bug reintroduced (release from sustain): 2 checks;
- attack restarting from 0: 2 checks;
- B4 reintroduced (voice state reset on every note): check B1;
- synth event offsets ignored: T1, A1;
- synth level also scaling the bus: G3;
- releasing-first stealing removed: P3. This one was missed at first, because the releasing voice was also the oldest; P3 was rewritten so the two rules disagree, and it now catches the mutant.

### 9.5 Render demonstration: `render_engine_022_render`

The demo is an original 4-bar piece at 120 BPM:

- a synthesised drum kit, with the open hi-hat choked by the closed hi-hat;
- a mono bass on an eighth-note pulse with accents, sliding up 2 semitones over the last beat;
- an FM bell melody over bars 2–4;
- 8-voice poly pad chords, one per bar (Dm7, Bb maj7, F maj7/A, C(add9)/E).

All 128 synth events and 52 drum events go through the queues at exact frames
in 256-frame blocks.

Output: `databreaks_engine_022_demo.wav`, 9.50 s (456,000 frames), 48 kHz,
16-bit stereo PCM. It is written into the build directory under CTest and
ignored by git.

The program checks itself:

- peak 0.8896 (written: 0.8721 left, 0.8896 right), 0 clipped samples;
- RMS 0.1276 left, 0.1265 right;
- solo RMS: drums 0.0862, mono 0.0842, FM 0.0349, poly 0.0216;
- the full mix equals the sum of the four solo renders to within 3.3e-16;
- a second render is bit-identical;
- every event was applied and none dropped.

Independently checked with Python's `wave` module:

- the header is valid and the file size matches it;
- MD5 `0cfa5643791ee4e77fae3c8353ae5c72`, identical across four runs and the default, Release `-O3` and ASan builds.

It is a software render on the development host, not an FM-1 recording, and
nobody has listened to it yet.

### 9.6 Remaining limitations

- The original `MonoSynth`/`FMSynth`/`PolySynth`/`calculateEnvelope` still have findings B1–B4. They are kept only for the 002–004 reference demos.
- Synth level, master level and pitch-bend changes apply instantly (pitch bend from the next sample), so large jumps can step. There is no smoothing or portamento.
- Voices use per-sample `sin`/`exp` in `double`; target cost is unknown.
- There is no velocity curve beyond linear, no per-voice pan for mono/FM, no modulation or aftertouch, no sustain pedal, and no MIDI byte parsing or device driver (out of scope).
- Each queue is single-producer; the configuration and direct-call APIs are audio-thread only.
- 255 posted synth events per block at most; the rest are dropped and counted.
- Nothing has been run on an FM-1.

## 10. Engine 023: effects engine

Files:

- new, in `core/audio/effects/`: `audio_effect.h`, `equalizer.h`, `compressor.h`, `soft_clipper.h`, `distortion.h`, `reverb.h`, `effect_chain.h`;
- new tests: `tests/audio/engine_023.cpp` (`unit`), `tests/audio/engine_023_render.cpp` (`render`), `tests/audio/engine_023_benchmark.cpp` (built, not a test);
- changed: `core/audio/audio_pipeline.h` (effects stage), `CMakeLists.txt`.

No existing class changed behaviour. All earlier tests pass unchanged. The 16
reference WAVs, the committed Engine 021 demo and the Engine 022 demo (MD5
`0cfa5643…`) are byte-identical.

### 10.1 What existed before

There was no standalone EQ, compressor, distortion or reverb.
`TapeMachine::process()` contains tape-specific DSP that is private and fixed:

- a fixed-threshold (0.45) compressor;
- `tanh` saturation;
- one-pole tape filters.

It is not reusable as general effects. Engine 023 does not duplicate or
replace it; Engine 024 will bring the whole tape stage into the chain.

Signal flow before Engine 023:

```
mixSources (drums + mono/FM/poly, each × level) → applyMaster (× master, hard clip)
```

### 10.2 Architecture

```
render(out, n) = mixSources → applyEffects → applyMaster

applyEffects = EffectChain<8>, in place on the summed bus, in order:
  Equalizer → Compressor → SoftClipper → Distortion → Reverb
  (Engine 024: pipeline.effects().insert(position, &tape))
```

- **Common base.** `AudioEffect` is a small polymorphic interface: one virtual call per block. `EffectBase<Derived>` runs the per-sample `Derived::tick()` inline and applies:
  - wet/dry mix: exactly the input at 0, exactly the effect output at 1;
  - bypass with a 20 ms linear crossfade;
  - `reset()`.
- **Full bypass.** A fully bypassed effect returns immediately: no processing, bit-exact output. An effect leaving full bypass is reset, with its parameters and mix snapped to their set values, so stale state never bursts back in and output doesn't depend on history.
- **Defaults.** Every effect in the pipeline starts **fully bypassed**, which keeps every earlier render bit-identical. When an effect is enabled it uses the default settings listed in §10.3.
- **Gain staging.** Each source level is applied once, each effect once, in order, and the master once. Effects process the combined bus. Bypassed effects never touch it.
- **Event timing.** Drum and synth events are applied inside `mixSources()`, before the effects, so their frame accuracy is unchanged. Check PI5: silence through frame 36, sound at frame 37 with every effect enabled.
- **Smoothing.** Parameters that could click glide linearly over 20 ms per sample (`SmoothedValue`), so results don't depend on block size: gains, frequencies (in log space), Q, threshold, ratio, knee, makeup, drive, output levels, room size, decay, damping, wet, dry and width. Distortion mode changes crossfade over 20 ms.
- **Threading.** All effect parameters and `process()` run on **one** (audio) thread. There is no control queue for effect parameters yet, and the engine is not described as thread-safe. Drum and synth event queues are unchanged.
- **Real-time behaviour.** No allocation, locks or I/O in `process()` or any setter. All state, including reverb delay lines, is in fixed `std::array` members.

### 10.3 Effects, DSP and parameter ranges

Non-finite values are always ignored and out-of-range values clamped.

| Effect | DSP | Parameters (range, default) | State size (`sizeof`) |
|---|---|---|---|
| `Equalizer` | Low shelf → mid peak → high shelf → output gain, per channel. Biquads from the published Audio EQ Cookbook equations (implemented here), transposed direct form II. Coefficients are recomputed per sample only while gliding. A 0 dB band is skipped, so a flat EQ is bit-exact | Low 20–1000 Hz (120), mid 100–10000 Hz (1000), high 1000–18000 Hz (8000), each gain ±18 dB (0); mid Q 0.2–8 (0.7); output ±18 dB (0) | 632 B |
| `Compressor` | Stereo-linked peak detector; one-pole envelope with attack/release; soft-knee curve in dB; gain applied equally to both channels; makeup; mix for parallel compression; gain-reduction meter | Threshold −60–0 dBFS (−18), ratio 1–20 (4), knee 0–24 dB (6), attack 0.1–200 ms (10), release 10–2000 ms (120), makeup −12–+24 dB (0) | 304 B |
| `SoftClipper` | Linear for \|x\| ≤ knee; above it k + (c−k)·tanh((\|x\|−k)/(c−k)). Value and slope are continuous at the knee and the output never exceeds the ceiling. This is separate from the master's hard safety clip | Drive 0–24 dB (0), knee 0.1–0.99 × ceiling (0.7), ceiling 0.1–1.0 (1.0) | 184 B |
| `Distortion` | Soft: tanh. Asymmetric: 1−e^−x for the positive half, −(1−e^1.6x)/1.6 for the negative half (adds even harmonics). Fuzz: 4x + 0.2 bias, clipped, cubic corner, shifted so 0 maps to 0. Every shape is bounded and maps 0 to 0; a ~10 Hz DC blocker follows; modes crossfade over 20 ms | Mode (soft), drive 0–36 dB (12), output −36–+6 dB (−6) | 256 B |
| `Reverb` | Mid → DC blocker → 4 allpass diffusers; side → DC blocker; an 8-line feedback delay network (lengths 1427–3517 samples × room size) with a Householder matrix, per-line damping low-pass and RT60-derived gains; two output sign patterns for L/R; mid/side width; dry and wet levels. Contractive loop (orthogonal matrix, gains < 1), so it is stable for every setting. Values below 1e-25 are flushed, so tails end in exact silence | Room size 0.25–1.0 (0.7), decay RT60 0.2–10 s (1.8), damping 0–1 (0.4), wet 0–1 (0.3), dry 0–1 (1.0), width 0–1 (1.0) | 163,560 B |
| All | Common: bypass, mix 0–1 (1.0) | | |

The defaults apply once an effect is enabled.

**Memory.** The reverb's delay storage is fixed at 162,808 bytes: (19,446 tank
+ 905 diffuser samples) × 8 bytes. That would be about 81 KB if ported to
`float`. Everything else is under 1 KB per effect. The whole `AudioPipeline`
object grows by about 165 KB. The FM-1's RAM is undocumented, so this has not
been checked against any hardware budget.

### 10.4 Tests: `tests/audio/engine_023.cpp`, 48 checks, all passing

**EQ (EQ1–EQ7)**

- Flat EQ is bit-exact (EQ1).
- Measured response:
  - low shelf +12 dB → +12.0 dB at 30 Hz, ~0 at 5 kHz;
  - mid +9 dB → +9.0 dB at 1 kHz;
  - high shelf −12 dB → −12 dB at 18 kHz (±1 dB).
- Poles are inside the unit circle at every range corner.
- A +18 dB change glides without steps.
- Invalid values are handled.

**Compressor (CO1–CO6)**

- Static curve: exactly 9 dB of reduction at 12 dB over threshold (4:1), and a continuous knee.
- Sustained −6 dBFS tone: steady output peak within 0.5 dB of the expected −15 dBFS.
- Step input: gain reduction after one 10 ms attack within 0.1 dB of theory, above 8.9 dB after 50 ms.
- Release after 100 ms within 0.15 dB of theory.
- Parallel mix and makeup.
- Validation.

**Soft clipper (SC1–SC4)**

- Bit-exact below the knee.
- Monotonic and never above the ceiling up to |x| = 100.
- Slope 1 at the knee.
- Exact tanh values and slope 0.66 above the knee.
- +24 dB drive stays at or below 1.0.

**Distortion (DI1–DI5)**

- All modes bounded at +36 dB drive.
- Silence maps to exact silence.
- Soft has odd harmonics only (H2/H1 below 1e-6); asymmetric adds H2.
- DC removed on both channels (mean 6e-16).
- Mode switch crossfades without a step.

**Reverb (RV1–RV10)**

- Memory as documented.
- Impulse decays 32.7 dB in 0.5 s for RT60 = 1 s (theory: 30).
- Wet output starts after the shortest delay.
- Out-of-phase noise at maximum size/decay with no damping is stable for 60 s and decays 354 dB.
- Finished tail is exact silence.
- Constant DC input: no DC build-up (mean below 1e-4).
- Wet 0 is bit-exact; width 0 gives L = R.
- Size and decay changes glide.
- Validation.

**Common (BY1–BY3, MX1–MX2, RS1, NL1, CH1)**

- Bypass is bit-exact after the fade, and the fade has no jump.
- Mix 0 is exact; mix 0.5 is the exact midpoint.
- `reset()` makes an effect behave like a new instance.
- Null and empty buffers are safe.
- Chain insert/remove/capacity/indexOf work.

**Pipeline (PI1–PI6, AU1, RT1)**

- Chain order; all bypassed by default.
- Drums + mono + FM + poly through all five effects: changed, no clipping.
- Block sizes 1/7/64/333/4096 bit-identical; repeat renders bit-identical.
- Event timing intact.
- 60 s stable with a 10 s reverb.
- Per-block automation of every effect: largest step 0.25 or less.
- Zero allocations over 2,000 rounds of processing, parameter changes, mode switches, bypass toggles and events.

Mutation checks, all caught:

- bypass never completing;
- reverb loop gain above 1;
- attack/release swapped;
- EQ gain not smoothed;
- either DC blocker removed;
- silence flush removed;
- mix not snapped on enable;
- hard clip instead of soft;
- chain order changed.

Three of these first survived and the tests were strengthened:

- the right-channel DC blocker (DI4 now checks both channels);
- the hard clip (SC4 added);
- chain order (PI1 now checks every slot).

Issues found during development and fixed:

- The reverb originally fed only the mono sum, so out-of-phase input produced no reverb. The side signal is now injected too.
- An immediate enable left the mix gliding, which made repeated renders differ. Leaving full bypass now snaps the mix.

### 10.5 Render demonstration: `render_engine_023_render`

The demo is an original piece in six 2-bar sections at 120 BPM, 4 s each:

| Section | Content | RMS | Difference from the all-bypassed render |
|---|---|---|---|
| 1 | Dry drums | 0.0965 | none (bit-identical) |
| 2 | Compressed drums | 0.0671 | −4.0 dB |
| 3 | Distorted drums (asymmetric, then fuzz) | 0.1451 | −7.2 dB |
| 4 | FM melody with reverb | 0.0848 | −2.7 dB |
| 5 | Mono bass with EQ | 0.1748 | −4.3 dB |
| 6 | Poly chords and drums through the complete chain | 0.1940 | −4.4 dB |

The difference column is relative to each section's level.

Output: `databreaks_engine_023_demo.wav`, 26.0 s, 48 kHz, 16-bit stereo PCM.
It is written into the build directory and not committed. All 202 events are
applied and none dropped; peak −1.85 dBFS; 0 clipped samples.

The program checks that the dry section is bit-identical to the all-bypassed
render, that every effect section differs, and that a second render is
bit-identical.

MD5 `9cbaecbb40c0b048e7d8dcb99565230d`, identical across runs and the default,
Release and ASan builds. The format was checked independently with Python's
`wave` module. It is a software render, not an FM-1 recording, and nobody has
listened to it yet.

### 10.6 Benchmark (host only: 4-vCPU Xeon @ 2.1 GHz virtual machine, GCC 13.3)

`databreaks_engine_023_benchmark`, 10 s of stereo audio per row, CPU time via
`std::clock`, Release `-O3`:

| Processing | ns per frame | Real-time factor |
|---|---|---|
| Equalizer (3 bands active) | 20.8 | 1003× |
| Compressor | 44.7 | 466× |
| SoftClipper | 13.0 | 1604× |
| Distortion soft / asymmetric / fuzz | 85.8 / 65.2 / 50.2 | 243× / 320× / 415× |
| Reverb | 96.5 | 216× |
| Full chain, block 16 / 128 / 1024 | 159 / 146 / 137 | 131× / 143× / 152× |
| Full chain, all bypassed | 0.1 | — |
| Complete pipeline (drums + 3 synths + master), effects bypassed / on | 185 / 331 | 113× / 63× |

All rows: 0 allocations.

The unoptimised default build is about 3–5 times slower: full chain 29×, and
the complete pipeline with effects 15.7×. Repeated runs on this shared
machine vary by up to about 20%.

These are measurements of a desktop-class host. They say nothing about
whether the FM-1 can run this chain in real time.

### 10.7 Remaining limitations

- There is no control queue for effect parameters (audio thread only), and no automation or sequencer yet. The per-sample smoothing is the foundation for both.
- There is no oversampling: high distortion drive aliases. EQ shelves are fixed at slope 1; there are no extra bands.
- The compressor has no sidechain, lookahead or RMS detection.
- The reverb has no pre-delay or modulation, and its delay memory is fixed at its maximum size.
- Room-size glides briefly bend the pitch of the tail.
- All processing is `double`. Cost and memory on the FM-1 are unknown.
- Tape is not yet in the chain (Engine 024).
- Nothing has been listened to, and nothing has been run on an FM-1.

## 11. Engine 024: tape in the effects chain

Files:

- new: `core/audio/effects/tape_effect.h`, `tests/audio/engine_024.cpp` (`unit`), `tests/audio/engine_024_render.cpp` (`render`), `tests/audio/engine_024_benchmark.cpp` (built, not a test);
- changed:
  - `core/audio/effects/audio_effect.h`: a latency hook (`HasLatency` / `alignedDry()`, `latencyFrames()`);
  - `core/audio/audio_pipeline.h`: tape inserted into the chain;
  - `core/audio/tape_machine.h`: `reset()` also restarts the hiss generator;
  - `tests/audio/engine_023.cpp` and `tests/audio/engine_023_benchmark.cpp` (see §11.6);
  - `CMakeLists.txt`.

### 11.1 Audit of the existing tape code

- **`TapeMachine`** (`tape_machine.h`) is the tape character processor.
  - It runs per sample: a 12 ms (576-frame) delay line modulated by wow (0.35 Hz) and flutter (6.3 Hz); a one-pole low-pass (cutoff set by speed, type, bias and age); a 110 Hz head-bump filter; a fixed-threshold (0.45) compressor; tanh saturation normalised by tanh(drive); hiss (xorshift noise); print-through from 0.18 s earlier; crosstalk; and a hard limit to ±1.
  - All twelve requested controls exist. Ranges are 0–1, plus speed (7.5/15/30 IPS) and type (ferric/chrome/metal).
  - It has no getters.
  - Its setters clamp with `std::clamp`, which lets NaN through.
  - It allocates four `std::vector` buffers in its constructor (268,800 bytes).
  - Its `reset()` cleared all state except the hiss generator.
- **`TapeProcessor`** is a thin block wrapper around `TapeMachine`.
- **`TapeTransport`, `TapeStreamTransport` and `TapeStreamingEngine`** play an attached recording at a tape speed with wow/flutter. They are sources, not insert effects, so they are not part of the effects chain.

### 11.2 Adapter architecture: `TapeEffect`

- **Ownership.** It derives from `EffectBase<TapeEffect>` and holds `TapeMachine machine_` by value: no pointers, shared objects or temporaries. Its lifetime is the pipeline's lifetime.
- **Processing.** `tick()` calls `machine_.process()` for every sample, so the wet signal is the unchanged tape DSP. Check A1 compares it with a separately driven `TapeMachine`, bit for bit, with every control active.
- **State.** The delay position, wow/flutter phases, filters, compressor envelope, print-through buffer and hiss generator all live in the machine. They carry across blocks; nothing resets at a block boundary. Check A2: blocks of 1/7/64/333/4096 are bit-identical.
- **Validation.** Non-finite values are ignored, then values are clamped to 0–1 before reaching the machine.
- **Smoothing.** The ten continuous controls glide over 20 ms and are applied to the machine per sample while gliding. Without this, a wow change moves the delay read position at once: a raw jump of 0.12 in check A11, against 0.026 with the glide, equal to the steady waveform.
- **Speed and type switching.** Applied through a duck: the tape signal glides into the aligned dry signal over 5 ms, the switch happens there, and it glides back over 5 ms. On a slow 2 Hz test tone, the raw machine steps 0.13 in one sample at saturation 0.8, against 0.002 with the duck (A12). The raw jump was measured up to 0.21 (about 20% of the signal) on a 300 Hz tone. Speed alone at saturation 0 steps only about 0.01, but every switch uses the duck. When fully bypassed or on `reset()`, switches apply at once; setting the current value does nothing.
- **Latency and wet/dry.** `TapeMachine` delays by 576 frames, so `TapeEffect` sets `HasLatency`. The shared mix then blends a 576-frame-delayed dry signal with the wet one (A3: mix 0 is the input delayed by exactly 576 frames; A5: mix 0.5 is the exact midpoint), avoiding comb filtering. The bypass crossfade goes to the undelayed input, and full bypass is bit-exact and undelayed (A6). The other five effects compile the original Engine 023 mixing code unchanged, and the Engine 023 demo is byte-identical.
- **Reset.** `resetState()` resets the machine, the dry line and the glides. With the one-line `TapeMachine::reset()` fix (it now also restores the hiss seed), a reset tape equals a new one, hiss included (A9). No existing test or render relied on the old behaviour: all tape tests pass and all tape reference WAVs are byte-identical.

### 11.3 Controls and ranges

These are `TapeMachine`'s own ranges, unchanged.

| Control | Range | Default | What it does in `TapeMachine` |
|---|---|---|---|
| Speed | 7.5 / 15 / 30 IPS | 15 | Low-pass cutoff 7 / 12 / 18 kHz; hiss 0.0009 / 0.00065 / 0.0004 |
| Type | Ferric / chrome / metal | Ferric | Cutoff × 0.78 / 1.0 / 1.12; hiss × 1.35 / 0.85 / 0.65; drive × 6 / 4.5 / 3.5 |
| Wow | 0–1 | 0 | Delay modulation ±1.5 ms at 0.35 Hz |
| Flutter | 0–1 | 0 | Delay modulation ±0.25 ms at 6.3 Hz |
| Hiss | 0–1 | 0 | Noise level × speed × type × (1 + 4 × age) |
| Saturation | 0–1 | 0 | Drive 1 + saturation × type × bias × (1 + age/2), then tanh(x × drive) / tanh(drive) |
| Bias | 0–1 | 0.5 | Cutoff × (0.85 + 0.15 bias); drive × (1.15 − 0.3 bias) |
| Compression | 0–1 | 0 | Above an envelope of 0.45, ratio 1 + 3 × compression (5 ms / 120 ms) |
| Head bump | 0–1 | 0 | Adds 18% × a 110 Hz low-pass band |
| Crosstalk | 0–1 | 0 | Up to 20% of the opposite channel |
| Print-through | 0–1 | 0 | Adds 12% × the input from 0.18 s earlier |
| Age | 0–1 | 0 | Cutoff × (1 − 0.45 age); more hiss and drive |

Behaviour to be aware of (existing, preserved and documented):

- Saturation raises small-signal gain by about drive / tanh(drive): +2.4 dB even at saturation 0, and more as saturation rises.
- The output is hard-limited to ±1.
- There is no output trim.

### 11.4 Chain

```
render = mixSources → EQ → Compressor → SoftClipper → Tape → Distortion → Reverb → master
```

Tape is inserted through `EffectChain::insert(indexOf(&softClipper) + 1, &tape)`.
The Engine 023 order of the other effects is unchanged.

- Tape starts fully bypassed, so every earlier render is unchanged. It is enabled, disabled and configured independently through `pipeline.tape()`.
- Master processing happens once.
- Events still apply before the effects. With tape enabled, the whole bus is delayed by 576 frames: a drum at frame 37 leaves the chain at frame 613 (P6).

### 11.5 Tests: `tests/audio/engine_024.cpp`, 35 checks, all passing

**Adapter (A1–A12)**

- Bit-exact with `TapeMachine`.
- Block-size invariance.
- 576-frame alignment and arrival.
- Wet/dry midpoint.
- Bypass exact and click-free.
- 50 bypass/enable cycles.
- Reset equals new.
- Validation.
- Wow/flutter glides.
- Speed/type duck.

**Character (C1–C16), each measured on the output**

- Speeds: 10 kHz level 7.5 < 15 < 30 IPS; slower tape is noisier.
- Types: high end ferric < chrome < metal; hiss ferric > chrome > metal; saturation (H3/H1) ferric > chrome > metal.
- Wow and flutter each modulate, alone and together.
- Saturation raises H3/H1 monotonically.
- Hiss: 0 is exact silence, and noise grows with the control, with no DC.
- Bias: more high end, less distortion.
- Compression lowers a loud tone.
- Head bump lifts 80 Hz.
- Crosstalk: none at 0; a 0.25 right/left ratio at 1 (0.2 / 0.8).
- Print-through echo appears 0.18 s later.
- Aging: duller and noisier.
- Every control at maximum on a full-scale tone stays finite and never above 1.0.
- 5 minutes of streaming: finite, at most 1.0, no DC.

**Pipeline (P1–P7)**

- Chain order, and tape bypassed by default.
- Tape alone in the chain equals `TapeEffect` over the summed bus, with the master applied once.
- Drums + mono + FM + poly through tape: finite, changed, block sizes 1/7/64/333/4096 bit-identical, repeat renders identical.
- Event timing (37 + 576).
- Zero allocations over 2,000 rounds of tape processing, control changes, speed/type switches, bypass cycles and resets.

**Existing tape regression tests** (all pass):

- `tape_test`, `transport_test`, `tape_character_test`;
- Engines 011–013;
- the renders of Engines 005–010 and 014, whose WAVs are byte-identical.

**Mutation checks, all caught:**

- dry not latency-aligned;
- controls not smoothed;
- switch without duck;
- reset not resetting the machine;
- NaN reaching the machine;
- the reset-seed fix reverted;
- tape at the end of the chain;
- tape not calling the machine.

### 11.6 Intentional changes to Engine 023 files

Adding a sixth slot changed two Engine 023 assumptions:

- `engine_023`'s PI1 now expects the six-slot order.
- Its `enableAll`/`disableAll` helpers (and the Engine 023 benchmark's section C) skip the slot named "Tape", so those checks keep measuring exactly the five Engine 023 effects.

No Engine 023 check was loosened. The Engine 023 render already bypasses every
slot and enables effects by name, so its output is byte-identical.

### 11.7 Render demonstration: `render_engine_024_render`

The demo is an original piece in seven 2-bar sections at 120 BPM, 4 s each:

| Section | Content | Brightness |
|---|---|---|
| 1 | Dry drums | 0.280 |
| 2 | Ferric, 15 IPS | 0.178 |
| 3 | Chrome | 0.204 |
| 4 | Metal | 0.216 |
| 5 | 7.5 then 30 IPS (drums + FM, same bar twice) | 0.241 at 7.5 IPS, 0.314 at 30 IPS |
| 6 | Poly chords with wow 1.0 / flutter 0.8 | |
| 7 | Full mix: drums, mono, FM, poly through EQ, compressor, tape, reverb | |

Brightness is the first-difference RMS divided by the RMS, measured on identical material.

Output: `databreaks_engine_024_demo.wav`, 30.0 s, 48 kHz, 16-bit stereo PCM.
It is written into the build directory and not committed. All 264 events are
applied and none dropped; peak −4.56 dBFS; 0 master-clipped samples.

The program checks that:

- section 1 is bit-identical to a render with tape bypassed, and every tape section differs from it;
- brightness orders ferric < chrome < metal and 7.5 < 30 IPS;
- a second render is bit-identical.

MD5 `1bd372c97763ecdee45d71108c440dfa`, identical across runs and the default,
Release and ASan builds. The format was checked independently with Python's
`wave` module.

An earlier version used tape saturation 0.7 and clipped the master
149 times, because of the tape's gain (§11.3). The demo now uses moderate
saturation and master 0.6.

It is a software render, not an FM-1 recording, and nobody has listened to
it yet.

### 11.8 Memory and performance (host only)

**Memory**

- `TapeMachine` heap buffers: 268,800 bytes (2 × 4,800 + 2 × 12,000 doubles), allocated once when the pipeline is constructed.
- `TapeEffect` object: 10,080 bytes, including the 9,216-byte aligned dry line.
- `AudioPipeline<16>` object: 198,968 bytes, plus the tape heap buffers.
- The FM-1's RAM budget is unknown, so nothing has been checked against it.

**Performance** (`databreaks_engine_024_benchmark`, Release `-O3`, 4-vCPU Xeon @ 2.1 GHz virtual machine, 10 s of audio per row, CPU time; repeated runs vary by up to about 20%):

| Processing | ns per frame | Real-time factor |
|---|---|---|
| Raw `TapeMachine::process` | 91.8 | 227× |
| `TapeEffect`, defaults | 101.6 | 205× |
| `TapeEffect` at 7.5 / 15 / 30 IPS | 99.5 / 101.2 / 101.9 | 209× / 206× / 205× |
| `TapeEffect`, wow + flutter | 99.9 | 209× |
| `TapeEffect`, every control active (block 16 / 128 / 1024) | 112.4 / 110.8 / 111.2 | about 186× |
| `TapeEffect`, a control automated every block | 114.7 | 182× |
| Full chain with tape, block 16 / 128 / 1024 | 322 / 258 / 260 | 65× / 81× / 80× |
| Pipeline (drums + 3 synths): all bypassed / tape only / full chain | 214 / 325 / 481 | 97× / 64× / 43× |

All rows: 0 allocations. The adapter costs about 10% over the raw machine.
The unoptimised build is 2–3 times slower.

These are host numbers. They do not show that the FM-1 can run tape, or the
chain, in real time.

### 11.9 Remaining limitations

- A 12 ms latency whenever tape is enabled: by design in `TapeMachine`, and not compensated elsewhere in the pipeline.
- Wow/flutter make the wet signal's delay vary around 576 frames, so a partial mix has a slight moving comb (a chorus-like effect).
- No output trim for the saturation gain.
- The output hard-limit at ±1 sits inside the tape stage, before distortion and reverb.
- Hiss is white noise from a fixed seed (deterministic).
- Speed and type switches cost a 10 ms dip to dry.
- The transport/playback classes are not integrated as sources.
- `TapeMachine` allocates with `std::vector` at construction; a target port may want static buffers.
- All processing is `double`. Nothing has been listened to, and nothing has been run on an FM-1.

## 12. Engine 025: sequencer foundation

Files:

- new:
  - in `core/sequencer/`: `musical_time.h`, `musical_clock.h`, `pattern.h`, `sequencer.h`;
  - `core/audio/synth_event.h`: `SynthTarget`, `SynthEventType` and `SynthEvent`, moved unchanged from `audio_pipeline.h` to avoid an include cycle;
  - tests: `tests/audio/engine_025.cpp` (`unit`), `tests/audio/engine_025_render.cpp` (`render`), `tests/audio/engine_025_benchmark.cpp` (built, not a test);
- changed:
  - `core/audio/drum_engine.h`: scheduled events;
  - `core/audio/audio_pipeline.h`: scheduled synth events, and the sequencer driven from `mixSources()`;
  - `CMakeLists.txt`.

No earlier test was changed. The sequencer is stopped by default, and with
no scheduled events every path is the Engine 024 code. All 16 reference
WAVs, the committed Engine 021 demo and the Engine 022–024 demos are
byte-identical.

### 12.1 Audit findings that shaped the design

- **Drum and synth queues have one producer.** `DrumEngine::postTrigger` and `AudioPipeline::postNoteOn/Off` push into SPSC queues meant for one producer thread. A sequencer running on the audio thread cannot also push there without creating a second producer.
- **Queued offsets are clamped into the current block.** An offset past the block lands on its last frame; it is not deferred to a later block. Each queue holds 255 events per block.
- **Direct calls are not frame-accurate.** `trigger()` and `noteOn()` apply at the start of the next rendered range.

So a sequencer needed its own audio-thread path that applies events at
exact frames.

### 12.2 Musical time and the clock

- **Ticks.** A tick is 1/960 of a quarter note (PPQ 960 = 2⁶·3·5). Straight notes to 1/256, triplets and quintuplets are whole ticks: a 16th is 240, a 16th triplet 160, a 32nd 120.
- **Time signatures.** n/d with n 1–32 and d ∈ {1, 2, 4, 8, 16, 32}. Ticks per beat = 960 × 4 / d; ticks per bar = n × ticks per beat. Bar/beat/tick positions are 0-based.
- **Tempo.** An integer number of milli-BPM: BPM × 1000, 0.001 BPM resolution, range 20–300 BPM. Invalid values are rejected.
- **Sample rate.** 8000–192000 Hz is accepted. The pipeline runs at 48 kHz; the clock is tested at 44.1 and 48 kHz.
- **Conversion.**
  - With N = fs × 60000 and D = milliBPM × 960, samples per tick = N/D.
  - Within a tempo segment starting at tick T0, whose exact time is O/D samples: x(t) = (O + (t − T0)·N) / D.
  - Tick t is delivered at sample ceil(x(t)), the first sample at or after its exact time.
  - The inverse is `firstTickAtOrAfter(s)`.
  - All of this uses 128-bit integers, so there is **no per-tick rounding and no drift**.
- **Tempo changes.** A change takes effect at the first tick boundary at or after the current position. The new segment's origin is the old exact time, rounded once to the new 1/D grid: an error below 1/D of a sample, once per change.
- **Measured.** After 24 hours at 123.456 BPM and 44.1 kHz, tick 170,665,574 lands within one sample of its exact time and equals the independent formula. A naive running sum of samples-per-tick drifts 12.9 samples over the same span (CL4).
- **Transport.** `start()`, `stop()` and `reset()` (back to tick 0). The clock advances only while playing.

### 12.3 Pattern model

| Item | Limit |
|---|---|
| Drum lanes | 27, one per pad (index 0–26) |
| Steps per lane | `MaxSteps` 256 |
| Step size | 60–3840 ticks (1/64 note to whole note) |
| Length | 1–16 bars of the pattern's time signature, a whole number of steps, at most 256 steps |
| Drum step | enabled; velocity 1–127; probability 0–100 %; microtiming ±stepTicks/2 |
| Notes | up to `MaxNotes` 512 synth `NoteEvent`s (target, note 0–127, velocity 1–127, probability, start tick inside the pattern, length 1–61,440 ticks) |

A step is a grid position. A note is a MIDI-style event, and several notes
can start on the same tick. Invalid indices or values are rejected (`false` or
`-1`) and change nothing. `clear`, `clearPad`, `clearDrums` and `clearNotes`
keep the shape. Memory: 127,024 bytes per `Pattern`.

### 12.4 Sequencer and sample-accurate scheduling

`AudioPipeline::mixSources()` calls `sequencer().process(n, sink)` before
rendering. For the block [s0, s0 + n):

1. Queued transport commands are applied.
2. After `stop()` or `reset()`, every sounding sequenced note gets a note-off at offset 0.
3. While playing, the ticks [firstTickAtOrAfter(s0), firstTickAtOrAfter(s0 + n)) are covered:
   - drum step k of loop L on pad p fires at tick L·len + k·step + micro;
   - note i starts at L·len + start and ends `length` ticks later, tracked in a 256-entry active list, so note-offs cross loop boundaries correctly.
4. Events are sorted by **(sample offset, kind, target, pad/note)**. At one sample: note-offs first (a note can be re-struck), then drum triggers in pad order 0..26, then note-ons by target (mono, FM, poly) and note number.
5. Each event goes to `drums().scheduleTrigger()` or `scheduleNoteOn/Off()` at its exact frame.
6. The clock advances by n.

**Audio-thread scheduled paths.** `DrumEngine::scheduleTrigger/scheduleEvent`
and `AudioPipeline::scheduleNoteOn/scheduleNoteOff/scheduleAllNotesOff` are new
and audio-thread only:

- Each is a fixed list of 1,024 events per block, kept in frame order.
- Each is merged with its queue inside the same `processBlock` / `mixSynths` call, rendering up to each event's frame.
- At the same frame, scheduled events come first. Queued events keep their exact earlier rule, computed independently, which is why outputs without scheduled events are bit-identical.
- Offsets beyond the block clamp to the last frame and are counted. A full list rejects and counts.

**Probability.** A splitmix64 hash of (seed, loop, lane, step), not a running
generator, so the decision doesn't depend on block size or evaluation order.

- 0 never fires; 100 always fires (1,000 loops each).
- 50 fired 486 times in 1,000 loops, identically for any block size; a different seed gave 491.
- A skipped note-on also skips its note-off.

**Capacity and overflow.**

- Each block can hold 1,024 drum triggers and note-ons; extra ones are dropped and counted (`droppedEventCount`).
- Note-offs have reserved space and are never dropped.
- At most 256 sequenced notes can sound at once; extra note-ons are dropped and counted.
- Scheduled lists hold 1,024 per block; anything the instruments could not take is counted in `undeliveredSequencerEventCount()`.
- The transport queue holds 63 commands.
- No loop is unbounded: the work per block is bounded by the events in it and the note count.

**Threading.**

- `process()`, pattern editing, clock configuration and `start/stop/reset` are **audio thread only** (or while audio is stopped).
- `postTransport()` (start, stop, reset, set tempo) may be called from **one** other thread through an SPSC queue, applied at the next block.
- Drum and synth `post*` queues are unchanged: one producer each.
- Nothing allocates, locks or does I/O.

### 12.5 Tests: `tests/audio/engine_025.cpp`, 38 checks, all passing

**Clock (CL1–CL9)**

- Exact samples at 120 BPM.
- 44.1/48 kHz × 60, 90, 120, 140, 123.456, 97.531, 20 and 300 BPM against an independent exact formula, including one hour in.
- 30.625 samples/tick at 90 BPM / 44.1 kHz.
- 24-hour drift.
- `firstTickAtOrAfter` consistency.
- Bar/beat/tick round-trips in 4/4, 3/4, 7/8, 6/8, 5/4, 12/16 and 1/32.
- Start/stop/reset.
- Tempo change at the next tick boundary.
- Invalid input.

**Patterns (PA1–PA5)**

- Lengths and time signatures.
- Rejected shapes.
- Step validation at every boundary.
- 512-note capacity and order.
- Clear behaviour.

**Drums (DR1–DR5)**

- All 27 pads at exact samples over two loops.
- 27 simultaneous hits in pad order.
- Microtiming, including a negative offset crossing a loop boundary.
- Probability 0/100.
- Seeded probability, block-independent.

**Synths (SY1–SY5)**

- FM, mono and poly notes with exact on/off and durations.
- Overlapping poly notes.
- Note-offs across the pattern boundary, exact for 50 loops.
- Off before on at the same sample.
- `stop()` releases notes.

**Scheduler (SC1–SC6)**

- 1,196 events (7/8, 137.25 BPM, microtiming, probability, notes) identical for blocks 1, 16, 32, 64, 128, 256, 512, 1024 and 4096 (and 777, 1000).
- Events on block boundaries land at offset 0; events crossing a boundary land in the later block at the exact offset.
- An over-full block delivers 1,024, drops 56 and counts them.
- Scheduled-list capacity and invalid input.
- 10 minutes: 2,400 hits and the clock exactly at bar 300.
- Same-frame tie order (scheduled before queued), made audible through choke and mono last-note priority.

**Pipeline audio (AU1–AU6)**

- 27 one-frame markers on exactly their samples, and nothing else.
- Velocity 64 at 64/127 of the pad level.
- A sequenced choke fades over 4 frames.
- Sequenced synth notes are bit-identical to hand-scheduled ones.
- Manual `trigger()` / `postTrigger()` still work while the sequencer runs.
- Full pipeline audio (drums + 3 synths + all 6 effects) bit-identical for blocks 1, 16, 32, 64, 128, 256, 512, 1024 and 4096.

**Real-time (RT1–RT2)**

- 0 allocations over 3,000 blocks of dense sequencing with transport changes and all effects.
- Transport commands from a second thread: all applied or counted. ThreadSanitizer is clean.

**Mutation checks, all caught:**

- floor instead of ceil;
- tempo change not on a tick boundary;
- offsets ignored;
- on before off;
- order-dependent randomness;
- microtiming ignored;
- `stop()` not releasing notes;
- note-off one tick late;
- queued-before-scheduled ties, for drums and for synths.

The tie mutants were caught only after SC6 was added for this review. One
equivalent mutant: the `− maxMicro` margin on the first drum loop index is
redundant, because microtiming of at most half a step can never cross into
the previous loop.

### 12.6 Render demonstration: `render_engine_025_render`

The demo is an original 4-bar pattern at 118.5 BPM, played entirely by the
sequencer for 12 s:

- kick on pad 0, snare on pad 9, closed/open hats on pads 13/14 (choke group), rim on pad 26;
- swing (+40 ticks on off-16ths) and probability ghost notes (55 % hats, 35 % rim);
- a mono bass pulse, FM melody and poly chords;
- EQ, compressor, tape and reverb on.

It is rendered with blocks 1, 64, 256, 1000 and 4096: all five are
bit-identical.

- 257 events delivered, 0 dropped, 0 undelivered.
- Peak −3.49 dBFS, 0 clipped.
- Output: `databreaks_engine_025_demo.wav`, 12.0 s, 48 kHz, 16-bit stereo, not committed.
- MD5 `ac6d55d8eaa9e3807003406e74956166`, identical across runs and the default, Release and ASan builds.

It is a software render, not an FM-1 recording, and nobody has listened to it
yet.

### 12.7 Benchmark (host only: 4-vCPU Xeon @ 2.1 GHz virtual machine, GCC 13.3, Release `-O3`)

| Measurement | Result |
|---|---|
| `sampleOfTick` / `firstTickAtOrAfter` | 5.6 / 4.1 ns per call |
| Sequencer alone, empty pattern | 0.04–0.07 µs per block (16–4096 frames) |
| Sequencer alone, 27 pads on every 16th | 0.06–0.31 µs per block |
| Sequencer alone, 27 pads + 48 notes per bar | 0.09–0.49 µs per block |
| Maximum load: 1,024 events in one block | 20.3 µs per block (19.8 ns per event) |
| Complete pipeline with that dense sequence, effects bypassed | 233–271 ns/frame (77–89× real time) |
| Complete pipeline with that dense sequence, all 6 effects on | 483–529 ns/frame (39–43× real time) |

All rows: 0 allocations.

Memory: `Pattern` 127,024 bytes and `Sequencer` 169,168 bytes, so
`AudioPipeline<16>` grows from 198,968 to 433,744 bytes (plus 268,800 bytes of
tape heap buffers).

These are host numbers. FM-1 CPU and RAM budgets are unverified, and the
pattern storage in particular may need shrinking there.

### 12.8 Limitations and what remains for Engine 026

- One pattern per sequencer. There are no pattern banks, chaining, song mode, or pattern switching at loop boundaries.
- No recording, overdubbing, quantisation, automation lanes or swing templates (microtiming is per step).
- Editing patterns or the clock from another thread is not supported; only transport commands cross threads. A command queue for pattern edits is Engine 026 work.
- Changing the pattern length while playing remaps the loop position at once (no boundary-aligned change).
- Same-pitch overlapping notes on one target follow MIDI semantics: the first note-off releases all of them.
- No locate/seek, count-in, metronome or external sync (MIDI clock).
- Tempo changes are instantaneous at the next tick; no ramps.
- `__int128` needs a 64-bit GCC/Clang host; a 32-bit target port needs another exact representation.
- The audio engine runs at 48 kHz only; the clock supports other rates for when that changes.
- Nothing has been listened to, and nothing has been run on an FM-1.

## 13. Engine 026: recording and performance capture

Files:

- new:
  - `core/sequencer/recorded_track.h`: `RecordedEvent`, `RecordTargets`, `RecordedTrack` (fixed event store, passes, undo/redo);
  - `core/sequencer/recorder.h`: `LiveEvent`, `RecordMode`, `WriteMode`, `SequencerCommand`, `Recorder` (settings, state, limits);
  - tests: `tests/audio/engine_026.cpp` (`unit`), `tests/audio/engine_026_render.cpp` (`render`), `tests/audio/engine_026_benchmark.cpp` (built, not a test);
- changed:
  - `core/sequencer/musical_clock.h`: fine ticks (`FinePerTick`, `sampleOfFine`, `firstFineAtOrAfter`, `fineAtSample`), additions only;
  - `core/sequencer/sequencer.h`: recorder integration, command and live-input queues, recorded and live playback, a final `order` tie-break;
  - `CMakeLists.txt`.

The Engine 025 clock and scheduler were **not redesigned**: tick placement,
the event sort and the delivery paths are unchanged. Internally, sounding
notes are tracked in fine ticks (tick × 1024), which gives the same samples.
No earlier test was changed. With nothing recorded and no live input, every
path produces the Engine 025 output: all 16 reference WAVs, the committed
Engine 021 demo and the Engine 022–025 demos are byte-identical.

Recording captures **musical events, not audio**.

### 13.1 Event model and timestamps

A `RecordedEvent` is either a drum hit (pad 0–26, velocity) or a synth note
(target mono/FM/poly, note 0–127, velocity, length). Each event stores:

| Field | Meaning |
|---|---|
| `startFine` | loop-relative musical position in **fine ticks** (1/1024 tick = 1/983,040 quarter note), in [0, loop length) |
| `lengthFine` | note length in fine ticks (0 for drums) |
| `captureSample` | the absolute clock sample of the input (the input's own timestamp) |
| `captureFine`, `captureLoop`, `captureEpoch` | absolute fine position, loop index and timeline epoch at capture |
| `sequence` | capture order (ties) |
| `createdPass`, `erasedPass`, `pending` | undo bookkeeping; `pending` while a note is still held |

Three time bases are kept apart:

- **Absolute sample:** the clock sample since the last reset. Live input carries it (`LiveEvent::sampleTime`).
- **Musical tick:** absolute fine position `fineAtSample(s)`. This is the latest fine tick whose exact time is ≤ s. Fine spacing is below one sample for every supported rate and tempo (worst case 0.59 samples at 192 kHz, 20 BPM), so `sampleOfFine(fineAtSample(s)) == s`: a recording replays on exactly the captured sample in later loops at the same tempo. `fineAtSample(0) == 0`, and a sample on an exact tick time maps to that tick.
- **Loop-relative:** `startFine = fine − loop × loopLengthFine`.

**No quantisation.** Raw timing is stored unchanged. `nearestStep(stepTicks)`
(rounding half up) and `timingErrorFine(stepTicks)` (signed; positive = late)
give Engine 027 the grid step and the error without changing anything.

**Boundaries.** Start inclusive, end exclusive, everywhere: loops, punch
windows, Replace regions, blocks.

- A hit 1 sample before the loop end is stored at the end of that loop.
- A hit exactly on the loop end is position 0 of the next loop.
- Tested before, at and after the boundary in Free and Loop modes (B1–B3).

**Latency.** Nothing is compensated, silently or otherwise.

- An input is recorded at the sample it carries. The producer must stamp it with the clock sample it means; how a future pad-scanning layer derives that from its own latency is left for the hardware work and must be documented then.
- The output pipeline adds no latency except Tape's documented 12 ms when Tape is enabled. That delays what you hear, not where events are stored.
- An input whose timestamp is already in the past when its block starts is **late**: it sounds at offset 0, is **not** recorded and is counted (`lateLiveCount`). A late note-off still ends its held note at the block start.

### 13.2 Modes

| Mode | Behaviour |
|---|---|
| Free record | One pass from arm (or punch-in) until disarm, punch-out, stop or reset. It may span several loops. |
| Loop record | A new pass for every loop index; each loop is one undo step. A note held across the wrap belongs to the pass it started in. |
| Overdub | Adds events; erases nothing. |
| Replace | As the record head passes, visible events of the armed kinds (`RecordTargets`: drums/mono/FM/poly) in the covered loop region are marked erased by this pass. Events outside the region, or of other kinds, stay. The pass's own events are never erased. |
| Punch | Records only positions in [punchIn, punchOut) (absolute ticks; the punch-in tick's delivery sample is the first recorded). Playback continues throughout. The pass closes at punch-out. |
| Targets | Unarmed kinds still sound live but are not recorded. |

- **Lazy passes.** A pass opens when it first records or erases something, so idle loops create no empty undo steps.
- **Live input sounds once.** The live input plays at its own sample. Its recorded copy plays from the next loop onwards: an event is skipped only in the loop and epoch in which it was captured.
- **Reset.** `reset()` starts a new epoch, so the recording plays again in loop 0.
- **Ordering at one sample.** Engine 025's rule (note-off → drums by pad → note-ons by target and note), then pattern events, then recorded events (capture order), then live input (arrival order).
- **Transport stopped.** Live input plays immediately (offset 0) and is not recorded.
- **Stop, disarm, reset.** Held notes end at that position (no stuck notes); the pass closes.

### 13.3 Undo and redo

- Nothing is deleted while a pass can still be undone. `visible = !pending && created by an active (or permanent) pass && not erased by an active pass`.
- **Undo** deactivates the newest active pass: its events disappear and the events it erased reappear. This restores the previous state field by field (U1). Undo during recording closes the open pass and forgets its held notes.
- **Redo** reactivates the oldest undone pass.
- Recording a new pass after an undo discards the undone passes physically.
- **Depth:** 16 passes. Opening a 17th makes the oldest permanent ("flattened": its erased events are removed and its events kept).
- Undo and redo are bounded: O(events) per call, with a visibility refresh on the next block.

### 13.4 Ownership and threading (no audio-thread mutex)

- The **audio thread owns** the `Pattern`, the `RecordedTrack` and all recorder state. Only `process()` and direct calls made on the audio thread (or while audio is stopped) touch them.
- Other threads **never mutate** them. They send work through three SPSC queues, one producer thread per queue (the same thread may feed all three):
  - `postTransport()`: 63 transport commands;
  - `postCommand()`: 63 `SequencerCommand`s (arm, disarm, record/write mode, targets, punch, undo, redo, clear recording, and pattern edits: set/clear step, add/remove note, clear, configure);
  - `postLive()`: 255 `LiveEvent`s.
- Everything queued is applied at the start of the next block, in that order. A full queue rejects and counts.
- Live input moves into a fixed, time-sorted pending list (512) until its block comes.
- This is "staged commands": no snapshots, no locks, no allocation. The pattern-edit commands close the Engine 025 gap where patterns could only be edited on the audio thread.

### 13.5 Capacities and overflow

Fixed storage: `RecordedTrack` 182,576 bytes (2,048 × 88-byte events),
`Recorder` 201,144 bytes. `Sequencer` grows from 169,168 to 411,328 bytes,
and `AudioPipeline<16>` from 433,744 to 675,904 bytes.

| Limit | Value | On overflow |
|---|---|---|
| Events in the track | 2,048 | input dropped from the recording, counted (`trackFullCount`) |
| Events per pass | 512 | dropped, counted (`passFullCount`) |
| Undo depth | 16 passes | oldest pass flattened (made permanent), counted |
| Notes being recorded at once | 64 | note dropped from the recording, counted (`heldFullCount`) |
| Pending live input | 512 | rejected, counted (`pendingFullCount`) |
| Live / command / transport queue | 255 / 63 / 63 | rejected, counted |
| Triggers + note-ons per block (pattern + recording) | 1,024 | dropped, counted (`droppedEventCount`); note-offs and live input use reserved space |
| Sounding sequenced/recorded notes | 256 | note-on dropped, counted |
| Invalid live input | — | rejected, counted (`invalidLiveCount`) |

Policy: **drop and count; never stop.** Recording continues, an input that
does not fit is still played if it fits, and stored events are never
overwritten or corrupted (C1–C9 overflow every one of these limits).

### 13.6 Tests: `tests/audio/engine_026.cpp`, 55 checks, all passing

| Area | Checks |
|---|---|
| Fine ticks | T1 (5 rates × 6 tempos: fine/tick consistency, `fineAtSample` exact and replaying, independent formula), T2 (across a tempo change) |
| Drums | D1–D7: sound once while capturing; replay on the captured sample in every later loop; stored fields; raw timing with grid step and signed error; all 27 pads with velocities incl. 27 simultaneous hits; rapid (every 3 samples) and repeated hits |
| Boundaries | B1–B6: loop end −1 / exact / +1 in Free and Loop modes; punch in/out inclusive/exclusive; punch at 123.456 BPM |
| Synths | S1–S7: mono/FM/poly durations with overlaps and out-of-order note-offs; exact replay of note-on/off; a note crossing the loop end; notes held at disarm/stop; record targets and stray note-offs; note-offs matched by target |
| Modes | M1–M6: overdub; replace limited to the punched region and the armed kinds; replace as the head passes; undo of replace; loop record one pass per loop; lazy passes |
| Undo | U1–U7: exact restore and redo; redo discarded by a new pass; depth 16 with flattening; undo during recording; empty history; held notes dropped on undo |
| Block size | K1, K2: one scenario (free, loop, overdub, replace, punch, undo/redo, reset; pattern too) gives identical events and identical track contents for blocks 1, 16, 32, 64, 128, 256, 512, 1024, 4096 at 120 and 123.456 BPM |
| Ordering / latency / transport | O1–O5: pattern → recorded → live at one sample; late input; input while stopped; reset epoch; late note-off |
| Capacities | C1–C9 (above) |
| Pipeline | P1: a recorded loop through `DrumEngine` (choke group, simultaneous and repeated hits) renders bit-identical to the live loop; P2: nothing undelivered or clipped |
| Threads / RT | R1: live input and commands from a second thread while rendering (every item applied or counted; TSan clean); R2: 0 allocations through capture, loop record, replace, undo/redo, playback, stop and clear |

**Mutation checks** (each change made temporarily in a scratch copy):

| Mutation | Caught by |
|---|---|
| no capture-loop skip (double playback) | D1, D2, D6, D7, B5, M3, C1, P1 |
| earliest instead of latest fine tick per sample | D3, D4, B1, B2, B4, B6, S1 |
| Replace erases nothing | M2, M3 |
| punch-out inclusive | B4 |
| eager passes | B2, M6 |
| reversed final tie-break | O1 |
| no epoch on reset | O4 |
| note-off ignores the target | S7 |
| late note-off ignored | O5 |
| undo keeps held notes | U7 |

The last three survived the first version of the test file; S7, O5 and U7
were added for them.

**Harness fix found by ASan.** `std::stable_sort` in the test helper uses the
nothrow `operator new`, which the allocation-counting replacement did not
cover (alloc/dealloc mismatch). The tests and the benchmark now replace and
count the nothrow forms too. This was a test-harness bug, not an engine bug.

### 13.7 Render demonstration: `render_engine_026_render`

A 2-bar loop at 120 BPM (exactly 4 s), Loop-record mode, **empty pattern**:

| Loop | What happens |
|---|---|
| 0 | a humanised drum part (kick, snare, closed/open hats in a choke group) is played live and recorded (pass 1) |
| 1 | drums play **from the recording**; a mono bass line is overdubbed (pass 2) |
| 2 | FM melody and poly chords are overdubbed (pass 3) |
| 3 | everything plays from the recording |
| 4 | Replace, drums only, punched over bar 2: a new hat/shaker figure (pass 4) |
| 5 | plays the replaced version |
| 6 | the replace pass is undone (during bar 1) |
| 7 | plays the restored original |

- The live input comes from a deterministic "performer" (fixed pseudo-random timing offsets). Commands are issued at musically neutral moments.
- Blocks 1, 64, 256, 1000 and 4096 are bit-identical.
- 70 events recorded; 49 visible before the replace, 57 after it, 49 after the undo.
- 512 events delivered, 0 dropped, 0 undelivered, 0 late, 0 recorder overflow.
- Peak 0.376 (−8.5 dBFS), 0 clipped.
- Output: `databreaks_engine_026_demo.wav`, 32.0 s, 48 kHz, 16-bit stereo, **not committed**.
- MD5 `537ccfb584f357c84a7f23bceb860f3e`, identical in the default, Release and ASan builds.

It is a software render, not an FM-1 recording, and nobody has listened to it.

### 13.8 Benchmark (host only: 4-vCPU Xeon @ 2.1 GHz virtual machine, GCC 13.3, Release `-O3`)

Wall-clock time per `Sequencer::process()` call (audio thread). Worst-case
values are single-block maxima on a shared virtual machine and include
scheduling noise.

| Audio thread | Block 64: mean / worst | Block 256: mean / worst |
|---|---|---|
| Armed, nothing played | 0.15 / 42 µs | 0.10 / 0.8 µs |
| Capture, loop record, 480 hits/s + notes | 3.6 / 75 µs | 2.5 / 30 µs |
| Playback of a full track (2,048 events) | 2.2 / 30 µs | 2.2 / 18 µs |
| Overdub on 1,536 events + live input | 2.5 / 33 µs | 3.4 / 21 µs |
| Replace erasing a 1,536-event track | 2.3 / 94 µs | 2.9 / 26 µs |
| Dense: 27×16 pattern + 48 notes + track + input | 2.6 / 35 µs | 3.6 / 63 µs |
| Loop-wrap blocks (replace, loop record) | — | 3.1 / 6.2 µs |
| Undo/redo + next block (2,048 events) | 6.1 / 17 µs | |
| Commit: new pass on a full history (flattening) | worst 10 µs | |

| Control thread | Cost |
|---|---|
| `postLive` | ~63 ns per call (upper bound; includes occasional same-thread draining) |
| `postCommand` | ~14 ns per call |

All rows: 0 allocations. Playback scans the whole track once per block
(O(events)). That is cheap at 2,048 events on this host, but it is the first
thing to index if the FM-1 budget turns out to be tight. These are host
numbers; FM-1 CPU and RAM budgets are unverified.

### 13.9 Limitations and what remains

- One recorded track per sequencer, alongside the one pattern. Recordings are not converted into pattern steps (that would be a quantise/commit step: Engine 027).
- Recorded events replay at their exact sample only at the capture tempo. After a tempo change they keep their musical (fine-tick) position.
- A note still held when its loop position comes round again is not played back until it has ended (its length is unknown).
- Same-pitch overlapping notes on one target: note-offs end the earliest held note (FIFO); synth playback follows Engine 025's MIDI semantics.
- Live input pending across a `reset()` keeps its old absolute timestamp. Producers must stamp input against the current timeline.
- Changing the pattern length while recorded material exists keeps loop-relative positions; events beyond the new length are not played.
- No input-latency model: whatever produces `LiveEvent`s must provide correct timestamps (§13.1).
- No count-in, metronome, pre-roll or automatic punch looping.
- Memory: +242 KB per sequencer. This may be too much for the FM-1, whose RAM is undocumented.
- Nothing has been listened to, and nothing has been run on an FM-1.

## 14. Recommended next steps (in order)

1. Review Engines 019–026 (branch `claude/engine-019-prep-transfer`), and listen to `databreaks_engine_021_demo.wav` and the Engine 022–026 demo renders.
2. Decide on R3 (restore the original Engine 003 or not). Deliberately deferred again in Engine 020.
3. Add a LICENSE (the maintainer's choice) and a README, and remove the stray empty file (R6).
4. ~~Engine 024: tape processor as an `AudioEffect` inserted into the chain.~~ Done (§11). Possible follow-ups:
   - an output trim for tape saturation gain;
   - optional latency compensation for parallel paths;
   - the tape transport as a playback source.
5. Later candidates:
   - a control/automation queue for effect parameters;
   - per-frame smoothing for source levels and pitch bend;
   - per-pad / per-voice pan;
   - velocity curves;
   - reduce drum `trigger()` cost: the choke scan is O(27 × voices) (§7.8);
   - ~~a step sequencer that posts drum and synth events~~ (Engine 025, §12).
6. ~~Engine 026: live recording, overdubbing, a cross-thread command queue for pattern edits~~ (§13).
7. Engine 027 (quantisation), building on §13.1:
   - quantise recorded events to a grid (nearest step, strength, swing) using `nearestStep` / `timingErrorFine`, non-destructively and undoably, keeping the original timing;
   - optionally commit recorded drums into pattern steps (with microtiming from the signed error);
   - decide what quantisation does to note lengths and to events near the loop end.
8. Later sequencer candidates (§12.8, §13.9):
   - pattern banks and chaining with switching at loop boundaries;
   - automation lanes;
   - locate/count-in/metronome;
   - an indexed recorded-track playback if the FM-1 budget needs it.
9. ~~Fix B2/B3 with a stateful ADSR, then B1/B4 with phase accumulators.~~ Done in Engine 022 for the new engines (§4.1); the original classes stay as references.
10. Hardware discovery as documentation only (`docs/hardware/`, with sources). No firmware layer and no flashing until there is a documented, tested recovery path.

## 15. Change log on this branch

- **`e54db9b`** (relative to `e75b325`):
  - ported `CLAUDE.md`, this report and `tests/unit/dsp_unit_tests.cpp` from `eb93203`;
  - `CMakeLists.txt`: added the Engine 016–018 targets, the unit-test target and 21 labelled CTest entries.

  No DSP code was changed.
- **Engine 019 commit:**
  - added `core/audio/drum_engine.h` and `tests/audio/engine_019.cpp`;
  - extended `core/audio/sample_voice_manager.h` (additions only, plus the NaN-volume guard);
  - `CMakeLists.txt`: added `databreaks_engine_019` to the targets, the warning loop and the `unit` tests;
  - updated `CLAUDE.md` and this report.

  No synth, tape, `SamplePlayer`, `SampleSliceMap` or `DrumPadBank` code changed.
- **Engine 020 commit:**
  - added `core/audio/drum_event_queue.h`, `tests/audio/engine_020.cpp` and `tests/audio/engine_020_benchmark.cpp`;
  - extended `core/audio/sample_voice_manager.h` (queued sounds, sound ids);
  - extended `core/audio/drum_engine.h` (stealing, queue, statistics, bounds guard);
  - `CMakeLists.txt`: added the `databreaks_engine_020` and `databreaks_engine_020_benchmark` targets, `Threads::Threads` for `engine_020`, the warning loop and the `unit` test entry;
  - updated `CLAUDE.md` and this report.

  No synth, tape, `SamplePlayer`, `SampleSliceMap` or `DrumPadBank` code changed. Engine 003 was not touched.
- **Engine 021 commit:**
  - added `core/audio/audio_pipeline.h`, `tests/audio/engine_021.cpp`, `tests/audio/engine_021_render.cpp` and `.gitignore`;
  - `core/audio/sample_voice_manager.h`: output gain;
  - `core/audio/drum_engine.h`: `setOutputLevel` / `outputLevel`;
  - `CMakeLists.txt`: two targets, the warning loop, one `unit` and one `render` test entry;
  - updated `CLAUDE.md` and this report.

  No synth, tape, `SamplePlayer`, `SampleSliceMap`, `DrumPadBank` or `StereoMixer` code changed. Engine 003 was not touched.
- **`e4d796c`:** added `databreaks_engine_021_demo.wav` (force-added on request; no source changes).
- **Engine 022 commit:**
  - added `instruments/synthesizers/synth_voices.h`, `instruments/synthesizers/synth_engine.h`, `core/audio/spsc_ring.h`, `tests/audio/engine_022.cpp` and `tests/audio/engine_022_render.cpp`;
  - `core/audio/audio_pipeline.h`: synth sources, levels and the synth event queue;
  - `core/audio/drum_event_queue.h`: now an alias of `SpscRing`;
  - `CMakeLists.txt`: two targets, `Threads::Threads` for `engine_022`, the warning loop, one `unit` and one `render` test entry;
  - updated `CLAUDE.md` and this report.

  The original synth classes, `calculateEnvelope`, `LayeringMixer`, `StereoMixer`, tape, `SamplePlayer`, `SampleSliceMap`, `DrumPadBank` and `DrumEngine` were not changed. Engine 003 was not touched.
- **Engine 023 commit:**
  - added `core/audio/effects/` (`audio_effect.h`, `equalizer.h`, `compressor.h`, `soft_clipper.h`, `distortion.h`, `reverb.h`, `effect_chain.h`), `tests/audio/engine_023.cpp`, `tests/audio/engine_023_render.cpp` and `tests/audio/engine_023_benchmark.cpp`;
  - `core/audio/audio_pipeline.h`: effects chain stage (all bypassed by default);
  - `CMakeLists.txt`: three targets, the warning loop, one `unit` and one `render` test entry;
  - updated `CLAUDE.md` and this report.

  No synth, drum, tape, sampler, mixer or queue code changed. Engine 003 was not touched.
- **Engine 024 commit:**
  - added `core/audio/effects/tape_effect.h`, `tests/audio/engine_024.cpp`, `tests/audio/engine_024_render.cpp` and `tests/audio/engine_024_benchmark.cpp`;
  - `core/audio/effects/audio_effect.h`: a latency hook (other effects compile the unchanged Engine 023 code);
  - `core/audio/audio_pipeline.h`: tape inserted after the soft clipper (bypassed by default), and `tape()`;
  - `core/audio/tape_machine.h`: `reset()` also restores the hiss seed;
  - `tests/audio/engine_023.cpp` and `tests/audio/engine_023_benchmark.cpp`: six-slot order, and Tape skipped in the Engine 023 helpers (§11.6);
  - `CMakeLists.txt`: three targets, the warning loop, one `unit` and one `render` test entry;
  - updated `CLAUDE.md` and this report.

  The tape DSP, synth, drum, sampler, mixer and queue code were not changed. Engine 003 was not touched.
- **Engine 025 commit:**
  - added `core/sequencer/` (`musical_time.h`, `musical_clock.h`, `pattern.h`, `sequencer.h`), `core/audio/synth_event.h` (synth event types moved unchanged out of `audio_pipeline.h`), `tests/audio/engine_025.cpp`, `tests/audio/engine_025_render.cpp` and `tests/audio/engine_025_benchmark.cpp`;
  - `core/audio/drum_engine.h`: audio-thread scheduled events merged with the queue in frame order;
  - `core/audio/audio_pipeline.h`: scheduled synth events, and the sequencer (stopped by default) driven from `mixSources()`;
  - `CMakeLists.txt`: three targets, `Threads::Threads` for `engine_025`, the warning loop, one `unit` and one `render` test entry;
  - updated `CLAUDE.md` and this report.

  No earlier test was changed. Synth, effect, tape, sampler and mixer DSP were not touched. Engine 003 was not touched.
- **Engine 026 commit:**
  - added `core/sequencer/recorded_track.h`, `core/sequencer/recorder.h`, `tests/audio/engine_026.cpp`, `tests/audio/engine_026_render.cpp` and `tests/audio/engine_026_benchmark.cpp`;
  - `core/sequencer/musical_clock.h`: fine ticks (additions only);
  - `core/sequencer/sequencer.h`: recording, recorded and live playback, command and live-input queues;
  - `CMakeLists.txt`: three targets, `Threads::Threads` for `engine_026`, the warning loop, one `unit` and one `render` test entry;
  - updated `CLAUDE.md` and this report.

  No earlier test was changed. The Engine 025 clock and scheduling rules, and synth, drum, effect, tape, sampler and mixer code, were not changed. Engine 003 was not touched.
