# DATABREAKs progress report

Updated 2026-11-18 for Engine 019 on branch `claude/engine-019-prep-transfer`.
Engine 019 starts from `e54db9b` (CTest/docs transfer), whose parent is
`e75b325` on `recovery/codespaces-engine-018` ("Recover audio engine
development through Engine 018").

History of this report:

- **`eb93203`:** written against `90e8dfb`, when only the synth prototypes existed.
- **`e54db9b`:** updated after the sample player, slice map, voice manager and 27-pad drum bank were recovered in `e75b325`.
- **Engine 019 commit:** updated again for the drum voice integration.

## 1. Summary

The repository contains host-side C++17 prototypes through **Engine 019**:

- synths (mono, FM, poly) and two mixers;
- a tape-emulation chain (Engines 005–014);
- a sample player (015);
- a sample slice map (016);
- a multi-voice sample manager (017);
- a 27-pad drum bank (018);
- **drum voice integration** (019).

With Engine 019, pad hits produce audio through managed sample voices, and
choke groups fade the voices that are actually sounding.

There is **still no FM-1 firmware**: no target toolchain, HAL, drivers, display,
pad scanning, storage or boot code. Engine 019 has been run only on a Linux
development host. It has **not** been run or validated on an FM-1.

## 2. What was verified (Engine 019 commit)

Environment: Linux, GCC 13.3.0, CMake 3.28.3.

| Check | Result |
|---|---|
| `cmake -S . -B build && cmake --build build -j2` | 22 executables, **0 warnings** (`-Wall -Wextra -Wpedantic`; build log searched for "warning" and "error") |
| `ctest --test-dir build --output-on-failure` | **22/22 pass** |
| `ctest --test-dir build -L unit` | **12/12 pass**: `dsp_unit_tests`, Engines 011, 012, 013, 015, 016, 017, 018, **019 (50 checks)**, `tape_test`, `transport_test`, `tape_character_test` |
| `ctest --test-dir build -L render` | **10/10 pass**. These only show that each program runs and writes its WAV files |
| AddressSanitizer + UndefinedBehaviorSanitizer | Separate Debug build (`-fsanitize=address,undefined -fno-sanitize-recover=all`, leak detection on): **22/22 pass**, no reports, 0 warnings |
| Render output unchanged | All 16 WAVs written by the render tests are **byte-identical** to those from `e54db9b` |
| Mutation checks on `engine_019` | Four deliberate faults were each caught by several checks: choke disabled; fade replaced by an abrupt stop; retrigger fade removed; activity sync removed. A planted allocation in `processBlock` was caught by check 41. Removing the generation check was **not** caught; see §6.8 |
| Listening test | **Not done** (no audio playback in this environment) |
| FM-1 hardware | **Not done, deliberately**: nothing flashed or connected |

## 3. Component status

| Component | Engine | Status | Notes |
|---|---|---|---|
| `audio_engine.h`, `StereoMixer` | 001–003 | Working prototype | Unit-tested. `StereoMixer` is no longer used by any demo |
| Synths + ADSR (`instruments/synthesizers/`) | 002–004 | Prototype with known bugs | B1–B4 below. **Unchanged** |
| `LayeringMixer` | 004 | Working prototype | No output clamp (the demo's WAV writer clamps). No unit test |
| Tape machine / transport / streaming | 005–014 | Working prototype | Assertion tests in 011–013 and the three `tape_*_test` programs. 005–010 and 014 are renders or benchmarks. **Unchanged** |
| `SamplePlayer` | 015 | Working prototype, tested | Linear interpolation, reverse, pitch/rate. **Unchanged** |
| `SampleSliceMap` | 016 | Working prototype, tested | Uses `std::vector`. **Unchanged** |
| `SampleVoiceManager<N>` | 017 (+019) | Working prototype, tested | Fixed pool, oldest-voice stealing. `processBlock` **adds** into the output. Engine 019 added slot-level control (§6.2) without changing the original API's results |
| `DrumPadBank<27>` | 018 | Working prototype, tested | **Unchanged**. Its `active` flag is now kept in sync by `DrumEngine` |
| `DrumEngine<N>` | 019 | Working prototype, tested | New; see §6 |
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
- **R6: housekeeping (open).**
  - There is a stray empty file at `.devcontainer/tests/audio/audio_test.cpp`.
  - There is no LICENSE, although the project is described as open source.
  - There is no README and no `.gitignore` (`build/`, `*.wav`).

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

### 6.9 Real-time safety

`trigger`, `processBlock`, `releasePad`, `releaseAll`, `stopPad` and `stopAll`:

- use only fixed-size `std::array` state;
- allocate no heap memory (check 41 counts global `operator new` calls over 50 rounds of triggers, chokes and blocks, plus a release and a stop);
- take no locks, perform no I/O and throw no exceptions.

There is no internal synchronisation, so configuration, triggers and
`processBlock` must be called from the same thread. Hand-off to a separate
audio thread (for example with a lock-free event queue) is future work.

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

- A hard steal (pool exhausted, nothing releasing) still cuts a voice without a fade. A future option is to reserve a voice for fades, or to steal the quietest voice.
- Activity changes caused by audio are only seen at block boundaries.
- There is no event queue or thread hand-off, and no sample-accurate trigger timing inside a block: triggers take effect at the start of the next `processBlock`.
- Per-pad pan, output routing and a pad mixer are not implemented. Voice gain is `pad volume × velocity curve`, as defined by `DrumPadBank`.
- Voices use `double` and linear interpolation, the same as `SamplePlayer`. Suitability for the eventual target is unknown until the hardware is documented.
- Nothing has been listened to, and nothing has been run on an FM-1.

## 7. Recommended next steps (in order)

1. Review Engine 019 (branch `claude/engine-019-prep-transfer`).
2. Decide on R3 (restore the original Engine 003 or not).
3. Add LICENSE, README and `.gitignore`, and remove the stray empty file (R6).
4. Engine 020 candidates:
   - a time-stamped event queue so triggers land at an exact frame inside a block;
   - per-pad pan and a pad mixer bus;
   - a softer voice-steal policy.
5. Fix B2/B3 with a stateful ADSR, then B1/B4 with phase accumulators, in separate commits with tests that fail first.
6. Hardware discovery as documentation only (`docs/hardware/`, with sources). No firmware layer and no flashing until there is a documented, tested recovery path.

## 8. Change log on this branch

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
