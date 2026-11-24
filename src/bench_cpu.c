/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Basic sequential CPU throughput: write, read and copy over a 4 MiB window.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "sdram_bench.h"

#define CACHE_HIT_SIZE (16U * 1024U)
#define MEMSET_VALUE   0xA5U

void k_write64(const struct bench_kernel *k)
{
	volatile uint64_t *p = k->dst;
	const uint32_t n = (uint32_t)(k->len / sizeof(uint64_t));

	for (uint32_t i = 0; i < n; i += 4U) {
		p[i + 0U] = PAT64(i + 0U);
		p[i + 1U] = PAT64(i + 1U);
		p[i + 2U] = PAT64(i + 2U);
		p[i + 3U] = PAT64(i + 3U);
	}
}

static void k_memset(const struct bench_kernel *k)
{
	memset(k->dst, MEMSET_VALUE, k->len);
}

void k_read64(const struct bench_kernel *k)
{
	volatile const uint64_t *p = k->src;
	const size_t n = k->len / sizeof(uint64_t);
	uint64_t acc = 0U;

	for (size_t i = 0; i < n; i += 4U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
	}
	bench_sink = acc;
}

/* SDRAM window -> SRAM buffer, SRAM_BUF_SIZE at a time */
static void k_memcpy_to_sram(const struct bench_kernel *k)
{
	for (size_t off = 0; off < k->len; off += SRAM_BUF_SIZE) {
		memcpy(k->dst, (const uint8_t *)k->src + off, SRAM_BUF_SIZE);
	}
}

/* SRAM buffer -> SDRAM window, SRAM_BUF_SIZE at a time */
static void k_memcpy_from_sram(const struct bench_kernel *k)
{
	for (size_t off = 0; off < k->len; off += SRAM_BUF_SIZE) {
		memcpy((uint8_t *)k->dst + off, k->src, SRAM_BUF_SIZE);
	}
}

static void k_memcpy(const struct bench_kernel *k)
{
	memcpy(k->dst, k->src, k->len);
}

static void k_copy64_to_sram(const struct bench_kernel *k)
{
	struct bench_kernel chunk = *k;

	chunk.len = SRAM_BUF_SIZE;
	for (size_t off = 0; off < k->len; off += SRAM_BUF_SIZE) {
		chunk.src = (const uint8_t *)k->src + off;
		k_copy64(&chunk);
	}
}

bool verify_write64(const struct bench_kernel *k, uint32_t calls)
{
	const uint64_t *p = k->dst;

	ARG_UNUSED(calls);

	for (uint32_t i = 0; i < (uint32_t)(k->len / sizeof(uint64_t)); i++) {
		if (p[i] != PAT64(i)) {
			printk("    verify %s @0x%08x failed\n", k->name,
			       (uint32_t)(uintptr_t)&p[i]);
			return false;
		}
	}

	return true;
}

static bool verify_memset(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t *p = k->dst;
	const uint32_t exp = MEMSET_VALUE * 0x01010101U;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < k->len / sizeof(uint32_t); i++) {
		if (p[i] != exp) {
			printk("    verify %s @0x%08x: 0x%08x != 0x%08x\n", k->name,
			       (uint32_t)(uintptr_t)&p[i], p[i], exp);
			return false;
		}
	}

	return true;
}

static bool verify_to_sram(const struct bench_kernel *k, uint32_t calls)
{
	const uint8_t *last = (const uint8_t *)k->src + k->len - SRAM_BUF_SIZE;

	ARG_UNUSED(calls);

	return memcmp(k->dst, last, SRAM_BUF_SIZE) == 0;
}

static bool verify_from_sram(const struct bench_kernel *k, uint32_t calls)
{
	ARG_UNUSED(calls);

	for (size_t off = 0; off < k->len; off += SRAM_BUF_SIZE) {
		if (memcmp((const uint8_t *)k->dst + off, k->src, SRAM_BUF_SIZE) != 0) {
			printk("    verify %s failed at offset 0x%x\n", k->name, (uint32_t)off);
			return false;
		}
	}

	return true;
}

#define SDRAM_TEST(_name, _fn, _dst, _src, _mult, _wr, _verify)                                    \
	{                                                                                          \
		.group = "cpu", .name = _name, .fn = _fn, .dst = _dst, .src = _src,                \
		.len = BENCH_BUF_SIZE, .bus_mult = _mult, .writes_sdram = _wr, .verify = _verify,  \
	}

static const struct bench_kernel tests[] = {
	SDRAM_TEST("write32", k_write32, BENCH_SDRAM_A, NULL, 1U, true, verify_write32),
	SDRAM_TEST("write64", k_write64, BENCH_SDRAM_A, NULL, 1U, true, verify_write64),
	SDRAM_TEST("memset", k_memset, BENCH_SDRAM_A, NULL, 1U, true, verify_memset),
	SDRAM_TEST("read32", k_read32, NULL, BENCH_SDRAM_A, 1U, false, NULL),
	SDRAM_TEST("read64", k_read64, NULL, BENCH_SDRAM_A, 1U, false, NULL),
	SDRAM_TEST("memcpy_sdram_to_sram", k_memcpy_to_sram, bench_sram_buf, BENCH_SDRAM_A, 1U,
		   false, verify_to_sram),
	SDRAM_TEST("memcpy_sram_to_sdram", k_memcpy_from_sram, BENCH_SDRAM_A, bench_sram_buf, 1U,
		   true, verify_from_sram),
	SDRAM_TEST("memcpy_sdram_to_sdram", k_memcpy, BENCH_SDRAM_B, BENCH_SDRAM_A, 2U, true,
		   verify_copy),
	SDRAM_TEST("copy64_sdram_to_sram", k_copy64_to_sram, bench_sram_buf, BENCH_SDRAM_A, 1U,
		   false, verify_to_sram),
	SDRAM_TEST("copy64_sram_to_sdram", k_copy64_from_sram, BENCH_SDRAM_A, bench_sram_buf, 1U,
		   true, verify_from_sram),
	SDRAM_TEST("copy64_sdram_to_sdram", k_copy64, BENCH_SDRAM_B, BENCH_SDRAM_A, 2U, true,
		   verify_copy),
	{
		.group = "ref", .name = "sram_write32", .fn = k_write32, .dst = bench_sram_buf,
		.len = SRAM_BUF_SIZE, .repeat = BENCH_BUF_SIZE / SRAM_BUF_SIZE,
		.verify = verify_write32,
	},
	{
		.group = "ref", .name = "sram_read32", .fn = k_read32, .src = bench_sram_buf,
		.len = SRAM_BUF_SIZE, .repeat = BENCH_BUF_SIZE / SRAM_BUF_SIZE,
	},
	{
		.group = "ref", .name = "cachehit_sdram_read32", .fn = k_read32,
		.src = BENCH_SDRAM_A, .len = CACHE_HIT_SIZE,
		.repeat = BENCH_BUF_SIZE / CACHE_HIT_SIZE, .warm = true,
	},
};

uint32_t bench_cpu_run(void)
{
	for (size_t i = 0; i < sizeof(bench_sram_buf); i++) {
		bench_sram_buf[i] = (uint8_t)(i * 7U + 3U);
	}

	bench_section("Sequential CPU throughput");

	return bench_kernel_suite(tests, ARRAY_SIZE(tests), true);
}
