# DATABREAKs progress report

Updated 2026-11-19 for Engine 020 on branch `claude/engine-019-prep-transfer`.
Engine 020 starts from `ce44e70` (Engine 019). Earlier: `e54db9b` (CTest/docs
transfer), whose parent is `e75b325` on `recovery/codespaces-engine-018`
("Recover audio engine development through Engine 018").

History of this report:

- **`eb93203`:** written against `90e8dfb`, when only the synth prototypes existed.
- **`e54db9b`:** updated after the sample player, slice map, voice manager and 27-pad drum bank were recovered in `e75b325`.
- **`ce44e70`:** updated for Engine 019 (drum voice integration).
- **Engine 020 commit:** updated for Engine 020 (drum engine performance and reliability).

## 1. Summary

The repository contains host-side C++17 prototypes through **Engine 020**:

- synths (mono, FM, poly) and two mixers;
- a tape-emulation chain (Engines 005–014);
- a sample player (015);
- a sample slice map (016);
- a multi-voice sample manager (017);
- a 27-pad drum bank (018);
- drum voice integration (019);
- **drum engine performance and reliability** (020).

With Engine 019, pad hits produce audio through managed sample voices, and
choke groups fade the voices that are actually sounding.

Engine 020 adds:

- voice stealing without a hard cut;
- a fixed-capacity lock-free event queue, so a second thread can post pad events, each applied at an exact frame;
- stress and determinism tests;
- a host benchmark.

There is **still no FM-1 firmware**: no target toolchain, HAL, drivers, display,
pad scanning, storage or boot code. Engines 019 and 020 have been run only on a
Linux development host. They have **not** been run or validated on an FM-1, and
the benchmark numbers describe that host, not the FM-1.

## 2. What was verified

### 2.1 Engine 020 commit

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

### 2.2 Engine 019 commit (`ce44e70`, for reference)

22/22 CTest; `engine_019` 50 checks; ASan + UBSan 22/22; 0 warnings; render
WAVs byte-identical to `e54db9b`.

## 3. Component status

| Component | Engine | Status | Notes |
|---|---|---|---|
| `audio_engine.h`, `StereoMixer` | 001–003 | Working prototype | Unit-tested. `StereoMixer` is no longer used by any demo |
| Synths + ADSR (`instruments/synthesizers/`) | 002–004 | Prototype with known bugs | B1–B4 below. **Unchanged** |
| `LayeringMixer` | 004 | Working prototype | No output clamp (the demo's WAV writer clamps). No unit test |
| Tape machine / transport / streaming | 005–014 | Working prototype | Assertion tests in 011–013 and the three `tape_*_test` programs. 005–010 and 014 are renders or benchmarks. **Unchanged** |
| `SamplePlayer` | 015 | Working prototype, tested | Linear interpolation, reverse, pitch/rate. **Unchanged** |
| `SampleSliceMap` | 016 | Working prototype, tested | Uses `std::vector`. **Unchanged** |
| `SampleVoiceManager<N>` | 017 (+019, 020) | Working prototype, tested | Fixed pool, oldest-voice stealing. `processBlock` **adds** into the output. Engine 019 added slot-level control (§6.2); Engine 020 added queued sounds and unique sound ids (§7.2). The original API's results are unchanged |
| `DrumPadBank<27>` | 018 | Working prototype, tested | **Unchanged**. Its `active` flag is now kept in sync by `DrumEngine` |
| `DrumEngine<N, E>` | 019, 020 | Working prototype, tested | See §6 and §7 |
| `DrumEventQueue<N>` | 020 | Working prototype, tested | Lock-free single-producer/single-consumer ring; see §7.4 |
| MIDI input, sequencing | — | Not started | The Engine 020 queue is the hand-off point a future input task can post to |
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

## 8. Recommended next steps (in order)

1. Review Engines 019 and 020 (branch `claude/engine-019-prep-transfer`).
2. Decide on R3 (restore the original Engine 003 or not). Deliberately deferred again in Engine 020.
3. Add LICENSE, README and `.gitignore`, and remove the stray empty file (R6).
4. Engine 021 candidates:
   - per-pad pan and a pad mixer bus;
   - a velocity curve;
   - reduce `trigger()` cost: the choke scan is O(27 × voices) (§7.8).
5. Fix B2/B3 with a stateful ADSR, then B1/B4 with phase accumulators, in separate commits with tests that fail first.
6. Hardware discovery as documentation only (`docs/hardware/`, with sources). No firmware layer and no flashing until there is a documented, tested recovery path.

## 9. Change log on this branch

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
