# M-VAVE FM-1 hardware and resource report (Engine 030)

Written 2026-11-21 against DATABREAKs `78a7da7` (Engine 029). This is a
research and measurement report: **no engine behaviour was changed**.
Nothing here was measured on a physical FM-1. Every hardware statement is
labelled with its evidence class.

## 0. Evidence classes

| Class | Meaning |
|---|---|
| **VERIFIED** | Stated in a manufacturer's own material: the chip maker's datasheet (JieLi), or the product maker's (M-VAVE) published firmware source code and build files. Quoted, not measured by us. |
| **STRONG INFERENCE** | Consistent evidence from several independent sources, or a datasheet fact that applies to the FM-1 only if the FM-1's chip is that exact part. Not stated by M-VAVE for this product. |
| **UNKNOWN** | No reliable source found, or sources conflict. |
| **DATABREAKs REQUIREMENT** | What the current engine needs, measured on the host by `engine_030` and the existing benchmarks. |

### Sources consulted

| ID | Source | Type | Use and confidence |
|---|---|---|---|
| **[M]** | `github.com/m-vave/fm1_open`, M-VAVE's open firmware repository. Files: `sdk/include/fm1.h`, `fm1_block.h`, `fm1_audio_io.h`; `app/board/fm1_input.h`, `fm1_audio.h`, `audio.c`, `device.c`, `panel.c`, `lcd.inc`, `fm1_lcd_hw.h`, `fm1_gpio.h`; `sdk/config/linker.ld.in`; `sdk/Build.ps1`; `sdk/manifest.json`; `sdk/synth.json`; `app/instrument.h`; `README.md` | Manufacturer's source code (GPL-3.0 with a linking exception; the engine is a binary library) | Highest available. It describes how M-VAVE's own firmware uses the hardware. It is not a hardware datasheet. |
| **[D]** | JieLi **AC7911B Datasheet V1.3** (2023-03-03) and **AC791N Hardware Design Guide V1.0** (2022-02-12), from a public GitHub mirror of JieLi's AC79 SDK (`amitv87/fw-AC79_AIoT_SDK`, branch `release/AC79NN_SDK_V1.2.0`, `doc/datasheet/`) | Chip maker's datasheet (text extracted with `pdftotext`) | High for the AC7911B part. Applies to the FM-1 only if its chip is an AC7911B (see S2). |
| **[R]** | `github.com/AL-255/FM-1-RE`: reverse engineering of the stock firmware V13 | Community firmware analysis (addresses exact; purposes inferred; the repo marks its own confidence) | Medium–high for stock-firmware behaviour. Internally inconsistent in places (noted below). |
| **[T]** | `github.com/aroum/fm1-custom-fw`, `REVERSE_ENGINEERING.md`: teardown and pin-map notes | Community teardown, single source, methodology partly unstated | Medium; used only where consistent with [M]/[D]. |
| **[I]** | `github.com/jvitkauskas/pi32v2-isa`: ISA, compiler and on-device probes (`docs/ISA.md`, `docs/COMPILER.md`, `docs/CPU1.md`, `hw/…excprobe`, `hw/…bootprobe`) | Community; measurements on one author's device | Medium–high for ISA and compiler facts; single-device timings. |
| **[A]** | `github.com/cicloid/awesome-fm-1`, a resource index | Community index | Used only to find sources. |
| **[F]** | `github.com/hugelton/felucca-bsp`, `docs/EXPERIMENTAL.md` | Community BSP | Low–medium; one item (cache RAM). |
| — | Retail and press pages (synthtopia, thesynthsource, manuals.plus, altstrument, m-vave.com) | Product summaries | **Not reachable from this environment**: only search-engine summaries were seen, so nothing from them is treated as verified. |

## 1. FM-1 hardware facts

### 1.1 Processor

