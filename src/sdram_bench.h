/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SDRAM_BENCH_H_
#define SDRAM_BENCH_H_

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdram_bench_shared.h"

#define SDRAM_NODE      DT_NODELABEL(sdram1)
#define SDRAM_BASE      ((uintptr_t)DT_REG_ADDR(SDRAM_NODE))
#define SDRAM_SIZE      ((size_t)DT_REG_SIZE(SDRAM_NODE))
#define SDRAM_BUS_BYTES 4U

BUILD_ASSERT(SDRAM_BASE == SB_SDRAM_BASE, "shared layout does not match the devicetree");
BUILD_ASSERT(SDRAM_SIZE == SB_SDRAM_SIZE, "shared layout does not match the devicetree");

#define BENCH_BUF_SIZE ((size_t)CONFIG_SDRAM_BENCH_BUF_SIZE)
#define BENCH_ITER     ((uint32_t)CONFIG_SDRAM_BENCH_ITERATIONS)

BUILD_ASSERT(BENCH_BUF_SIZE <= SB_WIN_SIZE, "benchmark size must fit in one window");

#define BENCH_SDRAM_A ((void *)SB_WIN_A)
#define BENCH_SDRAM_B ((void *)SB_WIN_B)

/* Internal SRAM buffer used as copy partner and as reference */
#define SRAM_BUF_SIZE (128U * 1024U)

BUILD_ASSERT((BENCH_BUF_SIZE % SRAM_BUF_SIZE) == 0U,
	     "benchmark size must be a multiple of the SRAM buffer");

extern uint8_t bench_sram_buf[SRAM_BUF_SIZE];
extern volatile uint64_t bench_sink;

#define PAT32(i) ((uint32_t)(i) ^ 0x5A5AA5A5U)
#define PAT64(i) (((uint64_t)~PAT32(i) << 32) | PAT32(i))

/* Cortex-M85 PMU event counters sampled around every timed region */
enum bench_pmu_event {
	PMU_L1D_REFILL,   /* L1 D-cache line refills */
	PMU_BUS_ACCESS,   /* bus accesses */
	PMU_MEM_ACCESS,   /* data memory accesses */
	PMU_STALL_BACKEND, /* cycles stalled by the backend (memory) */
	PMU_EVENTS,
};

/* Accumulated timing of one test */
struct bench_stats {
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t total_ns;
	uint32_t iters;
	size_t bytes;      /* bytes counted per iteration */
	uint32_t bus_mult; /* SDRAM bus bytes per counted byte, 0 if SDRAM is not involved */
	uint32_t ops;      /* accesses per iteration for ns/op, 0 if not reported */
	double mean_mbps;  /* Welford running mean / M2 of per-iteration MB/s */
	double m2_mbps;
	uint64_t pmu[PMU_EVENTS]; /* sums over all iterations */
	bool pmu_valid;
};

void bench_pmu_init(void);
bool bench_pmu_ok(void);
void bench_pmu_start(void);
void bench_pmu_stop(uint32_t out[PMU_EVENTS]);
void bench_stats_add_pmu(struct bench_stats *st, const uint32_t pmu[PMU_EVENTS]);

struct bench_kernel;
typedef void (*kernel_fn)(const struct bench_kernel *k);

/* One timed kernel invocation pattern */
struct bench_kernel {
	const char *group;
	const char *name;
	kernel_fn fn;
	void *dst;
	const void *src;
	size_t len;        /* address range handed to the kernel */
	size_t bytes;      /* bytes counted per call, 0 means len */
	uint32_t repeat;   /* calls per timed iteration, 0 means 1 */
	uint32_t ops;      /* accesses per call for ns/op, 0 if not reported */
	uint32_t bus_mult;
	uint32_t param;    /* kernel specific: ratio, stride, block size, count */
	bool writes_sdram; /* flush the cache inside the timed region */
	bool warm;         /* one untimed call before timing */
	void (*setup)(const struct bench_kernel *k);
	bool (*verify)(const struct bench_kernel *k, uint32_t calls);
};

/* Print a section title and the table header */
void bench_section(const char *title);
void bench_stats_init(struct bench_stats *st, size_t bytes, uint32_t bus_mult, uint32_t ops);
void bench_stats_add(struct bench_stats *st, uint64_t ns);
uint32_t bench_mbps_x10(uint64_t bytes, uint64_t ns);
void bench_report(const char *group, const char *test, const char *cache,
		  const struct bench_stats *st);
const char *bench_cache_name(void);
uint32_t bench_peak_mbps(void);
/* Performance problems that are not data errors (reported, not failed) */
void bench_add_warnings(uint32_t n);

bool bench_dcache_enabled(void);
void bench_dcache_set(bool on);

/* Make SDRAM and the cache coherent before/after a pass */
void bench_cache_sync(void);

/* Time a kernel BENCH_ITER times; irqs are locked during the timed region if lock_irq */
void bench_kernel_measure(const struct bench_kernel *k, bool lock_irq, struct bench_stats *st);
/* Measure, report and verify; returns the number of failures */
uint32_t bench_kernel_run(const struct bench_kernel *k);
/* Run a list in D-cache on and off modes (on only if !both_modes) */
uint32_t bench_kernel_suite(const struct bench_kernel *ks, size_t n, bool both_modes);

/* Kernels shared by the suites */
void k_write32(const struct bench_kernel *k);
void k_read32(const struct bench_kernel *k);
void k_copy64(const struct bench_kernel *k);
void k_copy64_from_sram(const struct bench_kernel *k);
void k_write64(const struct bench_kernel *k);
void k_read64(const struct bench_kernel *k);
bool verify_write64(const struct bench_kernel *k, uint32_t calls);
bool verify_write32(const struct bench_kernel *k, uint32_t calls);
bool verify_copy(const struct bench_kernel *k, uint32_t calls);

uint32_t sdram_integrity_run(void);
uint32_t sdram_retention_run(void);
uint32_t bench_stream_run(void);
uint32_t bench_cpu_run(void);
uint32_t bench_access_run(void);
uint32_t bench_dma_run(void);
uint32_t bench_concurrent_run(void);
uint32_t bench_sustained_run(void);

/* Continuous SDRAM-to-SDRAM DMA (window C to D) on dma0 channel 1 */
int bench_dma_bg_start(void);
void bench_dma_bg_stop(void);
uint64_t bench_dma_bg_bytes(void);

#endif /* SDRAM_BENCH_H_ */
