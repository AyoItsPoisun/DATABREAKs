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
019–025).

Everything in this repository is a **host-side C++17 prototype** built and
tested on a development machine. **Nothing here is FM-1 firmware.** There is no
target toolchain, startup code, HAL, drivers, display, pad scanning, storage,
or boot code. Never describe these prototypes as completed, installable, or
flashable FM-1 firmware.

### Components through Engine 025

| Header | Purpose | Tested by |
|---|---|---|
| `core/audio/audio_engine.h` | 48 kHz stereo constants, `StereoFrame`, `midiToHz`, `clampAudio` | `dsp_unit_tests` |
| `core/audio/mixer.h` | `StereoMixer` (master gain + hard clip). Since Engine 021 it is the master stage of `AudioPipeline` | `dsp_unit_tests`, `engine_021` |
| `core/audio/audio_pipeline.h` | `AudioPipeline<DrumVoices, DrumEvents, SynthEvents>`: the unified host render path — drums (Engine 021) plus mono, FM and poly synth sources (Engine 022), each added into a bus at its own level, then the effects chain (Engine 023, tape added in Engine 024), then the `StereoMixer` master stage; frame-accurate queued synth note events; audio-thread scheduled synth events and the sequencer (Engine 025) | `engine_021` (17), `engine_022` (29), `engine_023` (48), `engine_024` (35), `engine_025` (38), the render demos |
| `core/audio/layering_mixer.h` | `LayeringMixer`: mono + FM + poly layer volumes, master | render demos 003/004 only |
| `instruments/synthesizers/{mono,fm,poly}_synth.h`, `synth_parameters.h` | Original `MonoSynth`, `FMSynth`, `PolySynth` and stateless `calculateEnvelope` (Engines 002–004). **Unchanged** (findings B1–B4 still apply to them) so the 002–004 reference renders stay byte-identical | `dsp_unit_tests`, render demos 002–004 |
| `instruments/synthesizers/synth_voices.h` | Stateful `AdsrEnvelope`, `MonoVoice`, `FmVoice`, `PolyVoice`: same parameter structs and formulas, with phase accumulators and stateful envelopes (fix B1–B4); `sanitized()` parameter guards (Engine 022) | `engine_022` (matches the originals to ~1e-12) |
| `instruments/synthesizers/synth_engine.h` | `SynthEngine<Voice, N, Mono>`: note-on/off, velocity, pitch bend, voice allocation; `MonoSynthEngine`, `FmSynthEngine<4>`, `PolySynthEngine<8>` (Engine 022) | `engine_022` |
| `core/audio/tape_machine.h` | Tape emulation: speed, tape type, wow/flutter, hiss, saturation, head bump, bias, compression, crosstalk, print-through, age. DSP unchanged; Engine 024 made `reset()` also restart the hiss generator (reset = new machine) | `tape_test`, `tape_character_test`, Engines 005–011, `engine_024` |
| `core/audio/tape_transport.h` | Offline whole-buffer tape transport | `transport_test`, Engines 006/009/010 |
| `core/audio/tape_processor.h` | Wrapper around `TapeMachine` | Engine 011 |
| `core/audio/tape_stream_transport.h`, `tape_streaming_engine.h` | Block-streaming tape playback + processing | Engines 012–014 |
| `core/audio/sample_player.h` | `SamplePlayer`: region, pitch/rate, reverse, linear interpolation, block processing (Engine 015) | `engine_015` |
| `core/audio/sample_slice_map.h` | `SampleSliceMap`: even slicing + movable boundaries (Engine 016) | `engine_016` |
| `core/audio/sample_voice_manager.h` | `SampleVoiceManager<N>`: fixed voice pool, oldest-voice stealing, mixing (Engine 017). Engine 019 added slot-addressed `triggerInSlot`, `releaseVoice` (linear fade), `stopVoice`, per-slot queries and a per-slot generation counter. Engine 020 added queued sounds (`queueInSlot`, `cancelPending`, `hasPending`) and unique sound ids. Engine 021 added `setOutputGain` (bus level, default 1.0). The original API behaves as before | `engine_017`, `engine_019`, `engine_020` |
| `core/audio/drum_pad_bank.h` | `DrumPadBank<27>`: per-pad sample/region, volume, pitch, velocity sensitivity, reverse, choke groups → `TriggerEvent` (Engine 018). Unchanged by Engine 019 | `engine_018` |
| `core/audio/drum_engine.h` | `DrumEngine<VoiceCount = 16, EventCapacity = 256>`: 27 pads → managed sample voices, pad-to-voice ownership, real choke groups, 5 ms linear release fades, same-pad retrigger policy, voice-synchronised pad activity (Engine 019); click-free voice stealing, frame-accurate queued events, statistics (Engine 020); audio-thread `scheduleTrigger`/`scheduleEvent` merged with the queue in frame order (Engine 025) | `engine_019` (50 checks), `engine_020` (29 checks), `engine_025` |
| `core/audio/drum_event_queue.h` | `DrumEvent` and `DrumEventQueue<N>` (Engine 020); since Engine 022 an alias of `SpscRing<DrumEvent, N>` | `engine_020` (incl. a two-thread test) |
| `core/audio/synth_event.h` | `SynthTarget`, `SynthEventType`, `SynthEvent` — moved unchanged out of `audio_pipeline.h` in Engine 025 (the pipeline includes it) | — |
| `core/sequencer/musical_time.h` | 960 PPQ ticks, `TimeSignature`, bar/beat/tick conversion (Engine 025) | `engine_025` |
| `core/sequencer/musical_clock.h` | `MusicalClock`: exact tick↔sample mapping in 128-bit integers, milli-BPM tempo, start/stop/reset (Engine 025) | `engine_025` |
| `core/sequencer/pattern.h` | `Pattern`: 27 drum lanes × up to 256 steps (velocity, probability, microtiming) + up to 512 synth `NoteEvent`s (Engine 025) | `engine_025` |
| `core/sequencer/sequencer.h` | `Sequencer`: plays a Pattern into sample-exact `SequencerEvent`s; deterministic probability; SPSC transport commands (Engine 025) | `engine_025` (38 checks) |
| `core/audio/spsc_ring.h` | `SpscRing<T, N>`: the fixed-capacity lock-free single-producer/single-consumer ring, generalised from Engine 020's queue in Engine 022 (drum and synth events) | `engine_020`, `engine_022` (two-thread tests) |
| `core/audio/effects/audio_effect.h` | `AudioEffect` interface + `EffectBase<Derived>` (bypass with 20 ms crossfade, wet/dry, reset), `SmoothedValue`, dB helpers (Engine 023) | `engine_023` |
| `core/audio/effects/equalizer.h` | `Equalizer`: low shelf / mid peak / high shelf + output gain, cookbook biquads (Engine 023) | `engine_023` |
| `core/audio/effects/compressor.h` | `Compressor`: stereo-linked feed-forward, soft knee, attack/release, makeup (Engine 023) | `engine_023` |
| `core/audio/effects/soft_clipper.h` | `SoftClipper`: linear below the knee, tanh above, never above the ceiling (Engine 023) | `engine_023` |
| `core/audio/effects/distortion.h` | `Distortion`: soft / asymmetric / fuzz, DC blocker, mode crossfade (Engine 023) | `engine_023` |
| `core/audio/effects/reverb.h` | `Reverb`: 8-line feedback delay network, fixed 162,808-byte delay storage (Engine 023) | `engine_023` |
| `core/audio/effects/effect_chain.h` | `EffectChain<N>`: ordered, fixed-capacity effect slots with `insert()` (Engine 023) | `engine_023` |
| `core/audio/effects/tape_effect.h` | `TapeEffect`: owns a `TapeMachine` and runs it as an `AudioEffect` — validation, 20 ms glides, click-free speed/type switching, latency-aligned wet/dry (Engine 024) | `engine_024` (35) |

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
- `LayeringMixer` (synth demos 003/004) is unchanged. It is a legacy
  per-frame mixer for those demos; the pipeline does not use it (Engine 022
  synths have their own source levels).
