/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Access-pattern suite: mixed read/write, read-modify-write, random access,
 * pointer-chase latency, stride sweep, access-size sweep and block-size sweep.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "sdram_bench.h"

#define RAND_OPS     262144U
#define RAND_SEED    0x2545F491U
#define CHASE_HOPS   65536U
#define CHASE_STRIDE 64U
#define STRIDE_OPS   65536U
#define BLOCK_TOTAL  BENCH_BUF_SIZE
#define SMALL_WIN    (16U * 1024U)

static ALWAYS_INLINE uint32_t xorshift32(uint32_t x)
{
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

/* ---- mixed read/write: per 8-word group, R reads from src and 8-R writes to dst ---- */

static ALWAYS_INLINE void mix(const struct bench_kernel *k, const uint32_t reads)
{
	volatile const uint32_t *s = k->src;
	volatile uint32_t *d = k->dst;
	const size_t n = k->len / sizeof(uint32_t);
	uint32_t acc = 0U;

	for (size_t i = 0; i < n; i += 8U) {
		for (uint32_t r = 0; r < reads; r++) {
			acc += s[i + r];
		}
		for (uint32_t w = reads; w < 8U; w++) {
			d[i + w] = PAT32(i + w);
		}
	}
	bench_sink = acc;
}

static void k_mix_r6w2(const struct bench_kernel *k)
{
	mix(k, 6U);
}

static void k_mix_r4w4(const struct bench_kernel *k)
{
	mix(k, 4U);
}

static void k_mix_r2w6(const struct bench_kernel *k)
{
	mix(k, 2U);
}

static bool verify_mix(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t *d = k->dst;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < k->len / sizeof(uint32_t); i += 8U) {
		for (uint32_t w = k->param; w < 8U; w++) {
			if (d[i + w] != PAT32(i + w)) {
				printk("    verify %s @0x%08x failed\n", k->name,
				       (uint32_t)(uintptr_t)&d[i + w]);
				return false;
			}
		}
	}

	return true;
}

/* ---- read-modify-write: p[i] += 1 ---- */

static void k_rmw32(const struct bench_kernel *k)
{
	volatile uint32_t *p = k->dst;
	const size_t n = k->len / sizeof(uint32_t);

	for (size_t i = 0; i < n; i += 4U) {
		p[i + 0U] = p[i + 0U] + 1U;
		p[i + 1U] = p[i + 1U] + 1U;
		p[i + 2U] = p[i + 2U] + 1U;
		p[i + 3U] = p[i + 3U] + 1U;
	}
}

static void setup_rmw(const struct bench_kernel *k)
{
	k_write32(k);
	bench_cache_sync();
}

static bool verify_rmw(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t *p = k->dst;

	for (size_t i = 0; i < k->len / sizeof(uint32_t); i++) {
		if (p[i] != (PAT32(i) + calls)) {
			printk("    verify %s @0x%08x: 0x%08x != 0x%08x\n", k->name,
			       (uint32_t)(uintptr_t)&p[i], p[i], PAT32(i) + calls);
			return false;
		}
	}

	return true;
}

/* ---- random access: param accesses at xorshift32 word indices within len ---- */

static void k_rand_read32(const struct bench_kernel *k)
{
	volatile const uint32_t *p = k->src;
	const uint32_t mask = (uint32_t)(k->len / sizeof(uint32_t)) - 1U;
	uint32_t x = RAND_SEED;
	uint32_t acc = 0U;

	for (uint32_t i = 0; i < k->param; i++) {
		x = xorshift32(x);
		acc += p[x & mask];
	}
	bench_sink = acc;
}

static void k_rand_write32(const struct bench_kernel *k)
{
	volatile uint32_t *p = k->dst;
	const uint32_t mask = (uint32_t)(k->len / sizeof(uint32_t)) - 1U;
	const uint32_t count = k->param;
	uint32_t x = RAND_SEED;

	for (uint32_t i = 0; i < count; i++) {
		x = xorshift32(x);
		p[x & mask] = PAT32(x & mask);
	}
}

