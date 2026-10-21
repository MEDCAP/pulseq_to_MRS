# pulseq_plato

Runs Pulseq 1.5 `.seq` files on PLATO spectrometers (MR46xx) or `simulator.exe`.
It is our own replacement for `plato\bin\pulseq.exe`. The only API reference is
`C:\mrs\plato` (docs, headers, libs).

Plan: `C:\Users\kento\.claude\plans\i-want-to-get-keen-music.md`.
Measured PLATO behaviour: [test/FINDINGS.md](test/FINDINGS.md).

## Design in one paragraph

The block loop from `parsemr.cpp` (`GetBlock` / `decodeBlock`) runs on the **host** at build time:

- converts units and applies ppm offsets
- computes absolute times
- decompresses and deduplicates shapes into one waveform library stream per channel
- writes a flattened, time-ordered row table per channel (`fmt::Raw`)

Each PLATO channel (Tx, Rx, GA/GB/GC) then runs one small **realtime block loop**:

- `seek(table + pos)`, read the row, `_switch(kind)`
- play it with `rf(lib + off, n − 1, …)`, `gradient(…)` or `acquire2(…)` at its absolute `at`

The program size stays fixed whatever the sequence length. All data lives in stream memory.

## Status

- [x] Phase 0: simulator probes (experiments/, see test/FINDINGS.md). The vendor reference run works (`test/setup_ref.sh`).
- [x] Toolchain: builds with the vendor MSYS2 in `C:\smis\msys64\ucrt64` (g++ 13.1). Phase 0 logs are unchanged against `test/golden/phase0` (`test/regress_phase0.sh`).
- [x] Phase 1: host core. Loader, model, units, waveform libraries and channel tables, plus unit tests and `pulseq_plato_dump`.
- [x] Phase 2: FID end to end. `pulseq_plato.exe` runs the unrolled and block-loop programs on the simulator, and both give identical logs. The FID matches the vendor log (ADC times and gradients exact, RF within 0.3 %). The pump writes `<o>_0.cfl`. `test/regress_phase2.sh` checks everything (16 checks).
- [x] Phase 3: real sequences.
  - The scanner-proven vendor GRE (`gre_fromMRS_64.seq`) runs with your scanner's parameters, all 830 events on time, and matches the vendor log.
  - `origin()` rebasing: a 24 s run is on time past the 32-bit timeline, and forced rebasing gives identical logs.
  - Short-gap stress measured the per-channel row slack.
  - Memory is logged, and the RF phase is set with `setph`, so spoiled pulses share one waveform.
  - `test/regress_phase3.sh` runs 19 checks.
- [x] Phase 4 (scoped to your GRE examples, which use no extensions):
  - All 19 `.seq` files in `C:\Users\kento\dev\pulseq\matlab\dev\GRE2d` run on the simulator with every row on time and every sample (`test/run_folder.sh`).
  - RF up to 1 % over `-rfmax` is clipped, as the vendor does; more is refused with the `-rfmax` needed.
- [ ] Later: several receivers, extensions (rotation, trigger, soft delay), PowerScan plugin



## Build and test (Git Bash)

```bash
export PATH=/c/smis/msys64/ucrt64/bin:$PATH      # vendor GCC 13.1 toolchain, NOT C:\msys64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-smis-ucrt64.cmake
cmake --build build
ctest --test-dir build --output-on-failure
build/pulseq_plato_dump test/seqs/fid_v150.seq     # model + channel tables
build/pulseq_plato.exe -n test/seqs/fid_v150.seq   # build the realtime program only
```

- Configure fails if the compiler isn't the `C:/smis` g++ 13.x.
- Every exe gets the smis `libstdc++`/`libgcc_s`/`winpthread` (and `libcorelib.dll` if it uses PLATO) copied next to it, so it runs without any PATH setup.
- Our code is C++20 only: no C++23 library features, and no `std::format`.
- Test sequence generators need pypulseq in a private venv: `/c/Python312/python -m venv build/pyenv && build/pyenv/Scripts/python -m pip install pypulseq==1.5.0.post1`.



