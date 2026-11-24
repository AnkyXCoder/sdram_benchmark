/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * STREAM-style kernels (McCalpin): Copy, Scale, Add and Triad over three
 * 4 MiB arrays in SDRAM windows A, B and C, in float (single precision, the
 * Cortex-M85 FPU) and uint32_t variants. Bytes are counted as STREAM does:
 * 2 arrays for Copy and Scale, 3 for Add and Triad.
 */

#include <zephyr/kernel.h>

#include "sdram_bench.h"

#define N       (BENCH_BUF_SIZE / sizeof(uint32_t))
#define SCALARF 3.0f
#define SCALARU 3U

static float *const af = (float *)SB_WIN_A;
static float *const bf = (float *)SB_WIN_B;
static float *const cf = (float *)SB_WIN_C;
static uint32_t *const au = (uint32_t *)SB_WIN_A;
static uint32_t *const bu = (uint32_t *)SB_WIN_B;
static uint32_t *const cu = (uint32_t *)SB_WIN_C;

static void k_copy_f(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		cf[i] = af[i];
	}
}

static void k_scale_f(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		bf[i] = SCALARF * cf[i];
	}
}

static void k_add_f(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		cf[i] = af[i] + bf[i];
	}
}

static void k_triad_f(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		af[i] = bf[i] + (SCALARF * cf[i]);
	}
}

static void k_copy_u(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		cu[i] = au[i];
	}
}

static void k_scale_u(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		bu[i] = SCALARU * cu[i];
	}
}

static void k_add_u(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		cu[i] = au[i] + bu[i];
	}
}

static void k_triad_u(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		au[i] = bu[i] + (SCALARU * cu[i]);
	}
}

/* Each kernel is idempotent for unchanged inputs, so its output can be checked */
static bool verify_f(const struct bench_kernel *k, uint32_t calls)
{
	ARG_UNUSED(calls);

	for (size_t i = 0; i < N; i++) {
		bool ok;

		switch (k->param) {
		case 0:
			ok = cf[i] == af[i];
			break;
		case 1:
			ok = bf[i] == (SCALARF * cf[i]);
			break;
		case 2:
			ok = cf[i] == (af[i] + bf[i]);
			break;
		default:
			ok = af[i] == (bf[i] + (SCALARF * cf[i]));
			break;
		}
		if (!ok) {
			printk("    verify %s failed at element %u\n", k->name, (uint32_t)i);
			return false;
		}
	}

	return true;
}

static bool verify_u(const struct bench_kernel *k, uint32_t calls)
{
	ARG_UNUSED(calls);

	for (size_t i = 0; i < N; i++) {
		bool ok;

		switch (k->param) {
		case 0:
			ok = cu[i] == au[i];
			break;
		case 1:
			ok = bu[i] == (SCALARU * cu[i]);
			break;
		case 2:
			ok = cu[i] == (au[i] + bu[i]);
			break;
		default:
			ok = au[i] == (bu[i] + (SCALARU * cu[i]));
			break;
		}
		if (!ok) {
			printk("    verify %s failed at element %u\n", k->name, (uint32_t)i);
			return false;
		}
	}

	return true;
}

static void init_f(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		af[i] = 1.0f;
		bf[i] = 2.0f;
		cf[i] = 0.0f;
	}
	bench_cache_sync();
}

static void init_u(const struct bench_kernel *k)
{
	ARG_UNUSED(k);
	for (size_t i = 0; i < N; i++) {
		au[i] = (uint32_t)i;
		bu[i] = 2U * (uint32_t)i;
		cu[i] = 0U;
	}
	bench_cache_sync();
}

#define STREAM(_name, _fn, _arrays, _id, _verify, _setup)                                          \
	{.group = "stream", .name = _name, .fn = _fn, .len = BENCH_BUF_SIZE,                       \
	 .bytes = (_arrays) * BENCH_BUF_SIZE, .bus_mult = 1U, .param = (_id),                      \
	 .writes_sdram = true,                                                                     \
	 .setup = _setup, .verify = _verify}

static const struct bench_kernel stream_tests[] = {
	STREAM("copy_f32", k_copy_f, 2U, 0U, verify_f, init_f),
	STREAM("scale_f32", k_scale_f, 2U, 1U, verify_f, NULL),
	STREAM("add_f32", k_add_f, 3U, 2U, verify_f, NULL),
	STREAM("triad_f32", k_triad_f, 3U, 3U, verify_f, NULL),
	STREAM("copy_u32", k_copy_u, 2U, 0U, verify_u, init_u),
	STREAM("scale_u32", k_scale_u, 2U, 1U, verify_u, NULL),
	STREAM("add_u32", k_add_u, 3U, 2U, verify_u, NULL),
	STREAM("triad_u32", k_triad_u, 3U, 3U, verify_u, NULL),
};

uint32_t bench_stream_run(void)
{
	bench_section("STREAM Copy/Scale/Add/Triad, 3 x 4 MiB arrays (bytes as STREAM counts)");

	return bench_kernel_suite(stream_tests, ARRAY_SIZE(stream_tests), true);
}
