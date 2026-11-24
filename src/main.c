/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDRAM integrity and throughput benchmark for the EK-RA8P1 board.
 *
 * Results are printed on the console as a table and as RESULT,... CSV lines.
 * MB/s means 10^6 bytes per second.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/timing/timing.h>
#include <cmsis_core.h>
#include <bsp_api.h>
#include <string.h>

#include "sdram_bench.h"

#define MAX_RESULTS 320U

struct bench_result {
	const char *group;
	const char *test;
	const char *cache;
	struct bench_stats st;
};

static struct bench_result results[MAX_RESULTS];
static uint32_t result_count;
static uint32_t peak_mbps;
static uint32_t warning_count;

void bench_add_warnings(uint32_t n)
{
	warning_count += n;
}

uint32_t bench_mbps_x10(uint64_t bytes, uint64_t ns)
{
	if (ns == 0U) {
		return 0U;
	}

	/* bytes / us = MB/s, scaled by 10 for one decimal */
	return (uint32_t)((bytes * 10000U) / ns);
}

void bench_stats_init(struct bench_stats *st, size_t bytes, uint32_t bus_mult, uint32_t ops)
{
	memset(st, 0, sizeof(*st));
	st->min_ns = UINT64_MAX;
	st->bytes = bytes;
	st->bus_mult = bus_mult;
	st->ops = ops;
}

void bench_stats_add(struct bench_stats *st, uint64_t ns)
{
	st->min_ns = MIN(st->min_ns, ns);
	st->max_ns = MAX(st->max_ns, ns);
	st->total_ns += ns;
	st->iters++;

	/* Welford update of the per-iteration throughput (MB/s) */
	if (ns != 0U) {
		const double mbps = ((double)st->bytes * 1000.0) / (double)ns;
		const double delta = mbps - st->mean_mbps;

		st->mean_mbps += delta / (double)st->iters;
		st->m2_mbps += delta * (mbps - st->mean_mbps);
	}
}

void bench_stats_add_pmu(struct bench_stats *st, const uint32_t pmu[PMU_EVENTS])
{
	if (!bench_pmu_ok()) {
		return;
	}
	for (size_t e = 0; e < PMU_EVENTS; e++) {
		st->pmu[e] += pmu[e];
	}
	st->pmu_valid = true;
}

/* Standard deviation of the per-iteration MB/s, x10 */
static uint32_t sd_x10(const struct bench_stats *st)
{
	double var;
	double sd = 0.0;

	if (st->iters < 2U) {
		return 0U;
	}

	var = st->m2_mbps / (double)(st->iters - 1U);
	/* Newton iteration; avoids pulling in libm for one square root */
	if (var > 0.0) {
		sd = var;
		for (int i = 0; i < 30; i++) {
			sd = 0.5 * (sd + (var / sd));
		}
	}

	return (uint32_t)((sd * 10.0) + 0.5);
}

/* Coefficient of variation in 0.01 % units */
static uint32_t cv_x100(const struct bench_stats *st)
{
	if (st->mean_mbps <= 0.0) {
		return 0U;
	}

	return (uint32_t)((((double)sd_x10(st) / 10.0) * 10000.0) / st->mean_mbps);
}

/* Average measured time per iteration in 0.01 ms */
static uint32_t time_x100ms(const struct bench_stats *st)
{
	return (st->iters == 0U) ? 0U : (uint32_t)(st->total_ns / st->iters / 10000U);
}

/* Average MiB/s x10 */
static uint32_t mibps_x10(const struct bench_stats *st)
{
	if (st->total_ns == 0U) {
		return 0U;
	}

	return (uint32_t)(((uint64_t)st->bytes * st->iters * 10U * 1000000000ULL) /
			  (st->total_ns * 1048576ULL));
}

/* Fastest iteration gives max MB/s, slowest gives min MB/s */
static void stats_mbps(const struct bench_stats *st, uint32_t *min, uint32_t *avg, uint32_t *max)
{
	*min = bench_mbps_x10(st->bytes, st->max_ns);
	*max = bench_mbps_x10(st->bytes, st->min_ns);
	*avg = (st->iters == 0U) ? 0U
				 : bench_mbps_x10((uint64_t)st->bytes * st->iters, st->total_ns);
}

static uint32_t pct_of_peak(const struct bench_stats *st, uint32_t avg_x10)
{
	/* (avg_x10 / 10) * bus_mult * 100 / peak */
	return (peak_mbps == 0U) ? 0U
				 : (uint32_t)(((uint64_t)avg_x10 * st->bus_mult * 10U) / peak_mbps);
}