/* The written value depends only on the index, so repeated hits are harmless */
static bool verify_rand_write(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t *p = k->dst;
	const uint32_t mask = (uint32_t)(k->len / sizeof(uint32_t)) - 1U;
	uint32_t x = RAND_SEED;

	ARG_UNUSED(calls);

	for (uint32_t i = 0; i < k->param; i++) {
		x = xorshift32(x);
		if (p[x & mask] != PAT32(x & mask)) {
			printk("    verify %s @0x%08x failed\n", k->name,
			       (uint32_t)(uintptr_t)&p[x & mask]);
			return false;
		}
	}

	return true;
}

/* ---- pointer chase: dependent loads over a full-period LCG permutation ---- */

static void setup_chase(const struct bench_kernel *k)
{
	const uintptr_t base = (uintptr_t)k->src;
	const uint32_t nodes = (uint32_t)(k->len / CHASE_STRIDE);

	for (uint32_t i = 0; i < nodes; i++) {
		/* Hull-Dobell: c odd and a = 1 mod 4 give period 2^n */
		uint32_t next = ((i * 1664525U) + 1013904223U) & (nodes - 1U);

		*(volatile uint32_t *)(base + (i * CHASE_STRIDE)) =
			(uint32_t)(base + (next * CHASE_STRIDE));
	}
	bench_cache_sync();
}

static void k_chase(const struct bench_kernel *k)
{
	uint32_t p = (uint32_t)(uintptr_t)k->src;

	for (uint32_t i = 0; i < k->param; i++) {
		p = *(volatile const uint32_t *)p;
	}
	bench_sink = p;
}

/* ---- stride: one 32-bit read every param bytes ---- */

static void k_stride_read32(const struct bench_kernel *k)
{
	const uintptr_t base = (uintptr_t)k->src;
	const uint32_t stride = k->param;
	uint32_t acc = 0U;

	for (uint32_t i = 0; i < k->ops; i++) {
		acc += *(volatile const uint32_t *)(base + (i * stride));
	}
	bench_sink = acc;
}

/* ---- access size ---- */

static void k_write8(const struct bench_kernel *k)
{
	volatile uint8_t *p = k->dst;
	/* Local copy: volatile byte stores may alias *k and force a reload */
	const size_t len = k->len;

	for (size_t i = 0; i < len; i += 4U) {
		p[i + 0U] = (uint8_t)(i + 0U);
		p[i + 1U] = (uint8_t)(i + 1U);
		p[i + 2U] = (uint8_t)(i + 2U);
		p[i + 3U] = (uint8_t)(i + 3U);
	}
}

static bool verify_write8(const struct bench_kernel *k, uint32_t calls)
{
	const uint8_t *p = k->dst;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < k->len; i++) {
		if (p[i] != (uint8_t)i) {
			return false;
		}
	}

	return true;
}

static void k_write16(const struct bench_kernel *k)
{
	volatile uint16_t *p = k->dst;
	const size_t n = k->len / sizeof(uint16_t);

	for (size_t i = 0; i < n; i += 4U) {
		p[i + 0U] = (uint16_t)(i + 0U);
		p[i + 1U] = (uint16_t)(i + 1U);
		p[i + 2U] = (uint16_t)(i + 2U);
		p[i + 3U] = (uint16_t)(i + 3U);
	}
}

static bool verify_write16(const struct bench_kernel *k, uint32_t calls)
{
	const uint16_t *p = k->dst;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < k->len / sizeof(uint16_t); i++) {
		if (p[i] != (uint16_t)i) {
			return false;
		}
	}

	return true;
}

static void k_read8(const struct bench_kernel *k)
{
	volatile const uint8_t *p = k->src;
	uint32_t acc = 0U;

	for (size_t i = 0; i < k->len; i += 4U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
	}
	bench_sink = acc;
}

static void k_read16(const struct bench_kernel *k)
{
	volatile const uint16_t *p = k->src;
	const size_t n = k->len / sizeof(uint16_t);
	uint32_t acc = 0U;

	for (size_t i = 0; i < n; i += 4U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
	}
	bench_sink = acc;
}

