<!--
SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# SDRAM benchmark

This application targets `ek_ra8p1/r7ka8p1kflcac/cm85`. It verifies the
complete 64 MiB external SDRAM and then characterizes it. Results are in
[TEST_REPORT.md](TEST_REPORT.md).

| # | Validation point | Suite (Kconfig) | Tests |
| --- | --- | --- | --- |
| 1 | Read bandwidth | `SDRAM_BENCH_CPU`, `SDRAM_BENCH_DMA` | `read32`, `read64`, copies from SDRAM, DMA reads |
| 2 | Write bandwidth | `SDRAM_BENCH_CPU`, `SDRAM_BENCH_DMA` | `write32`, `write64`, `memset`, copies to SDRAM, DMA writes |
| 3 | Read/write together | `SDRAM_BENCH_ACCESS`, `SDRAM_BENCH_CPU` | `mix_r75_w25`, `mix_r50_w50`, `mix_r25_w75`, `rmw32`, SDRAM-to-SDRAM copies |
| 4 | Sequential access | `SDRAM_BENCH_CPU` | all `cpu` tests walk 4 MiB in address order |
| 5 | Random access | `SDRAM_BENCH_ACCESS` | `rand_read32_*`, `rand_write32_*`, pointer chase `chase_*`, `stride_*` sweep |
| 6 | Burst transfers | `SDRAM_BENCH_ACCESS`, `SDRAM_BENCH_DMA` | access-size sweep (8/16/32/64-bit, 8-word LDM/STM), block-size sweep, cache-line (32 B) fills, DMA unit and block sweeps |
| 7 | Sustained bandwidth | `SDRAM_BENCH_SUSTAINED_SEC` | write/read/copy cycles for a set time with every cycle checked |
| 8 | Concurrent bus masters | `SDRAM_BENCH_CONCURRENT`, `SDRAM_BENCH_CM33`, `SDRAM_BENCH_GLCDC` | CPU alone vs. with the DMAC, the CM33, GLCDC scan-out, and all of them together |

Before any measurement, the integrity test checks the whole device with the
D-cache off: data bus walking 1s and 0s, address bus, address as data,
inverted address, both checkerboard patterns, pseudo-random and
inverted-random patterns (a different xorshift64 seed per pass), and a
March C- run (w0, r0w1r1 x2, r1w0r0 x2, r0w1r1, r1w0r0) over all 64 MiB.
The pass count is `CONFIG_SDRAM_BENCH_INTEGRITY_PASSES`. If
`CONFIG_SDRAM_BENCH_RETENTION_SEC` is nonzero, a data-retention test then
writes the full device, idles for that many seconds (refresh only), and
verifies each 1 MiB block. If any step fails, throughput is not measured.

## Test descriptions

All CPU tests run twice, with the D-cache on and with it switched off at
runtime. Each test is repeated 8 times (`CONFIG_SDRAM_BENCH_ITERATIONS`),
with the cache cleaned and invalidated before every pass and interrupts
locked during the timed region. Unless stated otherwise, a pass moves
4 MiB. Data written by write, copy, mixed, random-write, block-write and DMA
tests is checked after the run, and a mismatch counts as a failure.

