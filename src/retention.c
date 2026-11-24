/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Data retention: fill the whole SDRAM with an address-dependent
 * pseudo-random pattern, leave the bus idle (only auto-refresh runs) for
 * CONFIG_SDRAM_BENCH_RETENTION_SEC seconds, then verify. Errors are
 * reported per 1 MiB block.
 */

#include <zephyr/kernel.h>

#include "sdram_bench.h"

#define BLOCK_BYTES (1024U * 1024U)
#define BLOCKS      (SDRAM_SIZE / BLOCK_BYTES)
#define WORDS       (SDRAM_SIZE / sizeof(uint32_t))
#define SEED        0xC001D00DU

static ALWAYS_INLINE uint32_t ret_word(size_t idx)
{
	uint32_t x = ((uint32_t)idx * 0x9E3779B1U) ^ SEED;

	x ^= x >> 15;
	x *= 0x2C1B3C6DU;
	x ^= x >> 12;
	return x ^ (uint32_t)(SDRAM_BASE + (idx * sizeof(uint32_t)));
}

uint32_t sdram_retention_run(void)
{
	const bool cache_was_on = bench_dcache_enabled();
	volatile uint32_t *p = (volatile uint32_t *)SDRAM_BASE;
	uint32_t total = 0U;
	uint32_t bad_blocks = 0U;
	int64_t t0;

	printk("\n--- Data retention: %u MiB, %u s idle, D-cache off ---\n",
	       (uint32_t)(SDRAM_SIZE / BLOCK_BYTES), (uint32_t)CONFIG_SDRAM_BENCH_RETENTION_SEC);
	bench_dcache_set(false);

	for (size_t i = 0; i < WORDS; i++) {
		p[i] = ret_word(i);
	}

	t0 = k_uptime_get();
	/* CPU sleeps: no SDRAM traffic other than the controller's auto-refresh */
	k_sleep(K_SECONDS(CONFIG_SDRAM_BENCH_RETENTION_SEC));
	printk("  idle for %u ms\n", (uint32_t)(k_uptime_get() - t0));

	for (size_t b = 0; b < BLOCKS; b++) {
		uint32_t errs = 0U;

		for (size_t i = b * (BLOCK_BYTES / 4U); i < (b + 1U) * (BLOCK_BYTES / 4U); i++) {
			const uint32_t got = p[i];

			if (got != ret_word(i)) {
				if (errs == 0U) {
					printk("  block %u first error @0x%08x: expected 0x%08x, "
					       "read 0x%08x\n",
					       (uint32_t)b, (uint32_t)(uintptr_t)&p[i], ret_word(i),
					       got);
				}
				errs++;
			}
		}
		if (errs != 0U) {
			printk("RETEN,block,%u,%u\n", (uint32_t)b, errs);
			bad_blocks++;
		}
		total += errs;
	}

	bench_dcache_set(cache_was_on);
	printk("RETEN,summary,%u,%u,%u\n", (uint32_t)CONFIG_SDRAM_BENCH_RETENTION_SEC, bad_blocks,
	       total);
	printk("Data retention %s (%u errors in %u of %u blocks)\n",
	       (total == 0U) ? "PASSED" : "FAILED", total, bad_blocks, (uint32_t)BLOCKS);

	return total;
}