static const uint32_t stm_pattern[8] = {
	0x01234567U, 0x89ABCDEFU, 0xFEDCBA98U, 0x76543210U,
	0x0F1E2D3CU, 0x4B5A6978U, 0x8796A5B4U, 0xC3D2E1F0U,
};

/* 8-word LDM: one 32-byte block per instruction */
static void k_read_ldm(const struct bench_kernel *k)
{
	const uint32_t *p = k->src;
	const uint32_t *end = (const uint32_t *)((const uint8_t *)k->src + k->len);

	__asm__ volatile("1:\n"
			 "ldmia %[p]!, {r2, r3, r4, r5, r6, r8, r10, r12}\n"
			 "cmp %[p], %[end]\n"
			 "blo 1b\n"
			 : [p] "+r"(p)
			 : [end] "r"(end)
			 : "r2", "r3", "r4", "r5", "r6", "r8", "r10", "r12", "cc", "memory");
}

/* 8-word STM: one 32-byte block per instruction */
static void k_write_stm(const struct bench_kernel *k)
{
	uint32_t *p = k->dst;
	uint32_t *end = (uint32_t *)((uint8_t *)k->dst + k->len);

	__asm__ volatile("ldmia %[pat], {r2, r3, r4, r5, r6, r8, r10, r12}\n"
			 "1:\n"
			 "stmia %[p]!, {r2, r3, r4, r5, r6, r8, r10, r12}\n"
			 "cmp %[p], %[end]\n"
			 "blo 1b\n"
			 : [p] "+r"(p)
			 : [end] "r"(end), [pat] "r"(stm_pattern)
			 : "r2", "r3", "r4", "r5", "r6", "r8", "r10", "r12", "cc", "memory");
}

static bool verify_stm(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t *p = k->dst;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < k->len / sizeof(uint32_t); i++) {
		if (p[i] != stm_pattern[i % 8U]) {
			return false;
		}
	}

	return true;
}

/* ---- block size: param-byte blocks at random block-aligned SDRAM offsets ---- */

static ALWAYS_INLINE uintptr_t block_addr(uint32_t *x, uint32_t block)
{
	*x = xorshift32(*x);
	return SDRAM_BASE + ((*x & ((uint32_t)(SDRAM_SIZE / block) - 1U)) * block);
}

static void k_block_read(const struct bench_kernel *k)
{
	const uint32_t block = k->param;
	uint32_t x = RAND_SEED;
	uint64_t acc = 0U;

	for (size_t done = 0; done < k->len; done += block) {
		volatile const uint64_t *p = (volatile const uint64_t *)block_addr(&x, block);

		for (uint32_t i = 0; i < block / sizeof(uint64_t); i += 4U) {
			acc += p[i + 0U];
			acc += p[i + 1U];
			acc += p[i + 2U];
			acc += p[i + 3U];
		}
	}
	bench_sink = acc;
}

static void k_block_write(const struct bench_kernel *k)
{
	const uint32_t block = k->param;
	const size_t len = k->len;
	uint32_t x = RAND_SEED;

	for (size_t done = 0; done < len; done += block) {
		uintptr_t a = block_addr(&x, block);
		volatile uint64_t *p = (volatile uint64_t *)a;
		const uint64_t v = PAT64(a);

		for (uint32_t i = 0; i < block / sizeof(uint64_t); i += 4U) {
			p[i + 0U] = v;
			p[i + 1U] = v;
			p[i + 2U] = v;
			p[i + 3U] = v;
		}
	}
}

static bool verify_block_write(const struct bench_kernel *k, uint32_t calls)
{
	const uint32_t block = k->param;
	uint32_t x = RAND_SEED;

	ARG_UNUSED(calls);

	for (size_t done = 0; done < k->len; done += block) {
		uintptr_t a = block_addr(&x, block);
		const uint64_t *p = (const uint64_t *)a;

		for (uint32_t i = 0; i < block / sizeof(uint64_t); i++) {
			if (p[i] != PAT64(a)) {
				printk("    verify %s @0x%08x failed\n", k->name, (uint32_t)a);
				return false;
			}
		}
	}

	return true;
}