/* Average time per access in 0.1 ns, 0 if the test has no access count */
static uint32_t ns_per_op_x10(const struct bench_stats *st)
{
	if ((st->ops == 0U) || (st->iters == 0U)) {
		return 0U;
	}

	return (uint32_t)((st->total_ns * 10U) / ((uint64_t)st->ops * st->iters));
}

void bench_section(const char *title)
{
	printk("\n--- %s ---\n", title);
	printk("%-7s %-26s %-5s %9s %9s %9s %6s %9s %6s %9s\n", "group", "test", "cache",
	       "min MB/s", "avg MB/s", "max MB/s", "%peak", "ns/op", "cv%", "ms/iter");
}

void bench_report(const char *group, const char *test, const char *cache,
		  const struct bench_stats *st)
{
	uint32_t min, avg, max;
	uint32_t nsop = ns_per_op_x10(st);
	uint32_t cv = cv_x100(st);
	uint32_t ms = time_x100ms(st);
	char pct[8] = "-";
	char ns[12] = "-";
	char cvs[16];
	char mss[16];

	stats_mbps(st, &min, &avg, &max);
	snprintk(cvs, sizeof(cvs), "%u.%02u%s", cv / 100U, cv % 100U, (cv > 500U) ? "!" : "");
	snprintk(mss, sizeof(mss), "%u.%02u", ms / 100U, ms % 100U);

	if (st->bus_mult != 0U) {
		snprintk(pct, sizeof(pct), "%u%%", pct_of_peak(st, avg));
	}
	if (nsop != 0U) {
		snprintk(ns, sizeof(ns), "%u.%u", nsop / 10U, nsop % 10U);
	}

	printk("%-7s %-26s %-5s %7u.%u %7u.%u %7u.%u %6s %9s %6s %9s\n", group, test, cache,
	       min / 10U, min % 10U, avg / 10U, avg % 10U, max / 10U, max % 10U, pct, ns, cvs,
	       mss);

	if (result_count < MAX_RESULTS) {
		results[result_count++] = (struct bench_result){
			.group = group, .test = test, .cache = cache, .st = *st};
	}
}

static void print_csv(void)
{
	printk("\nRESULT,group,test,cache,bytes,iters,min_MBps,avg_MBps,max_MBps,pct_of_peak,"
	       "ns_per_op,time_ms,MiBps,sd_MBps,cv_pct,pmu_refill,pmu_bus,pmu_mem,"
	       "pmu_stall_be,flag\n");
	for (uint32_t i = 0; i < result_count; i++) {
		const struct bench_result *r = &results[i];
		uint32_t min, avg, max;
		uint32_t nsop = ns_per_op_x10(&r->st);

		stats_mbps(&r->st, &min, &avg, &max);
		printk("RESULT,%s,%s,%s,%u,%u,%u.%u,%u.%u,%u.%u,", r->group, r->test, r->cache,
		       (uint32_t)r->st.bytes, r->st.iters, min / 10U, min % 10U, avg / 10U,
		       avg % 10U, max / 10U, max % 10U);
		if (r->st.bus_mult != 0U) {
			printk("%u,", pct_of_peak(&r->st, avg));
		} else {
			printk("-,");
		}
		if (nsop != 0U) {
			printk("%u.%u,", nsop / 10U, nsop % 10U);
		} else {
			printk("-,");
		}
		{
			const uint32_t ms = time_x100ms(&r->st);
			const uint32_t mib = mibps_x10(&r->st);
			const uint32_t sd = sd_x10(&r->st);
			const uint32_t cv = cv_x100(&r->st);

			printk("%u.%02u,%u.%u,%u.%u,%u.%02u,", ms / 100U, ms % 100U, mib / 10U,
			       mib % 10U, sd / 10U, sd % 10U, cv / 100U, cv % 100U);
			if (r->st.pmu_valid && (r->st.iters != 0U)) {
				for (size_t e = 0; e < PMU_EVENTS; e++) {
					printk("%u,", (uint32_t)(r->st.pmu[e] / r->st.iters));
				}
			} else {
				printk("-,-,-,-,");
			}
			printk("%s\n", (cv > 500U) ? "VAR" : "-");
		}
	}
}

const char *bench_cache_name(void)
{
	return bench_dcache_enabled() ? "on" : "off";
}

uint32_t bench_peak_mbps(void)
{
	return peak_mbps;
}