- `.gitignore` ignores `build/` and `*.wav`. `databreaks_engine_021_demo.wav`
  at the repository root was force-added on request (`e4d796c`).

### Engine 022 — synthesizer integration (implemented, host-side only)

- **Use the stateful engines for new synth work**: `MonoSynthEngine`,
  `FmSynthEngine<N=4>`, `PolySynthEngine<N=8>` (`synth_engine.h`), reached as
  `pipeline.mono()`, `.fm()`, `.poly()`. Do not modify the original
  `MonoSynth`/`FMSynth`/`PolySynth`/`calculateEnvelope`: the Engine 002–004
  reference renders depend on them byte for byte.
- The new voices reproduce the originals sample for sample for a normally
  played note (`engine_022` E1–E3). They differ only where the originals were
  wrong: early release, retrigger and block rendering (findings B1–B4).
- Control: `noteOn(note 0..127, velocity)` (velocity ≤ 0 = note-off, > 127
  clamped, gain = velocity/127), `noteOff`, `allNotesOff`, `reset` (hard
  stop), `setPitchBend(semitones, ±24)`, `setPitchBendMidi(0..16383, range)`,
  `setParameters` (sanitised), `setLevel`.
- Mono: last-note priority over a 16-note held stack. FM/poly allocation:
  same note → lowest idle → oldest releasing → oldest held; retrigger/steal
  restarts the attack from the current level with continuous phase.