/* ---- alternating regions: two line-strided streams param bytes apart ---- */

#define ALT_OPS    65536U
#define ALT_STRIDE 32U

static void k_alternate(const struct bench_kernel *k)
{
	const uintptr_t a = (uintptr_t)k->src;
	const uintptr_t b = a + k->param;
	uint32_t acc = 0U;

	for (uint32_t i = 0; i < ALT_OPS / 2U; i++) {
		acc += *(volatile const uint32_t *)(a + (i * ALT_STRIDE));
		acc += *(volatile const uint32_t *)(b + (i * ALT_STRIDE));
	}
	bench_sink = acc;
}

/* ---- alignment: 32-bit accesses at byte offset param, 64-bit at 0 or 4 ---- */

typedef uint32_t __aligned(1) u32_unaligned;

static void k_read32_off(const struct bench_kernel *k)
{
	volatile const u32_unaligned *p =
		(volatile const u32_unaligned *)((const uint8_t *)k->src + k->param);
	const size_t n = (k->len / sizeof(uint32_t)) - 1U;
	uint32_t acc = 0U;

	for (size_t i = 0; i < n; i += 4U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
	}
	bench_sink = acc;
}

static void k_write32_off(const struct bench_kernel *k)
{
	volatile u32_unaligned *p = (volatile u32_unaligned *)((uint8_t *)k->dst + k->param);
	const size_t n = (k->len / sizeof(uint32_t)) - 1U;

	for (size_t i = 0; i < n; i += 4U) {
		p[i + 0U] = PAT32(i + 0U);
		p[i + 1U] = PAT32(i + 1U);
		p[i + 2U] = PAT32(i + 2U);
		p[i + 3U] = PAT32(i + 3U);
	}
}

static bool verify_write32_off(const struct bench_kernel *k, uint32_t calls)
{
	const u32_unaligned *p = (const u32_unaligned *)((const uint8_t *)k->dst + k->param);
	const size_t n = (k->len / sizeof(uint32_t)) - 1U;

	ARG_UNUSED(calls);

	for (size_t i = 0; i < (n & ~(size_t)3U); i++) {
		if (p[i] != PAT32(i)) {
			printk("    verify %s @0x%08x failed\n", k->name,
			       (uint32_t)(uintptr_t)&p[i]);
			return false;
		}
	}

	return true;
}

/* LDRD/STRD need word alignment: offsets 0 (8-aligned) and 4 (only word-aligned) */
static void k_read64_off(const struct bench_kernel *k)
{
	volatile const uint64_t *p =
		(volatile const uint64_t *)((const uint8_t *)k->src + k->param);
	const size_t n = (k->len / sizeof(uint64_t)) - 1U;
	uint64_t acc = 0U;

	for (size_t i = 0; i < (n & ~(size_t)3U); i += 4U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
	}
	bench_sink = acc;
}

static void k_write64_off(const struct bench_kernel *k)
{
	volatile uint64_t *p = (volatile uint64_t *)((uint8_t *)k->dst + k->param);
	const uint32_t n = (uint32_t)((k->len / sizeof(uint64_t)) - 1U) & ~3U;

	for (uint32_t i = 0; i < n; i += 4U) {
		p[i + 0U] = PAT64(i + 0U);
		p[i + 1U] = PAT64(i + 1U);
		p[i + 2U] = PAT64(i + 2U);
		p[i + 3U] = PAT64(i + 3U);
	}
}

static bool verify_write64_off(const struct bench_kernel *k, uint32_t calls)
{
	const uint64_t *p = (const uint64_t *)((const uint8_t *)k->dst + k->param);
	const uint32_t n = (uint32_t)((k->len / sizeof(uint64_t)) - 1U) & ~3U;

	ARG_UNUSED(calls);

	for (uint32_t i = 0; i < n; i++) {
		if (p[i] != PAT64(i)) {
			printk("    verify %s @0x%08x failed\n", k->name,
			       (uint32_t)(uintptr_t)&p[i]);
			return false;
		}
	}

	return true;
}

