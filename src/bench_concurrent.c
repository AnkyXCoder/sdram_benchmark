/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Concurrent bus masters: CPU kernels measured alone and while another
 * master (DMAC, CM33, GLCDC scan-out, or all of them) uses the SDRAM.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/display.h>
#include <zephyr/timing/timing.h>
#include <string.h>
#include <bsp_api.h>

#include "sdram_bench.h"

#define MASTER_SOLO_MS 300
#define CM33_BOOT_MS   1000
#define CM33_ACK_MS    100
#define GLCDC_START_MS 500

struct master {
	const char *name;
	int (*start)(uint32_t mode);
	void (*stop)(void);
	uint64_t (*bytes)(void);
	bool (*check)(void); /* data / health check after stop, may be NULL */
	void (*health_reset)(void); /* per CPU test health window, may be NULL */
	bool (*healthy)(void);
	uint32_t mode;
	uint32_t bus_mult;
};

/* ---------------- DMAC background ---------------- */

static int dma_start(uint32_t mode)
{
	ARG_UNUSED(mode);
	return bench_dma_bg_start();
}

static bool dma_check(void)
{
	if (bench_dma_bg_bytes() < SB_WIN_SIZE) {
		return true; /* less than one full pass: nothing complete to compare */
	}

	return memcmp((const void *)SB_WIN_D, (const void *)SB_WIN_C, SB_WIN_SIZE) == 0;
}

/* ---------------- CM33 traffic generator ---------------- */

#if defined(CONFIG_SDRAM_BENCH_CM33)

static struct sb_mbox *const mbox = SB_MBOX;
static bool cm33_up;
static uint32_t cm33_mode;

/*
 * Same sequence as the FSP R_BSP_SecondaryCoreStart(), which is only built
 * with CONFIG_SOC_RA_ENABLE_START_SECOND_CORE. That option starts CPU1 at
 * boot, before the CM33 image is known to be ours, so it stays disabled.
 */
#define CPU1_ACTCSR_KEY 0xA5U

static void cm33_core_start(void)
{
	R_CPU_CTRL->CPU1INITVTOR = (uint32_t)DT_REG_ADDR(DT_NODELABEL(code_mram_cm33));
	R_CPU_CTRL->CPU1WAITCR = 0U;
	R_CPU_CTRL->CPU1ACTCSR = (CPU1_ACTCSR_KEY << R_CPU_CTRL_CPU1ACTCSR_KEY_Pos) |
				 R_CPU_CTRL_CPU1ACTCSR_ACTREQ_Msk;
}

static void mbox_ctl_flush(void)
{
	(void)sys_cache_data_flush_range((void *)&mbox->ctl, sizeof(mbox->ctl));
}

static void mbox_st_invd(void)
{
	(void)sys_cache_data_invd_range((void *)&mbox->st, sizeof(mbox->st));
}

static int cm33_boot(void)
{
	int64_t deadline;

	if (cm33_up) {
		return 0;
	}

	memset((void *)mbox, 0, sizeof(*mbox));
	(void)sys_cache_data_flush_range((void *)mbox, sizeof(*mbox));

	cm33_core_start();

	deadline = k_uptime_get() + CM33_BOOT_MS;
	while (k_uptime_get() < deadline) {
		mbox_st_invd();
		if (mbox->st.magic == SB_CM33_MAGIC) {
			cm33_up = true;
			return 0;
		}
		k_msleep(1);
	}

	return -ETIMEDOUT;
}

static int cm33_cmd(uint32_t cmd)
{
	const uint32_t seq = mbox->ctl.seq + 1U;
	int64_t deadline = k_uptime_get() + CM33_ACK_MS;

	mbox->ctl.cmd = cmd;
	mbox->ctl.seq = seq;
	mbox_ctl_flush();

	while (k_uptime_get() < deadline) {
		mbox_st_invd();
		if (mbox->st.ack == seq) {
			return 0;
		}
		k_busy_wait(50);
	}

	return -ETIMEDOUT;
}

