/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDRAM layout and CM85 <-> CM33 mailbox shared by the sdram_benchmark
 * (CM85) and cm33_traffic (CM33) images.
 */

#ifndef SDRAM_BENCH_SHARED_H_
#define SDRAM_BENCH_SHARED_H_

#include <stdint.h>

#define SB_SDRAM_BASE 0x68000000U
#define SB_SDRAM_SIZE 0x04000000U
#define SB_MIB        0x00100000U

/* 4 MiB windows; see README "SDRAM layout" */
#define SB_WIN_SIZE   (4U * SB_MIB)
#define SB_WIN_A      (SB_SDRAM_BASE + (0U * SB_MIB))  /* CPU primary */
#define SB_WIN_B      (SB_SDRAM_BASE + (4U * SB_MIB))  /* CPU copy target */
#define SB_WIN_C      (SB_SDRAM_BASE + (8U * SB_MIB))  /* background DMA source */
#define SB_WIN_D      (SB_SDRAM_BASE + (12U * SB_MIB)) /* background DMA target */
#define SB_FB0        (SB_SDRAM_BASE + (16U * SB_MIB)) /* GLCDC frame buffers */
#define SB_FB1        (SB_SDRAM_BASE + (18U * SB_MIB))
#define SB_CM33_SRC   (SB_SDRAM_BASE + (32U * SB_MIB)) /* CM33 traffic source */
#define SB_CM33_DST   (SB_SDRAM_BASE + (36U * SB_MIB)) /* CM33 copy target */
#define SB_MBOX_ADDR  (SB_SDRAM_BASE + SB_SDRAM_SIZE - 4096U)

/* CM33 moves data in blocks of this size and counts completed blocks */
#define SB_CM33_BLOCK 16384U
#define SB_CM33_MAGIC 0x33A5C0DEU

enum sb_cm33_cmd {
	SB_CMD_IDLE = 0,
	SB_CMD_READ = 1,
	SB_CMD_WRITE = 2,
	SB_CMD_COPY = 3,
};

/*
 * Each side writes only its own 32-byte line, so the CM85 can clean its
 * line and invalidate the CM33 line without losing data in either cache
 * write policy. The CM33 has no data cache.
 */
struct sb_mbox_ctl {
	volatile uint32_t cmd;
	volatile uint32_t seq;
	uint32_t reserved[6];
};

struct sb_mbox_status {
	volatile uint32_t magic;
	volatile uint32_t ack;    /* last ctl.seq the CM33 has switched to */
	volatile uint32_t blocks; /* blocks completed since the last command change */
	volatile uint32_t errors; /* CM33 copy verification failures */
	volatile uint32_t sink;
	uint32_t reserved[3];
};

struct sb_mbox {
	struct sb_mbox_ctl ctl;
	struct sb_mbox_status st;
};

#define SB_MBOX ((struct sb_mbox *)SB_MBOX_ADDR)

#endif /* SDRAM_BENCH_SHARED_H_ */