- Pipeline routing: `mixSources()` = drums (whole block, own queue) then
  synths rendered in segments between queued synth events. Each synth sums
  its voices × its level once; master once. Synth events:
  `postNoteOn/postNoteOff/postPitchBend/postAllNotesOff(target, …, offset)`,
  255 per block, overflow dropped and counted.
- Threading: direct synth calls are audio-thread only; synth `post*` from ONE
  producer thread (the same thread may also feed the drum queue).

### Engine 023 — effects engine (implemented, host-side only)

- Signal flow: `render()` = `mixSources()` (drums + synths, each × level) →
  `applyEffects()` (the chain, in place on the summed bus) → `applyMaster()`
  (× master, hard safety clip). Chain order since Engine 024: **EQ →
  Compressor → SoftClipper → Tape → Distortion → Reverb**, reached via
  `pipeline.eq()`, `.compressor()`, `.softClipper()`, `.tape()`,
  `.distortion()`, `.reverb()`, `.effects()`.
- **Every effect starts fully bypassed**, so existing renders are bit-exact.
  Enable with `setEnabled(true)` / `setBypassed(false)` (20 ms crossfade;
  `immediate = true` to skip it). A fully bypassed effect is not run at all.
  An effect leaving full bypass is reset and its parameters (and mix) snap to
  their set values.
- All effects derive from `EffectBase<Derived>` (`Derived::tick(StereoFrame)`
  per sample, in place). Shared: bypass, wet/dry `setMix` (exact at 0 and 1),
  `reset()`. Parameter setters ignore NaN/inf and clamp; gains, frequencies,
  thresholds and levels glide over 20 ms per sample (`SmoothedValue`).
- Tape was added in Engine 024 through `EffectChain::insert()` (see below).
  `engine_023` checks the five Engine 023 effects by name and skips Tape.
- Effect parameters are audio-thread only; there is no control queue yet.
- Reverb delay storage is fixed (162,808 bytes in `double`, ~81 KB if ported
  to float). The FM-1's RAM is undocumented; do not assume it fits.
- Benchmark: `databreaks_engine_023_benchmark` (built, not a CTest test; host
  numbers only).

### Engine 024 — tape in the effects chain (implemented, host-side only)