## Running

`pulseq_plato.exe` takes the same options as the vendor `pulseq.exe` (`-m -o -stdout -f -rfmax -gx -gy -gz -cic -rxmask -txmask -ptsmask`). Testing options:

- `-emit loop|unrolled`: the realtime program form (default `loop`).
- `-t0 N`: RT clock of sequence t = 0 (default 10000).
- `-epoch N`: insert `origin()` once a row would reach N clocks past the last one (default 2³⁰; small values test rebasing).
- `-allow-late`: run even though some rows are too close for the block loop. Without it such a sequence is refused (exit 3), because a late event is silent on hardware.
- `-timeout s`: how long to wait for the run.
- `-expect file`: writes the expected log time of every row, for `compare.py expect`.
- `-n`: build only, don't connect.

Receiver 0's data goes to `<o>_0.cfl` (complex64, like the vendor), or to stdout with `-stdout`. The first module in the `-m` file is the master. Tx, Rx and gradient channels are found by asking each module what it has.

### Settings file (`-f settings.yml`)
One YAML file can carry all of these settings:
- the sequence and output
- PTS base and NCO frequency, and RF full scale
- gradient full scale per axis
- Tx/Rx masks
- the CIC filter
- the module list
- the testing options

`-f` takes either a settings file (a value ending in `.yml`/`.yaml`) or, as in the vendor `pulseq.exe`, a frequency in Hz.

```bash
build/pulseq_plato -f config/sim_reference.yml                  # everything from the file
build/pulseq_plato -f config/sim_reference.yml other.seq -o x   # options on the command line override it
```

| File | Purpose |
|---|---|
| `config/sim_reference.yml` | The scanner's reference parameters against the **local simulator** (use this for testing). |
| `config/scanner.yml` | Same parameters with the scanner module addresses from `plato/bin/modules.txt`. **Drives hardware.** It has no `sequence:`, so the `.seq` goes on the command line. |

**Frequency model.** The PTS synthesizer provides the base frequency, selected by `pts_mask` (table from the scanner's `C:\smis\smis.ini [PplVars]`). On Tx and Rx the NCO is programmed with `-f` (positive, the vendor's `DefObserveFrequency`). The high-side masks add it to the base: 1H 270 MHz + 28.037464 MHz = 298.037464 MHz. The low-side masks subtract it: 13C 104 MHz − 29.046641 MHz = 74.953359 MHz. The run prints the result.
- The Pulseq 1.5 ppm offsets scale with the physical frequency, base ± `-f`.
- Low side (sign −1): a physical offset +Δ needs NCO −Δ, so every RF/ADC frequency offset, phase offset and RF shape phase is negated (`ScanConfig::sideband`). The received samples are written conjugated, so +Hz in the .cfl is a higher physical frequency, as for 1H.
- Like the vendor, `pts_mask` is written to UIO bits 8–15 (`setout(mask << 8)`, `00004d00` for 77, `0000be00` for 190) before the sequence starts and held until it ends. The UIO component exists only when `pts_mask` ≠ 0.

| `pts_mask` | 0 | 77 | 109 | 47 | 190 | 63 | 107 |
|---|---|---|---|---|---|---|---|
| base | none | 270 MHz | 250 MHz | 150 MHz | 104 MHz | 114 MHz | 58 MHz |
| NCO | + | + | + | − | − | − | − |
| nucleus (7 T) | — | 1H | 19F | 31P | 13C | 129Xe | 15N |

Other masks are refused, including 110 (54 MHz, marked "DO NOT USE" in smis.ini). The earlier table (15 = 170, 93 = 230, 13 = 370 MHz) came from a non-scanner smis.ini.

Keys (all optional; unknown keys, bad numbers, unknown PTS masks and bad addresses are errors with `file:line`):