static int cm33_start(uint32_t mode)
{
	int ret = cm33_boot();

	if (ret != 0) {
		return ret;
	}

	cm33_mode = mode;

	return cm33_cmd(mode);
}

static void cm33_stop(void)
{
	(void)cm33_cmd(SB_CMD_IDLE);
}

static uint64_t cm33_bytes(void)
{
	mbox_st_invd();

	return (uint64_t)mbox->st.blocks * SB_CM33_BLOCK;
}

static bool cm33_check(void)
{
	const uint32_t *d = (const uint32_t *)SB_CM33_DST;

	mbox_st_invd();
	if (mbox->st.blocks < (SB_WIN_SIZE / SB_CM33_BLOCK)) {
		return true;
	}

	bench_cache_sync();

	if (cm33_mode == SB_CMD_COPY) {
		return memcmp((const void *)SB_CM33_DST, (const void *)SB_CM33_SRC,
			      SB_WIN_SIZE) == 0;
	}

	if (cm33_mode == SB_CMD_WRITE) {
		for (uint32_t i = 0; i < SB_WIN_SIZE / sizeof(uint32_t); i++) {
			if (d[i] != PAT32(i)) {
				printk("    CM33 write check @0x%08x failed\n",
				       (uint32_t)(uintptr_t)&d[i]);
				return false;
			}
		}
	}

	return true;
}

#endif /* CONFIG_SDRAM_BENCH_CM33 */

/* ---------------- GLCDC scan-out ---------------- */

#if defined(CONFIG_SDRAM_BENCH_GLCDC)

#define GLCDC_NODE     DT_NODELABEL(zephyr_lcdif)
#define GLCDC_LINE_IRQ DT_IRQ_BY_NAME(GLCDC_NODE, line, irq)

static const struct device *const disp = DEVICE_DT_GET(GLCDC_NODE);
static size_t frame_bytes;
static uint32_t frame_ns;
static atomic_t flips;
static atomic_t flip_enable;
static bool glcdc_started;
static int64_t scan_t0_us;
static struct display_buffer_descriptor fb_desc;

K_THREAD_STACK_DEFINE(flip_stack, 1024);
static struct k_thread flip_thread;

/* Alternate the two frame buffers through the display API */
static void flip_entry(void *p1, void *p2, void *p3)
{
	const void *fb[2] = {(const void *)SB_FB0, (const void *)SB_FB1};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t n = 0U; atomic_get(&flip_enable) != 0; n++) {
		if (display_write(disp, 0, 0, &fb_desc, fb[n & 1U]) == 0) {
			atomic_inc(&flips);
		}
	}
}

/*
 * Frame period from the GLCDC line-detect flag (VPOS), polled with
 * interrupts locked. Independent of the display driver.
 */
static uint32_t glcdc_hw_frame_ns(void)
{
	const uint32_t frames_to_time = 8U;
	timing_t t0 = 0U, t1;
	unsigned int key = irq_lock();

	for (uint32_t f = 0; f <= frames_to_time; f++) {
		R_GLCDC->SYSCNT.STCLR = R_GLCDC_SYSCNT_STCLR_VPOSCLR_Msk;
		while ((R_GLCDC->SYSCNT.STMON & R_GLCDC_SYSCNT_STMON_VPOS_Msk) == 0U) {
		}
		if (f == 0U) {
			t0 = timing_counter_get();
		}
	}
	t1 = timing_counter_get();
	irq_unlock(key);

	return (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&t0, &t1)) / frames_to_time);
}

static void glcdc_underflow_clear(void)
{
	R_GLCDC->SYSCNT.DTCTEN |= R_GLCDC_SYSCNT_DTCTEN_L1UNDFDTC_Msk;
	R_GLCDC->SYSCNT.STCLR = R_GLCDC_SYSCNT_STCLR_L1UNDFCLR_Msk;
}

static bool glcdc_underflow(void)
{
	return (R_GLCDC->SYSCNT.STMON & R_GLCDC_SYSCNT_STMON_L1UNDF_Msk) != 0U;
}

