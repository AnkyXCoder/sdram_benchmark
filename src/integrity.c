/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full-device SDRAM integrity test. Runs with the data cache disabled so
 * that every access reaches the SDRAM.
 */

#include <zephyr/kernel.h>

#include "sdram_bench.h"

#define MAX_REPORTED 8U
#define SDRAM_WORDS  (SDRAM_SIZE / sizeof(uint32_t))

BUILD_ASSERT((SDRAM_WORDS & (SDRAM_WORDS - 1U)) == 0U, "SDRAM size must be a power of two");

enum fill_kind {
	FILL_ADDR,
	FILL_INV_ADDR,
	FILL_CHECKER,
	FILL_INV_CHECKER,
	FILL_RANDOM,
	FILL_INV_RANDOM,
};

/* Seed of the pseudo-random pattern; changes every pass */
static uint32_t rand_seed;

/* Stateless hash so any word's expected value can be recomputed */
static ALWAYS_INLINE uint32_t rand_word(size_t idx)
{
	uint32_t x = ((uint32_t)idx * 0x9E3779B1U) + rand_seed;

	x ^= x >> 16;
	x *= 0x85EBCA6BU;
	x ^= x >> 13;
	x *= 0xC2B2AE35U;
	x ^= x >> 16;
	return x;
}

static void mismatch(uint32_t *count, volatile const uint32_t *addr, uint32_t exp, uint32_t got)
{
	if (*count < MAX_REPORTED) {
		printk("    mismatch @0x%08x: expected 0x%08x, read 0x%08x\n",
		       (uint32_t)(uintptr_t)addr, exp, got);
	}
	(*count)++;
}

/* Walking 1s and 0s on the data bus at one address */
static uint32_t test_data_bus(volatile uint32_t *addr)
{
	uint32_t fails = 0U;

	for (uint32_t bit = 1U; bit != 0U; bit <<= 1) {
		const uint32_t pat[2] = {bit, ~bit};

		for (size_t i = 0; i < ARRAY_SIZE(pat); i++) {
			uint32_t got;

			addr[0] = pat[i];
			/* Drive the bus with the complement before reading back */
			addr[1] = ~pat[i];
			got = addr[0];
			if (got != pat[i]) {
				mismatch(&fails, addr, pat[i], got);
			}
		}
	}

	return fails;
}

/* Stuck-high, stuck-low and shorted address lines at power-of-two word offsets */
static uint32_t test_addr_bus(volatile uint32_t *base)
{
	const uint32_t pat = 0xAAAAAAAAU;
	const uint32_t anti = 0x55555555U;
	const size_t mask = SDRAM_WORDS - 1U;
	uint32_t fails = 0U;

	for (size_t off = 1U; (off & mask) != 0U; off <<= 1) {
		base[off] = pat;
	}

	base[0] = anti;
	for (size_t off = 1U; (off & mask) != 0U; off <<= 1) {
		if (base[off] != pat) {
			mismatch(&fails, &base[off], pat, base[off]);
		}
	}
	base[0] = pat;

	for (size_t test = 1U; (test & mask) != 0U; test <<= 1) {
		base[test] = anti;

		if (base[0] != pat) {
			mismatch(&fails, &base[0], pat, base[0]);
		}

		for (size_t off = 1U; (off & mask) != 0U; off <<= 1) {
			if ((off != test) && (base[off] != pat)) {
				mismatch(&fails, &base[off], pat, base[off]);
			}
		}

		base[test] = pat;
	}

	return fails;
}

static ALWAYS_INLINE uint32_t fill_value(enum fill_kind kind, size_t idx)
{
	const uint32_t addr = (uint32_t)(SDRAM_BASE + (idx * sizeof(uint32_t)));
	const uint32_t checker = ((idx & 1U) != 0U) ? 0xAAAAAAAAU : 0x55555555U;

	switch (kind) {
	case FILL_ADDR:
		return addr;
	case FILL_INV_ADDR:
		return ~addr;
	case FILL_CHECKER:
		return checker;
	case FILL_INV_CHECKER:
		return ~checker;
	case FILL_RANDOM:
		return rand_word(idx);
	default:
		return ~rand_word(idx);
	}
}

static uint32_t test_fill(volatile uint32_t *base, enum fill_kind kind)
{
	uint32_t fails = 0U;

	for (size_t i = 0; i < SDRAM_WORDS; i++) {
		base[i] = fill_value(kind, i);
	}

	for (size_t i = 0; i < SDRAM_WORDS; i++) {
		uint32_t exp = fill_value(kind, i);
		uint32_t got = base[i];

		if (got != exp) {
			mismatch(&fails, &base[i], exp, got);
		}
	}

	return fails;
}

