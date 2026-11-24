/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMAC memory-to-memory SDRAM throughput (dma0 channel 0) with transfer-unit
 * and block-size sweeps, and a continuous background engine (channel 1) used
 * by the concurrent tests.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/timing/timing.h>
#include <string.h>
#include <bsp_api.h>

#include "sdram_bench.h"

#define DMA_FG_CHANNEL 0U
#define DMA_BG_CHANNEL 1U
#define DMA_BG_CHUNK   (16U * 1024U)
#define DMA_TIMEOUT_MS 100
#define DMA_MAX_XFERS  65535U

static const struct device *const dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma0));

static int dma_setup_adj(uint32_t channel, uint32_t unit, size_t chunk, dma_callback_t cb,
			 void *user_data, uint16_t src_adj, uint16_t dst_adj,
			 struct dma_block_config *blk, struct dma_config *cfg)
{
	*blk = (struct dma_block_config){
		.source_address = (uint32_t)(uintptr_t)bench_sram_buf,
		.dest_address = (uint32_t)SDRAM_BASE,
		.block_size = chunk,
		.source_addr_adj = src_adj,
		.dest_addr_adj = dst_adj,
	};
	*cfg = (struct dma_config){
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = unit,
		.dest_data_size = unit,
		.source_burst_length = unit,
		.dest_burst_length = unit,
		.block_count = 1U,
		.head_block = blk,
		.dma_callback = cb,
		.user_data = user_data,
	};

	return dma_config(dma_dev, channel, cfg);
}

static int dma_setup(uint32_t channel, uint32_t unit, size_t chunk, dma_callback_t cb,
		     struct dma_block_config *blk, struct dma_config *cfg)
{
	return dma_setup_adj(channel, unit, chunk, cb, NULL, DMA_ADDR_ADJ_INCREMENT,
			     DMA_ADDR_ADJ_INCREMENT, blk, cfg);
}

/* ---------------- background engine (channel 1) ---------------- */

static atomic_t bg_running;
static uint64_t bg_bytes;
static uint32_t bg_off;
static K_SEM_DEFINE(bg_stopped, 0, 1);

static void bg_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(user_data);

	if (status < 0) {
		atomic_clear(&bg_running);
		k_sem_give(&bg_stopped);
		return;
	}

	bg_bytes += DMA_BG_CHUNK;
	bg_off = (bg_off + DMA_BG_CHUNK) % SB_WIN_SIZE;

	if (!atomic_get(&bg_running) ||
	    (dma_reload(dev, channel, SB_WIN_C + bg_off, SB_WIN_D + bg_off, DMA_BG_CHUNK) != 0) ||
	    (dma_start(dev, channel) != 0)) {
		atomic_clear(&bg_running);
		k_sem_give(&bg_stopped);
	}
}

int bench_dma_bg_start(void)
{
	static struct dma_block_config blk;
	static struct dma_config cfg;
	int ret;

	if (!device_is_ready(dma_dev)) {
		return -ENODEV;
	}

	ret = dma_setup(DMA_BG_CHANNEL, 4U, DMA_BG_CHUNK, bg_cb, &blk, &cfg);
	if (ret != 0) {
		return ret;
	}

	bench_cache_sync();
	bg_bytes = 0U;
	bg_off = 0U;
	k_sem_reset(&bg_stopped);
	atomic_set(&bg_running, 1);

	ret = dma_reload(dma_dev, DMA_BG_CHANNEL, SB_WIN_C, SB_WIN_D, DMA_BG_CHUNK);
	if (ret == 0) {
		ret = dma_start(dma_dev, DMA_BG_CHANNEL);
	}
	if (ret != 0) {
		atomic_clear(&bg_running);
	}

	return ret;
}

void bench_dma_bg_stop(void)
{
	if (atomic_cas(&bg_running, 1, 0)) {
		(void)k_sem_take(&bg_stopped, K_MSEC(DMA_TIMEOUT_MS));
	}
}

uint64_t bench_dma_bg_bytes(void)
{
	unsigned int key = irq_lock();
	uint64_t b = bg_bytes;

	irq_unlock(key);

	return b;
}

/* ---------------- foreground tests (channel 0) ---------------- */

#if defined(CONFIG_SDRAM_BENCH_DMA)

enum dma_dir {
	DIR_SRAM_TO_SDRAM,
	DIR_SDRAM_TO_SRAM,
	DIR_SDRAM_TO_SDRAM,
};

struct dma_test {
	const char *name;
	enum dma_dir dir;
	uint32_t unit;
	size_t chunk;
};