static int64_t now_us(void)
{
	return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static int glcdc_first_start(void)
{
	struct display_capabilities caps;
	int64_t deadline;

	if (!device_is_ready(disp)) {
		return -ENODEV;
	}

	display_get_capabilities(disp, &caps);
	frame_bytes = (size_t)caps.x_resolution * caps.y_resolution *
		      DISPLAY_BITS_PER_PIXEL(caps.current_pixel_format) / 8U;
	fb_desc = (struct display_buffer_descriptor){
		.buf_size = frame_bytes,
		.width = caps.x_resolution,
		.height = caps.y_resolution,
		.pitch = caps.x_resolution,
	};
	printk("    GLCDC %ux%u, %u bytes/frame, frame buffers 0x%08x/0x%08x\n",
	       caps.x_resolution, caps.y_resolution, (uint32_t)frame_bytes, SB_FB0, SB_FB1);

	for (size_t i = 0; i < frame_bytes / sizeof(uint32_t); i++) {
		((uint32_t *)SB_FB0)[i] = PAT32(i);
		((uint32_t *)SB_FB1)[i] = ~PAT32(i);
	}
	bench_cache_sync();

	/* The first display_write() starts the GLCDC */
	atomic_set(&flip_enable, 1);
	k_thread_create(&flip_thread, flip_stack, K_THREAD_STACK_SIZEOF(flip_stack), flip_entry,
			NULL, NULL, NULL, K_PRIO_COOP(2), 0, K_NO_WAIT);
	k_thread_name_set(&flip_thread, "glcdc_flip");

	deadline = k_uptime_get() + GLCDC_START_MS;
	while (atomic_get(&flips) < 1) {
		if (k_uptime_get() > deadline) {
			atomic_clear(&flip_enable);
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}

	/* Driver diagnostic: completed vsync waits per second over 200 ms */
	atomic_clear(&flips);
	k_msleep(200);
	printk("    display_write() vsync waits completed: %u per second\n",
	       (uint32_t)atomic_get(&flips) * 5U);
	atomic_clear(&flip_enable);
	(void)k_thread_join(&flip_thread, K_MSEC(100));

	/*
	 * Scan-out continues without the driver. Its line-detect interrupt is
	 * not needed and would load the CPU, so it is disabled.
	 */
	irq_disable(GLCDC_LINE_IRQ);
	R_GLCDC->SYSCNT.INTEN &= ~R_GLCDC_SYSCNT_INTEN_VPOSINTEN_Msk;

	frame_ns = glcdc_hw_frame_ns();
	if ((frame_ns == 0U) || (frame_ns > 100000000U)) {
		return -EIO;
	}

	glcdc_underflow_clear();
	k_msleep(100);
	printk("    GLCDC frame period %u us (%u.%u frames/s), scan-out %u.%u MB/s, "
	       "idle underflow: %s\n",
	       frame_ns / 1000U, (uint32_t)(10000000000ULL / frame_ns) / 10U,
	       (uint32_t)(10000000000ULL / frame_ns) % 10U,
	       (uint32_t)(((uint64_t)frame_bytes * 10000U) / frame_ns) / 10U,
	       (uint32_t)(((uint64_t)frame_bytes * 10000U) / frame_ns) % 10U,
	       glcdc_underflow() ? "yes" : "no");

	scan_t0_us = now_us();
	glcdc_started = true;

	return 0;
}

static int glcdc_start(uint32_t mode)
{
	ARG_UNUSED(mode);

	if (!glcdc_started) {
		int ret = glcdc_first_start();

		if (ret != 0) {
			return ret;
		}
	}

	glcdc_underflow_clear();

	return 0;
}

static void glcdc_stop(void)
{
	/* The display API cannot stop scan-out; it continues in the background */
}

/* Scan-out runs continuously: bytes fetched = elapsed / frame period x frame size */
static uint64_t glcdc_bytes(void)
{
	return ((uint64_t)(now_us() - scan_t0_us) * 1000U / frame_ns) * frame_bytes;
}

static bool glcdc_no_underflow(void)
{
	return !glcdc_underflow();
}

/* Underflow is reported per CPU test as a warning; only a stalled or
 * changed frame timing fails the check.
 */
static bool glcdc_check(void)
{
	uint32_t ns = glcdc_hw_frame_ns();
	bool ok = true;

	if ((ns > (frame_ns + (frame_ns / 100U))) || (ns < (frame_ns - (frame_ns / 100U)))) {
		printk("    GLCDC frame period changed: %u -> %u ns\n", frame_ns, ns);
		ok = false;
	}

	return ok;
}

#endif /* CONFIG_SDRAM_BENCH_GLCDC */

/* ---------------- all masters together ---------------- */

static const struct master masters[] = {
	{"dma_sdram_to_sdram", dma_start, bench_dma_bg_stop, bench_dma_bg_bytes, dma_check, NULL,
	 NULL, 0U, 2U},
#if defined(CONFIG_SDRAM_BENCH_CM33)
	{"cm33_read", cm33_start, cm33_stop, cm33_bytes, cm33_check, NULL, NULL, SB_CMD_READ, 1U},
	{"cm33_write", cm33_start, cm33_stop, cm33_bytes, cm33_check, NULL, NULL, SB_CMD_WRITE,
	 1U},
	{"cm33_copy", cm33_start, cm33_stop, cm33_bytes, cm33_check, NULL, NULL, SB_CMD_COPY, 2U},
#endif
#if defined(CONFIG_SDRAM_BENCH_GLCDC)
	{"glcdc_scanout", glcdc_start, glcdc_stop, glcdc_bytes, glcdc_check,
	 glcdc_underflow_clear, glcdc_no_underflow, 0U, 1U},
#endif
};

/* Combined scenario: DMAC + CM33 copy + GLCDC (whatever is built in) */
static const struct master *const combo[] = {
	&masters[0],
#if defined(CONFIG_SDRAM_BENCH_CM33)
	&masters[3],
#endif
#if defined(CONFIG_SDRAM_BENCH_GLCDC)
	&masters[ARRAY_SIZE(masters) - 1U],
#endif
};

static const struct bench_kernel cpu_tests[] = {
	{.group = "conc", .name = "write32", .fn = k_write32, .dst = BENCH_SDRAM_A,
	 .len = BENCH_BUF_SIZE, .bus_mult = 1U, .writes_sdram = true, .verify = verify_write32},
	{.group = "conc", .name = "read32", .fn = k_read32, .src = BENCH_SDRAM_A,
	 .len = BENCH_BUF_SIZE, .bus_mult = 1U},
	{.group = "conc", .name = "copy64_sdram_to_sdram", .fn = k_copy64, .dst = BENCH_SDRAM_B,
	 .src = BENCH_SDRAM_A, .len = BENCH_BUF_SIZE, .bus_mult = 2U, .writes_sdram = true,
	 .verify = verify_copy},
};

static uint32_t cpu_alone_x10[ARRAY_SIZE(cpu_tests)];

#define CPU_WRITE BIT(0)
#define CPU_READ  BIT(1)
#define CPU_COPY  BIT(2)
#define CPU_ALL   (CPU_WRITE | CPU_READ | CPU_COPY)

/* ref.md matrix: DMA read/write = background SDRAM-to-SDRAM DMA */
static const struct master *const ref_dma[] = {&masters[0]};
#if defined(CONFIG_SDRAM_BENCH_GLCDC)
static const struct master *const ref_dma_disp[] = {&masters[0],
						    &masters[ARRAY_SIZE(masters) - 1U]};
#endif

static uint32_t rate_x10(uint64_t bytes, int64_t ms)
{
	return (ms <= 0) ? 0U : (uint32_t)((bytes * 10U) / ((uint64_t)ms * 1000U));
}

static void print_x10(const char *prefix, uint32_t v)
{
	printk("%s%u.%u", prefix, v / 10U, v % 10U);
}

static int start_all(const struct master *const *m, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = m[i]->start(m[i]->mode);

		if (ret != 0) {
			printk("    %s start failed (%d)\n", m[i]->name, ret);
			for (size_t j = 0; j < i; j++) {
				m[j]->stop();
			}
			return ret;
		}
	}

	return 0;
}

static void stop_all(const struct master *const *m, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		m[i]->stop();
	}
}