- `pipeline.tape()` is a `TapeEffect` (`core/audio/effects/tape_effect.h`) that
  **owns** a `TapeMachine` by value and calls `TapeMachine::process()` for
  every sample: the wet signal is exactly the existing tape DSP (tested bit for
  bit). Do not change `TapeMachine`'s DSP to suit the adapter.
- Controls (TapeMachine's own ranges and defaults): `setSpeed` (7.5/15/30 IPS,
  default 15), `setTapeType` (ferric/chrome/metal, default ferric), and 0..1:
  `setWow`, `setFlutter`, `setHiss`, `setSaturation`, `setHeadBump`, `setBias`
  (default 0.5), `setCompression`, `setCrosstalk`, `setPrintThrough`, `setAge`
  (others default 0). NaN/inf ignored (TapeMachine's own setters let NaN
  through), values clamped, all continuous controls glide over 20 ms.
- Speed/type switches duck the tape signal to the aligned dry signal over
  5 ms, switch, and return over 5 ms (raw TapeMachine switching steps by up
  to ~0.2 at high saturation). Setting the current value is a no-op.
- **Latency:** TapeMachine delays by 12 ms (576 frames) plus wow/flutter.
  `latencyFrames()` reports it; wet/dry mixes against a 576-frame-delayed dry
  signal (no comb filter). Enabling tape delays the whole bus by 12 ms;
  bypass returns to the undelayed signal (crossfaded).
- **Gain:** TapeMachine's saturation also raises small-signal gain (about
  drive / tanh(drive), +2.4 dB at defaults, more with saturation; output is
  hard-limited to ±1). There is no output trim; lower the input or master.
- Memory: 268,800 bytes of TapeMachine heap buffers (allocated once, when the
  pipeline is constructed) + a 9,216-byte aligned dry line. Nothing is
  allocated while processing.
- The transport classes (`TapeTransport`, `TapeStreamTransport`,
  `TapeStreamingEngine`) play stored recordings; they are not part of the
  insert effect.
- Benchmark: `databreaks_engine_024_benchmark` (built, not a CTest test).

### Engine 025 — sequencer foundation (implemented, host-side only)

- `pipeline.sequencer()` owns a `MusicalClock` and one `Pattern`. It runs
  inside `mixSources()` on the audio thread and is **stopped by default**, so
  nothing changes until `start()`.
- **Time:** a tick is 1/960 quarter note. Tempo is integer milli-BPM
  (20–300 BPM). Tick t is delivered at sample `ceil(x(t))`, where
  `x(t) = (O + (t − T0)·fs·60000) / (milliBPM·960)`, computed in `__int128`:
  no drift. Tempo changes take effect at the next tick boundary (one rounding
  to 1/D sample per change). Never replace this with floating-point
  accumulation.
- **Delivery:** the sequencer must not use `post*` (those queues have one
  producer, a control thread). It uses the audio-thread scheduled paths:
  `drums().scheduleTrigger()` and `pipeline.scheduleNoteOn/Off()` (1,024 per
  block each). These are merged with the queues in frame order; at the same
  frame, scheduled events come before queued ones.
- **Ordering:** sequencer events at the same sample are note-off → drum (pads
  0..26) → note-on (mono, FM, poly; by note number).
- **Probability:** a hash of (seed, loop, lane, step) — never a running
  random generator — so results are independent of block size. 0 = never,
  100 = always.
- **Capacity:** 1,024 triggers + note-ons per block (more are dropped and
  counted), 256 sounding sequenced notes, note-offs never dropped. Transport
  queue: 63 commands from ONE other thread.
- **Threading:** pattern editing, clock settings and direct transport calls
  are audio-thread only (or while stopped). `postTransport()` is the only
  cross-thread entry.
- Benchmark: `databreaks_engine_025_benchmark`.


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
core/audio/                 engine-wide types, mixers, tape, sampler, pads, pipeline
core/audio/effects/         EQ, compressor, soft clipper, tape, distortion, reverb, chain
core/sequencer/             musical time, clock, pattern, sequencer (Engine 025)
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
ctest --test-dir build --output-on-failure          # all 33 tests
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