bool bench_dcache_enabled(void)
{
	return (SCB->CCR & SCB_CCR_DC_Msk) != 0U;
}

void bench_dcache_set(bool on)
{
	if (!IS_ENABLED(CONFIG_DCACHE) || (on == bench_dcache_enabled())) {
		return;
	}

	if (on) {
		sys_cache_data_enable();
	} else {
		sys_cache_data_disable();
	}
}

void bench_cache_sync(void)
{
	if (bench_dcache_enabled()) {
		(void)sys_cache_data_flush_and_invd_all();
	}
}

static void print_dcache_geometry(void)
{
	uint32_t ccsidr;
	uint32_t line, ways, sets;

	SCB->CSSELR = 0U; /* level 1 data cache */
	__DSB();
	ccsidr = SCB->CCSIDR;
	line = 1U << (((ccsidr & SCB_CCSIDR_LINESIZE_Msk) >> SCB_CCSIDR_LINESIZE_Pos) + 4U);
	ways = CCSIDR_WAYS(ccsidr) + 1U;
	sets = CCSIDR_SETS(ccsidr) + 1U;

	printk("D-cache        : %u KiB (%u sets x %u ways x %u B), %s, %s\n",
	       (line * ways * sets) / 1024U, sets, ways, line,
	       bench_dcache_enabled() ? "enabled" : "disabled",
	       IS_ENABLED(CONFIG_CPU_RA_DCACHE_WRITEBACK) ? "write-back" : "write-through");
}

/* Controller configuration as programmed by the memc driver */
static void print_sdramc(void)
{
	static const char *const bl_names[8] = {"1", "2", "4", "8", "rsvd", "rsvd", "rsvd",
						"page"};
	const uint32_t sdtr = R_BUS->SDRAM.SDTR;
	const uint32_t mr = R_BUS->SDRAM.SDMOD;

	printk("SDRAMC         : SDCCR=0x%02x SDCMOD=0x%02x SDAMOD=0x%02x SDADR=0x%02x "
	       "SDTR=0x%08x SDMOD=0x%04x SDRFCR=0x%04x SDRFEN=0x%02x SDSR=0x%02x\n",
	       R_BUS->SDRAM.SDCCR, R_BUS->SDRAM.SDCMOD, R_BUS->SDRAM.SDAMOD, R_BUS->SDRAM.SDADR,
	       sdtr, mr, R_BUS->SDRAM.SDRFCR, R_BUS->SDRAM.SDRFEN, R_BUS->SDRAM.SDSR);
	printk("SDRAM timing   : CL=%u tRCD=%u tRP=%u tWR=%u tRAS=%u (SDCLK), mode register "
	       "burst length %s, CAS latency %u, refresh count %u\n",
	       (uint32_t)((sdtr & R_BUS_SDRAM_SDTR_CL_Msk) >> R_BUS_SDRAM_SDTR_CL_Pos),
	       (uint32_t)(((sdtr & R_BUS_SDRAM_SDTR_RCD_Msk) >> R_BUS_SDRAM_SDTR_RCD_Pos) + 1U),
	       (uint32_t)(((sdtr & R_BUS_SDRAM_SDTR_RP_Msk) >> R_BUS_SDRAM_SDTR_RP_Pos) + 1U),
	       (uint32_t)(((sdtr & R_BUS_SDRAM_SDTR_WR_Msk) >> R_BUS_SDRAM_SDTR_WR_Pos) + 1U),
	       (uint32_t)(((sdtr & R_BUS_SDRAM_SDTR_RAS_Msk) >> R_BUS_SDRAM_SDTR_RAS_Pos) + 1U),
	       bl_names[mr & 0x7U], (mr >> 4) & 0x7U,
	       (uint32_t)(((R_BUS->SDRAM.SDRFCR & R_BUS_SDRAM_SDRFCR_RFC_Msk) >>
			   R_BUS_SDRAM_SDRFCR_RFC_Pos) +
			  1U));
}

