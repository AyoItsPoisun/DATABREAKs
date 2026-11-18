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
`recovery/codespaces-engine-018` (recovered Codespaces work through Engine 018).

Everything in this repository is a **host-side C++17 prototype** built and
tested on a development machine. **Nothing here is FM-1 firmware.** There is no
target toolchain, startup code, HAL, drivers, display, pad scanning, storage,
or boot code. Never describe these prototypes as completed, installable, or
flashable FM-1 firmware.

### Components through Engine 018

| Header | Purpose | Tested by |
|---|---|---|
| `core/audio/audio_engine.h` | 48 kHz stereo constants, `StereoFrame`, `midiToHz`, `clampAudio` | `dsp_unit_tests` |
| `core/audio/mixer.h` | `StereoMixer` (master gain + hard clip) — no longer used by any engine demo | `dsp_unit_tests` |
| `core/audio/layering_mixer.h` | `LayeringMixer`: mono + FM + poly layer volumes, master | render demos 003/004 only |
| `instruments/synthesizers/*.h` | `MonoSynth`, `FMSynth`, `PolySynth`, stateless ADSR | `dsp_unit_tests` (known bugs B1–B4) |
| `core/audio/tape_machine.h` | Tape emulation: speed, tape type, wow/flutter, hiss, saturation, head bump, bias, compression, crosstalk, print-through, age | `tape_test`, `tape_character_test`, Engines 005–011 |
| `core/audio/tape_transport.h` | Offline whole-buffer tape transport | `transport_test`, Engines 006/009/010 |
| `core/audio/tape_processor.h` | Wrapper around `TapeMachine` | Engine 011 |
| `core/audio/tape_stream_transport.h`, `tape_streaming_engine.h` | Block-streaming tape playback + processing | Engines 012–014 |
| `core/audio/sample_player.h` | `SamplePlayer`: region, pitch/rate, reverse, linear interpolation, block processing (Engine 015) | `engine_015` |
| `core/audio/sample_slice_map.h` | `SampleSliceMap`: even slicing + movable boundaries (Engine 016) | `engine_016` |
| `core/audio/sample_voice_manager.h` | `SampleVoiceManager<N>`: fixed voice pool, oldest-voice stealing, mixing (Engine 017) | `engine_017` |
| `core/audio/drum_pad_bank.h` | `DrumPadBank<27>`: per-pad sample/region, volume, pitch, velocity sensitivity, reverse, choke groups → `TriggerEvent` (Engine 018) | `engine_018` |

### Engine 019 (next task) — what is and is not connected

`DrumPadBank::trigger()` produces a `TriggerEvent` (sample, region, rate,
volume, `chokedPads[27]`) and tracks a *logical* per-pad `active` flag.
`SampleVoiceManager` plays audio but has **no concept of pads**. Nothing yet
turns a `TriggerEvent` into a voice, and nothing applies `chokedPads` to
sounding audio. Engine 019 must provide that integration (see
`docs/PROGRESS_REPORT.md` §6). Do not change the synth/DSP implementations
as part of Engine 019 unless the maintainer asks.

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
ctest --test-dir build --output-on-failure          # all 21 tests
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