| # | Item | Class | Value | Source |
|---|---|---|---|---|
| S1 | SoC family | **VERIFIED** | JieLi **AC791N / WL82** | [M] `fm1_gpio.h` "AC791N/WL82 port addresses", `fm1_audio.h` "WL82 ALNK0", `fm1_lcd_hw.h` "AC791N SPI1" |
| S2 | Exact part | STRONG INFERENCE | **AC7911B**, QFN-48 (6 × 6 mm) | [T] reports the PCB marking; matches [D]'s QFN-48 pin map and [M]'s pin usage. Single teardown source. |
| S3 | CPU core | **VERIFIED** | JieLi **pi32v2**, compiler target `-mcpu=r3` | [M] `Build.ps1` (`-target pi32v2 -mcpu=r3`), `manifest.json` ("PI32v2 clang 4.0.1 r3") |
| S4 | Word size | **VERIFIED** | 32-bit | [D] "RISC 32-bit CPU"; [M] "ELF32 PI32v2" |
| S5 | Cores | STRONG INFERENCE | **2** | [D] "Double core RISC 32-bit CPU" (if AC7911B); [I] `CPU1.md` starts and uses the second core on an FM-1; [R] boot code brings up cpu1. [R]'s hardware map also says "Single CPU core (`CPU_CORE_NUM 1`)": conflicting configuration, not hardware. |
| S6 | Maximum clock | STRONG INFERENCE | **320 MHz** | [D] "DC-320MHz operation" (if AC7911B) |
| S7 | Operating clock | **UNKNOWN** (conflicting) | stock firmware **240 MHz** (24 MHz × PLL) | [R] (`pll_clock_init(24000000, 240000000)`, high). [I] reads `CLK_CON2 = 6` "(360 MHz)" at bootloader entry, above [D]'s maximum. [I]'s timing probes ran at 96 MHz. [M] reads the clock at run time only (`fm1_device_clock()`). |
| S8 | FPU | **VERIFIED** (presence, for AC7911B); STRONG INFERENCE (for the FM-1) | an FPU exists | [D] "Double core RISC 32-bit CPU (Support FPU)"; [M] compiles with `-mfprev1` |
| S9 | FPU precision | STRONG INFERENCE | **single precision (binary32) only; `double` is done in software** | [I] `ISA.md`: FPU group = "single-precision add, sub, mul, div, multiply-add/sub, …"; §9: "`double` arithmetic goes through helper functions (software)"; `COMPILER.md`: `__adddf3`/`__muldf3`/`__divdf3` calls. [M] compiles with `-cl-single-precision-constant`. [R] classifies the stock math library as "libm + soft-float". The datasheet does not state the precision. |
| S10 | Measured instruction costs | STRONG INFERENCE (one device) | int ALU 1 cycle; int multiply 2; int divide 10; float add / multiply 2; float divide 4; load / store 2; call + return ≈ 22.5; **no dual issue** | [I] `hw/…excprobe` (code in SRAM, at 96 MHz) |
| S11 | Wireless (chip) | **VERIFIED** (AC7911B) | Bluetooth 2.1 + EDR / BLE 5.3; Wi-Fi 802.11b/g/n | [D] |

### 1.2 Memory

