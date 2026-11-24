/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CM33 SDRAM traffic generator. Waits for commands from the CM85 in the
 * SDRAM mailbox and reads, writes or copies 4 MiB windows in 16 KiB blocks,
 * counting completed blocks.
 */

#include <zephyr/kernel.h>

#include "sdram_bench_shared.h"

#define PAT32(i) ((uint32_t)(i) ^ 0x5A5AA5A5U)

static uint32_t sink;

static void block_read(uint32_t off)
{
	volatile const uint64_t *p = (volatile const uint64_t *)(SB_CM33_SRC + off);
	uint64_t acc = 0U;

	for (uint32_t i = 0; i < SB_CM33_BLOCK / sizeof(uint64_t); i += 4U) {
		acc += p[i + 0U];
		acc += p[i + 1U];
		acc += p[i + 2U];
		acc += p[i + 3U];
	}
	sink += (uint32_t)acc;
}

static void block_write(uint32_t off)
{
	volatile uint32_t *p = (volatile uint32_t *)(SB_CM33_DST + off);
	const uint32_t base = off / sizeof(uint32_t);

	for (uint32_t i = 0; i < SB_CM33_BLOCK / sizeof(uint32_t); i += 4U) {
		p[i + 0U] = PAT32(base + i + 0U);
		p[i + 1U] = PAT32(base + i + 1U);
		p[i + 2U] = PAT32(base + i + 2U);
		p[i + 3U] = PAT32(base + i + 3U);
	}
}

static void block_copy(uint32_t off)
{
	volatile const uint64_t *s = (volatile const uint64_t *)(SB_CM33_SRC + off);
	volatile uint64_t *d = (volatile uint64_t *)(SB_CM33_DST + off);

	for (uint32_t i = 0; i < SB_CM33_BLOCK / sizeof(uint64_t); i += 4U) {
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

int main(void)
{
	struct sb_mbox *const mb = SB_MBOX;
	uint32_t seen = mb->ctl.seq;
	uint32_t cmd = SB_CMD_IDLE;
	uint32_t off = 0U;

	mb->st.blocks = 0U;
	mb->st.errors = 0U;
	mb->st.ack = seen;
	mb->st.magic = SB_CM33_MAGIC;

	for (;;) {
		const uint32_t seq = mb->ctl.seq;

		if (seq != seen) {
			cmd = mb->ctl.cmd;
			off = 0U;
			mb->st.blocks = 0U;
			mb->st.ack = seq;
			seen = seq;
		}

		switch (cmd) {
		case SB_CMD_READ:
			block_read(off);
			break;
		case SB_CMD_WRITE:
			block_write(off);
			break;
		case SB_CMD_COPY:
			block_copy(off);
			break;
		default:
			/* Poll the mailbox rarely so idle CM33 adds no SDRAM load */
			k_msleep(1);
			continue;
		}

		off = (off + SB_CM33_BLOCK) % SB_WIN_SIZE;
		mb->st.blocks = mb->st.blocks + 1U;
		mb->st.sink = sink;
	}

	return 0;
}
