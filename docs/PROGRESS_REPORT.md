# DATABREAKs progress report

Updated 2026-11-18 for branch `claude/engine-019-prep-transfer`, based on
`recovery/codespaces-engine-018` @ `e75b325` ("Recover audio engine development
through Engine 018").

History: the first version of this report (commit `eb93203`, branch
`claude/engine-019-prep`) was written against `90e8dfb`, which had only the
synth prototypes. At that time the sample player, slicing, voice manager and
27-pad drum bank were **not in the repository**. They have since been
recovered in `e75b325`, and this report has been updated to match.

## 1. Summary

The repository now contains host-side C++17 prototypes through **Engine 018**:

- synths (mono, FM, poly) and two mixers;
- a tape-emulation chain (Engines 005–014);
- a **sample player** (Engine 015);
- a **sample slice map** (Engine 016);
- a **multi-voice sample manager** (Engine 017);
- a **27-pad drum bank with choke-group logic** (Engine 018).

Everything builds and every test passes on the host.

The drum bank and the voice manager are **not yet connected**. Pads produce
trigger events and choke decisions, but nothing turns those into sounding or
silenced voices. That connection is **Engine 019**.

There is **still no FM-1 firmware**: no target toolchain, HAL, drivers, display,
pad scanning, storage or boot code. Nothing here can be installed on the device.

## 2. What was verified (this branch)

Environment: Linux, GCC 13.3.0, CMake 3.28.3. Clean build directory.

| Check | Result |
|---|---|
| Configure + build | 21 executables, **0 warnings** (`-Wall -Wextra -Wpedantic`) |
| `ctest` (all) | **21/21 pass** |
| `ctest -L unit` | **11/11 pass**: `dsp_unit_tests` (35 assertions), Engines 011, 012, 013, 015, 016 (30), 017 (22), 018 (28), `tape_test`, `transport_test`, `tape_character_test` |
| `ctest -L render` | **10/10 pass**: Engine 002 (`audio_test`), 003–010, 014. These only show that each program runs to completion and writes its WAV files |
| Recovered branch as pushed (before my changes) | Built and ran all 17 of its targets: every one exited 0 |
| Rendered audio analysis | Re-measured the Poly chord-boundary click (B1) on the new Engine 003/004 renders: still present |
| Listening test | **Not done** (no audio playback in this environment) |
| Hardware | **Not done, deliberately**: nothing flashed or connected |

## 3. Component status

| Component | Engine | Status | Notes |
|---|---|---|---|
| `audio_engine.h`, `StereoMixer` | 001–003 | Working prototype | Unit-tested. `StereoMixer` is no longer used by any demo |
| Synths + ADSR (`instruments/synthesizers/`) | 002–004 | Prototype with known bugs | B1–B4 below. **Unchanged** on this branch |
| `LayeringMixer` | 004 | Working prototype | No output clamp (the demo's WAV writer clamps). No unit test |
| Tape machine / transport / streaming | 005–014 | Working prototype | Assertion tests in 011–013 and the three `tape_*_test` programs. 005–010 and 014 are renders or benchmarks |
| `SamplePlayer` | 015 | Working prototype, tested | Linear interpolation, reverse, pitch/rate. Stops instantly (no fade) |
| `SampleSliceMap` | 016 | Working prototype, tested | Uses `std::vector` |
| `SampleVoiceManager<N>` | 017 | Working prototype, tested | Fixed pool, oldest-voice stealing. `processBlock` **adds** into the output, so the caller must zero the buffer |
| `DrumPadBank<27>` | 018 | Working prototype, tested | Produces a `TriggerEvent` with `chokedPads`. Its `active` flag is logical only |
| Pad bank → voices, real choke | 019 | **Not started** | Next task |
| Event/MIDI layer, sequencing | — | Not started | |
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

## 5. Other issues found in the recovered code

- **R1: Engines 016–018 were missing from the build.** Their sources were recovered, but `CMakeLists.txt` had no targets for them, so the slice map, voice manager and pad-bank tests never ran. **Fixed on this branch** by adding the three targets and registering them in CTest. All three pass.
- **R2: there was no CTest registration.** Test programs existed, but `ctest` ran nothing. **Fixed:** 21 tests, labelled `unit` or `render`.
- **R3: Engine 003 and Engine 004 have identical source**, apart from the output filename, and produce byte-identical WAVs. Engine 004's banner also says "ENGINE 003". The original Engine 003 (synths in sequence through `StereoMixer`) appears to have been overwritten during recovery. It still exists in `90e8dfb`. Not changed here; the maintainer should decide whether to restore it.
- **R4: the pad bank's `active` flag never clears by itself.** A pad stays "active" after its sample has finished, until it is choked or stopped. This is harmless for choke decisions (choking a finished pad does nothing), but `isActive()` doesn't mean "sounding". Engine 019 should take sounding state from the voices.
- **R5: retriggering the same pad stacks voices.** `trigger()` skips the triggering pad when choking (`i == padIndex`), so hitting the same pad again plays a second voice on top of the first. Engine 019 needs an explicit retrigger policy.
- **R6: housekeeping.**
  - There is a stray empty file at `.devcontainer/tests/audio/audio_test.cpp`.
  - There is no LICENSE, although the project is described as open source.
  - There is no README and no `.gitignore` (`build/`, `*.wav`).

## 6. Engine 019: pad bank → audio voices with real choke groups

The building blocks are already there:

- `DrumPadBank::trigger()` provides the sample pointer, region, `playbackRate`, `volume`, `reverse` and `chokedPads`.
- `SampleVoiceManager::trigger()` accepts exactly those parameters.

What's missing:

1. **Pad ownership on voices.** Add a `padIndex` (or "no pad") to each voice, plus queries such as `voicesForPad(p)` and `isPadSounding(p)`.
2. **Choke = a fast fade, not an instant stop.** `SamplePlayer::stop()` cuts at full amplitude, which clicks. Give each voice a short linear or exponential gain ramp (about 1–5 ms; a constant to be agreed), and free the voice when the ramp reaches 0.
3. **A thin integration layer** (for example `DrumPadEngine` / `PadVoiceRouter`, a new header) that:
   - calls `bank.trigger(pad, velocity, event)`;
   - for each `event.chokedPads[i]`, fades out every voice owned by pad `i`;
   - applies the retrigger policy for the same pad (R5);
   - starts a voice tagged with `event.padIndex`;
   - exposes `processBlock`, and clears the bank's `active` flag when a pad's last voice ends (R4).
4. **Keep 015–018 behaviour intact.** Engines 015–018 must keep passing unchanged. Prefer adding to the existing classes over changing their behaviour.
5. **Tests (Engine 019, `unit` label), each one failing before the work:**
   - a pad triggers sound with the correct gain and rate;
   - triggering a pad in choke group G fades every other sounding pad in G to silence within the fade length;
   - no sample-to-sample step above a threshold during a choke;
   - pads in group 0, or in other groups, are unaffected;
   - the same-pad retrigger policy works as specified;
   - stealing under a full pool is predictable;
   - velocity 0 and invalid pad indices (≥ 27) do nothing;
   - all 27 pads can be addressed;
   - no allocation in `processBlock`.

## 7. Recommended next steps (in order)

1. Review and merge this branch into the recovered line of development.
2. Engine 019, as described in §6.
3. Decide whether to restore the original Engine 003 (R3).
4. Add LICENSE, README and `.gitignore`, and remove the stray empty file (R6).
5. Fix B2/B3 with a stateful ADSR, then B1/B4 with phase accumulators, in separate commits with tests that fail first. Keep the stateless envelope until the demos are migrated, so existing renders stay reproducible.
6. Hardware discovery as documentation only (`docs/hardware/`, with sources). No firmware layer and no flashing until there is a documented, tested recovery path.

## 8. Changes on this branch (relative to `e75b325`)

- Ported from `eb93203` (cherry-picked with `-x`): `CLAUDE.md`, `docs/PROGRESS_REPORT.md` and `tests/unit/dsp_unit_tests.cpp`. The two documents were updated for the recovered code.
- `CMakeLists.txt`: **the recovered file was kept in full**, and only these were added:
  - the `databreaks_engine_016`, `databreaks_engine_017` and `databreaks_engine_018` targets;
  - the `databreaks_dsp_unit_tests` target;
  - all four added to the warning-flag loop;
  - `enable_testing()` and 21 labelled CTest entries.

  `eb93203`'s older `CMakeLists.txt` was **not** used.
- No synth, DSP, tape, sampler or pad-bank code was changed, so rendered audio is unchanged.