static void print_banner(void)
{
	uint32_t bclk = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_BCLK);

	/* SDCLK runs at BCLK; the 32-bit bus moves 4 bytes per SDCLK */
	peak_mbps = (uint32_t)(((uint64_t)bclk * SDRAM_BUS_BYTES) / 1000000U);

	printk("\n*** sdram_benchmark: %s ***\n", CONFIG_BOARD_TARGET);
	printk("Firmware       : %s, built %s %s\n", SB_GIT_REV, __DATE__, __TIME__);
	printk("CPU clock      : %u Hz (timing counter %u Hz)\n", SystemCoreClock,
	       (uint32_t)timing_freq_get());
	printk("BCLK / SDCLK   : %u Hz (SDCKOCR=0x%02x BCKCR=0x%02x)\n", bclk,
	       R_SYSTEM->SDCKOCR, R_SYSTEM->BCKCR);
	printk("SDRAM          : 0x%08x, %u MiB, %u-bit bus\n", (uint32_t)SDRAM_BASE,
	       (uint32_t)(SDRAM_SIZE / (1024U * 1024U)), SDRAM_BUS_BYTES * 8U);
	printk("SDRAM type     : SDR, 1 transfer/clock, %u MT/s\n", bclk / 1000000U);
	printk("Theoretical    : %u MB/s = %u bit / 8 x %u MHz x 1 transfer/clock\n", peak_mbps,
	       SDRAM_BUS_BYTES * 8U, bclk / 1000000U);
	print_sdramc();
	print_dcache_geometry();
	printk("Windows        : A=0x%08x B=0x%08x C=0x%08x D=0x%08x, %u KiB per pass, "
	       "%u iterations\n",
	       SB_WIN_A, SB_WIN_B, SB_WIN_C, SB_WIN_D, (uint32_t)(BENCH_BUF_SIZE / 1024U),
	       BENCH_ITER);
	printk("Other regions  : FB0=0x%08x FB1=0x%08x CM33=0x%08x/0x%08x mailbox=0x%08x\n",
	       SB_FB0, SB_FB1, SB_CM33_SRC, SB_CM33_DST, SB_MBOX_ADDR);
	printk("SRAM buffer    : 0x%08x, %u KiB\n", (uint32_t)(uintptr_t)bench_sram_buf,
	       SRAM_BUF_SIZE / 1024U);
	printk("Suites         : integrity=%s cpu=%s access=%s dma=%s concurrent=%s cm33=%s "
	       "glcdc=%s sustained=%u s\n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_INTEGRITY) ? "y" : "n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_CPU) ? "y" : "n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_ACCESS) ? "y" : "n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_DMA) ? "y" : "n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_CONCURRENT) ? "y" : "n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_CM33) ? "y" : "n",
	       IS_ENABLED(CONFIG_SDRAM_BENCH_GLCDC) ? "y" : "n",
	       (uint32_t)CONFIG_SDRAM_BENCH_SUSTAINED_SEC);
	printk("Integrity      : %u pass(es), retention %u s\n",
	       (uint32_t)CONFIG_SDRAM_BENCH_INTEGRITY_PASSES,
	       (uint32_t)CONFIG_SDRAM_BENCH_RETENTION_SEC);
	printk("Units          : MB/s = 10^6 bytes/s (MiB/s in CSV); %%peak = avg x SDRAM bus "
	       "bytes per byte / theoretical; ns/op = average time per access; cv%% = "
	       "stddev/mean of per-iteration MB/s, '!' above 5%%\n");
}

int main(void)
{
	uint32_t failures = 0U;

	timing_init();
	timing_start();

	bench_pmu_init();
	print_banner();

	if (IS_ENABLED(CONFIG_SDRAM_BENCH_INTEGRITY)) {
		failures += sdram_integrity_run();
		if (failures != 0U) {
			printk("\nIntegrity failed, throughput not measured\n");
			goto out;
		}
	}

	if (CONFIG_SDRAM_BENCH_RETENTION_SEC > 0) {
		failures += sdram_retention_run();
	}

	if (IS_ENABLED(CONFIG_SDRAM_BENCH_CPU)) {
		failures += bench_cpu_run();
	}

	/* Random-write and chase tests cover all of SDRAM, so they run before
	 * any other bus master uses its region.
	 */
	if (IS_ENABLED(CONFIG_SDRAM_BENCH_ACCESS)) {
		failures += bench_access_run();
		failures += bench_stream_run();
	}

	if (IS_ENABLED(CONFIG_SDRAM_BENCH_DMA)) {
		failures += bench_dma_run();
	}

	if (IS_ENABLED(CONFIG_SDRAM_BENCH_CONCURRENT)) {
		failures += bench_concurrent_run();
	}

	if (CONFIG_SDRAM_BENCH_SUSTAINED_SEC > 0) {
		failures += bench_sustained_run();
	}

	print_csv();

out:
	timing_stop();
	printk("\nsdram_benchmark %s (%u failures, %u warnings)\n",
	       (failures == 0U) ? "PASSED" : "FAILED", failures, warning_count);

	return 0;
}
