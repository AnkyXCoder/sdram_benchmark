/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared timed-kernel runner and basic access kernels.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/timing/timing.h>
#include <string.h>
#include <cmsis_core.h>

#include "sdram_bench.h"

static const uint32_t pmu_event_ids[PMU_EVENTS] = {
	[PMU_L1D_REFILL] = ARM_PMU_L1D_CACHE_REFILL,
	[PMU_BUS_ACCESS] = ARM_PMU_BUS_ACCESS,
	[PMU_MEM_ACCESS] = ARM_PMU_MEM_ACCESS,
	[PMU_STALL_BACKEND] = ARM_PMU_STALL_BACKEND,
};

static bool pmu_ok;

void bench_pmu_init(void)
{
	uint32_t counted = 0U;

	/* The PMU needs the debug/trace block enabled, as the DWT does */
	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	ARM_PMU_Enable();
	for (uint32_t e = 0; e < PMU_EVENTS; e++) {
		ARM_PMU_Set_EVTYPER(e, pmu_event_ids[e]);
	}
	ARM_PMU_CNTR_Enable(BIT_MASK(PMU_EVENTS));

	/* Self-test: software-increment each counter and read it back. If the
	 * counters cannot move, the PMU is unusable on this device.
	 */
	ARM_PMU_EVCNTR_ALL_Reset();
	ARM_PMU_CNTR_Increment(BIT_MASK(PMU_EVENTS));
	for (uint32_t e = 0; e < PMU_EVENTS; e++) {
		counted += ARM_PMU_Get_EVCNTR(e);
	}
	pmu_ok = counted != 0U;
	printk("PMU            : TYPE=0x%08x AUTHSTATUS=0x%08x, software increment self-test "
	       "%s (event counters %s)\n",
	       PMU->TYPE, PMU->AUTHSTATUS, pmu_ok ? "PASSED" : "FAILED",
	       pmu_ok ? "used" : "reported as n/a");
	ARM_PMU_EVCNTR_ALL_Reset();
}

bool bench_pmu_ok(void)
{
	return pmu_ok;
}

void bench_pmu_start(void)
{
	ARM_PMU_EVCNTR_ALL_Reset();
}

void bench_pmu_stop(uint32_t out[PMU_EVENTS])
{
	if (!pmu_ok) {
		return;
	}
	for (uint32_t e = 0; e < PMU_EVENTS; e++) {
		out[e] = ARM_PMU_Get_EVCNTR(e);
	}
}

uint8_t bench_sram_buf[SRAM_BUF_SIZE] __aligned(32);

/* Keeps read loops from being optimized away */
volatile uint64_t bench_sink;

void k_write32(const struct bench_kernel *k)
{
	volatile uint32_t *p = k->dst;
	const size_t n = k->len / sizeof(uint32_t);

	for (size_t i = 0; i < n; i += 8U) {
		p[i + 0U] = PAT32(i + 0U);
		p[i + 1U] = PAT32(i + 1U);
		p[i + 2U] = PAT32(i + 2U);
		p[i + 3U] = PAT32(i + 3U);
		p[i + 4U] = PAT32(i + 4U);
		p[i + 5U] = PAT32(i + 5U);
		p[i + 6U] = PAT32(i + 6U);
		p[i + 7U] = PAT32(i + 7U);
	}
}

void k_read32(const struct bench_kernel *k)
{
	volatile const uint32_t *p = k->src;
	const size_t n = k->len / sizeof(uint32_t);
	uint32_t acc = 0U;

	for (size_t i = 0; i < n; i += 8U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
		acc += p[i + 4U];
		acc += p[i + 5U];
		acc += p[i + 6U];
		acc += p[i + 7U];
	}
	bench_sink = acc;
}

/* Word-wise copy with 64-bit loads and stores, 32 bytes per iteration */
static ALWAYS_INLINE void copy64(void *dst, const void *src, size_t len)
{
	volatile uint64_t *d = dst;
	volatile const uint64_t *s = src;
	const size_t n = len / sizeof(uint64_t);

	for (size_t i = 0; i < n; i += 4U) {
		uint64_t a = s[i + 0U];
		uint64_t b = s[i + 1U];
		uint64_t c = s[i + 2U];
		uint64_t e = s[i + 3U];

		d[i + 0U] = a;
		d[i + 1U] = b;
		d[i + 2U] = c;
		d[i + 3U] = e;
	}
}