| Group | Test | What it does |
| --- | --- | --- |
| `cpu` | `write32`/`write64`/`memset`, `read32`/`read64` | sequential stores/loads (unrolled 32 B per iteration), picolibc `memset()` |
| `cpu` | `memcpy_*`, `copy64_*` | picolibc `memcpy()` (byte loop in this toolchain) and an `LDRD`/`STRD` loop: SDRAM to SRAM, SRAM to SDRAM, SDRAM to SDRAM |
| `ref` | `sram_*`, `cachehit_sdram_read32` | the same kernels on internal SRAM; 16 KiB of SDRAM re-read from the cache |
| `mixed` | `mix_rXX_wYY` | per 8-word group: XX% of the words read from window A, YY% written to window B |
| `mixed` | `rmw32` | `p[i] += 1` over 4 MiB; checked against the number of passes |
| `random` | `rand_read32_<win>` | 262144 32-bit reads at xorshift32 word indices in a 16 KiB, 4 MiB or 64 MiB window; ns/op per access |
| `random` | `rand_write32_<win>` | the same for writes (value depends on the index, so it can be checked) |
| `latency` | `chase_<win>` | dependent-load pointer chase over a full-period LCG permutation of 64-byte nodes; ns/op = load-to-use latency |
| `stride` | `stride_<S>` | one 32-bit read every S bytes (S = 4 B to 256 KiB), up to 65536 reads, within 64 MiB |
| `size` | `read8..read64`, `read_ldm8x32`, `write8..write64`, `write_stm8x32` | access-width sweep over 4 MiB; LDM/STM move 32 B per instruction |
| `block` | `block_read_<B>`, `block_write_<B>` | 4 MiB moved as B-byte blocks (32 B to 128 KiB) at random block-aligned SDRAM offsets |
| `alt` | `alternate_2^<k>` | sequential reads alternating between two 4 KiB windows 2^k bytes apart (32 B to 32 MiB) |
| `align` | `align_*_o<off>` | sequential read/write sweeps starting at byte offset 0-3 (and offset 4 for 64-bit) |
| `wset` | `read32_<win>`, `write32_<win>`, `copy64_<win>` | sequential passes inside a working set of 4 KiB to 32 MiB; shows cache-level knees |
| `stream` | `stream_copy/scale/add/triad_32|_f32` | STREAM-style kernels over 4 MiB operands, integer and float; `_unc` variants run uncached |
| `dma` | `dma_*` | DMAC channel 0: SRAM to SDRAM, SDRAM to SRAM, SDRAM to SDRAM; transfer-unit sweep (1/2/4 B) and block-size sweep (1 KiB to 128 KiB) |
| `dma` | `dma_sram_to_crc`, `dma_sdram_to_crc`, `dma_crc_to_sdram` | DMA to/from the CRC engine data register (memory<->peripheral); the DMA-fed CRC is compared with a CPU-fed CRC (`DMACRC,...,match`) |
| `dma` | `dma_mch2_*`, `dma_mch4_*` | 2 and 4 DMAC channels copying SDRAM to SDRAM concurrently |
| `conc` | `CONC,...` lines | see [Concurrent tests](#concurrent-tests) |
| `sust` | `SUST,...` lines | see [Sustained test](#sustained-test) |

### Concurrent tests

The CPU kernels `write32`, `read32` and `copy64_sdram_to_sdram` (cache on,
interrupts enabled) are measured alone. They are then measured while each
other bus master runs:

| Master | Traffic | How its throughput is measured |
| --- | --- | --- |
| `dma_sdram_to_sdram` | DMAC channel 1, window C to D in 16 KiB blocks, re-armed from its interrupt | completed blocks |
| `cm33_read`/`cm33_write`/`cm33_copy` | `cm33_traffic` image on the CM33 (250 MHz, no data cache), 4 MiB windows at 32 MiB | blocks reported through an SDRAM mailbox |
| `glcdc_scanout` | GLCDC scanning a 1024x600 RGB565 frame buffer at 57.4 frames/s | frame size divided by the frame period, which is measured from the GLCDC line-detect flag |
| `all_masters` | DMAC + CM33 copy + GLCDC at the same time | sum of the above |

The scenario matrix runs first: `ref_s1_cpu_read`,
`ref_s2_cpu_write` and `ref_s3_mixed` (CPU + background DMAC, no display),
then `ref_s4_display` and `ref_s5_display` (the same plus GLCDC scan-out,
started last because it cannot be stopped).

Each scenario prints an `idle` line (the master alone, CPU sleeping) and one
line per CPU kernel. A line shows:

- CPU throughput alone and loaded, and the change in percent,
- the master's throughput alone and loaded,
- total SDRAM bus traffic, counting copies twice,
- `other_status`.

After a scenario stops, the DMAC and CM33 destination windows are compared
with their sources. The GLCDC frame period is measured again and must be
within 1% of the idle value.

GLCDC underflow is checked during every CPU kernel run. An underflow is a
performance result, not a data error: it is reported as
`other_status=glcdc_underflow` and counted as a warning, and the run is not
failed.

### Sustained test

`CONFIG_SDRAM_BENCH_SUSTAINED_SEC` seconds of write/read/copy cycles.
With `CONFIG_SDRAM_BENCH_SUSTAINED_FULL` (used by `overlay-stress.conf`)
the cycles rotate over fifteen 4 MiB windows covering 60 MiB, each cycle
writes a new pseudo-random pattern, and data errors are logged as
`SUSTERR` lines. Without it, the cycles run over windows A to D (16 MiB)
with a fixed pattern. Each cycle:

1. `write32` to window w, then check the window,
2. `read32` window w,
3. `copy64` window w to window w+1, then check the copy.

Every `CONFIG_SDRAM_BENCH_SUSTAINED_REPORT_SEC` seconds a `SUST` line gives
the interval averages and the error count. At the end:

- `SUSTSUM` lines give, per operation, the first and last interval and the
  drift (in 0.1% units),
- the `sust` table rows give the per-pass min/avg/max for the whole run.

## Theory and units

- SDCLK = BCLK = 1 GHz / 8 = 125 MHz. Both clocks are read from the CGC at
  runtime. The 32-bit data bus gives a theoretical **500 MB/s**.
- MB/s means 10^6 bytes per second. `%peak` = avg MB/s x SDRAM bus bytes per
  counted byte / 500. The factor is 2 for SDRAM-to-SDRAM copies and
  read-modify-write.
- `ns/op` is the average time per access: per random access, per strided
  read, per dependent load, or per DMA transfer unit.

## Cache and MPU behaviour

- The RA8P1 SoC defaults leave `CONFIG_DCACHE` and `CONFIG_CACHE_MANAGEMENT`
  off. This application enables both and switches the D-cache at runtime.
- The `sdram1` node has no `zephyr,memory-attr`. The MPU runs with
  `PRIVDEFENA`, so SDRAM is Normal, cacheable memory under the Armv8-M
  default map.
- The D-cache is 16 KiB (128 sets x 4 ways x 32 B, read from `CCSIDR`). The
  policy is write-through (`CPU_RA_DCACHE_FORCE_WRITETHROUGH`) unless
  `overlay-writeback.conf` is used. With the cache on, write tests include
  `sys_cache_data_flush_all()` in the timed region.
- The `cachehit_sdram_read32`, `rand_read32_16k` and `chase_16k` tests show
  cache-hit results, for comparison with the SDRAM results.

## SDRAM layout

| Region | Address | Used by |
| --- | --- | --- |
| A, B | `0x68000000`, `0x68400000` (4 MiB each) | CPU tests (B is the copy target) |
| C, D | `0x68800000`, `0x68C00000` | background DMA source and target |
| FB0, FB1 | `0x69000000`, `0x69200000` | GLCDC frame buffers (1.2 MB each) |
| CM33 | `0x6A000000` (source), `0x6A400000` (target) | CM33 traffic generator |
| Mailbox | `0x6BFFF000` | CM85/CM33 command and status (one 32-byte cache line per side) |

The random-write, chase, stride and block tests cover all 64 MiB. They run
before any other bus master is started. The layout is defined in
`include/sdram_bench_shared.h`, which both images include.

## Configuration

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_SDRAM_BENCH_BUF_SIZE` | 4194304 | bytes per pass (1 to 4 MiB) |
| `CONFIG_SDRAM_BENCH_ITERATIONS` | 8 | timed iterations per test |
| `CONFIG_SDRAM_BENCH_INTEGRITY` | y | full-device integrity test first |
| `CONFIG_SDRAM_BENCH_INTEGRITY_PASSES` | 1 | pseudo-random pattern passes |
| `CONFIG_SDRAM_BENCH_RETENTION_SEC` | 0 | data-retention interval (0 disables) |
| `CONFIG_SDRAM_BENCH_CPU` | y | sequential CPU suite |
| `CONFIG_SDRAM_BENCH_ACCESS` | y | mixed, random, latency, stride, size and block suites |
| `CONFIG_SDRAM_BENCH_DMA` | n | DMAC suite |
| `CONFIG_SDRAM_BENCH_DMA_CHUNK` | 131072 | block size of the base DMA tests |
| `CONFIG_SDRAM_BENCH_CONCURRENT` | n | concurrent suite (DMAC background) |
| `CONFIG_SDRAM_BENCH_CM33` | n | add the CM33 master (set by the sysbuild build) |
| `CONFIG_SDRAM_BENCH_GLCDC` | n | add GLCDC scan-out (`overlay-glcdc.conf` + `glcdc.overlay`) |
| `CONFIG_SDRAM_BENCH_SUSTAINED_SEC` | 0 | sustained test duration, 0 disables |
| `CONFIG_SDRAM_BENCH_SUSTAINED_FULL` | n | rotate over 15 windows (60 MiB) with a fresh random pattern each cycle |
| `CONFIG_SDRAM_BENCH_SUSTAINED_REPORT_SEC` | 10 | sustained report interval |

| File | Purpose |
| --- | --- |
| `prj.conf` | base configuration |
| `overlay-dma.conf` | add the DMAC suite |
| `overlay-writeback.conf` | D-cache write-back |
| `overlay-concurrent.conf` | concurrent suite only (CPU and access suites off) |
| `overlay-glcdc.conf` + `glcdc.overlay` | GLCDC scan-out in the concurrent suite |
| `overlay-sustained.conf` | 600 s sustained run only |
| `overlay-integrity.conf` | 3 integrity passes + 300 s retention |
| `overlay-stress.conf` | 3600 s full-device stress (`SUSTAINED_FULL`) |
| `sysbuild.cmake`, `cm33_traffic/` | CM33 traffic generator image; enables the CM33 tests |

Other settings in `prj.conf`:

- `CONFIG_SOC_RA_ENABLE_START_SECOND_CORE=n`: the CM33 stays in reset unless
  the concurrent suite starts it. The suite writes the CPU1 start registers
  directly, after the mailbox is initialized. An old CM33 image in MRAM once
  reset the system every 4 s.
- `CONFIG_UART_INTERRUPT_DRIVEN=y`: without it, `uart_renesas_ra8_sci_b.c`
  does not compile in this tree.
- `CONFIG_SPEED_OPTIMIZATIONS` is not used. At `-O2`, GCC puts FPU
  instructions in FSP `bsp_clock_init()`, which runs before the FPU is
  enabled, and the board faults at boot (UsageFault NOCP). Only the
  benchmark sources are built with `-O2`.

The `cm33_traffic` Kconfig defines placeholders for the `OS_*` symbols that
the CM33 board defconfig assigns. These symbols do not exist in this
workspace, and without the placeholders every CM33 build aborts.

The board overlay enables `dma0` with channel interrupts 88 and 89.

## Build and flash

From the west workspace root, with `source .venv/bin/activate`:

```sh
APP=sdram_benchmark
BOARD=ek_ra8p1/r7ka8p1kflcac/cm85

# 1. sequential + access suites, write-through (about 60 s)
west build -p always -b $BOARD $APP -d $APP/build

# 2. same with write-back D-cache
west build -p always -b $BOARD $APP -d $APP/build_wb -- -DEXTRA_CONF_FILE=overlay-writeback.conf

# 3. plus the DMAC suite
west build -p always -b $BOARD $APP -d $APP/build_dma -- -DEXTRA_CONF_FILE=overlay-dma.conf

# 4. concurrent bus masters: CM85 + CM33 images, DMAC, CM33 and GLCDC
west build --sysbuild -p always -b $BOARD $APP -d $APP/build_conc -- \
  "-Dsdram_benchmark_EXTRA_CONF_FILE=overlay-concurrent.conf;overlay-glcdc.conf" \
  -Dsdram_benchmark_EXTRA_DTC_OVERLAY_FILE=glcdc.overlay

# 5. sustained, 600 s
west build -p always -b $BOARD $APP -d $APP/build_sust -- -DEXTRA_CONF_FILE=overlay-sustained.conf

# 6. integrity: 3 passes + 300 s retention (about 10 min)
west build -p always -b $BOARD $APP -d $APP/build_integrity -- -DEXTRA_CONF_FILE=overlay-integrity.conf

# 7. stress: 3600 s over 60 MiB (about 65 min)
west build -p always -b $BOARD $APP -d $APP/build_stress -- -DEXTRA_CONF_FILE=overlay-stress.conf

west flash -d $APP/build      # build_conc flashes the CM33 image first, then the CM85 image
```

## Capturing results

The console is `uart0` at 115200 baud on the FTDI adapter (`/dev/ttyUSB0`).
The capture script opens the port, flashes the build, and records output
until the final `sdram_benchmark PASSED|FAILED` line. It writes:

- the log,
- `RESULT` lines as CSV,
- `CONC`, `SUST` and `SUSTSUM` lines, if present, to `<stem>_conc.csv`,
  `<stem>_sust.csv` and `<stem>_sustsum.csv`.

```sh
cd $APP
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build      --log results/cpu.log        --csv results/cpu.csv
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build_wb   --log results/writeback.log  --csv results/writeback.csv
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build_dma  --log results/dma.log        --csv results/dma.csv
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build_conc --log results/concurrent.log --csv results/concurrent.csv
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build_sust --log results/sustained.log  --csv results/sustained.csv --timeout 720
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build_integrity --log results/integrity.log --csv results/integrity.csv --timeout 720
python3 scripts/capture_uart.py --port /dev/ttyUSB0 --flash build_stress --log results/stress.log --csv results/stress.csv --timeout 3900
```

Exit codes: 0 for `PASSED`, 1 for `FAILED`, 2 for timeout (default 180 s;
the default build needs `--timeout 400`).

## Output format

```text
group   test                       cache  min MB/s  avg MB/s  max MB/s  %peak     ns/op
cpu     write32                    on        244.7     244.7     244.7    48%         -
random  rand_read32_4m             on         17.6      17.7      17.7     3%     225.9
...
RESULT,group,test,cache,bytes,iters,min_MBps,avg_MBps,max_MBps,pct_of_peak,ns_per_op,time_ms,MiBps,sd_MBps,cv_pct,pmu_refill,pmu_bus,pmu_mem,pmu_stall_be,flag
CONC,scenario,cpu_test,cpu_alone_MBps,cpu_loaded_MBps,...,other_status
SUST,t_s,cycles,write_MBps,read_MBps,copy_MBps,errors
SUSTERR,cycle,window,op[,address,expected,actual]
SUSTSUM,op,intervals,first_MBps,last_MBps,drift_pct_x10,interval_min_MBps,interval_max_MBps
sdram_benchmark PASSED (0 failures, 3 warnings)

The RESULT row adds the measured pass time (time_ms), MiB/s, per-pass
standard deviation (sd_MBps) and coefficient of variation (cv_pct). Rows
with cv_pct > 5 are flagged VAR. The pmu_* columns are PMU event counts;
they are n/a on this device (see Limitations).
```

## Limitations

- **GLCDC scan-out.** Only the GLCDC is used; no panel, bridge or DSI host
  is enabled. The display API cannot stop scan-out, so the GLCDC tests run
  last.
- **GLCDC driver.** On this board the Zephyr GLCDC driver's
  `display_write()` vsync wait completes about 180,000 times per second
  instead of once per frame. The benchmark therefore disables the
  line-detect interrupt after starting scan-out, and takes timing from the
  hardware flag.
- **DMA.** The Zephyr RA DMAC driver supports only normal mode with software
  start and single blocks of up to 65535 units.
- **Clock and temperature.** SDCLK is taken from the CGC registers and has
  not been measured on the pin. Temperature is not logged; there is no
  die-temperature driver. Only room temperature was tested; temperature
  and voltage corners need a chamber or supply variation and are listed in
  TEST_REPORT.md as not covered.
- **PMU counters.** The Cortex-M85 PMU is present (8 event counters) but
  does not count on this device: a software-increment self-test fails.
  The benchmark prints the PMU identification values at boot and reports
  counter data as `n/a`.

## Troubleshooting

- **No output.** Halt with J-Link and read `CFSR` (`0xE000ED28`).
  `0x00080000` means FSP startup code was built with FPU instructions; do
  not enable `CONFIG_SPEED_OPTIMIZATIONS`.
- **The banner repeats every few seconds.** Something resets the system.
  Check that `CONFIG_SOC_RA_ENABLE_START_SECOND_CORE=n` and read `RSTSR1`
  (`0x4001E0C0`).
- **`cm33_* start failed (-116)`.** The CM33 did not answer within 1 s.
  Flash the sysbuild build (`build_conc`) so the `cm33_traffic` image is at
  `0x020C0000`.
- **`glcdc_scanout start failed`.** `glcdc.overlay` or
  `CONFIG_RENESAS_RA_GLCDC_FB_NUM=0` is missing.