static uint64_t bytes_all(const struct master *const *m, size_t n)
{
	uint64_t b = 0U;

	for (size_t i = 0; i < n; i++) {
		b += m[i]->bytes();
	}

	return b;
}

/* SDRAM bus bytes moved by the masters (bytes x bus multiplier) */
static uint64_t bus_bytes_all(const struct master *const *m, size_t n)
{
	uint64_t b = 0U;

	for (size_t i = 0; i < n; i++) {
		b += m[i]->bytes() * m[i]->bus_mult;
	}

	return b;
}

static uint32_t warnings;

static void health_reset_all(const struct master *const *m, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (m[i]->health_reset != NULL) {
			m[i]->health_reset();
		}
	}
}

static const char *health_all(const struct master *const *m, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if ((m[i]->healthy != NULL) && !m[i]->healthy()) {
			return "glcdc_underflow";
		}
	}

	return "ok";
}

static uint32_t run_scenario(const char *name, const struct master *const *m, size_t n,
			     uint32_t cpu_mask)
{
	uint32_t fails = 0U;
	uint32_t solo_x10, solo_bus_x10;
	int64_t t0;
	int ret;

	ret = start_all(m, n);
	if (ret != 0) {
		printk("CONC,%s,-,start_failed,%d\n", name, ret);
		return 1U;
	}

	/* Master throughput with the CPU idle */
	t0 = k_uptime_get();
	{
		uint64_t b0 = bytes_all(m, n);
		uint64_t bb0 = bus_bytes_all(m, n);

		health_reset_all(m, n);
		k_msleep(MASTER_SOLO_MS);
		solo_x10 = rate_x10(bytes_all(m, n) - b0, k_uptime_get() - t0);
		solo_bus_x10 = rate_x10(bus_bytes_all(m, n) - bb0, k_uptime_get() - t0);
		printk("CONC,%s,idle,-,-,-,-,-", name);
		print_x10(",", solo_x10);
		print_x10(",-,", solo_bus_x10);
		printk(",-,-,-,%s\n", health_all(m, n));
	}

	for (size_t i = 0; i < ARRAY_SIZE(cpu_tests); i++) {
		const struct bench_kernel *k = &cpu_tests[i];

		if ((cpu_mask & BIT(i)) == 0U) {
			continue;
		}
		struct bench_stats st;
		uint32_t cpu_min, cpu_avg, cpu_max, other_x10, other_bus_x10, total_x10;
		uint64_t b0, bb0;
		const char *health;
		int64_t s0;

		health_reset_all(m, n);
		s0 = k_uptime_get();
		b0 = bytes_all(m, n);
		bb0 = bus_bytes_all(m, n);
		bench_kernel_measure(k, false, &st);
		other_x10 = rate_x10(bytes_all(m, n) - b0, k_uptime_get() - s0);
		other_bus_x10 = rate_x10(bus_bytes_all(m, n) - bb0, k_uptime_get() - s0);

		cpu_min = bench_mbps_x10(st.bytes, st.max_ns);
		cpu_max = bench_mbps_x10(st.bytes, st.min_ns);
		cpu_avg = bench_mbps_x10((uint64_t)st.bytes * st.iters, st.total_ns);
		total_x10 = (cpu_avg * k->bus_mult) + other_bus_x10;
		health = health_all(m, n);
		if (strcmp(health, "ok") != 0) {
			warnings++;
		}

		printk("CONC,%s,%s", name, k->name);
		print_x10(",", cpu_alone_x10[i]);
		print_x10(",", cpu_avg);
		print_x10(",", cpu_min);
		print_x10(",", cpu_max);
		printk(",%d", (cpu_alone_x10[i] == 0U)
				      ? 0
				      : (int)(((int64_t)cpu_avg - cpu_alone_x10[i]) * 100 /
					      cpu_alone_x10[i]));
		print_x10(",", solo_x10);
		print_x10(",", other_x10);
		print_x10(",", solo_bus_x10);
		print_x10(",", other_bus_x10);
		print_x10(",", total_x10);
		printk(",%u%%,%s\n",
		       (bench_peak_mbps() == 0U) ? 0U : (total_x10 * 10U) / bench_peak_mbps(),
		       health);

		bench_cache_sync();
		if ((k->verify != NULL) && !k->verify(k, 0U)) {
			printk("    FAIL: CPU data check after %s with %s\n", k->name, name);
			fails++;
		}
	}

	stop_all(m, n);

	for (size_t i = 0; i < n; i++) {
		if ((m[i]->check != NULL) && !m[i]->check()) {
			printk("    FAIL: %s check after %s\n", m[i]->name, name);
			fails++;
		}
	}

	return fails;
}