void k_copy64(const struct bench_kernel *k)
{
	copy64(k->dst, k->src, k->len);
}

/* SRAM buffer -> SDRAM window, SRAM_BUF_SIZE at a time */
void k_copy64_from_sram(const struct bench_kernel *k)
{
	for (size_t off = 0; off < k->len; off += SRAM_BUF_SIZE) {
		copy64((uint8_t *)k->dst + off, k->src, SRAM_BUF_SIZE);
	}
}

bool verify_write32(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t *p = k->dst;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < k->len / sizeof(uint32_t); i++) {
		if (p[i] != PAT32(i)) {
			printk("    verify %s @0x%08x: 0x%08x != 0x%08x\n", k->name,
			       (uint32_t)(uintptr_t)&p[i], p[i], PAT32(i));
			return false;
		}
	}

	return true;
}

bool verify_copy(const struct bench_kernel *k, uint32_t calls)
{
	ARG_UNUSED(calls);

	return memcmp(k->dst, k->src, k->len) == 0;
}

void bench_kernel_measure(const struct bench_kernel *k, bool lock_irq, struct bench_stats *st)
{
	const bool cached = bench_dcache_enabled();
	const uint32_t repeat = (k->repeat == 0U) ? 1U : k->repeat;
	const size_t bytes = (k->bytes == 0U) ? k->len : k->bytes;

	bench_stats_init(st, bytes * repeat, k->bus_mult, k->ops * repeat);

	if (k->setup != NULL) {
		k->setup(k);
	}

	for (uint32_t it = 0; it < BENCH_ITER; it++) {
		uint32_t pmu[PMU_EVENTS];
		timing_t t0, t1;
		unsigned int key = 0U;

		bench_cache_sync();
		if (k->warm) {
			k->fn(k);
		}

		if (lock_irq) {
			key = irq_lock();
		}
		bench_pmu_start();
		t0 = timing_counter_get();
		for (uint32_t r = 0; r < repeat; r++) {
			k->fn(k);
		}
		/* Count write-back of dirty lines as part of the write */
		if (cached && k->writes_sdram) {
			(void)sys_cache_data_flush_all();
		}
		t1 = timing_counter_get();
		bench_pmu_stop(pmu);
		if (lock_irq) {
			irq_unlock(key);
		}

		bench_stats_add(st, timing_cycles_to_ns(timing_cycles_get(&t0, &t1)));
		bench_stats_add_pmu(st, pmu);
	}
}

uint32_t bench_kernel_run(const struct bench_kernel *k)
{
	const uint32_t repeat = (k->repeat == 0U) ? 1U : k->repeat;
	struct bench_stats st;

	bench_kernel_measure(k, true, &st);
	bench_report(k->group, k->name, bench_cache_name(), &st);

	bench_cache_sync();
	if ((k->verify != NULL) &&
	    !k->verify(k, (BENCH_ITER * repeat) + (k->warm ? BENCH_ITER : 0U))) {
		printk("    FAIL: data check after %s (cache %s)\n", k->name, bench_cache_name());
		return 1U;
	}

	return 0U;
}

uint32_t bench_kernel_suite(const struct bench_kernel *ks, size_t n, bool both_modes)
{
	static const bool modes[] = {true, false};
	const bool cache_was_on = bench_dcache_enabled();
	uint32_t fails = 0U;

	for (size_t m = 0; m < ARRAY_SIZE(modes); m++) {
		if ((modes[m] && !IS_ENABLED(CONFIG_DCACHE)) || (!modes[m] && !both_modes)) {
			continue;
		}

		bench_dcache_set(modes[m]);

		for (size_t i = 0; i < n; i++) {
			fails += bench_kernel_run(&ks[i]);
		}
	}

	bench_dcache_set(cache_was_on);

	return fails;
}