/* ---- working-set sweep: copy uses the two halves of the set ---- */

static void k_copy64_half(const struct bench_kernel *k)
{
	const struct bench_kernel c = {
		.dst = (uint8_t *)k->dst + (k->len / 2U), .src = k->src, .len = k->len / 2U};

	k_copy64(&c);
}

static bool verify_copy_half(const struct bench_kernel *k, uint32_t calls)
{
	ARG_UNUSED(calls);

	return memcmp((const uint8_t *)k->dst + (k->len / 2U), k->src, k->len / 2U) == 0;
}

/* ---- test tables ---- */

#define FULL ((void *)SDRAM_BASE)

static const struct bench_kernel mixed_tests[] = {
	{.group = "mixed", .name = "mix_r75_w25", .fn = k_mix_r6w2, .dst = BENCH_SDRAM_B,
	 .src = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE, .bus_mult = 1U, .param = 6U,
	 .writes_sdram = true, .verify = verify_mix},
	{.group = "mixed", .name = "mix_r50_w50", .fn = k_mix_r4w4, .dst = BENCH_SDRAM_B,
	 .src = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE, .bus_mult = 1U, .param = 4U,
	 .writes_sdram = true, .verify = verify_mix},
	{.group = "mixed", .name = "mix_r25_w75", .fn = k_mix_r2w6, .dst = BENCH_SDRAM_B,
	 .src = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE, .bus_mult = 1U, .param = 2U,
	 .writes_sdram = true, .verify = verify_mix},
	{.group = "mixed", .name = "rmw32", .fn = k_rmw32, .dst = BENCH_SDRAM_A,
	 .len = BENCH_BUF_SIZE, .bus_mult = 2U, .writes_sdram = true, .setup = setup_rmw,
	 .verify = verify_rmw},
};

#define RAND_READ(_name, _base, _len, _warm)                                                       \
	{.group = "random", .name = _name, .fn = k_rand_read32, .src = _base, .len = _len,         \
	 .bytes = RAND_OPS * 4U, .ops = RAND_OPS, .bus_mult = 1U, .param = RAND_OPS,               \
	 .warm = _warm}
#define RAND_WRITE(_name, _base, _len)                                                             \
	{.group = "random", .name = _name, .fn = k_rand_write32, .dst = _base, .len = _len,        \
	 .bytes = RAND_OPS * 4U, .ops = RAND_OPS, .bus_mult = 1U, .param = RAND_OPS,               \
	 .writes_sdram = true, .verify = verify_rand_write}
#define CHASE(_name, _base, _len, _warm)                                                           \
	{.group = "latency", .name = _name, .fn = k_chase, .src = _base, .len = _len,              \
	 .bytes = CHASE_HOPS * 4U, .ops = CHASE_HOPS, .param = CHASE_HOPS, .warm = _warm,          \
	 .setup = setup_chase}

static const struct bench_kernel random_tests[] = {
	RAND_READ("rand_read32_16k", BENCH_SDRAM_A, SMALL_WIN, true),
	RAND_READ("rand_read32_4m", BENCH_SDRAM_A, SB_WIN_SIZE, false),
	RAND_READ("rand_read32_64m", FULL, SDRAM_SIZE, false),
	RAND_WRITE("rand_write32_4m", BENCH_SDRAM_A, SB_WIN_SIZE),
	RAND_WRITE("rand_write32_64m", FULL, SDRAM_SIZE),
	CHASE("chase_16k", BENCH_SDRAM_A, SMALL_WIN, true),
	CHASE("chase_4m", BENCH_SDRAM_A, SB_WIN_SIZE, false),
	CHASE("chase_64m", FULL, SDRAM_SIZE, false),
};