uint32_t bench_concurrent_run(void)
{
	uint32_t fails = 0U;

	printk("\n--- Concurrent bus masters (D-cache %s, interrupts enabled) ---\n",
	       bench_cache_name());
	printk("CONC,scenario,cpu_test,cpu_alone_MBps,cpu_loaded_MBps,cpu_min,cpu_max,"
	       "cpu_change_pct,other_alone_MBps,other_loaded_MBps,other_alone_bus_MBps,"
	       "other_loaded_bus_MBps,sdram_bus_total_MBps,pct_of_peak,other_status\n");

	/* Source data for the background masters */
	{
		const struct bench_kernel c = {.dst = (void *)SB_WIN_C, .len = SB_WIN_SIZE};
		uint32_t *src = (uint32_t *)SB_CM33_SRC;

		k_write32(&c);
		/* Differs from the CM33 write pattern so the copy check is meaningful */
		if (IS_ENABLED(CONFIG_SDRAM_BENCH_CM33)) {
			for (uint32_t w = 0; w < SB_WIN_SIZE / sizeof(uint32_t); w++) {
				src[w] = ~PAT32(w);
			}
		}
		bench_cache_sync();
	}

	/* CPU baseline with interrupts enabled, same as the loaded runs */
	for (size_t i = 0; i < ARRAY_SIZE(cpu_tests); i++) {
		struct bench_stats st;

		bench_kernel_measure(&cpu_tests[i], false, &st);
		cpu_alone_x10[i] = bench_mbps_x10((uint64_t)st.bytes * st.iters, st.total_ns);
	}

	/*
	 * ref.md contention matrix, scenarios 1-3 (display idle). They run
	 * before the GLCDC is started, because scan-out cannot be stopped.
	 */
	fails += run_scenario("ref_s1_cpu_read", NULL, 0U, CPU_READ);
	fails += run_scenario("ref_s2_cpu_write", NULL, 0U, CPU_WRITE);
	fails += run_scenario("ref_s3_cpu_read_dma", ref_dma, ARRAY_SIZE(ref_dma), CPU_READ);

	for (size_t i = 0; i < ARRAY_SIZE(masters); i++) {
		const struct master *m = &masters[i];

		fails += run_scenario(m->name, &m, 1U, CPU_ALL);
	}

	if (ARRAY_SIZE(combo) > 1U) {
		fails += run_scenario("all_masters", combo, ARRAY_SIZE(combo), CPU_ALL);
	}

#if defined(CONFIG_SDRAM_BENCH_GLCDC)
	/* ref.md scenarios 4-5: display active */
	fails += run_scenario("ref_s4_cpu_read_dma_display", ref_dma_disp, ARRAY_SIZE(ref_dma_disp),
			      CPU_READ);
	fails += run_scenario("ref_s5_cpu_copy_dma_display", ref_dma_disp, ARRAY_SIZE(ref_dma_disp),
			      CPU_COPY);
#endif

	if (warnings != 0U) {
		printk("WARN: %u concurrent measurements saw a master health problem "
		       "(see other_status)\n", warnings);
	}
	bench_add_warnings(warnings);

	return fails;
}