#define DMA_CHUNK ((size_t)CONFIG_SDRAM_BENCH_DMA_CHUNK)

BUILD_ASSERT(DMA_CHUNK <= SRAM_BUF_SIZE, "DMA chunk must fit in the SRAM buffer");
BUILD_ASSERT((BENCH_BUF_SIZE % DMA_CHUNK) == 0U, "benchmark size must be a multiple of the chunk");

static const struct dma_test tests[] = {
	{"dma_sram_to_sdram", DIR_SRAM_TO_SDRAM, 4U, DMA_CHUNK},
	{"dma_sdram_to_sram", DIR_SDRAM_TO_SRAM, 4U, DMA_CHUNK},
	{"dma_sdram_to_sdram", DIR_SDRAM_TO_SDRAM, 4U, DMA_CHUNK},
	/* transfer unit sweep, 32 KiB blocks */
	{"dma_wr_unit1_32k", DIR_SRAM_TO_SDRAM, 1U, 32768U},
	{"dma_wr_unit2_32k", DIR_SRAM_TO_SDRAM, 2U, 32768U},
	{"dma_wr_unit4_32k", DIR_SRAM_TO_SDRAM, 4U, 32768U},
	{"dma_rd_unit1_32k", DIR_SDRAM_TO_SRAM, 1U, 32768U},
	{"dma_rd_unit2_32k", DIR_SDRAM_TO_SRAM, 2U, 32768U},
	{"dma_rd_unit4_32k", DIR_SDRAM_TO_SRAM, 4U, 32768U},
	/* block size sweep, 4-byte units */
	{"dma_wr_unit4_1k", DIR_SRAM_TO_SDRAM, 4U, 1024U},
	{"dma_wr_unit4_4k", DIR_SRAM_TO_SDRAM, 4U, 4096U},
	{"dma_wr_unit4_16k", DIR_SRAM_TO_SDRAM, 4U, 16384U},
	{"dma_wr_unit4_64k", DIR_SRAM_TO_SDRAM, 4U, 65536U},
	{"dma_wr_unit4_128k", DIR_SRAM_TO_SDRAM, 4U, 131072U},
	{"dma_rd_unit4_1k", DIR_SDRAM_TO_SRAM, 4U, 1024U},
	{"dma_rd_unit4_4k", DIR_SDRAM_TO_SRAM, 4U, 4096U},
	{"dma_rd_unit4_16k", DIR_SDRAM_TO_SRAM, 4U, 16384U},
	{"dma_rd_unit4_64k", DIR_SDRAM_TO_SRAM, 4U, 65536U},
	{"dma_rd_unit4_128k", DIR_SDRAM_TO_SRAM, 4U, 131072U},
};

static K_SEM_DEFINE(fg_done, 0, 1);
static volatile int fg_result;

static void fg_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	fg_result = status;
	k_sem_give(&fg_done);
}

static void dir_addrs(const struct dma_test *t, size_t n, uint32_t *src, uint32_t *dst)
{
	const uint32_t sram = (uint32_t)(uintptr_t)bench_sram_buf;
	const uint32_t off = (uint32_t)(n * t->chunk);

	switch (t->dir) {
	case DIR_SRAM_TO_SDRAM:
		*src = sram;
		*dst = SB_WIN_A + off;
		break;
	case DIR_SDRAM_TO_SRAM:
		*src = SB_WIN_A + off;
		*dst = sram;
		break;
	default:
		*src = SB_WIN_A + off;
		*dst = SB_WIN_B + off;
		break;
	}
}

static int dma_pass(const struct dma_test *t)
{
	for (size_t n = 0; n < (BENCH_BUF_SIZE / t->chunk); n++) {
		uint32_t src, dst;
		int ret;

		dir_addrs(t, n, &src, &dst);

		ret = dma_reload(dma_dev, DMA_FG_CHANNEL, src, dst, t->chunk);
		if (ret == 0) {
			ret = dma_start(dma_dev, DMA_FG_CHANNEL);
		}
		if (ret != 0) {
			return ret;
		}

		if (k_sem_take(&fg_done, K_MSEC(DMA_TIMEOUT_MS)) != 0) {
			(void)dma_stop(dma_dev, DMA_FG_CHANNEL);
			return -ETIMEDOUT;
		}

		if (fg_result < 0) {
			return fg_result;
		}
	}

	return 0;
}