#define STRIDE(_s)                                                                                 \
	{.group = "stride", .name = "stride_" #_s, .fn = k_stride_read32, .src = FULL,             \
	 .len = MIN((size_t)STRIDE_OPS * (_s), SDRAM_SIZE),                                        \
	 .bytes = MIN((size_t)STRIDE_OPS, SDRAM_SIZE / (_s)) * 4U,                                 \
	 .ops = MIN(STRIDE_OPS, (uint32_t)(SDRAM_SIZE / (_s))), .bus_mult = 1U, .param = (_s)}

static const struct bench_kernel stride_tests[] = {
	STRIDE(4),    STRIDE(8),     STRIDE(16),    STRIDE(32),   STRIDE(64),
	STRIDE(128),  STRIDE(256),   STRIDE(512),   STRIDE(1024), STRIDE(2048),
	STRIDE(4096), STRIDE(8192),  STRIDE(16384), STRIDE(65536), STRIDE(262144),
};

#define SIZE_RD(_name, _fn)                                                                        \
	{.group = "size", .name = _name, .fn = _fn, .src = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE,   \
	 .bus_mult = 1U}
#define SIZE_WR(_name, _fn, _verify)                                                               \
	{.group = "size", .name = _name, .fn = _fn, .dst = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE,   \
	 .bus_mult = 1U, .writes_sdram = true, .verify = _verify}

static const struct bench_kernel size_tests[] = {
	SIZE_RD("read8", k_read8),
	SIZE_RD("read16", k_read16),
	SIZE_RD("read32", k_read32),
	SIZE_RD("read64", k_read64),
	SIZE_RD("read_ldm8x32", k_read_ldm),
	SIZE_WR("write8", k_write8, verify_write8),
	SIZE_WR("write16", k_write16, verify_write16),
	SIZE_WR("write32", k_write32, verify_write32),
	SIZE_WR("write64", k_write64, verify_write64),
	SIZE_WR("write_stm8x32", k_write_stm, verify_stm),
};

#define BLOCK_RD(_b)                                                                               \
	{.group = "block", .name = "block_read_" #_b, .fn = k_block_read, .len = BLOCK_TOTAL,      \
	 .bus_mult = 1U, .param = (_b)}
#define BLOCK_WR(_b)                                                                               \
	{.group = "block", .name = "block_write_" #_b, .fn = k_block_write, .len = BLOCK_TOTAL,    \
	 .bus_mult = 1U, .param = (_b), .writes_sdram = true, .verify = verify_block_write}

static const struct bench_kernel block_tests[] = {
	BLOCK_RD(32),   BLOCK_RD(128),  BLOCK_RD(512),   BLOCK_RD(2048),
	BLOCK_RD(8192), BLOCK_RD(32768), BLOCK_RD(131072),
	BLOCK_WR(32),   BLOCK_WR(128),  BLOCK_WR(512),   BLOCK_WR(2048),
	BLOCK_WR(8192), BLOCK_WR(32768), BLOCK_WR(131072),
};

#define ALT(_k)                                                                                    \
	{.group = "alt", .name = "alternate_2^" #_k, .fn = k_alternate, .src = FULL,               \
	 .bytes = ALT_OPS * 4U, .ops = ALT_OPS, .bus_mult = 1U, .param = (1U << (_k))}

static const struct bench_kernel alt_tests[] = {
	ALT(5),  ALT(6),  ALT(7),  ALT(8),  ALT(9),  ALT(10), ALT(11), ALT(12), ALT(13), ALT(14),
	ALT(15), ALT(16), ALT(17), ALT(18), ALT(19), ALT(20), ALT(21), ALT(22), ALT(23), ALT(24),
	ALT(25),
};

#define ALIGN_RD(_name, _fn, _off)                                                                 \
	{.group = "align", .name = _name, .fn = _fn, .src = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE,  \
	 .bus_mult = 1U, .param = (_off)}
#define ALIGN_WR(_name, _fn, _off, _verify)                                                        \
	{.group = "align", .name = _name, .fn = _fn, .dst = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE,  \
	 .bus_mult = 1U, .param = (_off), .writes_sdram = true, .verify = _verify}