/*
 * March C-: {up w0} {up r0 w1} {up r1 w0} {down r0 w1} {down r1 w0} {down r0}
 * on 32-bit words with all-0 / all-1 backgrounds. Detects stuck-at,
 * transition, coupling and address-decoder faults.
 */
static ALWAYS_INLINE void march_rw(volatile uint32_t *p, uint32_t exp, uint32_t wr,
				   uint32_t *fails)
{
	const uint32_t got = *p;

	if (got != exp) {
		mismatch(fails, p, exp, got);
	}
	*p = wr;
}

static uint32_t test_march_c(volatile uint32_t *base, enum fill_kind kind)
{
	const uint32_t zero = 0x00000000U;
	const uint32_t one = 0xFFFFFFFFU;
	uint32_t fails = 0U;

	ARG_UNUSED(kind);

	for (size_t i = 0; i < SDRAM_WORDS; i++) {
		base[i] = zero;
	}
	for (size_t i = 0; i < SDRAM_WORDS; i++) {
		march_rw(&base[i], zero, one, &fails);
	}
	for (size_t i = 0; i < SDRAM_WORDS; i++) {
		march_rw(&base[i], one, zero, &fails);
	}
	for (size_t i = SDRAM_WORDS; i-- > 0U;) {
		march_rw(&base[i], zero, one, &fails);
	}
	for (size_t i = SDRAM_WORDS; i-- > 0U;) {
		march_rw(&base[i], one, zero, &fails);
	}
	for (size_t i = SDRAM_WORDS; i-- > 0U;) {
		const uint32_t got = base[i];

		if (got != zero) {
			mismatch(&fails, &base[i], zero, got);
		}
	}

	return fails;
}

static uint32_t run_step(const char *name, uint32_t (*fn)(volatile uint32_t *, enum fill_kind),
			 enum fill_kind kind)
{
	int64_t start = k_uptime_get();
	uint32_t fails = fn((volatile uint32_t *)SDRAM_BASE, kind);
	int64_t elapsed = k_uptime_get() - start;

	printk("  %-28s %s  %6u ms  %u errors\n", name, (fails == 0U) ? "PASS" : "FAIL",
	       (uint32_t)elapsed, fails);

	return fails;
}

static uint32_t step_data_bus(volatile uint32_t *base, enum fill_kind kind)
{
	ARG_UNUSED(kind);
	return test_data_bus(base);
}

static uint32_t step_addr_bus(volatile uint32_t *base, enum fill_kind kind)
{
	ARG_UNUSED(kind);
	return test_addr_bus(base);
}

uint32_t sdram_integrity_run(void)
{
	const bool cache_was_on = bench_dcache_enabled();
	uint32_t fails = 0U;

	printk("\n--- Integrity: %u MiB at 0x%08x, D-cache off ---\n",
	       (uint32_t)(SDRAM_SIZE / (1024U * 1024U)), (uint32_t)SDRAM_BASE);

	bench_dcache_set(false);

	for (uint32_t pass = 1U; pass <= CONFIG_SDRAM_BENCH_INTEGRITY_PASSES; pass++) {
		uint32_t pass_fails = 0U;

		rand_seed = 0x1234567U * pass;
		printk("  pass %u/%u (random seed 0x%08x)\n", pass,
		       (uint32_t)CONFIG_SDRAM_BENCH_INTEGRITY_PASSES, rand_seed);

		pass_fails += run_step("data bus walking 1/0", step_data_bus, FILL_ADDR);
		pass_fails += run_step("address bus", step_addr_bus, FILL_ADDR);
		pass_fails += run_step("address as data", test_fill, FILL_ADDR);
		pass_fails += run_step("inverted address", test_fill, FILL_INV_ADDR);
		pass_fails += run_step("checkerboard 55/AA", test_fill, FILL_CHECKER);
		pass_fails += run_step("inverted checkerboard AA/55", test_fill, FILL_INV_CHECKER);
		pass_fails += run_step("random pattern", test_fill, FILL_RANDOM);
		pass_fails += run_step("inverted random pattern", test_fill, FILL_INV_RANDOM);
		pass_fails += run_step("March C-", test_march_c, FILL_ADDR);

		printk("INTEG,%u,%u\n", pass, pass_fails);
		fails += pass_fails;
	}

	bench_dcache_set(cache_was_on);

	printk("Integrity %s\n", (fails == 0U) ? "PASSED" : "FAILED");

	return fails;
}