| Key | Command line | Notes |
|---|---|---|
| `sequence` | positional | relative to the settings file |
| `output` | `-o` | relative to the working directory |
| `rf.pts_mask` | `-ptsmask` | PTS base frequency (table above); hex like `0x4d` is accepted |
| `rf.frequency_hz` | `-f` (number) | NCO frequency (> 0) = offset from the PTS base, added or subtracted per the table, Tx and Rx |
| `rf.max_amplitude_hz` | `-rfmax` | B1 in Hz at full-scale RF |
| `gradients.max_hz_per_m.{x,y,z}` | `-gx -gy -gz` | Hz/m at full-scale DAC |
| `channels.tx_mask`, `rx_mask` | `-txmask -rxmask` | bit *i* = channel *i* (as the vendor GUI); one bit each (1, 2, 4, 8) picks the Tx/Rx channel, e.g. `rx_mask: 4` for 13C on the dual-tuned 1H/13C coil (channel 3) |
| `receiver.cic` | `-cic` | `on`/`off` |
| `triggering.respiratory` | `-trigger` | `on`/`off`, default off; see [Respiratory triggering](#respiratory-triggering) |
| `modules` (list of `{name, ip, port}`) or `modules_file` | `-m` | first module = master; `-m` replaces the list |
| `advanced.{emit, t0_clocks, epoch_clocks, timeout_s, allow_late, message_latency_clocks, stdout}` | `-emit -t0 -epoch -timeout -allow-late -stdout` | testing options (`-trigger-lead`, `-trigger-settle` have no yml key) |

On the simulator a `-f config/sim_reference.yml` run and the same run with flags give identical event logs. The PTS byte appears as `UOUT 00004d00` from clock 110 to the end (the vendor sets it at 139). The YAML reader (`src/config/yaml_lite.*`) handles a strict subset: block and flow maps and lists, quoted strings and comments. It rejects tabs, anchors, tags and multi-line scalars.

### Respiratory triggering
`triggering: {respiratory: true}` (or `-trigger on`) makes the scan wait for the respiratory monitor before every repetition. The Pulseq file is not changed: the Pulseq `TRIGGERS` extension is Siemens-only and is not used.
- **Where it waits:** at the start of every block that contains an excitation pulse (RF `use` = excitation, `'e'`). Refocusing, inversion, saturation and preparation pulses never wait. If the .seq has no excitation pulse a warning says nothing is gated. Everything before that block plays as scheduled; everything from it on plays at its normal offsets from the moment the gate arrives. The total time is therefore the nominal time plus the waits.
- **The gate:** UIO input **bit 0 rising** (low, then high; the SA Instruments 1030 sets it high, with its own delay and pulse width). The UIO channel starts looking 50 µs after the block's nominal start, so a pulse during the previous repetition is ignored and the next pulse releases the repetition. One pulse releases only one repetition.
- **How:** every channel (Tx, Rx, GA, GB, GC and UIO) gets a trigger row in its table at each gate. UIO waits for the edge and broadcasts a PLATO message; PLATO activates it on every channel at the same clock cycle. The other channels wait for it in `receivel`, then all restart their timers (`origin()`), so the gradients, RF and ADC keep their timing relative to each other. Gradient waveforms are not joined across a gate. The oscillators keep running, so Tx and Rx stay coherent; only the absolute carrier phase of a pulse depends on when the gate arrives.
- **Timing after the gate:** bit 0 rising to the first event is the message latency (about 52 µs on the scanner, `advanced.message_latency_clocks`) + 2 µs + 10 µs (`-trigger-settle`, `-trigger-lead`), the same on every channel. In the simulator the RF opens 12.5 µs after the edge.
- **Safety:** with no gate the run cannot finish. The default timeout is the sequence time + 10 s + 60 s per gate (`advanced.timeout_s` replaces it); on timeout, or on Ctrl+C, the sequence runner is stopped. Don't just kill the program: the realtime loop would keep waiting on the hardware.
- `-n` prints `triggering: respiratory, N gate(s) …`. With triggering off the tables are identical to before.

### command to run vendor pulseq interpreter

usage: pulseq [options...] sequence-file
Options:
  -m       module-configuration-file
  -o       output-file
  -f       rf-frequency-hz
  -rfmax   max-rf-amp-hz
  -rxmask  receiver-mask
  -txmask  transmitter-mask
  -ptsmask PTS mask
  -cic     on/off
  -gx      max-gradient-amp-hz/m
  -gy      max-gradient-amp-hz/m
  -gz      max-gradient-amp-hz/m

gradient_max Hz/m default=1xe6

rf_max Hz default=100

Example:

`pulseq.exe -o gre01 -gx 57290000 -gy 61090000 -gz 60712000 -ptsmask 77 -rfmax 100 -f 27998450 -m modules.txt -cic off -rxmask 1 -txmask 1 {.seq filepath}`

## Simulator

```bash
test/setup_ref.sh                     # once per fresh build/: private pulseq.exe + runtime in build/ref
test/run_sim.sh fid_unrolled test/seqs/fid_v150.seq -emit unrolled   # our exe, reference scan parameters
test/run_sim.sh fid_loop test/seqs/fid_v150.seq
python test/compare.py expect test/out/fid_loop.expect test/out/fid_loop   # every row on time
python test/compare.py same test/out/fid_unrolled test/out/fid_loop     # loop == unrolled
python test/compare.py ref test/out/fid_loop test/golden/phase0/ref_fid_v150  # vs vendor
test/regress_phase2.sh                # all Phase 2 checks (+ --update to rewrite test/golden/phase2)
test/regress_phase3.sh                # GRE (scanner parameters), 24 s run, gap stress (+ --update)
test/run_folder.sh [folder]           # every .seq of a folder (default: your GRE2d examples), ~7 min
build/pulseq_plato_dump -wave 12 x.seq  # also print gradient waveform values (DAC counts)
build/pulseq_plato_dump -export w.csv x.seq   # every played sample in Hz, Hz/m, rad (src/tables/export.hpp)
build/pyenv/Scripts/python test/plot_waveforms.py x.seq [-f -rfmax -gx -gy -gz] [--t0 s --t1 s] [-o x.png]
                                      # our tables vs pypulseq waveforms_and_times: plot + max difference per channel
UNTIL=400000 test/run_probe.sh p06 build/experiments/p06_tx_block_loop.exe
test/regress_phase0.sh                # all probes + reference FID vs test/golden/phase0
```

- Logs go to `test/out/`. Only one `simulator.exe` may run at a time.
- Python is `C:\Python312\python` (not on the Git Bash PATH as `python3`).
- The simulator exits as soon as every channel has exited, so exit status is never observable there (FINDINGS T7).



## Conventions implemented by the host core (src/)

The loader and table builder follow these conventions. Phase 2 checked the timing rules on the simulator (every row lands on its expected clock). The RF/ADC phase rules are still unverified.


| Item            | Rule                                                                                                                                                                                                                                                                                                                                                                                                                                           | Source                           |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------- |
| Time zero       | sequence t=0 = RT clock `startOffset` (10000). All times are int64 clocks, and rows must fit in int32.                                                                                                                                                                                                                                                                                                                                         | vendor uses ≈10049 (FINDINGS V1) |
| RF samples      | centred: `at = start + delay + raster/2`, as the vendor does. Library waveform = N points + trailing zero; row stores `points − 1`. The waveform keeps only the shape phase.                                                                                                                                                                                                                                                                   | V1/V2, S4, S6                    |
| RF phase        | per pulse `phaseOffset − (offset phase of earlier pulses)` as a `setph` word, timed `txPhaseLeadClk` = 50 before the gate. Pulses that differ only in phase (RF spoiling) share one waveform.                                                                                                                                                                                                                                                  | X3, X4                           |
| RF amplitude    | `amp·mag / rfmax`. Up to 1 % over full scale is clipped (warning), as the vendor does; more is an error that names the `-rfmax` needed.                                                                                                                                                                                                                                                                                                        | V11                              |
| RF gate         | `rf()` doesn't gate. Rows run `unblank(gateOn)` at the RF **block start**, so the RF delay (`rfDeadTime`) is the unblank time, as the vendor does. The gate opens at least 40 clocks before the first sample. `blank(gateOff)` runs 50 clocks after the trailing zero, because `rf()` returns ~28 clocks after its last point.                                                                                                                 | V1, V8, X2                       |
| RF offsets      | `rf()` applies freq like `setfreqofs`, so the NCO integrates it. Each pulse's phase field subtracts the offset phase accumulated by earlier pulses. So pulses are referenced to a continuous carrier at `-f`, with the offset ramp starting at the first sample. The simulator shows the offset from the first sample to the trailing zero. **Phase continuity is unverified**, because the simulator's phase column can't be interpreted yet. | docs §6.2.15                     |
| ppm columns     | `freq += freqPPM·1e-6·f0`, `phase += phasePPM·f0[MHz]` (units per `ExternalSequence.h`)                                                                                                                                                                                                                                                                                                                                                        |                                  |
| Gradients       | sampled at raster **edges**, starting at `start + delay`. Trapezoid, extended trapezoid and arbitrary shapes (centre samples + first/last) all become piecewise-linear edge samples.                                                                                                                                                                                                                                                           | V4                               |
| Gradient joints | when a waveform ends on the edge where the next one on the same axis starts, it drops its last point. A waveform that ends non-zero with no continuation gets a zero point and a warning.                                                                                                                                                                                                                                                      | S6                               |
| ADC             | Sample k represents the centre of its dwell step `[start + k·d, start + (k+1)·d]`. The CIC integrates the grid window before each latch, so `at = start + delay` and the latches fall at the step ends (the vendor latches at the centres, dwell/2 earlier). `grid = dwell`, or `dwell/4` with `-cic on`.                                                                                                                                      | R1, R4, R5                       |
| ADC offsets     | `setfreq(f0+off)` at `at − 51`, then `setph`, then `acquire2(at)` back to back. The 51-clock lead (`rxFreqLeadClk`) makes `acquire2` reset the CIC grid exactly at `at`. Frequency returns to base 50 clocks after the last latch (`restoreAt`). The phase word subtracts the offset phase from earlier ADCs.                                                                                                                                  | R2, R5                           |
| Rows            | 8 words = 32 B = one 256-bit prefetch block. Layout in `src/tables/channel_table.hpp`.                                                                                                                                                                                                                                                                                                                                                         | docs §4.6                        |
| Timing check    | A row's first timed call (`gateOn − 50`, `at` or `at − 51`) must be at least **Tx 100 / Rx 160 / gradient 60** clocks after the previous row's last timed action on the same channel. Measured minimum: 90/150/50. Closer rows are listed in `timingIssues`, and `pulseq_plato` refuses to run them without `-allow-late`.                                                                                                                     | T11                              |
| Long runs       | Row times count from the channel's last `origin()`. A channel inserts an `ORIGIN` row once a row would reach > 2³⁰ clocks past it, placed in a gap that keeps the slack on both sides. `origin(T)` takes effect at T + 10.                                                                                                                                                                                                                     | T10, T12                         |
| Emitters        | The unrolled and block-loop programs run the same per-row calls (`RowCalls`) on the same variables. Only the filling differs (literals vs stream reads), and it happens before the row's first timed call, so their logs are identical. Every channel ends with `resume(end)` and `exit(0)` in its END row.                                                                                                                                    | T8                               |


Not supported yet (the loader raises a clear error): rotation, trigger and soft-delay extensions; oversampled arbitrary gradients; ADC phase-modulation shapes; files older than Pulseq 1.5. Labels are ignored.

## Layout


| Path                                                     | Contents                                                                                                                                                                                            |
| -------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cmake/Plato.cmake`, `cmake/toolchain-smis-ucrt64.cmake` | PLATO imported targets; the toolchain pinned to `C:\smis`                                                                                                                                           |
| `pulseq/`                                                | upstream `ExternalSequence` (MIT, unmodified, see SOURCE.txt)                                                                                                                                       |
| `src/config` | settings file: strict YAML-subset reader (`yaml_lite`), schema (`scan_settings`) |
| `config/` | `sim_reference.yml` (local simulator), `scanner.yml` (hardware addresses); use with `-f` |
| `src/units`                                              | clocks, NCO frequency/phase words, full-scale fractions                                                                                                                                             |
| `src/model`                                              | `SequenceModel`: blocks, RF/gradient/ADC events in clocks and physical units                                                                                                                        |
| `src/seq`                                                | `LoadSequence`: ExternalSequence → model (the host block loop)                                                                                                                                      |
| `src/shapes`                                             | RF / gradient waveform libraries (deduplicated, byte offsets)                                                                                                                                       |
| `src/tables`                                             | `BuildTables`: model → per-channel rows (Tx, Rx, GA/GB/GC) with `origin()` rebasing; `WriteExpect`: expected log times                                                                              |
| `src/realtime`                                           | `BuildProgram`: tables → PLATO `_sequence` (Tx, Rx, GA, GB, GC), unrolled or block loop. The only code that builds syntax trees.                                                                    |
| `src/plato`                                              | modules file, channel discovery, runner, acquisition pump → `.cfl`                                                                                                                                  |
| `src/main.cpp`                                           | `pulseq_plato.exe` (vendor-compatible CLI)                                                                                                                                                          |
| `src/tools/dump_main.cpp`                                | `pulseq_plato_dump`                                                                                                                                                                                 |
| `unit_tests/`                                            | ctest unit tests (units, loader, tables, program construction)                                                                                                                                      |
| `test/FINDINGS.md`                                       | measured PLATO behaviour (simulator probes, vendor reference, scanner hardware)                                                                                                                     |
| `experiments/`                                           | probes (p01–p08 Phase 0, p09 `origin()`, p10 gap stress)                                                                                                                                            |
| `test/seqs/`                                             | Pulseq 1.5 test sequences: `fid_v150`, `features_v150`, `gre_fromMRS_64` (the vendor GRE that imaged on the scanner), `long_fid_v150` (24 s, from `gen_long_fid.py`; pypulseq 1.5 in `build/pyenv`) |
| `test/*.sh`, `test/golden/`                              | simulator runners (`run_probe.sh`, `run_sim.sh`), reference setup, Phase 0/2 regressions and their golden logs                                                                                      |
| `test/compare.py`                                        | log checks: `expect` (lateness), `same`, `ref` (vs vendor), `cfl`                                                                                                                                   |




## How it works

One `pulseq_plato.exe` run goes through four stages. Stages 1–2 are plain C++ (no PLATO headers) and run once on the host. Stage 3 builds the realtime programs, and stage 4 loads and runs them.

```
.seq --(1) LoadSequence--> SequenceModel --(2) BuildTables--> per-channel row tables
                                                             + waveform libraries
     --(3) BuildProgram--> _sequence: Tx, Rx, GA, GB, GC --(4) plato::Run--> <o>_0.cfl
```



### 1. Parse: `.seq` → `SequenceModel` (`src/seq/seq_loader.cpp`)

- The upstream Pulseq reader (`pulseq/ExternalSequence`, unmodified) does the file parsing: `es.load(path)` and then, for every block, `GetBlock(iB)` + `decodeBlock()`, the same loop as upstream `parsemr.cpp`.
- Files older than Pulseq 1.5 are refused. The rasters come from `[DEFINITIONS]`.
- For each block the loader:
  - refuses rotation, trigger and soft-delay extensions with a clear error;
  - converts every time from µs to integer RT clocks (10 ns), and errors if a time is not a whole number of clocks;
  - keeps a running block start time `t` (int64 clocks from sequence start).
- Each event becomes a `model::RfEvent`, `GradEvent` or `AdcEvent` in physical units:
  - **RF:** amplitude (Hz), magnitude and phase shapes, frequency/phase offsets including the 1.5 ppm columns (`freqPPM·1e-6·f0`, `phasePPM·f0[MHz]`), and `start = block start + delay`.
  - **Gradients:** every kind becomes values at raster *edges*, piecewise-linear between corner points. A trapezoid's corners are its ramp and flat times. An extended trapezoid uses its time/shape points. An arbitrary shape uses `first`, its raster-centre samples, and `last`. A gradient that runs past the end of its block is an error.
  - **ADC:** number of samples, dwell (clocks), `start = block start + delay`, offsets.
- The result is fully resolved: absolute times and no IDs or compressed shapes left. `pulseq_plato_dump` prints it.



### 2. Tables and streams: model → rows + libraries (`src/tables`, `src/shapes`)

`BuildTables` turns the model into what the spectrometer reads: **one row table per channel**, in time order, and **one waveform library per channel type**.

**Waveform libraries** (`RfLibrary`, `GradLibrary`). Every distinct waveform is stored once:

- They are compared after quantisation to what the hardware sees (int16 amplitude, phase and gradient; int32 frequency).
- RF points are `fmt::Modulation` (8 B: amplitude, phase, frequency as fractions of full scale); gradient points are `fmt::Gradient` (4 B).
- Every RF waveform ends with an explicit zero point, because outputs hold their last value.
- A gradient that ends non-zero with no continuation gets a zero point and a warning.
- RF waveforms carry only the *shape* phase; the per-pulse phase goes into the row. So the 64 RF-spoiled pulses of the GRE share one 2001-point waveform (16 KB).
- Rows refer to waveforms by byte offset into the library stream.

**Rows.** Each row is 8 × 32-bit words = 32 B, one stream prefetch block. Words 0–1 are `kind` and `at`; time words are RT clocks. The other words depend on the kind:


| kind         | channel  | w2             | w3         | w4            | w5         | w6                      | w7         |
| ------------ | -------- | -------------- | ---------- | ------------- | ---------- | ----------------------- | ---------- |
| `RF` (1)     | Tx       | library offset | points − 1 | raster        | gate on    | gate off                | phase word |
| `GRAD` (2)   | GA/GB/GC | library offset | points − 1 | raster        |            |                         |            |
| `ADC` (3)    | Rx       | samples        | dwell      | NCO freq word | phase word | freq restore (0 = none) | CIC grid   |
| `ORIGIN` (4) | any      |                |            |               |            |                         |            |
| `END` (0)    | all      |                |            |               |            |                         |            |


How the rows are filled:

- **Absolute times.** Event times get the conventions in the table above: RF samples centred, gate at the block start, ADC latched at the end of each dwell step, and so on. Times are kept as absolute int64 clocks until the last step.
- **Phase bookkeeping.** RF and ADC frequency offsets make the NCO integrate extra phase. Each row's phase word subtracts the phase accumulated by earlier rows on that channel, so every pulse and ADC stays referenced to one continuous carrier at `-f`.
- **Gradient joints.** When one gradient ends on the raster edge where the next starts on the same axis, the first one drops its last point, so the joint value plays once.
- **Timing check.** The table builder checks the gap between consecutive rows on the same channel against the slack measured for the block loop (Tx 100, Rx 160, gradients 60 clocks). Rows that are too close go to `timingIssues`, and the run is refused unless `-allow-late` is given.
- **Rebasing.** `Rebase()` turns absolute times into 32-bit epoch-relative values. Once a row would reach more than 2³⁰ clocks past the channel's last `origin()`, an `ORIGIN` row is inserted in the gap before it. Later rows then count from `origin time + 10`.
- **END row.** Every table ends with an `END` row at the sequence end.

Example: the first rows of `gre_fromMRS_64.seq` (`pulseq_plato_dump -rows 3 …`):

```
Tx  0 RF   at=12050    lib=+0     points=2001 period=100  gate=[10000, 212100] ph16=0x0000
    1 RF   at=5012050  lib=+0     points=2001 period=100  gate=[5010000, 5212100] ph16=0x5333
GC  0 GRAD at=10000    lib=+28100 points=204  period=1000  (slice select)
Rx  0 ADC  at=352000   n=64 dwell=5000 ... grid=5000
```

**Streams.** In stage 3 each table becomes a `_stream<fmt::Raw>` (the flattened words) and each library a `_stream<fmt::Modulation>` or `_stream<fmt::Gradient>`. The PLATO runner loads them into the module's 1 GiB stream memory. A stream used by several channels of one module is loaded once, so GA, GB and GC share one gradient library. The GRE needs about 18 KB on the Tx module, 2 KB on Rx and 46 KB on the gradient module.

### 3. Realtime programs: one component per channel (`src/realtime/program.cpp`)

`BuildProgram` makes a `_sequence` with five components: **Tx** (transmitter), **Rx** (receiver, with the Transparent or CicCompensation FIR), and **GA, GB, GC** (logical gradients x, y, z). Each component runs on its own CPU in parallel. The components never talk to each other: they stay in step only because every timed call carries an absolute `at`, and each call blocks until its time.

Each component runs the same program shape:

```cpp
// init
setscale(32767)                                    // Tx, GA/GB/GC
setfreq(f0); setph(0); resetosc(0, 5000)           // Tx and Rx: coherent NCOs before t = 0
seek(library)                                      // a stream must be seek'd to be loaded
// block loop: the code size doesn't depend on the sequence length
pos = 0
for (i = 0; i < rows; ++i) {
  seek(table + pos)                                // rf()/gradient() move the stream pointer
  kind = read(); at = read()
  switch (kind) {                                  // only the kinds this table contains
    case RF/GRAD/ADC: w2.. = read(); <RowCalls>
    case ORIGIN:      origin(at)
    case END:         resume(at); exit(0)
  }
  pos += 32
}
```

`RowCalls`, the timed calls per row:


| row  | calls                                                                                               |
| ---- | --------------------------------------------------------------------------------------------------- |
| RF   | `setph(w7, gateOn − 50)`; `unblank(gateOn)`; `rf(lib + w2, w3, w4, at)`; `blank(gateOff)`           |
| GRAD | `gradient(lib + w2, w3, w4, at)`                                                                    |
| ADC  | `setfreq(w4, at − 51)`; `setph(w5, at − 51)`; `acquire2(w2, w3, w7, at)`; `if (w6) setfreq(f0, w6)` |


Each RF call has its own clock. The ADC lead makes `acquire2` execute, and so reset the CIC grid, exactly at `at`.

How a row plays:

- `rf()` and `gradient()` play `w3 + 1` library points, one every `period` clocks, starting at `at`.
- `acquire2` latches one sample every `dwell` clocks from `at + dwell`, then calls `complete()`, which closes one acquisition for the data pump.
- The engine adds a fixed 10-clock latency to every timed call.
- While a waveform's last points play, the CPU is already preparing the next row. This is why rows need the slack above.

There is also a debug form, `-emit unrolled`: one straight-line block per row, filling the same variables with literals instead of stream reads, then running the same `RowCalls`. It only works for small tables, and it gives a simulator log identical to the block loop's.

### 4. Execution on the spectrometer (`src/plato/session.cpp`)

1. **Connect.** `CreateSpectrometer`, then `CreateModule` for each line of the `-m` file (`name ip port`). The first module is the master.
2. **Map.** Each module is asked what it has. Tx goes on the first module with a transmitter, Rx on the first with a receiver, and GA/GB/GC on the first with gradient partials (`ModuleChannel::Transmitter(0)`, `Receiver(0)`, `GradientA/B/C(0)`).
3. **Compile and load.** `CreateSequenceRunner` compiles the five programs and loads code, data and streams. Code/data sizes are printed per component (instruction memory is 8 KB per channel; the block loop uses under 900 B).
4. **Acquire.** Before `Run()`, an `AcquisitionDataPump` connects to the Rx data source on a worker thread. Each `complete()` arrives as an acquisition (one or more fragments): 24-bit big-endian I/Q pairs. These are reordered by acquisition ID and written as complex64 to `<o>_0.cfl`, or to stdout with `-stdout`.
5. **Run and wait.** `Run()` starts every component together. The host polls `GetExitStatus` until all components exit (timeout: duration + 10 s). It then reports exit codes and timing, division-by-zero and gradient-overrun flags. Finally it waits until the pump has every expected sample, and fails if any is missing.