static bool dma_verify(const struct dma_test *t)
{
	switch (t->dir) {
	case DIR_SRAM_TO_SDRAM:
		for (size_t off = 0; off < BENCH_BUF_SIZE; off += t->chunk) {
			if (memcmp((const uint8_t *)BENCH_SDRAM_A + off, bench_sram_buf,
				   t->chunk) != 0) {
				printk("    verify failed at offset 0x%x\n", (uint32_t)off);
				return false;
			}
		}
		return true;
	case DIR_SDRAM_TO_SRAM:
		return memcmp(bench_sram_buf,
			      (const uint8_t *)BENCH_SDRAM_A + BENCH_BUF_SIZE - t->chunk,
			      t->chunk) == 0;
	default:
		return memcmp(BENCH_SDRAM_B, BENCH_SDRAM_A, BENCH_BUF_SIZE) == 0;
	}
}

/* ---------------- memory <-> peripheral (CRC unit) ---------------- */

#define CRC_GPS_CRC32 4U
#define CRC_CHUNK     131072U

static void crc_reset(void)
{
	/* CRC-32, LSB first; DORCLR clears CRCDOR to 0 */
	R_CRC->CRCCR0 = (uint8_t)(CRC_GPS_CRC32 | R_CRC_CRCCR0_DORCLR_Msk);
}

/* Fixed-address side: step 0 for that end */
static int dma_pass_generic(uint32_t src, size_t src_step, uint32_t dst, size_t dst_step,
			    size_t chunk)
{
	for (size_t n = 0; n < (BENCH_BUF_SIZE / chunk); n++) {
		int ret = dma_reload(dma_dev, DMA_FG_CHANNEL, src + (uint32_t)(n * src_step),
				     dst + (uint32_t)(n * dst_step), chunk);

		if (ret == 0) {
			ret = dma_start(dma_dev, DMA_FG_CHANNEL);
		}
		if (ret != 0) {
			return ret;
		}
		if (k_sem_take(&fg_done, K_MSEC(DMA_TIMEOUT_MS)) != 0) {
			(void)dma_stop(dma_dev, DMA_FG_CHANNEL);
			return -ETIMEDOUT;
		}
		if (fg_result < 0) {
			return fg_result;
		}
	}

	return 0;
}

static uint32_t crc_cpu_reference(void)
{
	const uint32_t *p = (const uint32_t *)SB_WIN_A;

	crc_reset();
	for (size_t i = 0; i < BENCH_BUF_SIZE / sizeof(uint32_t); i++) {
		R_CRC->CRCDIR = p[i];
	}

	return R_CRC->CRCDOR;
}

static uint32_t dma_crc_run(void)
{
	static struct dma_block_config blk;
	static struct dma_config cfg;
	const uint32_t crcdir = (uint32_t)(uintptr_t)&R_CRC->CRCDIR;
	const uint32_t crcdor = (uint32_t)(uintptr_t)&R_CRC->CRCDOR;
	const struct bench_kernel fill = {.dst = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE};
	struct bench_stats st;
	uint32_t fails = 0U;
	uint32_t dma_crc = 0U;
	uint32_t ref;
	int ret;

	R_BSP_MODULE_START(FSP_IP_CRC, 0U);

	k_write32(&fill);
	bench_cache_sync();
	ref = crc_cpu_reference();

	/* SDRAM -> CRCDIR: DMA read stream into a fixed peripheral register */
	ret = dma_setup_adj(DMA_FG_CHANNEL, 4U, CRC_CHUNK, fg_cb, NULL, DMA_ADDR_ADJ_INCREMENT,
			    DMA_ADDR_ADJ_NO_CHANGE, &blk, &cfg);
	bench_stats_init(&st, BENCH_BUF_SIZE, 1U, BENCH_BUF_SIZE / 4U);
	for (uint32_t it = 0; (it < BENCH_ITER) && (ret == 0); it++) {
		timing_t t0, t1;

		bench_cache_sync();
		crc_reset();
		t0 = timing_counter_get();
		ret = dma_pass_generic(SB_WIN_A, CRC_CHUNK, crcdir, 0U, CRC_CHUNK);
		t1 = timing_counter_get();
		dma_crc = R_CRC->CRCDOR;
		bench_stats_add(&st, timing_cycles_to_ns(timing_cycles_get(&t0, &t1)));
		if (dma_crc != ref) {
			break;
		}
	}
	if (ret != 0) {
		printk("    FAIL: dma_sdram_to_crc transfer -> %d\n", ret);
		fails++;
	} else {
		bench_report("dma", "dma_sdram_to_periph_crc", bench_cache_name(), &st);
		printk("DMACRC,%08x,%08x,%s\n", dma_crc, ref,
		       (dma_crc == ref) ? "match" : "MISMATCH");
		if (dma_crc != ref) {
			printk("    FAIL: CRC of DMA-fed data 0x%08x != CPU-fed 0x%08x\n", dma_crc,
			       ref);
			fails++;
		}
	}

	/* CRCDOR -> SDRAM: DMA write stream from a fixed peripheral register */
	ret = dma_setup_adj(DMA_FG_CHANNEL, 4U, CRC_CHUNK, fg_cb, NULL, DMA_ADDR_ADJ_NO_CHANGE,
			    DMA_ADDR_ADJ_INCREMENT, &blk, &cfg);
	bench_stats_init(&st, BENCH_BUF_SIZE, 1U, BENCH_BUF_SIZE / 4U);
	for (uint32_t it = 0; (it < BENCH_ITER) && (ret == 0); it++) {
		timing_t t0, t1;

		bench_cache_sync();
		t0 = timing_counter_get();
		ret = dma_pass_generic(crcdor, 0U, SB_WIN_A, CRC_CHUNK, CRC_CHUNK);
		t1 = timing_counter_get();
		bench_stats_add(&st, timing_cycles_to_ns(timing_cycles_get(&t0, &t1)));
	}
	if (ret != 0) {
		printk("    FAIL: dma_crc_to_sdram transfer -> %d\n", ret);
		fails++;
	} else {
		const uint32_t *p = (const uint32_t *)SB_WIN_A;
		const uint32_t v = R_CRC->CRCDOR;

		bench_report("dma", "dma_periph_crc_to_sdram", bench_cache_name(), &st);
		bench_cache_sync();
		for (size_t i = 0; i < BENCH_BUF_SIZE / sizeof(uint32_t); i++) {
			if (p[i] != v) {
				printk("    FAIL: CRCDOR copy @0x%08x: 0x%08x != 0x%08x\n",
				       (uint32_t)(uintptr_t)&p[i], p[i], v);
				fails++;
				break;
			}
		}
	}

	return fails;
}

