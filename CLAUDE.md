# CLAUDE.md — DATABREAKs

Guidance for Claude (and other contributors) working in this repository.
Read this file, `docs/PROGRESS_REPORT.md`, and the code before changing anything.

## Project goal

DATABREAKs is an original, open-source **replacement firmware and
music-production platform for the M-VAVE FM-1**. The long-term goal is a
complete, independent firmware that runs on the FM-1 hardware. The device has
**27 physical pads**.

## Current reality (keep this section honest and up to date)

Development history: `90e8dfb` (initial synths) → `e75b325` on
`recovery/codespaces-engine-018` (recovered Codespaces work through Engine 018)
→ `claude/engine-019-prep-transfer` (CTest/docs transfer, then Engines
019–021).

Everything in this repository is a **host-side C++17 prototype** built and
tested on a development machine. **Nothing here is FM-1 firmware.** There is no
target toolchain, startup code, HAL, drivers, display, pad scanning, storage,
or boot code. Never describe these prototypes as completed, installable, or
flashable FM-1 firmware.

### Components through Engine 021

| Header | Purpose | Tested by |
|---|---|---|
| `core/audio/audio_engine.h` | 48 kHz stereo constants, `StereoFrame`, `midiToHz`, `clampAudio` | `dsp_unit_tests` |
| `core/audio/mixer.h` | `StereoMixer` (master gain + hard clip). Since Engine 021 it is the master stage of `AudioPipeline` | `dsp_unit_tests`, `engine_021` |
| `core/audio/audio_pipeline.h` | `AudioPipeline<DrumVoices, DrumEvents>`: the unified host render path — sources (drums now) added into a bus at their levels, then the `StereoMixer` master stage (Engine 021) | `engine_021` (17 checks), `render_engine_021_render` |
| `core/audio/layering_mixer.h` | `LayeringMixer`: mono + FM + poly layer volumes, master | render demos 003/004 only |
| `instruments/synthesizers/*.h` | `MonoSynth`, `FMSynth`, `PolySynth`, stateless ADSR | `dsp_unit_tests` (known bugs B1–B4) |
| `core/audio/tape_machine.h` | Tape emulation: speed, tape type, wow/flutter, hiss, saturation, head bump, bias, compression, crosstalk, print-through, age | `tape_test`, `tape_character_test`, Engines 005–011 |
| `core/audio/tape_transport.h` | Offline whole-buffer tape transport | `transport_test`, Engines 006/009/010 |
| `core/audio/tape_processor.h` | Wrapper around `TapeMachine` | Engine 011 |
| `core/audio/tape_stream_transport.h`, `tape_streaming_engine.h` | Block-streaming tape playback + processing | Engines 012–014 |
| `core/audio/sample_player.h` | `SamplePlayer`: region, pitch/rate, reverse, linear interpolation, block processing (Engine 015) | `engine_015` |
| `core/audio/sample_slice_map.h` | `SampleSliceMap`: even slicing + movable boundaries (Engine 016) | `engine_016` |
| `core/audio/sample_voice_manager.h` | `SampleVoiceManager<N>`: fixed voice pool, oldest-voice stealing, mixing (Engine 017). Engine 019 added slot-addressed `triggerInSlot`, `releaseVoice` (linear fade), `stopVoice`, per-slot queries and a per-slot generation counter. Engine 020 added queued sounds (`queueInSlot`, `cancelPending`, `hasPending`) and unique sound ids. Engine 021 added `setOutputGain` (bus level, default 1.0). The original API behaves as before | `engine_017`, `engine_019`, `engine_020` |
| `core/audio/drum_pad_bank.h` | `DrumPadBank<27>`: per-pad sample/region, volume, pitch, velocity sensitivity, reverse, choke groups → `TriggerEvent` (Engine 018). Unchanged by Engine 019 | `engine_018` |
| `core/audio/drum_engine.h` | `DrumEngine<VoiceCount = 16, EventCapacity = 256>`: 27 pads → managed sample voices, pad-to-voice ownership, real choke groups, 5 ms linear release fades, same-pad retrigger policy, voice-synchronised pad activity (Engine 019); click-free voice stealing, frame-accurate queued events, statistics (Engine 020) | `engine_019` (50 checks), `engine_020` (29 checks) |
| `core/audio/drum_event_queue.h` | `DrumEventQueue<N>`: fixed-capacity lock-free single-producer/single-consumer event ring (Engine 020) | `engine_020` (incl. a two-thread test) |

