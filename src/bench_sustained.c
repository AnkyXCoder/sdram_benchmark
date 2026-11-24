/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sustained bandwidth / stress: write, read and copy cycles for
 * CONFIG_SDRAM_BENCH_SUSTAINED_SEC seconds, with every cycle's data checked
 * and a report every CONFIG_SDRAM_BENCH_SUSTAINED_REPORT_SEC. By default the
 * cycles rotate over windows A-D (16 MiB) with a fixed pattern; with
 * CONFIG_SDRAM_BENCH_SUSTAINED_FULL they rotate over 15 windows (60 MiB, all
 * of SDRAM except the window holding the mailbox) with a new pseudo-random
 * pattern every cycle.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/timing/timing.h>
#include <string.h>

#include "sdram_bench.h"

#define SUST_WINDOWS (IS_ENABLED(CONFIG_SDRAM_BENCH_SUSTAINED_FULL) ? 15U : 4U)

BUILD_ASSERT((15U * SB_WIN_SIZE) <= (SB_MBOX_ADDR - SB_SDRAM_BASE),
	     "stress windows overlap the mailbox");

static ALWAYS_INLINE uint32_t rand_word(uint32_t idx, uint32_t seed)
{
	uint32_t x = (idx * 0x9E3779B1U) + seed;

	x ^= x >> 16;
	x *= 0x85EBCA6BU;
	x ^= x >> 13;
	return x;
}

/* Pseudo-random fill, seed in param */
static void k_write_rand(const struct bench_kernel *k)
{
	volatile uint32_t *p = k->dst;
	const uint32_t n = (uint32_t)(k->len / sizeof(uint32_t));
	const uint32_t seed = k->param;

	for (uint32_t i = 0; i < n; i += 4U) {
		p[i + 0U] = rand_word(i + 0U, seed);
		p[i + 1U] = rand_word(i + 1U, seed);
		p[i + 2U] = rand_word(i + 2U, seed);
		p[i + 3U] = rand_word(i + 3U, seed);
	}
}

static uint32_t verify_rand(const struct bench_kernel *k, uint32_t cycle, uint32_t w)
{
	const uint32_t *p = k->dst;
	uint32_t errs = 0U;

	for (uint32_t i = 0; i < (uint32_t)(k->len / sizeof(uint32_t)); i++) {
		const uint32_t exp = rand_word(i, k->param);

		if (p[i] != exp) {
			if (errs == 0U) {
				printk("SUSTERR,%u,%u,write,0x%08x,0x%08x,0x%08x\n", cycle, w,
				       (uint32_t)(uintptr_t)&p[i], exp, p[i]);
			}
			errs++;
		}
	}

	return errs;
}

enum sust_op {
	OP_WRITE,
	OP_READ,
	OP_COPY,
	OP_COUNT,
};

static const char *const op_names[OP_COUNT] = {"sust_write32", "sust_read32",
					       "sust_copy64_sdram_to_sdram"};
static const uint32_t op_bus_mult[OP_COUNT] = {1U, 1U, 2U};

struct interval {
	uint64_t ns[OP_COUNT];
	uint64_t bytes[OP_COUNT];
};

/*
 * Interrupts stay enabled: the run is paced by the system clock, and long
 * interrupt locks would lose system timer ticks and stretch the duration.
 */
static uint64_t timed(const struct bench_kernel *k)
{
	const bool cached = bench_dcache_enabled();
	timing_t t0, t1;

	bench_cache_sync();
	t0 = timing_counter_get();
	k->fn(k);
	if (cached && k->writes_sdram) {
		(void)sys_cache_data_flush_all();
	}
	t1 = timing_counter_get();

	return timing_cycles_to_ns(timing_cycles_get(&t0, &t1));
}