| # | Item | Class | Value | Source |
|---|---|---|---|---|
| M1 | Internal SRAM used by M-VAVE's firmware | **VERIFIED** (as M-VAVE's linker configures it) | `RAM0` = 0x01C00000 … 0x01C7FE00 minus 180 bytes = **523,596 bytes (≈ 511 KiB)**. Heap end at the top of `RAM0`. | [M] `linker.ld.in` |
| M2 | Extra cache-way RAM | **VERIFIED** (configured) | 7 of 8 data-cache ways × 4 KiB = **28,672 bytes** (`CACHE_RAM_SIZE`) | [M] `linker.ld.in`; [F] confirms the technique on one device |
| M3 | Total SRAM in the chip | **UNKNOWN** | — | The datasheet text does not state it. M1 + M2 ≈ 539 KiB is what M-VAVE's build uses. |
| M4 | SDRAM / PSRAM | STRONG INFERENCE | **none usable** | [M] `SDRAM_SIZE = 0` (a zero-length `sdram` region); [R]'s memory map has RAM only at 0x01C0xxxx. [D] encodes in-package memory as ONE code (flash *or* SDRAM *or* PSRAM), and the FM-1 has 1 MiB of flash (M6). [T]'s "8 MB SDRAM in AC7911BB" has no stated source and is contradicted by the above. |
| M5 | Platform RAM already used | STRONG INFERENCE | stock V13 statics (.data + .bss) = **135,676 bytes**; M-VAVE's open build requires a heap of **≥ 49,152 bytes** and keeps audio code in internal SRAM | [R] `01-boot.md` (high); [M] `Build.ps1` checks |
| M6 | Flash (program and data) | **VERIFIED** (size, as linked) / STRONG INFERENCE (part) | **1 MiB** NOR, XIP at 0x02000000; parts Puya P25Q80H (JEDEC 0x856014), later UC25HQ80 (0xB36014) | [M] `linker.ld.in` `rom LENGTH = 1024*1024`; part IDs from [T], [I], [A]. Package (in-package vs SOIC-8) conflicts: [T] vs [F]. |
| M7 | Flash for user data | **VERIFIED** (M-VAVE's layout) | **536,576 bytes**: projects 0–68 KiB, then a shared 456 KiB sample bank ("about 19.55 seconds" of new samples); final sectors hold DX7 banks and preferences | [M] `instrument.h` (`GROOVE_FLASH_BYTES`), `Build.ps1` `user_bytes=536576` |
| M8 | Flash erase unit | **VERIFIED** | **4,096-byte sectors** | [M] `fm1.h` `FM1_FLASH_SECTOR_BYTES 4096U`, "Erase clears exactly one aligned sector" |
| M9 | Flash endurance / write time | **UNKNOWN** | — | — |

### 1.3 Audio

| # | Item | Class | Value | Source |
|---|---|---|---|---|
| A1 | Sample rate | **VERIFIED** | nominal **44,100 Hz** (`FM1_SAMPLE_RATE`); the hardware runs at **44,118 Hz** (`FM1_AUDIO_HARDWARE_RATE 44118U`) | [M] `fm1_block.h`, `fm1_audio.h`; [R] independently measured 44,118 in the stock firmware |
| A2 | Block size | **VERIFIED** (M-VAVE open firmware) | **128 frames**, stereo, interleaved, one interrupt per block (≈ 2.9 ms) | [M] `fm1_block.h` (`FM1_BLOCK_FRAMES 128`, `FM1_CHANNELS 2`), `fm1_audio_io.h` |
| A3 | Block size (stock firmware) | STRONG INFERENCE | 64 frames | [R]: a firmware choice, not a hardware limit |
| A4 | Output word format | **VERIFIED** | **24-bit stereo** on audio link ALNK0 (I2S-style), samples as signed **Q23 in `int32_t`** | [M] `fm1_audio.h` "WL82 ALNK0: 24-bit stereo, two DMA halves, 128 frames per interrupt"; `audio_output.h` "Q23" |
| A5 | Codec / DAC | **UNKNOWN** (conflicting) | The AC7911B has an internal "16-bit DAC with headphone amplifier" (S/N 99 dB A-weighted, THD+N −72 dB, ≥ 15 mW into 32 Ω) per [D]. M-VAVE drives ALNK0 at 24 bits with an 11.2896 MHz (256 fs) audio clock, and [T] reports an external codec/amplifier (part not given). [R] describes the internal DAC. | [D], [M], [T], [R] |
| A6 | Audio inputs | **VERIFIED** (chip) / UNKNOWN (product) | chip: 4 × 16-bit audio ADC; FM-1 use unknown | [D] |
| A7 | USB / Bluetooth audio | **VERIFIED** | USB audio PCM Q23; Bluetooth audio PCM 16-bit | [M] `fm1_audio_io.h` |
| A8 | Speaker | **UNKNOWN** | — | No primary source |

### 1.4 Controls and display

| # | Item | Class | Value | Source |
|---|---|---|---|---|
| C1 | Key / button matrix | **VERIFIED** | **41 positions**, 11 columns (`FM1_NKEY=41`, `FM1_NCOL=11`) = **27 note keys (F3…G5) + 14 panel buttons** | [M] `fm1_input.h`; `fm1.h` `keys` "bits 0..26: F3..G5", `buttons` "0..13"; `fm1_led_set` "0..13 buttons, 14..40 note keys" (keys and buttons have LEDs) |
| C2 | Key type | STRONG INFERENCE | **digital on/off matrix keys, not velocity-sensitive**; key material UNKNOWN (only unverified retail summaries) | [M] reports keys as single bits, with no velocity field in the input API; [T]: diode-isolated matrix via two 74HC595 shift registers. Velocity exists in M-VAVE's sequencer data, not in the key input. |
| C3 | Input scan | **VERIFIED** | panel scan on TIMER5 at **10 kHz** | [M] `device.c` |
| C4 | Encoders | **VERIFIED** | **7 rotary encoders**, reported as signed detents | [M] `fm1_input.h` `FM1_NENC=7`; `fm1.h` `encoders[7]` |
| C5 | Potentiometer | **VERIFIED** | **1 master potentiometer**, 10-bit ADC (PB6 / ADC4) | [M] `panel.c`, `device.c` |
| C6 | Aftertouch, pressure, pitch/mod wheels | **UNKNOWN** | not in M-VAVE's input API ([R] mentions wheel ADC code inherited from an SDK, marked low confidence) | [M], [R] |
| C7 | Display | **VERIFIED** | **240 × 240 RGB565 TFT**, ST7789 command set, SPI1, DMA in 240 × 16-pixel strips | [M] `fm1.h` (`FM1_SCREEN_WIDTH/HEIGHT 240`, "RGB565"), `lcd.inc` "ST7789 initialization", `fm1_lcd_hw.h` |

### 1.5 Connectivity, power and firmware

| # | Item | Class | Value | Source |
|---|---|---|---|---|
| X1 | MIDI | **VERIFIED** | input: DIN (via 3.5 mm TRS), USB and BLE MIDI; output: USB and BLE; clock, start/stop/continue and song position supported | [M] README, `fm1.h` (`FM1_MIDI_DIN`, `FM1_MIDI_USB`, `FM1_MIDI_BLE`) |
| X2 | USB | **VERIFIED** (chip) | USB 1.1 full-speed OTG controller; the FM-1 enumerates as a USB-MIDI + USB-audio composite device | [D] "FUSB 1.1 OTG"; [M] USB audio API; [R] captured descriptors |
| X3 | Bluetooth | **VERIFIED** | BLE MIDI (up to 8 devices) and Bluetooth audio | [M] `fm1.h` (`FM1_BLE_MAX_DEVICES 8U`), `fm1_audio_io.h` |
| X4 | Battery | **VERIFIED** (monitoring) / **UNKNOWN** (capacity, life, charger) | Li-Po battery voltage on a divider into PB1 / ADC3; chip VBAT 2.2–5.5 V | [M] `panel.c`, `device.c`; [D] electrical table. A 2000 mAh capacity appears only in unverified retail summaries. |
| X5 | Power behaviour | **UNKNOWN** | sleep, wake, brown-out and power-off behaviour not documented | [R] notes a tickless low-power idle path (med) |
| X6 | Firmware update | **VERIFIED** | official `.fwsc` images; M-VAVE's updater and a web workbench over the standard FM-1 update protocol (USB-MIDI) | [M] README; [A] lists the official downloads |
| X7 | Bootloader / recovery | STRONG INFERENCE | mask-ROM USB download mode (appears as "WL82 UBOOT1.00"), which firmware cannot overwrite; used by several independent recovery tools | [A], [R], [I]; entry needs a USB "key" dongle or tool |
| X8 | Debug access | STRONG INFERENCE | no exposed debug header and no recovery button | [T] (single teardown), consistent with [A] |
| X9 | Storage reachable by firmware | **VERIFIED** | the 1 MiB SPI NOR only (no SD slot in M-VAVE's API); sector erase / program APIs | [M] `fm1.h` |
| X10 | Stock firmware engine | STRONG INFERENCE | 6-operator FM (msfa / Dexed-derived), **12 voices, integer/fixed-point DSP**, 64-frame blocks | [R] (high); 12 voices is also in retail summaries |
| X11 | M-VAVE open engine | **VERIFIED** | 8 tracks, **8 voices** (`GROOVE_VOICES = 8`), 16 project slots of 4,064 bytes, integer Q23 output | [M] `instrument.h`, README |

## 2. Unknowns that matter for DATABREAKs

1. **Total SRAM**, and how much of it remains free with M-VAVE's or our own platform layer (USB, BLE, display buffers, RTOS) running. M1 + M2 ≈ 539 KiB is an upper bound for what is mapped.
2. **The operating CPU clock** of a custom firmware (240, 320 or "360" MHz; S7), and whether the second core can run DSP in production. [I] calls its second-core setup "a tested setup, not a complete specification".
3. **Real cost of software `double` on pi32v2.** Only the call overhead (≈ 22.5 cycles) is measured; the helper bodies are not.
4. **The codec path (A5):** internal DAC or external codec, its true bit depth and noise, output latency.
5. **Flash endurance and write/erase times** (M9), which decide how often a bank may be saved.
6. **Power and brown-out behaviour (X5)**, which decides whether two-slot atomic saves are needed.
7. Speaker, audio input, and any pressure or aftertouch sensing.

## 3. DATABREAKs current requirements

These are host measurements (x86-64, LP64) pinned by `tests/audio/engine_030.cpp`. **Updated for Engine 031:** the zero-initialised `PatternBank` slot storage is 640 B smaller, so the bank, `Sequencer`, `AudioPipeline` and `LoadScratch` figures below are 640 B lower than in the original Engine 030 report; the 32-bit column is unchanged from Engine 029. The FM-1 resource profile and its RAM model are in `core/target/` and `docs/PROGRESS_REPORT.md` §18. 32-bit layout estimates (i686 with 8-byte-aligned doubles, a proxy for ARM EABI-like layouts, *not* pi32v2) are from Engine 029. With `-DDATABREAKS_PORTABLE_INT128=ON` (no native `__int128`, as on a 32-bit target), `Sequencer` and `AudioPipeline<16>` are 8 bytes smaller (654,616 / 919,192) because the clock is 8- rather than 16-byte aligned; `engine_030` pins both layouts.

### 3.1 RAM

| Object | Host bytes | 32-bit estimate | Lifetime |
|---|---|---|---|
| **`AudioPipeline<16>`** | **919,200** | 874,808 | permanent |
| ↳ `Sequencer` | 654,624 | 632,072 | permanent |
|   ↳ working `Pattern` (dense 27 × 256 steps + 512 notes) | 127,024 | 127,024 | permanent |
|   ↳ `Recorder` incl. `RecordedTrack` (2,048 × 88 B events) | 201,152 (track 182,584) | 182,512 (track) | permanent |
|   ↳ `DefaultPatternBank` (pools: 8,192 steps, 1,024 notes, 2,048 events) | 199,840 | 199,960 | permanent (persisted) |
|   ↳ per-block event buffer (2,816 × 32 B) | 90,112 | — | permanent |
|   ↳ quantize cache | 16,384 | 16,384 | permanent |
|   ↳ arrangement | 272 | — | permanent (persisted) |
| ↳ `Reverb` (8 delay lines of `double`) | 163,560 | 163,400 | permanent |
| ↳ `DrumEngine<16>` | 45,544 | 24,440 | permanent |
| ↳ `TapeEffect` | 10,080 | — | permanent |
| `TapeMachine` heap delay lines (allocated at construction) | **268,800** | 268,800 | permanent |
| **Runtime total** (pipeline + tape heap) | **1,188,000** | **≈ 1,143,600** | |
| `pattern_io::LoadScratch` | 509,448 | 509,496 | only while loading (caller-provided) |
| Sample audio | **768,000 bytes per second** of stereo (16-byte double frames, 48 kHz) | same | while loaded |

### 3.2 Flash / code

| Item | Value |
|---|---|
| Engine code, `-Os` proxy | ≈ 57.5 KB `.text` on x86-64; ≈ 76 KB on i686, for a TU that instantiates the full pipeline, rendering, the sequencer and DBPB save/load. Not pi32v2 code. |
| Static-placement finding | A *statically* allocated `DefaultPatternBank` is constant-initialised: its records have non-zero defaults (velocity 100, …), so the compiler places it in **`.data`, costing ≈ 200 KB of flash image AND 200 KB of RAM**, copied at boot. **Fixed in Engine 031:** a default bank is now all zero bytes and a static one is placed in `.bss` (verified with `nm`, GCC and Clang). Still in initialised data when declared as standalone statics: `RecordedTrack` (182,584 B), `Recorder` (201,152 B), `DrumEngine<16>` (45,544 B), `Arrangement` (272 B); inside `Sequencer` / `AudioPipeline` / `LoadScratch` / `PatternData` they are in `.bss` with GCC and Clang because those classes have run-time constructors (the pi32v2 toolchain is unchecked). `AudioPipeline` and `LoadScratch` land in `.bss` only because their constructors run at start-up. Measured with `nm` on the proxy build. |
| Saved bank (DBPB) | typically ≈ 5 KB per pattern; **206,026 bytes worst case** (all pools full; pinned by `engine_030` R3) |

### 3.3 Numeric format, rate, voices, input

- **Numeric format:** all DSP is **`double`** (516 uses in `core/` and `instruments/`; `StereoFrame` = 2 × `double`). There is no `float` path.
- **Sample rate:** fixed at compile time at **48,000 Hz** (`SAMPLE_RATE`); filter and time constants derive from it. `AudioStreamConfig` rejects 44,100 Hz (pinned by `engine_030` R4).
- **Voices:** **29**: 16 drum sample voices, 1 mono, 4 FM, 8 poly (`engine_030` R4).
- **Input:** pads take velocity 1–127; `InputRouter` rejects velocity 0.
- **Block size:** any (tested 1–4,096); 128 is fine.

### 3.4 DSP cost (host, measured; Release, ~2.1 GHz VM)

| Component | Host time per stereo frame |
|---|---|
| 16 active drum voices | 73.8 ns (4.4 ns per voice) |
| Reverb | 51 ns |
| Tape | 79–149 ns |
| Full chain without tape | 106–132 ns |
| Pipeline, effects bypassed | 186–193 ns |
| **Pipeline, full chain with tape** | **466 ns** |

These host numbers use hardware `double` on an out-of-order superscalar
core. They do not transfer to the FM-1 by clock scaling (see §5.2).

## 4. Side-by-side comparison

| Resource | FM-1 (class) | DATABREAKs now | Ratio / verdict |
|---|---|---|---|
| RAM | ≈ 511 KiB `RAM0` + 28 KiB cache RAM (VERIFIED as configured); no SDRAM (STRONG INFERENCE); platform needs ≥ 48 KiB heap, and stock statics are 132.5 KiB | **≈ 1.14–1.19 MB** at run time | **≈ 2.1–2.3× the entire mapped SRAM**: does not fit |
| … sequencer alone | same | 632–655 KB | > 100 % of SRAM |
| … reverb + tape delay lines | same | ≈ 432 KB of `double` delay memory | ≈ 80 % of SRAM |
| … load scratch | same | +510 KB during a load | cannot coexist with anything |
| Program flash | 1 MiB total, minus 536,576 B user data = **at most 512,000 B (≈ 500 KiB) for boot loader, code and constants** (VERIFIED layout) | ≈ 60–80 KB of code (proxy); +200 KB if a bank is placed statically as `.data` | code fits; static bank placement must be avoided |
| User flash | 536,576 B, 4 KiB sectors (VERIFIED) | DBPB 5 KB per pattern typical, 206 KB worst case; atomic two-slot saves double that | fits typical use; worst case competes with samples |
| Sample storage | 456 KiB bank ≈ 19.55 s in M-VAVE's (unstated) format, i.e. ≈ 23.9 KB/s | 768 KB per second of audio, held in RAM | **the format must change** (flash-resident, compact) |
| Floating point | single-precision FPU; `double` in software (STRONG INFERENCE) | 100 % `double` | **largest CPU risk** |
| CPU budget | 240–320 MHz single-issue: **5,442–7,256 cycles per frame per core** at 44.1 kHz | host 466 ns/frame with hardware `double` | see §5.2: estimated to exceed the budget by a wide margin |
| Sample rate | 44,100 nominal / 44,118 Hz hardware (VERIFIED) | fixed 48,000 Hz | **mismatch**: retuning is a deliberate audio change |
| Audio block | 128 frames, `int32` Q23 stereo (VERIFIED) | any block size; `double` output; `Int32` conversion already exists (Engine 029) | compatible (Q23 scaling to add in the adapter) |
| Voices | stock 12 integer FM voices; M-VAVE open engine 8 voices | 29 `double` voices | a target voice profile is needed |
| Keys | 27 on/off note keys + 14 buttons, 7 encoders, 1 pot (VERIFIED) | 27 velocity-sensitive "pads" | key count matches; **velocity does not exist** on the hardware (STRONG INFERENCE) |
| Display | 240 × 240 RGB565 (VERIFIED) | no display code | new work |
| Storage medium | the same 1 MiB NOR as the code; 4 KiB sectors | `StorageBackend` with atomic replace (Engine 029) | needs a two-sector (A/B) flash backend |

## 5. Biggest blockers

### 5.1 RAM: the engine needs about twice the chip's entire SRAM

The sequencer alone (655 KB) exceeds the SRAM. That's before any audio
state, platform stack, USB/BLE buffers or display strip buffers. The
dominant items are:

- **sized for a desktop:** the dense working `Pattern` (127 KB), the 2,048-event `RecordedTrack` (183 KB), the bank pools (200 KB) and the 90 KB event buffer;
- **`double` delay memory:** reverb (164 KB) and tape (269 KB).

### 5.2 Software double precision

The FPU is single-precision only (STRONG INFERENCE, S9), so every
`double` add, multiply, divide, compare and conversion becomes a helper
call (`__adddf3`, `__muldf3`, `__ltdf2`, `__fixdfsi`, … per [I]). The call
and return alone measure ≈ 22.5 cycles [I]; the work inside the helpers is
not measured.

A **lower-bound estimate, not a measurement**, counted in the actual drum
path (`sample_player.h` and `sample_voice_manager.h`), per active voice
per frame:

- about 14 `double` arithmetic operations: 6 for two-channel interpolation, the fraction, the rate multiply, the position step, the gain, and 4 for the mix;
- about 4 comparisons and about 3 integer↔`double` conversions;
- that is **≈ 20 software-float calls**.

At 16 voices that is ≈ 320 calls. At the measured call overhead *alone*,
that is **≥ 7,200 cycles per frame**. That already exceeds the whole
per-frame budget at 240 MHz (5,442 cycles) and nearly fills it at 320 MHz
(7,256), before the helper work itself, the synths, the effects or the
sequencer.

The real cost must be measured on hardware (§9). Even at 320 MHz, or with
the second core, the full `double` pipeline is very unlikely to run in real
time.

### 5.3 Sample rate and data formats

- The engine is compiled for 48 kHz; the FM-1 runs at 44.1 kHz (44,118 Hz).
- Samples are held in RAM as 16-byte `double` stereo frames: the 456 KiB sample bank would hold about 0.6 s that way.

### 5.4 Input model

There are 27 keys, but they are on/off. Velocity-dependent features
(velocity sensitivity, recorded velocities) will need a fixed or
user-chosen velocity, or an encoder.

## 6. What can remain unchanged

- **Sequencer logic** (Engines 025–028): integer, exact, block-size independent, allocation- and lock-free. The 32-bit-clean clock arithmetic is from Engine 029.
- **The quantizer, the arrangement logic, and the DBPB format and its streaming storage boundary** (flash sectors map directly onto `ByteSink`/`ByteSource`/`StorageBackend`).
- **The platform boundaries:** `AudioRenderCallback` (a 128-frame Q23 block is a straightforward adapter), `InputEvent` / `InputRouter` (27 keys, 14 buttons and 7 encoders map onto pads, buttons and encoders), and `StorageBackend`.
- **The real-time rules** (no allocation or locks on the audio path, SPSC queues) and the block-size independence.
- **The algorithms themselves.** Nothing here requires simplifying them; it requires changing their *numeric representation and capacities*.

## 7. What probably needs adaptation (not done here)

1. **A target resource profile**: compile-time capacities chosen for the FM-1 while the host profile stays bit-identical. Today's values are desktop-sized: `Pattern::MaxSteps` / `MaxNotes`, `RecordedTrack::MaxEvents`, the bank pools, `MaxEventsPerBlock` and the event buffer, the voice counts, and the reverb and tape delay lengths.
2. **A single-precision (`float`) or fixed-point DSP path.** This is a **deliberate audio change**: it needs its own reference renders and tolerance-based comparisons against the `double` engine, not byte identity. It also halves every delay line (reverb ≈ 81 KB, tape ≈ 134 KB).
3. **Sample rate as a configuration** (44,100 / 44,118 Hz on the FM-1), with coefficients derived per rate.
4. **Flash-resident samples** in a compact format (`int16` or a compressed format), played through non-owning views instead of `std::vector<StereoFrame>`.
5. **Zero-initialisable static layouts** for the bank (and similar pools), so static instances cost `.bss`, not `.data` + flash.
6. **Load without a full staging copy** on the target (a two-pass validate-then-apply, or loading into an inactive A/B slot), because 510 KB of scratch cannot exist there.
7. **A flash `StorageBackend`** with A/B sectors for atomic saves, sized for typical banks.
8. **A target voice and effect budget** decided from measurement: for example 8–12 voices, like M-VAVE's own engines, with tape and reverb optional.

## 8. Is the 16-voice pipeline reasonable for the FM-1?

**Not as it is.**

- M-VAVE's own engines run **12** (stock, integer FM) and **8** (open engine) voices on this chip.
- DATABREAKs runs 29 `double` voices plus six `double` effects.
- In RAM alone, the 16-voice `DrumEngine` (45 KB) is affordable; the problem is the rest of the pipeline.
- In CPU, 16 voices of software-`double` mixing are estimated to exceed the frame budget on their own (§5.2).

A target-specific voice and resource configuration will be required. The
existing templates (`DrumEngine<VoiceCount>`, `FmSynthEngine<N>`,
`PolySynthEngine<N>`, `PatternBank<…>`) already allow it without changing
the host configuration. The right numbers must come from on-device
measurement.

## 9. What must be measured on physical hardware

(None of this was done in Engine 030; flashing requires the maintainer's explicit go-ahead.)

1. The CPU clock a custom firmware actually gets, and its stability.
2. Cycles per frame of each DATABREAKs component built with the pi32v2 toolchain, in `double` **and** in `float`, on one core and on two.
3. Free SRAM with the platform layer (USB, BLE, display, RTOS) active.
4. The audio output path: internal DAC vs external codec, real bit depth, noise, output latency from the end of a block to audio.
5. Key-scan latency and debounce (10 kHz scan), encoder behaviour, and whether any key velocity or pressure information exists at all.
6. Flash program/erase times and the safe save frequency; behaviour on power loss during a save.
7. Battery capacity, current draw at each CPU load, and brown-out behaviour.

## 10. Recommended Engine 031

**"FM-1 resource profile": make the target's memory and voice budget a
first-class, testable configuration, without changing the host engine.**

- Introduce a compile-time **resource profile** (host profile = today's values, bit-identical renders; an **FM-1 budget profile** with reduced, documented capacities). It covers pattern dimensions, recorded events, bank pools, event-buffer size, voice counts and delay lengths, chosen from §4/§7.
- Add an accounting test in the style of `engine_030` that sums every permanent object for each profile and fails if the FM-1 profile exceeds a stated budget. The budget must be an explicit assumption, for example ≤ 384 KiB of the ≈ 511 KiB, leaving room for the platform.
- Make static instances zero-initialisable (`.bss`, not `.data`).
- Keep all algorithms and the host output unchanged. The `float` DSP path and the 44.1 kHz rate are **separate, later engines**: they change audio and need their own listening and tolerance methodology, plus the on-device measurements in §9.

This gives the first concrete answer to "does it fit in RAM?" before any
numeric change is attempted.
