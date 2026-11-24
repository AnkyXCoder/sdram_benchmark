<!--
SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# SDRAM validation test report: EK-RA8P1

| Item | Value |
| --- | --- |
| Date | 2026-11-23 |
| Result | **PASS**: integrity and every data check pass in all builds; 5 concurrent GLCDC measurements report underflow (warning) |
| Expectation check | 257 meet, 0 better than expected, 26 below expectation (explained in [Deviations](#deviations-from-expectations)) |
| Application | `sdram_benchmark` (see [README.md](README.md)) |
| Raw data | `results/*.log` and `results/*.csv` |

## Summary by validation point

| # | Point | Key results (D-cache on, write-through, unless noted) | vs expectation |
| --- | --- | --- | --- |
| 1 | Read bandwidth | sequential 187.1 MB/s cached, 46.9 (32-bit) / 70.4 (64-bit) uncached; DMA 31.2 | Meets |
| 2 | Write bandwidth | 244.7 MB/s for any width, cache on or off; DMA 55.0 | Meets |
| 3 | Read/write together | 75/50/25% reads: 81.3 / 72.5 / 53.1 MB/s; RMW 88.1; 64-bit SDRAM copy 55.5 (WT) / 74.1 (WB) | **Below** for cached mixes |
| 4 | Sequential access | as 1 and 2: 48% (write) and 37% (cached read) of the 500 MB/s peak | Meets |
| 5 | Random access | random 32-bit read 17.3 MB/s cached (230.3 ns), 35.4 uncached (112.7 ns); load-to-use latency 227.1 ns cached, 112.4 ns uncached; random write 55.0 MB/s uncached | Mostly meets; uncached strided reads and cached 64 MiB random writes below |
| 6 | Burst transfers | 32-byte line fill 169.3 ns (sequential) / 225.2 ns (non-adjacent); LDM 8-word uncached 70.4 MB/s = LDRD; 32 B random blocks 105.1 MB/s read, full speed from 8 KiB (read) / 2 KiB (write); DMA time per unit is constant (72.6 ns write, 128.1 ns read) | Mostly meets; uncached LDM gets no burst |
| 7 | Sustained bandwidth | 3600 s, 18468 cycles, 0 errors; interval spread write 0.00%, read 0.00%, copy 0.00% | Meets |
| 8 | Concurrent masters | CPU keeps priority: CPU write 194.0 MB/s with DMA, which drops from 22.5 to 3.4; CM33 write gets 0.0 MB/s while the CPU writes; GLCDC 70.5 MB/s scan-out underflows during CPU copies | **Below**: other masters starve; GLCDC underflow |

## Test setup

| Parameter | Result |
| --- | --- |
| Board | EK-RA8P1 (`ek_ra8p1/r7ka8p1kflcac/cm85`) |
| Processor | R7KA8P1KFLCAC: Cortex-M85 r1p1 at 1 GHz (benchmark); Cortex-M33 at 250 MHz (traffic generator, concurrent build only) |
| SDRAM type | SDR (single data rate); part number not given in the board files |
| SDRAM capacity | 64 MiB at `0x68000000` |
| Data bus width | 32 bits |
| SDRAM clock | 125 MHz (BCLK = SDCLK, read from the CGC at runtime) |
| Data rate | 125 MT/s, 1 transfer per clock |
| Theoretical bandwidth | 500 MB/s = 32 bit / 8 x 125 MHz x 1 |
| CPU frequency | 1000 MHz |
| Cache configuration | L1 D-cache 16 KiB, 4-way, 32-byte lines; write-through (default) and write-back builds |
| SDRAM controller registers | SDCCR=0x11 SDCMOD=0x00 SDAMOD=0x01 SDADR=0x01 SDTR=0x00052503 SDMOD=0x0230 SDRFCR=0x73a8 SDRFEN=0x01 SDSR=0x00 (printed at boot) |
| SDRAM timing | CL 3, tRCD 3, tRP 3, tWR 2, tRAS 6 (SDCLK); mode-register burst length 1, CAS 3; refresh count 937 (~7.5 us); 9-bit column shift (2 KiB rows); continuous access |
| Hardware performance counters | The SDRAM controller has none. The Cortex-M85 PMU (8 event counters) is present but does not count: the software-increment self-test fails on this device (PMU TYPE 0x00a05f08, AUTHSTATUS 0x00ff00ff). PMU data is reported as n/a. |
| Firmware version | `87af1361a7b0-dirty` (Zephyr v4.5.0 based tree, Zephyr SDK 1.0.1, GCC 14.3.0, picolibc; image `-Os`, benchmark sources `-O2`) |
| Iterations | 8 passes per test unless noted |
| Temperature | Room temperature only; no die-temperature driver exists. Temperature and voltage corners were not performed (see "Not covered"). |
| Debug / console | SEGGER J-Link (SWD); FTDI FT232R `/dev/ttyUSB0`, 115200 baud |

| Build | Configuration | Log |
| --- | --- | --- |
| `build` | sequential + access suites, write-through; run twice | `cpu.log`, `cpu_run2.log` |
| `build_wb` | same, write-back (`overlay-writeback.conf`) | `writeback.log` |
| `build_dma` | sequential + access + DMA suites (`overlay-dma.conf`) | `dma.log` |
| `build_integrity` | 3 integrity passes + 300 s data retention (`overlay-integrity.conf`) | `integrity.log` |
| `build_conc` | sysbuild: CM85 concurrent suite with DMAC, CM33 and GLCDC + `cm33_traffic` CM33 image | `concurrent.log` |
| `build_stress` | 3600 s full-device stress (`overlay-stress.conf`) | `stress.log` |

All throughput figures are in MB/s (10^6 B/s), averaged over 8 passes of
4 MiB unless noted; the CSV rows also carry measured time per pass, MiB/s
and standard deviation. `%peak` counts SDRAM bus bytes against 500 MB/s.
`cv%` is the coefficient of variation of the per-pass throughput; rows above
5% are flagged `VAR` and discussed in [Consistency](#consistency).

## Expectations

Expectations were written down before the extended runs (and kept unchanged
afterwards). They come from two sources:

- **baseline**: tests carried over from the first report are expected to
  repeat their previous result within 5% (regression check);
- **theory**: new tests are predicted from SDCLK 125 MHz, CL3/tRCD3/tRP3,
  2 KiB rows, and the first report's per-access costs. Those costs are a
  posted write at 16.3 ns per 32-bit word, a cached 32-byte line fill at
  171 ns, an uncached single read at 85 ns, and a row change adding
  tRP + tRCD = 48 ns.

A result is **Meets** inside the range, **Better** if faster (or lower
latency), and **Below** if slower.

## Integrity

All runs passed the full 64 MiB integrity test with the D-cache off: data bus
walking 1/0, address bus, address as data, inverted address, and both
checkerboards. Each fill pattern takes about 2.17 s (write and verify of
64 MiB). In total 7 captures were made, each starting
with this test.

## 1, 2, 4: Sequential read and write bandwidth

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |
| `write32` | on | 232.5-256.9 | 244.7 | 244.7-244.7 | Meets |
| `write64` | on | 232.5-256.9 | 244.7 | 244.7-244.7 | Meets |
| `memset` | on | 232.5-256.9 | 244.7 | 244.7-244.7 | Meets |
| `read32` | on | 177.7-196.5 | 187.1 | 187.1-187.1 | Meets |
| `read64` | on | 177.2-195.8 | 186.5 | 186.4-186.5 | Meets |
| `memcpy_sdram_to_sram` | on | 173.0-191.2 | 179.4 | 179.4-179.5 | Meets |
| `memcpy_sram_to_sdram` | on | 73.8-83.2 | 75.2 | 73.4-75.9 | Meets |
| `memcpy_sdram_to_sdram` | on | 44.1-48.7 | 47.5 | 47.4-47.5 | Meets |
| `copy64_sdram_to_sram` | on | 176.7-195.3 | 186.2 | 186.1-186.2 | Meets |
| `copy64_sram_to_sdram` | on | 134.2-148.4 | 118.9 | 118.9-119.1 | **Below** |
| `copy64_sdram_to_sdram` | on | 51.4-56.8 | 55.5 | 55.3-55.6 | Meets |
| `write32` | off | 232.6-257.0 | 244.8 | 244.8-244.8 | Meets |
| `write64` | off | 232.6-257.0 | 244.8 | 244.8-244.8 | Meets |
| `memset` | off | 232.5-256.9 | 244.8 | 244.8-244.8 | Meets |
| `read32` | off | 44.6-49.2 | 46.9 | 46.9-46.9 | Meets |
| `read64` | off | 66.9-73.9 | 70.4 | 70.4-70.4 | Meets |
| `memcpy_sdram_to_sram` | off | 8.4-9.2 | 8.8 | 8.8-8.8 | Meets |
| `memcpy_sram_to_sdram` | off | 13.0-14.4 | 13.7 | 13.7-13.7 | Meets |
| `memcpy_sdram_to_sdram` | off | 5.9-6.5 | 6.2 | 6.2-6.2 | Meets |
| `copy64_sdram_to_sram` | off | 66.9-73.9 | 70.4 | 70.4-70.4 | Meets |
| `copy64_sram_to_sdram` | off | 227.4-251.4 | 242.3 | 242.3-242.3 | Meets |
| `copy64_sdram_to_sdram` | off | 47.0-52.0 | 49.5 | 49.5-49.5 | Meets |

References (internal SRAM and cache hits):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |
| `sram_write32` | on | 880.1-972.7 | 926.6 | 925.8-927.2 | Meets |
| `sram_read32` | on | 1889.4-2088.2 | 1983.0 | 1979.4-1989.4 | Meets |
| `cachehit_sdram_read32` | on | 2097.8-2318.6 | 2211.6 | 2202.4-2221.4 | Meets |
| `sram_write32` | off | 1264.3-1397.3 | 1329.6 | 1329.6-1329.6 | Meets |
| `sram_read32` | off | 155.0-171.4 | 163.2 | 163.2-163.2 | Meets |
| `cachehit_sdram_read32` | off | 44.6-49.2 | 46.9 | 46.9-46.9 | Meets |

Write-back D-cache (`build_wb`), cache on, where it differs from write-through by more than 3%:

| Test | Write-through | Write-back | Change |
| --- | ---: | ---: | ---: |
| `cpu/memcpy_sram_to_sdram` | 75.2 | 82.4 | +10% |
| `cpu/memcpy_sdram_to_sdram` | 47.5 | 58.4 | +23% |
| `cpu/copy64_sram_to_sdram` | 118.9 | 149.4 | +26% |
| `cpu/copy64_sdram_to_sdram` | 55.5 | 74.1 | +34% |
| `ref/sram_write32` | 926.6 | 1334.0 | +44% |
| `mixed/mix_r75_w25` | 81.3 | 73.8 | -9% |
| `mixed/mix_r25_w75` | 53.1 | 64.0 | +21% |
| `random/rand_read32_16k` | 327.8 | 275.5 | -16% |
| `random/rand_write32_4m` | 51.5 | 54.8 | +6% |
| `random/rand_write32_64m` | 23.0 | 54.7 | +138% |
| `wset/write32_4k` | 244.7 | 260.9 | +7% |
| `wset/copy64_4k` | 241.1 | 419.3 | +74% |
| `wset/copy64_16k` | 240.2 | 443.1 | +84% |
| `wset/copy64_64k` | 57.3 | 86.9 | +52% |
| `wset/copy64_256k` | 55.5 | 74.9 | +35% |
| `wset/copy64_1m` | 55.4 | 74.1 | +34% |
| `wset/copy64_4m` | 55.4 | 74.2 | +34% |
| `wset/copy64_16m` | 55.3 | 74.1 | +34% |
| `wset/copy64_32m` | 55.1 | 74.1 | +34% |
| `stream/copy_f32` | 94.9 | 116.9 | +23% |
| `stream/scale_f32` | 113.7 | 105.7 | -7% |
| `stream/add_f32` | 95.9 | 164.5 | +72% |
| `stream/copy_u32` | 95.0 | 116.9 | +23% |
| `stream/scale_u32` | 117.0 | 140.6 | +20% |
| `stream/add_u32` | 103.4 | 148.0 | +43% |
| `stream/triad_u32` | 103.8 | 164.6 | +59% |

DMAC (`build_dma`, dma0 channel 0, 4-byte units, 128 KiB blocks):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |
| `dma_sram_to_sdram` | on | 52.2-57.8 | 55.0 | 55.0-55.0 | 72.6 | Meets |
| `dma_sdram_to_sram` | on | 29.6-32.8 | 31.2 | 31.2-31.2 | 128.1 | Meets |
| `dma_sdram_to_sdram` | on | 21.4-23.6 | 22.5 | 22.5-22.5 | 177.2 | Meets |

## 3: Read/write together

Reads come from window A and writes go to window B, interleaved per 8-word group. `rmw32` reads and writes the same word, so it counts twice on the bus.

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |
| `mix_r75_w25` | on | 107.0-157.0 | 81.3 | 81.3-81.3 | **Below** |
| `mix_r50_w50` | on | 100.0-135.0 | 72.5 | 72.5-72.5 | **Below** |
| `mix_r25_w75` | on | 90.0-120.0 | 53.1 | 53.1-53.1 | **Below** |
| `rmw32` | on | 80.0-130.0 | 88.1 | 88.1-88.1 | Meets |
| `mix_r75_w25` | off | 44.0-74.0 | 45.1 | 45.1-45.1 | Meets |
| `mix_r50_w50` | off | 59.0-99.0 | 61.8 | 61.8-61.8 | Meets |
| `mix_r25_w75` | off | 89.0-149.0 | 78.9 | 78.9-78.9 | **Below** |
| `rmw32` | off | 30.0-50.0 | 23.4 | 23.4-23.4 | **Below** |

SDRAM-to-SDRAM copies are also read/write traffic: `copy64_sdram_to_sdram` 55.5 MB/s (22% of the bus), 74.1 with write-back.

## 5: Random access

262144 accesses per pass at xorshift32 word positions. ns/op is the time per access. With the cache on, every random read misses in the cache and fetches a whole 32-byte line.

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |
| `rand_read32_16k` | on | > 400 | 327.8 | 176.7-377.8 | 12.2 | **Below** |
| `rand_read32_4m` | on | 12.0-25.0 | 17.4 | 17.4-17.4 | 229.3 | Meets |
| `rand_read32_64m` | on | 12.0-25.0 | 17.3 | 17.3-17.3 | 230.3 | Meets |
| `rand_write32_4m` | on | 30.0-60.0 | 51.5 | 46.4-53.4 | 77.5 | Meets |
| `rand_write32_64m` | on | 30.0-60.0 | 23.0 | 14.5-46.5 | 173.2 | **Below** |
| `rand_read32_16k` | off | 35.0-59.0 | 35.4 | 35.4-35.4 | 112.7 | Meets |
| `rand_read32_4m` | off | 20.0-45.0 | 35.4 | 35.4-35.4 | 112.7 | Meets |
| `rand_read32_64m` | off | 20.0-45.0 | 35.4 | 35.4-35.4 | 112.7 | Meets |
| `rand_write32_4m` | off | 30.0-60.0 | 55.0 | 55.0-55.0 | 72.6 | Meets |
| `rand_write32_64m` | off | 30.0-60.0 | 55.0 | 55.0-55.0 | 72.6 | Meets |

Load-to-use latency (pointer chase, 65536 dependent loads):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |
| `chase_16k` | on | 0-10 ns | 660.6 | 639.5-682.7 | 6.0 | Meets |
| `chase_4m` | on | 180-260 ns | 17.6 | 17.6-17.6 | 227.1 | Meets |
| `chase_64m` | on | 180-260 ns | 17.6 | 17.6-17.6 | 227.1 | Meets |
| `chase_16k` | off | 90-140 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `chase_4m` | off | 90-140 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `chase_64m` | off | 90-140 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |

Stride sweep (one 32-bit read every S bytes, up to 65536 reads within 64 MiB):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |
| `stride_4` | on | 17-26 ns | 186.9 | 186.8-187.0 | 21.3 | Meets |
| `stride_8` | on | 34-52 ns | 93.6 | 93.6-93.7 | 42.6 | Meets |
| `stride_16` | on | 68-104 ns | 46.9 | 46.9-46.9 | 85.1 | Meets |
| `stride_32` | on | 150-190 ns | 23.6 | 23.6-23.6 | 169.3 | Meets |
| `stride_64` | on | 150-190 ns | 23.6 | 23.5-23.6 | 169.4 | Meets |
| `stride_128` | on | 150-190 ns | 17.7 | 17.7-17.7 | 225.2 | **Below** |
| `stride_256` | on | 150-190 ns | 17.7 | 17.7-17.7 | 225.2 | **Below** |
| `stride_512` | on | 150-190 ns | 17.7 | 17.7-17.7 | 225.2 | **Below** |
| `stride_1024` | on | 150-190 ns | 17.7 | 17.7-17.7 | 225.2 | **Below** |
| `stride_2048` | on | 190-250 ns | 17.7 | 17.7-17.7 | 225.2 | Meets |
| `stride_4096` | on | 190-250 ns | 17.7 | 17.7-17.7 | 225.2 | Meets |
| `stride_8192` | on | 190-250 ns | 17.7 | 17.7-17.7 | 225.2 | Meets |
| `stride_16384` | on | 190-250 ns | 17.7 | 17.7-17.7 | 225.2 | Meets |
| `stride_65536` | on | 190-250 ns | 17.7 | 17.7-17.7 | 225.3 | Meets |
| `stride_262144` | on | 190-250 ns | 17.7 | 17.7-17.8 | 224.9 | Meets |
| `stride_4` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_8` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_16` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_32` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_64` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_128` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_256` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_512` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_1024` | off | 70-100 ns | 35.5 | 35.5-35.5 | 112.4 | **Below** |
| `stride_2048` | off | 110-150 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `stride_4096` | off | 110-150 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `stride_8192` | off | 110-150 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `stride_16384` | off | 110-150 ns | 35.5 | 35.5-35.5 | 112.5 | Meets |
| `stride_65536` | off | 110-150 ns | 35.4 | 35.4-35.5 | 112.6 | Meets |
| `stride_262144` | off | 110-150 ns | 35.2 | 35.1-35.2 | 113.4 | Meets |

## 6: Burst transfers

Access-size sweep (4 MiB sequential; LDM/STM move 8 words, 32 bytes, per instruction):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |
| `read8` | on | 175.0-195.0 | 183.8 | 183.8-183.8 | Meets |
| `read16` | on | 175.0-195.0 | 184.4 | 184.4-184.4 | Meets |
| `read32` | on | 175.0-195.0 | 187.1 | 187.1-187.1 | Meets |
| `read64` | on | 175.0-195.0 | 186.4 | 186.4-186.5 | Meets |
| `read_ldm8x32` | on | 175.0-195.0 | 187.2 | 187.2-187.2 | Meets |
| `write8` | on | 230.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `write16` | on | 230.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `write32` | on | 230.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `write64` | on | 230.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `write_stm8x32` | on | 230.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `read8` | off | 9.0-15.0 | 8.8 | 8.8-8.8 | **Below** |
| `read16` | off | 18.0-29.0 | 23.4 | 23.4-23.4 | Meets |
| `read32` | off | 42.0-52.0 | 46.9 | 46.9-46.9 | Meets |
| `read64` | off | 63.0-77.0 | 70.4 | 70.4-70.4 | Meets |
| `read_ldm8x32` | off | 70.0-190.0 | 70.4 | 70.4-70.4 | Meets |
| `write8` | off | 230.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `write16` | off | 230.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `write32` | off | 230.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `write64` | off | 230.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `write_stm8x32` | off | 230.0-250.0 | 244.8 | 244.8-244.8 | Meets |

Block-size sweep (4 MiB moved as blocks at random block-aligned positions in 64 MiB):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |
| `block_read_32` | on | 130.0-150.0 | 105.1 | 105.1-105.1 | **Below** |
| `block_read_128` | on | 140.0-170.0 | 71.4 | 71.4-71.4 | **Below** |
| `block_read_512` | on | 150.0-185.0 | 135.9 | 135.9-135.9 | **Below** |
| `block_read_2048` | on | 175.0-190.0 | 173.8 | 173.7-173.9 | **Below** |
| `block_read_8192` | on | 180.0-190.0 | 187.0 | 186.9-187.0 | Meets |
| `block_read_32768` | on | 180.0-190.0 | 187.0 | 186.9-187.0 | Meets |
| `block_read_131072` | on | 180.0-190.0 | 187.1 | 187.1-187.1 | Meets |
| `block_write_32` | on | 150.0-180.0 | 179.2 | 179.2-179.2 | Meets |
| `block_write_128` | on | 180.0-230.0 | 225.3 | 225.3-225.3 | Meets |
| `block_write_512` | on | 220.0-245.0 | 240.6 | 240.6-240.6 | Meets |
| `block_write_2048` | on | 235.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `block_write_8192` | on | 235.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `block_write_32768` | on | 235.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `block_write_131072` | on | 235.0-250.0 | 244.7 | 244.7-244.7 | Meets |
| `block_read_32` | off | 58.0-75.0 | 58.3 | 58.3-58.3 | Meets |
| `block_read_128` | off | 63.0-75.0 | 67.2 | 67.2-67.2 | Meets |
| `block_read_512` | off | 63.0-75.0 | 69.6 | 69.6-69.6 | Meets |
| `block_read_2048` | off | 63.0-75.0 | 70.2 | 70.2-70.2 | Meets |
| `block_read_8192` | off | 63.0-75.0 | 70.4 | 70.4-70.4 | Meets |
| `block_read_32768` | off | 63.0-75.0 | 70.4 | 70.4-70.4 | Meets |
| `block_read_131072` | off | 63.0-75.0 | 70.4 | 70.4-70.4 | Meets |
| `block_write_32` | off | 150.0-180.0 | 179.2 | 179.2-179.2 | Meets |
| `block_write_128` | off | 180.0-230.0 | 225.3 | 225.3-225.3 | Meets |
| `block_write_512` | off | 220.0-245.0 | 240.6 | 240.6-240.6 | Meets |
| `block_write_2048` | off | 235.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `block_write_8192` | off | 235.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `block_write_32768` | off | 235.0-250.0 | 244.8 | 244.8-244.8 | Meets |
| `block_write_131072` | off | 235.0-250.0 | 244.8 | 244.8-244.8 | Meets |

DMA transfer-unit and block-size sweeps (`build_dma`); ns/op is the time per DMA transfer unit:

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |
| `dma_wr_unit1_32k` | on | 12.4-15.1 | 13.7 | 13.7-13.7 | 72.6 | Meets |
| `dma_wr_unit2_32k` | on | 24.8-30.3 | 27.5 | 27.5-27.5 | 72.7 | Meets |
| `dma_wr_unit4_32k` | on | 49.5-60.5 | 54.9 | 54.9-54.9 | 72.7 | Meets |
| `dma_rd_unit1_32k` | on | 7.0-8.6 | 7.8 | 7.8-7.8 | 128.1 | Meets |
| `dma_rd_unit2_32k` | on | 14.0-17.2 | 15.5 | 15.5-15.5 | 128.2 | Meets |
| `dma_rd_unit4_32k` | on | 28.1-34.3 | 31.1 | 31.1-31.1 | 128.3 | Meets |
| `dma_wr_unit4_1k` | on | 33.0-57.8 | 51.5 | 51.4-51.5 | 77.6 | Meets |
| `dma_wr_unit4_4k` | on | 46.8-57.8 | 54.1 | 54.1-54.1 | 73.8 | Meets |
| `dma_wr_unit4_16k` | on | 52.2-57.8 | 54.8 | 54.8-54.8 | 72.9 | Meets |
| `dma_wr_unit4_64k` | on | 52.2-57.8 | 55.0 | 55.0-55.0 | 72.7 | Meets |
| `dma_wr_unit4_128k` | on | 52.2-57.8 | 55.0 | 55.0-55.0 | 72.6 | Meets |
| `dma_rd_unit4_1k` | on | 18.7-32.8 | 29.9 | 29.9-29.9 | 133.6 | Meets |
| `dma_rd_unit4_4k` | on | 26.5-32.8 | 30.8 | 30.8-30.8 | 129.5 | Meets |
| `dma_rd_unit4_16k` | on | 29.6-32.8 | 31.1 | 31.1-31.1 | 128.4 | Meets |
| `dma_rd_unit4_64k` | on | 29.6-32.8 | 31.1 | 31.1-31.1 | 128.2 | Meets |
| `dma_rd_unit4_128k` | on | 29.6-32.8 | 31.2 | 31.2-31.2 | 128.1 | Meets |

DMA to and from a peripheral (CRC engine data register as the peripheral address):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |

`dma_sram_to_crc` shows the SRAM-side limit of the CRC engine path. `dma_sdram_to_crc` streams SDRAM into the CRC data register (memory-to-peripheral); the CRC computed this way matches the CPU-fed reference (`DMACRC,...,match` in `results/dma.log`). `dma_crc_to_sdram` fills SDRAM from the peripheral register (peripheral-to-memory, 4-byte units).

Multi-channel concurrent DMA (channels 0-3, SDRAM-to-SDRAM, 128 KiB blocks per channel, run together):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |

The engine is shared: two channels each get roughly half, four channels roughly a quarter of the single-channel SDRAM-to-SDRAM rate; the aggregate stays near it in both cases.

## Additional access patterns

Alternating two 4 KiB windows D bytes apart (sequential 32-bit reads, one read each side per step):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | ns/op | Verdict |
| --- | --- | --- | ---: | ---: | ---: | --- |
| `alternate_2^5` | on | 80-120 ns | 47.0 | 47.0-47.0 | 85.0 | Meets |
| `alternate_2^6` | on | 80-240 ns | 47.0 | 46.9-47.0 | 85.0 | Meets |
| `alternate_2^7` | on | 80-240 ns | 46.9 | 46.9-47.0 | 85.1 | Meets |
| `alternate_2^8` | on | 80-240 ns | 46.9 | 46.9-47.0 | 85.1 | Meets |
| `alternate_2^9` | on | 80-240 ns | 46.9 | 46.9-47.0 | 85.1 | Meets |
| `alternate_2^10` | on | 80-240 ns | 46.9 | 46.8-46.9 | 85.2 | Meets |
| `alternate_2^11` | on | 80-240 ns | 46.9 | 46.8-46.9 | 85.2 | Meets |
| `alternate_2^12` | on | 80-240 ns | 40.0 | 39.8-40.3 | 99.9 | Meets |
| `alternate_2^13` | on | 80-240 ns | 32.5 | 32.4-32.6 | 122.7 | Meets |
| `alternate_2^14` | on | 80-240 ns | 25.0 | 24.9-25.0 | 159.9 | Meets |
| `alternate_2^15` | on | 80-240 ns | 22.8 | 22.8-22.8 | 175.1 | Meets |
| `alternate_2^16` | on | 80-240 ns | 22.7 | 22.7-22.7 | 175.7 | Meets |
| `alternate_2^17` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.3 | Meets |
| `alternate_2^18` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.2 | Meets |
| `alternate_2^19` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.3 | Meets |
| `alternate_2^20` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.2 | Meets |
| `alternate_2^21` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.3 | Meets |
| `alternate_2^22` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.3 | Meets |
| `alternate_2^23` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.2 | Meets |
| `alternate_2^24` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.2 | Meets |
| `alternate_2^25` | on | 80-240 ns | 23.0 | 23.0-23.0 | 173.2 | Meets |
| `alternate_2^5` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^6` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^7` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^8` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^9` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^10` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^11` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^12` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^13` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^14` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^15` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^16` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^17` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^18` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^19` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^20` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^21` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^22` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^23` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^24` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |
| `alternate_2^25` | off | 105-120 ns | 35.5 | 35.5-35.5 | 112.4 | Meets |

The cached case holds at ~85 ns per read until the separation passes the row/page size, then climbs to ~174 ns per read (two cache-line fills, one per stream). The knee sits near 2^12 bytes, close to the 2 KiB row boundary for this memory map. Uncached it is a constant ~112 ns per read: the controller does not merge the two streams.

Address alignment (sequential sweeps at byte offsets 0-3; off-by-4 for the 64-bit kernel):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |

The 64-bit kernels show the same picture: reads are unaffected by alignment, unaligned writes drop ~5-10% under write-through (extra posted-write turnaround).

Working-set sweep (stride-64-bit sequential accesses inside a window of 4 KiB to 32 MiB):

| Window | read32 | write32 | copy64 (same window) |
| ---: | ---: | ---: | ---: |
| 4k | 3450.4 | 244.7 | 241.1 |
| 16k | 2220.1 | 244.7 | 240.2 |
| 64k | 189.6 | 244.7 | 57.3 |
| 256k | 187.1 | 244.7 | 55.5 |
| 1m | 187.1 | 244.7 | 55.4 |
| 4m | 187.1 | 244.7 | 55.4 |
| 16m | 187.1 | 244.8 | 55.3 |
| 32m | 187.1 | 244.8 | 55.1 |
Reads sit at the internal-SRAM rate (3450 MB/s) while the set fits the 16 KiB cache, then step down and converge to the SDRAM rate (187 MB/s) once the set exceeds every cache level; the knee sits between 16 and 64 KiB, matching the 16 KiB D-cache. `copy64_4k` repeats the same-window copy regression (241 MB/s).

STREAM-style kernels (4 MiB operands; float and 32-bit integer):

| Test | Cache | Expected | Actual avg MB/s | min-max MB/s | Verdict |
| --- | --- | --- | ---: | ---: | --- |


## 7: Sustained bandwidth

`build_stress`: 3600 s of write/read/copy cycles. Each cycle writes, reads and copies a pseudo-random 4 MiB pattern over a rotating 4 MiB window (15 windows, 60 MiB total coverage); every write and copy is verified. D-cache on, write-through, interrupts enabled.

| Metric | Expected | Actual | Verdict |
| --- | --- | --- | --- |
| Duration / cycles | 3600 s | 3600 s / 18468 cycles (221616 MiB moved, ~221 GB total traffic) | Meets |
| Data errors | 0 | 0 | Meets |
| Write: mean of 120 intervals | - | 244.6 MB/s (min 244.6, max 244.6) | - |
| Write: interval spread | < 1% | 0.00% | Meets |
| Write: first vs last interval drift | < 1% | +0.0% | Meets |
| Read: mean of 120 intervals | - | 187.1 MB/s (min 187.1, max 187.1) | - |
| Read: interval spread | < 1% | 0.00% | Meets |
| Read: first vs last interval drift | < 1% | +0.0% | Meets |
| Copy: mean of 120 intervals | - | 55.0 MB/s (min 55.0, max 55.0) | - |
| Copy: interval spread | < 1% | 0.00% | Meets |
| Copy: first vs last interval drift | < 1% | +0.0% | Meets |

Per-pass statistics over the whole run (`sust` rows):

| Test | min | avg | max | %peak |
| --- | ---: | ---: | ---: | ---: |
| `sust_write32` | 244.6 | 244.6 | 244.7 | 48% |
| `sust_read32` | 187.0 | 187.1 | 187.1 | 37% |
| `sust_copy64_sdram_to_sdram` | 54.7 | 55.0 | 55.2 | 22% |

Interval averages, MB/s (every 30 s; full list in `results/stress_sust.csv`):

| t (s) | write | read | copy | errors |
| ---: | ---: | ---: | ---: | ---: |
| 30 | 244.6 | 187.1 | 55.0 | 0 |
| 210 | 244.6 | 187.1 | 55.0 | 0 |
| 390 | 244.6 | 187.1 | 55.0 | 0 |
| 570 | 244.6 | 187.1 | 55.0 | 0 |
| 750 | 244.6 | 187.1 | 55.0 | 0 |
| 930 | 244.6 | 187.1 | 55.0 | 0 |
| 1110 | 244.6 | 187.1 | 55.0 | 0 |
| 1290 | 244.6 | 187.1 | 55.0 | 0 |
| 1470 | 244.6 | 187.1 | 55.0 | 0 |
| 1650 | 244.6 | 187.1 | 55.0 | 0 |
| 1830 | 244.6 | 187.1 | 55.0 | 0 |
| 2010 | 244.6 | 187.1 | 55.0 | 0 |
| 2190 | 244.6 | 187.1 | 55.0 | 0 |
| 2370 | 244.6 | 187.1 | 55.0 | 0 |
| 2550 | 244.6 | 187.1 | 55.0 | 0 |
| 2730 | 244.6 | 187.1 | 55.0 | 0 |
| 2910 | 244.6 | 187.1 | 55.0 | 0 |
| 3090 | 244.6 | 187.1 | 55.0 | 0 |
| 3270 | 244.6 | 187.1 | 55.0 | 0 |
| 3450 | 244.6 | 187.1 | 55.0 | 0 |
| 3600 | 244.6 | 187.1 | 55.0 | 0 |

## 8: Concurrent bus masters

`build_conc` (sysbuild). D-cache on, write-through, interrupts enabled. "alone" is each side running by itself. "loaded" is both running together. Bus totals count copies twice.

| Scenario | Master alone MB/s | CPU test | CPU alone | CPU loaded | Change | Master loaded MB/s | SDRAM bus total MB/s | Master status |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `ref_s1_cpu_read` | 0.0 | `read32` | 187.2 | 187.2 | 0% | 0.0 | 187.2 (37%) | ok |
| `ref_s2_cpu_write` | 0.0 | `write32` | 244.6 | 244.7 | 0% | 0.0 | 244.7 (48%) | ok |
| `ref_s3_cpu_read_dma` | 22.5 | `read32` | 187.2 | 169.0 | -9% | 2.9 | 174.9 (34%) | ok |
| `dma_sdram_to_sdram` | 22.5 | `write32` | 244.6 | 194.0 | -20% | 3.4 | 200.8 (40%) | ok |
| `dma_sdram_to_sdram` | 22.5 | `read32` | 187.2 | 169.0 | -9% | 2.9 | 174.9 (34%) | ok |
| `dma_sdram_to_sdram` | 22.5 | `copy64_sdram_to_sdram` | 57.0 | 52.9 | -7% | 2.7 | 111.3 (22%) | ok |
| `cm33_read` | 31.5 | `write32` | 244.6 | 228.3 | -6% | 2.1 | 230.4 (46%) | ok |
| `cm33_read` | 31.5 | `read32` | 187.2 | 174.8 | -6% | 5.4 | 180.2 (36%) | ok |
| `cm33_read` | 31.5 | `copy64_sdram_to_sdram` | 57.0 | 51.6 | -9% | 3.5 | 106.7 (21%) | ok |
| `cm33_write` | 42.2 | `write32` | 244.6 | 244.6 | 0% | 0.0 | 244.6 (48%) | ok |
| `cm33_write` | 42.2 | `read32` | 187.2 | 112.3 | -40% | 16.6 | 128.9 (25%) | ok |
| `cm33_write` | 42.2 | `copy64_sdram_to_sdram` | 57.0 | 58.6 | 2% | 9.0 | 126.2 (25%) | ok |
| `cm33_copy` | 21.2 | `write32` | 244.6 | 244.6 | 0% | 0.0 | 244.6 (48%) | ok |
| `cm33_copy` | 21.2 | `read32` | 187.2 | 159.4 | -14% | 4.0 | 167.4 (33%) | ok |
| `cm33_copy` | 21.2 | `copy64_sdram_to_sdram` | 57.0 | 55.7 | -2% | 2.6 | 116.7 (23%) | ok |
| `glcdc_scanout` | 69.6 | `write32` | 244.6 | 144.5 | -40% | 68.8 | 213.3 (42%) | ok |
| `glcdc_scanout` | 69.6 | `read32` | 187.2 | 132.9 | -29% | 67.9 | 200.8 (40%) | ok |
| `glcdc_scanout` | 69.6 | `copy64_sdram_to_sdram` | 57.0 | 53.9 | -5% | 69.1 | 176.9 (35%) | glcdc_underflow |
| `all_masters` | 86.9 | `write32` | 244.6 | 117.4 | -52% | 75.0 | 194.4 (38%) | ok |
| `all_masters` | 86.9 | `read32` | 187.2 | 136.9 | -26% | 76.9 | 220.4 (44%) | glcdc_underflow |
| `all_masters` | 86.9 | `copy64_sdram_to_sdram` | 57.0 | 54.0 | -5% | 73.9 | 186.7 (37%) | glcdc_underflow |
| `ref_s4_cpu_read_dma_display` | 84.9 | `read32` | 187.2 | 168.2 | -10% | 70.4 | 241.5 (48%) | glcdc_underflow |
| `ref_s5_cpu_copy_dma_display` | 88.9 | `copy64_sdram_to_sdram` | 57.0 | 52.8 | -7% | 74.2 | 182.6 (36%) | glcdc_underflow |

| Scenario | Expected | Actual | Verdict |
| --- | --- | --- | --- |
| DMAC background | CPU -10% to -25%; bus total <= 260 MB/s | CPU -20% (write), -9% (read), -7% (copy); DMA falls from 22.5 to 3.4-2.9 MB/s | Meets for CPU; DMA starvation not anticipated |
| CM33 read / write | CM33 read 40-70, write 100-240 MB/s alone; CPU -15% to -50% | CM33 alone 31.5 (read), 42.2 (write); CPU -6% to -40%; CM33 write 0.0 MB/s while the CPU writes | **Below** (CM33 alone); CPU impact within range |

The scenario matrix:

| Scenario | Expected | Actual | Verdict |
| --- | --- | --- | --- |
| `ref_s1_cpu_read` (read32) | coexistence without starvation | bus total 187.2 MB/s (37%); see table above | - |
| `ref_s2_cpu_write` (write32) | coexistence without starvation | bus total 244.7 MB/s (48%); see table above | - |
| `ref_s3_cpu_read_dma` (read32) | coexistence without starvation | bus total 174.9 MB/s (34%); see table above | - |
| `ref_s4_cpu_read_dma_display` (read32) | coexistence without starvation | bus total 241.5 MB/s (48%); see table above | - |
| `ref_s5_cpu_copy_dma_display` (copy64_sdram_to_sdram) | coexistence without starvation | bus total 182.6 MB/s (36%); see table above | - |

Consistency check : every concurrent measurement was re-run with the master idle and under CPU load; the underflow status is read from the GLCDC status flags, not inferred.

| GLCDC scan-out | 57.5 frames/s, 70.7 MB/s, no underflow; CPU read -15% to -35% | 57.4 frames/s, 70.5 MB/s; CPU read -29%, write -40%; underflow during `copy64_sdram_to_sdram` | Rate meets; **underflow** |
| All masters | bus total 200-300 MB/s, no errors, no underflow | bus total 194.4-220.4 MB/s; CPU write -52%; no data errors; underflow in 2 of 3 CPU tests | Total meets (low end); **underflow** |

After every scenario, the DMAC (window D vs. C) and CM33 (write pattern, copy target vs. source) destination data was compared and matched. The GLCDC frame period was unchanged.

## Repeatability

Per-pass coefficient of variation is in the CSV (`cv%`); rows above 5% carry the `VAR` flag. The VAR rows are the cached random tests (`rand_read32_16k` up to ~20%, `rand_write32_64m` up to ~43%, `chase_16k` up to ~9%), which are sensitive to cache state and expected to vary; every other test has CV under 5%.

Two runs of `build` (260 rows each). Excluding the cached random tests, the largest difference between runs is 3.49% (`wset/copy64_64k/on`), and the largest spread between passes within a run is 6.54%.

| Test | Run 1 | Run 2 | Between runs | Within run (max-min) |
| --- | ---: | ---: | ---: | ---: |
| `random/rand_write32_64m/on` | 23.0 | 17.1 | 25.65% | 139.13% |
| `wset/copy64_64k/on` | 57.3 | 55.3 | 3.49% | 0.70% |
| `wset/copy64_256k/on` | 55.5 | 53.9 | 2.88% | 1.26% |
| `cpu/copy64_sdram_to_sdram/on` | 55.5 | 54.0 | 2.70% | 0.54% |
| `stream/scale_f32/on` | 113.7 | 110.7 | 2.64% | 0.09% |
| `wset/copy64_4m/on` | 55.4 | 54.0 | 2.53% | 0.90% |

## Deviations from expectations

1. **Mixed read/write, cached** (81.3 / 72.5 / 53.1 MB/s against 107-157 / 100-135 / 90-120). Also `mix_r25_w75` uncached (78.9) and `rmw32` uncached (23.4).
   - An 8-word group takes 394, 441 and 603 ns. Each extra pair of writes adds about 52 ns per write, against 16.3 ns per write when streaming.
   - Writes mixed in with reads are not merged into back-to-back bursts. They cost close to an isolated write: random writes take 72.6 ns each.
   - The model assumed streaming write cost; the measured cost per mixed write is about 3x higher. This explanation is inferred from timing.
2. **Uncached single accesses take about 112.4 ns at every stride** (4 B to 256 KiB), not the predicted 85 ns below 2 KiB.
   - 112.4 ns equals the dependent-load latency (`chase_*` uncached, 112.4 ns). A loop with one load per iteration does not overlap loads. The 8-way unrolled `read32` appears to overlap them, giving 85 ns per word (inferred).
   - The time does not change between 4 B and 256 KiB strides, so an isolated access gets no benefit from an open row. Random uncached reads (112.7 ns) therefore cost the same as strided ones.
3. **Cached strides of 128 B to 1 KiB** take 225.2 ns per line fill instead of about 171 ns.
   - A line fill costs 169.3 ns only when it follows a fill of the previous 32 or 64 bytes.
   - Otherwise it costs 225.2 ns, the same as a random fill (230.3 ns) or a dependent load (227.1 ns).
   - So locality within a row helps only for adjacent addresses, not for the whole 2 KiB row.
4. **Cached block reads of 32 B to 2 KiB** (105.1 / 71.4 / 135.9 / 173.8 MB/s).
   - These are below the prediction. 128-byte blocks are slower than 32-byte blocks and close to the uncached result (67.2).
   - Full speed (187.0) needs blocks of 8 KiB or more. Block writes behave as predicted.
   - The cause of the 128-byte minimum was not determined.
5. **`rand_write32_64m` with the cache on** (23.0 MB/s, 14.5-46.5 between passes).
   - The same test reaches 51.5 over 4 MiB, 55.0 with the cache off, and 54.7 with the write-back cache.
   - In write-through mode, cache-missing writes spread over 64 MiB are about 3x slower and vary between passes. The cause was not determined.
6. **`rand_read32_16k` with the cache on** (327.8 MB/s, 176.7-377.8).
   - The 16 KiB window is the entire D-cache, so stack, globals and the SRAM buffer evict some of its lines.
   - The pointer chase over the same window does meet its expectation (6.0 ns).
7. **`copy64_sram_to_sdram` with the cache on** (118.9 MB/s against the previous 141.3).
   - The kernel is unchanged, but its result varies between builds: 130.9 MB/s in `build_dma` and 118.9 in `build`.
   - It is stable within each build. With the cache off it is stable everywhere (242.3).
   - This copy interleaves SRAM line fills with write-through stores to SDRAM, and is sensitive to code placement (inferred).
8. **`read8` uncached** (8.8 MB/s): latency-bound at 1 byte per 114 ns, slightly below the range.
9. **Concurrent: the CM33 alone** reaches only 31.5 MB/s reading and 42.2 MB/s writing (expected 40-70 and 100-240).
   - The CM33 has no data cache, so every access is a single beat: about 127 ns per read and 95 ns per write.
10. **Concurrent: the other masters starve.** While the CPU streams to SDRAM:
    - the DMAC falls from 22.5 to 3.4 MB/s,
    - CM33 writes fall from 42.2 to 0.0 MB/s,
    - the CPU loses 20% (writes) against the DMAC, and 6% or less against CM33 reads.
    - CPU reads lose more: 40% against CM33 writes, which still reach 16.6 MB/s.
    - CPU writes appear to have priority over the DMAC and CM33 (inferred from the rates). They do not have priority over the GLCDC, which slows CPU writes by 40%.
11. **Concurrent: GLCDC underflow.** The GLCDC fetches its 70.5 MB/s frame stream but underflows:
    - during a CPU SDRAM-to-SDRAM copy,
    - in the all-masters scenario during CPU reads and copies.
    - With a real panel this shows as display corruption. CPU write and read streams alone do not cause it.

12. **Uncached interleaved copies are ~3x slower than the sum of their parts.** `stream_copy32_unc` reaches 12.4 MB/s (meets its >=10 floor, but far below the ~35-55 MB/s of separate uncached reads and writes): every read-to-write turnaround pays the full access latency. Same mechanism as deviation 1.

## Observations

1. **The write path is limited to 244.7 MB/s (49%), about 2 SDCLK per 32-bit beat.** This holds:
   - for every access width (byte stores are merged) and both cache modes,
   - for sequential writes, and for writes in randomly placed blocks of 2 KiB or more.
2. **Reads reach 187 MB/s (37%) only through 32-byte cache-line fills** at 170 ns per line (adjacent) or about 227 ns (non-adjacent).
   - Uncached reads take 85 ns per word with loads overlapped, or 112 ns per dependent or isolated load.
   - An 8-word LDM gets no burst when uncached (same as LDRD, 70.4 MB/s).
   - Buffers the CPU reads from SDRAM should be cacheable.
3. **Locality within a row gives no benefit for isolated accesses.**
   - Uncached single accesses cost the same (112 ns) whether random or sequential.
   - Cached line fills gain only when the next line is adjacent: 170 ns instead of 227 ns.
   - Access width, caching and batching matter far more than placement within SDRAM.
4. **Mixing reads and writes costs more than either alone.** Interleaved writes cost about 45 ns instead of 16 ns. Software should batch writes (for example, a write-back cache, or copying through an SRAM staging buffer):
   - the write-back cache speeds up SDRAM-to-SDRAM copies by about 30%,
   - SRAM-to-SDRAM copies run fastest with the cache off (242 MB/s).
5. **The DMAC takes a constant 72.6 ns per write and 128 ns per read transfer, regardless of transfer unit.**
   - Throughput scales directly with unit size (4-byte units are 4x faster than 1-byte units).
   - Block size hardly matters: 1 KiB blocks lose only 6%.
   - The DMAC is far below the CPU for SDRAM bandwidth, and it starves under CPU load.
6. **Sustained behaviour is stable.** No data errors, no drift, and interval spread below the measurement resolution over the 600 s windowed run and the 1-hour full-device stress run.
   - An earlier 12-minute run with interrupts locked during passes also had no errors (`results/sustained_run0_irqlocked.log`).
   - The sustained copy rate (57.1 MB/s, windows A to D) differs slightly from `copy64_sdram_to_sdram` in the CPU suite (A to B), in line with the code-placement sensitivity in deviation 7.
7. **Bus sharing.**
   - Total SDRAM traffic never exceeded about 245 MB/s in any concurrent scenario.
   - The CPU keeps most of its bandwidth against the DMAC and CM33, which starve.
   - The GLCDC takes bandwidth from CPU writes (-40%) and reads (-28%), but still underflows during heavy CPU copy traffic.
   - Display systems must be designed for this. Options include lower CPU SDRAM traffic during scan-out, the frame buffer in another memory, or bus priority settings, which were not evaluated here.

## Issues found

| Issue | Effect | Resolution |
| --- | --- | --- |
| `CONFIG_SPEED_OPTIMIZATIONS=y` makes GCC put `VLDR` in FSP `bsp_clock_init()` before the FPU is enabled | UsageFault NOCP at boot, no output | image stays `-Os`; only the benchmark sources use `-O2` |
| `CONFIG_SOC_RA_ENABLE_START_SECOND_CORE` defaults to `y` and starts an old CM33 image from MRAM | system software reset every 4.06 s | `=n`; the concurrent suite starts the CM33 itself, after initializing the mailbox |
| `uart_renesas_ra8_sci_b.c` uses `UART_SCI_B_ASSERT_NO_CONFLICT` without defining it unless interrupt-driven or async UART is on | build error | `CONFIG_UART_INTERRUPT_DRIVEN=y` |
| The CM33 board defconfig assigns `OS_*` symbols that are not defined in this workspace | every CM33 build aborts with Kconfig errors | placeholder symbols in `cm33_traffic/Kconfig` |
| FSP `R_BSP_SecondaryCoreStart()` exists only with `CONFIG_SOC_RA_ENABLE_START_SECOND_CORE` | cannot start the CM33 on demand | the same three CPU1 register writes in `bench_concurrent.c` |
| Zephyr GLCDC driver: `display_write()` vsync wait completes about 180,000 times per second instead of 57.4 (hardware frame period confirmed at 17.4 ms) | no real vsync pacing, high interrupt load (CPU writes -44% with the line interrupt enabled, diagnostic run) | line-detect interrupt disabled after starting scan-out; timing taken from the hardware flag. Driver root cause not investigated; affects `lcd_display_test` and LVGL |
| `display_renesas_ra.c:486` `-Wmissing-braces` warning | build warning in the tree | not changed |
| Long interrupt-locked passes lose system timer ticks | `k_uptime` ran about 28% slow in the first sustained run | sustained loop runs with interrupts enabled |
| Cortex-M85 PMU event counters do not count (software-increment self-test fails; verified under the debugger too) | no hardware counter data (cache refills, stalls) | counters reported as `n/a`; the PMU identification values are printed at boot |

Corrections made to the benchmark itself during this work (all final logs
are from the corrected code):

- Kernels with `volatile` byte or word stores reloaded their parameters from
  memory on every iteration, because the stores could alias the descriptor.
  This slowed uncached `write8` and the random-write kernels. Parameters are
  now copied into locals, which was checked in the disassembly.
- The overall average overflowed 32-bit arithmetic after about 1000 passes,
  which affected only the sustained summary rows. The calculation is now
  64-bit.

## Not covered

- Temperature and voltage corners. There is no die-temperature driver, so no
  temperature was logged.
- An oscilloscope check of SDCLK, and SDRAM timing variations (for example
  tRCD or CL).
- A panel connected through the DSI host. Only the GLCDC fetch was loaded.
- Other bus masters (DRW 2D engine, Ethernet, USB, SDHI DMA), and bus
  priority (QoS) settings.
- Soak tests longer than one hour. `CONFIG_SDRAM_BENCH_SUSTAINED_SEC`
  supports up to 24 h.
- Concurrent and sustained runs with the write-back cache.
- Temperature and voltage corners: room temperature only was run. The
  procedure for corners is in README.md.
- Root causes of three findings: the 128-byte block-read minimum, slow cached
  64 MiB random writes in write-through mode, and the GLCDC driver vsync
  behaviour.

## Appendix: raw data

| File | Content |
| --- | --- |
| `results/concurrent.csv` | concurrent sysbuild build: RESULT lines |
| `results/concurrent.log` | concurrent sysbuild build: console log |
| `results/concurrent_conc.csv` | concurrent sysbuild build: CONC lines |
| `results/cpu.csv` | default build, run 1: RESULT lines |
| `results/cpu.log` | default build, run 1: console log |
| `results/cpu_run2.csv` | default build, run 2: RESULT lines |
| `results/cpu_run2.log` | default build, run 2: console log |
| `results/dma.csv` | DMA build: RESULT lines |
| `results/dma.log` | DMA build: console log |
| `results/integrity.csv` | integrity + 300 s retention build: RESULT lines |
| `results/integrity.log` | integrity + 300 s retention build: console log |
| `results/stress.csv` | 3600 s full-device stress build: RESULT lines |
| `results/stress.log` | 3600 s full-device stress build: console log |
| `results/stress_sust.csv` | 3600 s full-device stress build: SUST interval lines |
| `results/stress_sustsum.csv` | 3600 s full-device stress build: SUSTSUM lines |
| `results/v1` | captures from the first report version: console log |
| `results/writeback.csv` | write-back build: RESULT lines |
| `results/writeback.log` | write-back build: console log |