static const struct bench_kernel align_tests[] = {
	ALIGN_RD("read32_off0", k_read32_off, 0U),
	ALIGN_RD("read32_off1", k_read32_off, 1U),
	ALIGN_RD("read32_off2", k_read32_off, 2U),
	ALIGN_RD("read32_off3", k_read32_off, 3U),
	ALIGN_RD("read64_off0", k_read64_off, 0U),
	ALIGN_RD("read64_off4", k_read64_off, 4U),
	ALIGN_WR("write32_off0", k_write32_off, 0U, verify_write32_off),
	ALIGN_WR("write32_off1", k_write32_off, 1U, verify_write32_off),
	ALIGN_WR("write32_off2", k_write32_off, 2U, verify_write32_off),
	ALIGN_WR("write32_off3", k_write32_off, 3U, verify_write32_off),
	ALIGN_WR("write64_off0", k_write64_off, 0U, verify_write64_off),
	ALIGN_WR("write64_off4", k_write64_off, 4U, verify_write64_off),
};

/* Each test moves at least 4 MiB per timed iteration */
#define WS_REPEAT(_sz) (((_sz) >= SB_WIN_SIZE) ? 1U : (uint32_t)(SB_WIN_SIZE / (_sz)))
#define WS(_label, _sz)                                                                            \
	{.group = "wset", .name = "read32_" _label, .fn = k_read32, .src = FULL, .len = (_sz),     \
	 .repeat = WS_REPEAT(_sz), .bus_mult = 1U},                                                \
	{.group = "wset", .name = "write32_" _label, .fn = k_write32, .dst = FULL, .len = (_sz),   \
	 .repeat = WS_REPEAT(_sz), .bus_mult = 1U, .writes_sdram = true,                           \
	 .verify = verify_write32},                                                                \
	{.group = "wset", .name = "copy64_" _label, .fn = k_copy64_half, .dst = FULL, .src = FULL, \
	 .len = (_sz), .bytes = (_sz) / 2U, .repeat = WS_REPEAT(_sz), .bus_mult = 2U,              \
	 .writes_sdram = true, .verify = verify_copy_half}

static const struct bench_kernel wset_tests[] = {
	WS("4k", 4096U),           WS("16k", 16384U),         WS("64k", 65536U),
	WS("256k", 262144U),       WS("1m", 1048576U),        WS("4m", 4194304U),
	WS("16m", 16777216U),      WS("32m", 33554432U),
};

uint32_t bench_access_run(void)
{
	uint32_t fails = 0U;

	bench_section("Mixed read/write");
	fails += bench_kernel_suite(mixed_tests, ARRAY_SIZE(mixed_tests), true);

	bench_section("Random access and latency (ns/op = per access or per dependent load)");
	fails += bench_kernel_suite(random_tests, ARRAY_SIZE(random_tests), true);

	bench_section("Stride sweep (one 32-bit read per stride)");
	fails += bench_kernel_suite(stride_tests, ARRAY_SIZE(stride_tests), true);

	bench_section("Access size sweep");
	fails += bench_kernel_suite(size_tests, ARRAY_SIZE(size_tests), true);

	bench_section("Block size sweep (random block-aligned positions in 64 MiB)");
	fails += bench_kernel_suite(block_tests, ARRAY_SIZE(block_tests), true);

	bench_section("Alternating regions: two 32 B-strided streams 2^k bytes apart");
	fails += bench_kernel_suite(alt_tests, ARRAY_SIZE(alt_tests), true);

	bench_section("Alignment (byte offset of 32-bit and 64-bit accesses)");
	fails += bench_kernel_suite(align_tests, ARRAY_SIZE(align_tests), true);

	bench_section("Working-set sweep 4 KiB - 32 MiB (>= 4 MiB moved per iteration)");
	fails += bench_kernel_suite(wset_tests, ARRAY_SIZE(wset_tests), true);

	return fails;
}