/* ---------------- several channels at once ---------------- */

#define MC_MAX   4U
#define MC_CHUNK 131072U

struct mc_chan {
	uint32_t ch;
	uint32_t src;
	uint32_t dst;
	size_t done;
	timing_t t_end;
	struct dma_block_config blk;
	struct dma_config cfg;
};

static struct mc_chan mc[MC_MAX];
static K_SEM_DEFINE(mc_sem, 0, MC_MAX);

static void mc_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	struct mc_chan *c = user_data;

	c->done += MC_CHUNK;
	if ((status >= 0) && (c->done < BENCH_BUF_SIZE) &&
	    (dma_reload(dev, channel, c->src + (uint32_t)c->done, c->dst + (uint32_t)c->done,
			MC_CHUNK) == 0) &&
	    (dma_start(dev, channel) == 0)) {
		return;
	}
	c->t_end = timing_counter_get();
	k_sem_give(&mc_sem);
}

static const char *const mc_names[2][MC_MAX + 1U] = {
	{"dma_2ch_ch0", "dma_2ch_ch1", NULL, NULL, "dma_2ch_total"},
	{"dma_4ch_ch0", "dma_4ch_ch1", "dma_4ch_ch2", "dma_4ch_ch3", "dma_4ch_total"},
};

static uint32_t dma_multichannel_run(uint32_t nch, const char *const *names)
{
	struct bench_stats st[MC_MAX + 1U];
	uint32_t fails = 0U;

	for (uint32_t i = 0; i < nch; i++) {
		const struct bench_kernel fill = {.dst = (void *)(SDRAM_BASE + (i * 8U * SB_MIB)),
						  .len = BENCH_BUF_SIZE};

		mc[i].ch = i;
		mc[i].src = (uint32_t)SDRAM_BASE + (i * 8U * SB_MIB);
		mc[i].dst = mc[i].src + (4U * SB_MIB);
		k_write32(&fill);
		if (dma_setup_adj(i, 4U, MC_CHUNK, mc_cb, &mc[i], DMA_ADDR_ADJ_INCREMENT,
				  DMA_ADDR_ADJ_INCREMENT, &mc[i].blk, &mc[i].cfg) != 0) {
			printk("    FAIL: dma_config channel %u\n", i);
			return 1U;
		}
		bench_stats_init(&st[i], BENCH_BUF_SIZE, 2U, 0U);
	}
	bench_stats_init(&st[MC_MAX], (size_t)nch * BENCH_BUF_SIZE, 2U, 0U);

	for (uint32_t it = 0; it < BENCH_ITER; it++) {
		timing_t t0;
		uint64_t worst = 0U;

		bench_cache_sync();
		k_sem_reset(&mc_sem);
		t0 = timing_counter_get();
		for (uint32_t i = 0; i < nch; i++) {
			mc[i].done = 0U;
			if ((dma_reload(dma_dev, i, mc[i].src, mc[i].dst, MC_CHUNK) != 0) ||
			    (dma_start(dma_dev, i) != 0)) {
				printk("    FAIL: start channel %u\n", i);
				return fails + 1U;
			}
		}
		for (uint32_t i = 0; i < nch; i++) {
			if (k_sem_take(&mc_sem, K_SECONDS(2)) != 0) {
				printk("    FAIL: %u-channel DMA timeout\n", nch);
				return fails + 1U;
			}
		}
		for (uint32_t i = 0; i < nch; i++) {
			uint64_t ns = timing_cycles_to_ns(timing_cycles_get(&t0, &mc[i].t_end));

			bench_stats_add(&st[i], ns);
			worst = MAX(worst, ns);
		}
		bench_stats_add(&st[MC_MAX], worst);
	}

	for (uint32_t i = 0; i < nch; i++) {
		bench_report("dma", names[i], bench_cache_name(), &st[i]);
	}
	bench_report("dma", names[MC_MAX], bench_cache_name(), &st[MC_MAX]);

	bench_cache_sync();
	for (uint32_t i = 0; i < nch; i++) {
		if (memcmp((const void *)mc[i].dst, (const void *)mc[i].src, BENCH_BUF_SIZE) != 0) {
			printk("    FAIL: channel %u copy check\n", i);
			fails++;
		}
	}

	return fails;
}