### Engine 019 — drum voice integration (implemented, host-side only)

`DrumEngine<VoiceCount>` in `core/audio/drum_engine.h` owns one
`DrumPadBank<27>` and one `SampleVoiceManager<VoiceCount>`. The 27 pads and the
voice count are independent (default 16 voices, like the manager's default).

- **Use it as the entry point for pad playback.** Configure pads through its
  forwarding setters (`assignSample`, `setVolume`, `setPitch`,
  `setVelocitySensitivity`, `setChokeGroup`, `setReverse`, `setRegion`,
  `clearSample`), hit pads with `trigger(pad, velocity)`, and render with
  `processBlock(out, n)`. `bank()` and `voices()` are read-only, so nothing can
  bypass ownership tracking.
- `processBlock` **adds into** the output buffer (same convention as
  `SampleVoiceManager`). Pad activity is updated at the end of each block.
- Ownership: each voice slot records `{pad, soundId}` (Engine 020; Engine 019
  used the per-slot generation). A record counts only while that sound is
  the one sounding in the slot, or the one queued there.
- Choke and same-pad retrigger (default `RetriggerPolicy::FadeAndRetrigger`)
  start a linear fade of `releaseFadeFrames()` frames (default 240 = 5 ms at
  48 kHz). Fades only ever get quieter or faster.
- Slot choice when hitting a pad (Engine 020): lowest free slot → quietest
  releasing voice → oldest playing voice → earliest queued sound. A stolen
  voice gets a short steal fade (default 48 frames = 1 ms, at most 10 ms),
  and the new sound starts on the very next frame in the same slot.
- Full details, limitations and test results: `docs/PROGRESS_REPORT.md` §6
  (Engine 019) and §7 (Engine 020).
- Do not change the synth or tape DSP as part of drum-engine work.

### Engine 020 — performance and reliability (implemented, host-side only)

- **Threading model:** `processBlock` and every non-`post*` method belong to
  ONE audio thread. The `post*` methods (`postTrigger`, `postReleasePad`,
  `postStopPad`, `postStopAll`) may be called from ONE other thread; they
  only touch the lock-free queue. Nothing else is thread-safe. Do not
  describe the engine as generally thread-safe.
- Posted events carry a frame offset inside the next `processBlock` call and
  are applied there in FIFO order. Offsets are clamped to the block. If the
  queue is full, the event is rejected, `post*` returns false and
  `droppedEventCount()` increases.
- Benchmark: `databreaks_engine_020_benchmark` is built but is **not** a CTest
  test. Its numbers describe the development host, not the FM-1. Build it in
  Release for meaningful timings.

### Engine 021 — unified audio pipeline (implemented, host-side only)

- **Render through `AudioPipeline`** (`core/audio/audio_pipeline.h`), not by
  calling `DrumEngine::processBlock` directly, whenever you want final
  output. It is the one place where sources meet the master stage. Do not add
  a second mixer or master path; add new sources (e.g. synth layers) inside
  `mixSources()` with their own level.
- Signal flow: `drums().processBlock` adds the summed drum voices × drum level
  into the bus buffer → `applyMaster` (NaN/inf → 0, then the existing
  `StereoMixer`: × master level, hard clip to ±1).
- Gain staging (each applied once): sample × pad volume × velocity curve ×
  fade × **drum level** (`setDrumLevel`, in `SampleVoiceManager`'s summing,
  default 1.0) × **master level** (`setMasterLevel`, default **1.0** in the
  pipeline; `StereoMixer`'s own default stays 0.8).
- `render()` ADDS into the caller's buffer (existing bus audio is summed, not
  overwritten), then masters the whole buffer; call it once per block as the
  last stage. `mixSources()` adds without mastering. The caller clears the
  buffer for a fresh mix.
- No scratch buffer and no block splitting, so any block size works and
  queued event offsets stay frame-exact. Posted events for one block are
  limited to `eventCapacity()` (255); extra ones are dropped and counted.
- `LayeringMixer` (synth demos 003/004) is unchanged and not yet routed
  through the pipeline: the synths are still stateless per-sample functions
  (finding B4).
- `.gitignore` ignores `build/` and `*.wav`.

## Hardware constraints and safety rules

- **Unknown and must not be assumed:** the FM-1's MCU/SoC, GPIO mappings,
  pad-matrix wiring, display controller and protocol, audio codec, flash
  layout, bootloader, and recovery method. Anything about these must come from
  documented, verified sources or measurement, and must be recorded with its
  source (in `docs/hardware/`) before code depends on it.
- **Never flash hardware, erase or overwrite existing firmware**, or run any
  tool that talks to a connected device, unless the maintainer explicitly asks
  for that specific action at that time.
- Do not copy vendor firmware, binaries, or proprietary code. All code must
  be original.
- Write DSP so that it can later be ported to a constrained target:
  - no heap allocation, I/O, exceptions or locks in per-sample/per-block audio
    paths (note: `TapeMachine` and `SampleSliceMap` currently use `std::vector`,
    `TapeTransport::render()` returns a `std::vector` — acceptable on host, to be
    revisited for the target);
  - fixed-capacity containers (`std::array`) for voices and pads;
  - keep a path open to `float`/fixed-point (the target may lack a
    double-precision FPU; this is not yet known);
  - keep hardware access behind interfaces so host builds stay testable.

## Layout

```
core/audio/                 engine-wide types, mixers, tape, sampler, pads
instruments/synthesizers/   mono / FM / poly synths, ADSR
tests/audio/engine_NNN.cpp  one program per engine milestone (render demo or assertion test)
tests/audio/tape_*_test.cpp assertion tests for tape components
tests/unit/                 assertion unit tests (dsp_unit_tests)
docs/                       progress report; docs/hardware/ for verified hardware facts
```

Headers are included either by relative path (`../../core/audio/x.h`) or by
bare name (`x.h`, via the `include_directories` in `CMakeLists.txt`). Both work.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure          # all 25 tests
ctest --test-dir build -L unit --output-on-failure  # assertion tests only
```

- CTest labels: **`unit`** = programs that check results and exit non-zero on
  failure; **`render`** = demos/benchmarks that only prove the program runs and
  writes WAV files (into the build directory). Render tests are not evidence
  that audio is correct.
- Requires CMake ≥ 3.20 and a C++17 compiler. Verified with GCC 13.3 / CMake 3.28 on Linux.
- Builds use `-Wall -Wextra -Wpedantic`; keep them warning-free.
- Do not commit WAV files or build directories.

## Development rules

1. Read the existing code and this file before changing anything. Do not start
   over or replace the architecture without the maintainer's agreement.
2. Preserve working functionality. A change that alters rendered audio must be
   intentional and stated in the commit message.
3. Every new engine/component gets an assertion test registered in CTest with
   the `unit` label — add **both** the `add_executable` and the CTest entry
   (Engines 016–018 were once left out of the build by mistake).
4. Run the full build and `ctest` after every change; report exactly what was
   verified (host build, unit tests, listening) and what was not (hardware).
5. Known bugs are listed in `docs/PROGRESS_REPORT.md`; fix them with tests that
   fail before the fix and pass after.
6. Keep engine milestones (Engine 0NN) as separate, reviewable commits.
7. Pad indices are 0–26 (27 pads); validate indices at API boundaries.
8. Audio-path code (`processBlock`, `trigger`, `post*`, release/stop) must not
   allocate, lock or block. `engine_019` check 41 and `engine_020` check 14
   enforce this by counting `operator new` calls (reliable in the default
   unoptimised build; optimisers may remove paired new/delete). Extend that
   pattern to new real-time code.
9. Code shared between threads must be tested under ThreadSanitizer
   (`-fsanitize=thread`), and the claim must stay as narrow as the test.