uint32_t bench_sustained_run(void)
{
	const int64_t duration_ms = (int64_t)CONFIG_SDRAM_BENCH_SUSTAINED_SEC * 1000;
	const int64_t report_ms = (int64_t)CONFIG_SDRAM_BENCH_SUSTAINED_REPORT_SEC * 1000;
	struct bench_stats total[OP_COUNT];
	uint32_t first_x10[OP_COUNT] = {0};
	uint32_t last_x10[OP_COUNT] = {0};
	uint32_t ivl_min_x10[OP_COUNT];
	uint32_t ivl_max_x10[OP_COUNT] = {0};
	struct interval ivl = {0};
	uint32_t errors = 0U;
	uint32_t intervals = 0U;
	uint64_t cycles = 0U;
	int64_t start, next_report;

	for (size_t o = 0; o < OP_COUNT; o++) {
		bench_stats_init(&total[o], BENCH_BUF_SIZE, op_bus_mult[o], 0U);
		ivl_min_x10[o] = UINT32_MAX;
	}

	printk("\n--- Sustained bandwidth: %u s, %u windows (%u MiB), %s pattern, report every "
	       "%u s, D-cache %s, interrupts enabled ---\n",
	       (uint32_t)CONFIG_SDRAM_BENCH_SUSTAINED_SEC, SUST_WINDOWS,
	       (SUST_WINDOWS * SB_WIN_SIZE) / SB_MIB,
	       IS_ENABLED(CONFIG_SDRAM_BENCH_SUSTAINED_FULL) ? "pseudo-random" : "fixed",
	       (uint32_t)CONFIG_SDRAM_BENCH_SUSTAINED_REPORT_SEC, bench_cache_name());
	printk("SUST,t_s,cycles,write_MBps,read_MBps,copy_MBps,errors\n");

	start = k_uptime_get();
	next_report = start + report_ms;

	while ((k_uptime_get() - start) < duration_ms) {
		const uint32_t w = (uint32_t)(cycles % SUST_WINDOWS);
		void *win = (void *)(SB_WIN_A + (w * SB_WIN_SIZE));
		void *nxt = (void *)(SB_WIN_A + (((w + 1U) % SUST_WINDOWS) * SB_WIN_SIZE));
		const struct bench_kernel kw = {
			.name = op_names[OP_WRITE],
			.fn = IS_ENABLED(CONFIG_SDRAM_BENCH_SUSTAINED_FULL) ? k_write_rand
									    : k_write32,
			.dst = win, .len = BENCH_BUF_SIZE, .writes_sdram = true,
			.param = 0x9E3779B9U * ((uint32_t)cycles + 1U)};
		const struct bench_kernel kr = {.name = op_names[OP_READ], .fn = k_read32,
						.src = win, .len = BENCH_BUF_SIZE};
		const struct bench_kernel kc = {.name = op_names[OP_COPY], .fn = k_copy64,
						.dst = nxt, .src = win, .len = BENCH_BUF_SIZE,
						.writes_sdram = true};
		const struct bench_kernel *ops[OP_COUNT] = {&kw, &kr, &kc};

		for (size_t o = 0; o < OP_COUNT; o++) {
			uint64_t ns = timed(ops[o]);

			bench_stats_add(&total[o], ns);
			ivl.ns[o] += ns;
			ivl.bytes[o] += BENCH_BUF_SIZE;

			/* Check the data after the write and after the copy */
			if (o == OP_WRITE) {
				bench_cache_sync();
				if (IS_ENABLED(CONFIG_SDRAM_BENCH_SUSTAINED_FULL)) {
					errors += verify_rand(&kw, (uint32_t)cycles, w);
				} else if (!verify_write32(&kw, 0U)) {
					errors++;
				}
			} else if (o == OP_COPY) {
				bench_cache_sync();
				if (!verify_copy(&kc, 0U)) {
					printk("SUSTERR,%u,%u,copy\n", (uint32_t)cycles, w);
					errors++;
				}
			}
		}
		cycles++;

		if (k_uptime_get() >= next_report) {
			uint32_t x10[OP_COUNT];

			for (size_t o = 0; o < OP_COUNT; o++) {
				x10[o] = bench_mbps_x10(ivl.bytes[o], ivl.ns[o]);
				if (intervals == 0U) {
					first_x10[o] = x10[o];
				}
				last_x10[o] = x10[o];
				ivl_min_x10[o] = MIN(ivl_min_x10[o], x10[o]);
				ivl_max_x10[o] = MAX(ivl_max_x10[o], x10[o]);
			}
			intervals++;

			printk("SUST,%u,%u,%u.%u,%u.%u,%u.%u,%u\n",
			       (uint32_t)((k_uptime_get() - start) / 1000), (uint32_t)cycles,
			       x10[0] / 10U, x10[0] % 10U, x10[1] / 10U, x10[1] % 10U,
			       x10[2] / 10U, x10[2] % 10U, errors);

			memset(&ivl, 0, sizeof(ivl));
			next_report += report_ms;
		}
	}

	bench_section("Sustained totals (per-pass min/avg/max over the whole run)");
	for (size_t o = 0; o < OP_COUNT; o++) {
		bench_report("sust", op_names[o], bench_cache_name(), &total[o]);
	}

	printk("\nSUSTSUM,op,intervals,first_MBps,last_MBps,drift_pct_x10,interval_min_MBps,"
	       "interval_max_MBps\n");
	for (size_t o = 0; o < OP_COUNT; o++) {
		int32_t drift = (first_x10[o] == 0U)
					? 0
					: (int32_t)(((int64_t)last_x10[o] - first_x10[o]) * 1000 /
						    first_x10[o]);

		printk("SUSTSUM,%s,%u,%u.%u,%u.%u,%d,%u.%u,%u.%u\n", op_names[o], intervals,
		       first_x10[o] / 10U, first_x10[o] % 10U, last_x10[o] / 10U,
		       last_x10[o] % 10U, drift, ivl_min_x10[o] / 10U, ivl_min_x10[o] % 10U,
		       ivl_max_x10[o] / 10U, ivl_max_x10[o] % 10U);
	}
	printk("Sustained: %u cycles, %u MiB moved, %u data errors\n", (uint32_t)cycles,
	       (uint32_t)((cycles * OP_COUNT * BENCH_BUF_SIZE) / (1024U * 1024U)), errors);

	return errors;
}