uint32_t bench_dma_run(void)
{
	static struct dma_block_config blk;
	static struct dma_config cfg;
	uint32_t fails = 0U;

	bench_section("DMAC throughput (dma0 ch0, software trigger)");

	if (!device_is_ready(dma_dev)) {
		printk("    FAIL: %s not ready\n", dma_dev->name);
		return 1U;
	}

	for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
		const struct dma_test *t = &tests[i];
		struct bench_stats st;
		int ret;

		if (((t->chunk / t->unit) > DMA_MAX_XFERS) || (t->chunk > SRAM_BUF_SIZE) ||
		    ((BENCH_BUF_SIZE % t->chunk) != 0U)) {
			printk("    skip %s: block exceeds hardware or buffer limits\n", t->name);
			continue;
		}

		ret = dma_setup(DMA_FG_CHANNEL, t->unit, t->chunk, fg_cb, &blk, &cfg);
		if (ret != 0) {
			printk("    FAIL: %s dma_config -> %d\n", t->name, ret);
			fails++;
			continue;
		}

		/* Re-seed the SRAM source so each SRAM to SDRAM test checks fresh data */
		for (size_t b = 0; b < SRAM_BUF_SIZE; b++) {
			bench_sram_buf[b] = (uint8_t)((b * 13U) + i);
		}

		bench_stats_init(&st, BENCH_BUF_SIZE, (t->dir == DIR_SDRAM_TO_SDRAM) ? 2U : 1U,
				 (uint32_t)(BENCH_BUF_SIZE / t->unit));

		for (uint32_t it = 0; (it < BENCH_ITER) && (ret == 0); it++) {
			timing_t t0, t1;

			/* DMA bypasses the cache: write dirty lines back, drop stale ones */
			bench_cache_sync();

			t0 = timing_counter_get();
			ret = dma_pass(t);
			t1 = timing_counter_get();

			bench_stats_add(&st, timing_cycles_to_ns(timing_cycles_get(&t0, &t1)));
		}

		if (ret != 0) {
			printk("    FAIL: %s transfer -> %d\n", t->name, ret);
			fails++;
			continue;
		}

		bench_report("dma", t->name, bench_cache_name(), &st);

		bench_cache_sync();
		if (!dma_verify(t)) {
			printk("    FAIL: data check after %s\n", t->name);
			fails++;
		}
	}

	bench_section("DMAC memory <-> peripheral (CRC unit, fixed-address end)");
	fails += dma_crc_run();

	bench_section("DMAC multiple channels at once (SDRAM to SDRAM, 128 KiB blocks)");
	fails += dma_multichannel_run(2U, mc_names[0]);
	fails += dma_multichannel_run(4U, mc_names[1]);

	return fails;
}

#endif /* CONFIG_SDRAM_BENCH_DMA */
