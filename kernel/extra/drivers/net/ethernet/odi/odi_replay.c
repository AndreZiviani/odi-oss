// SPDX-License-Identifier: GPL-2.0
/*
 * odi_replay.c -- odi_replay_run(), the one loop every register replay
 * goes through (odi_replay.h has the four sequences and what differs
 * between them).
 *
 * The register accessors are those of odi_switch_reg.h in the kernel and
 * of test/odi_switch_mock.h in the host unity build.
 */
#include "odi_replay.h"
#include "odi_replay_blob.h"
#include "odi_switch_reg.h"
#include "odi_switch_tbl.h"
#include "odi_soc.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/printk.h>
#define ODI_REPLAY_LOG_ERR(fmt, ...) pr_err("odi_replay: " fmt, ##__VA_ARGS__)
#else
#include <errno.h>
#include <stdio.h>
#define ODI_REPLAY_LOG_ERR(fmt, ...) fprintf(stderr, "odi_replay: " fmt, ##__VA_ARGS__)
#endif

int odi_replay_soc_write(uint32_t addr, uint32_t val)
{
	if (addr < ODI_SOC_KSEG1 || addr - ODI_SOC_KSEG1 >= ODI_SOC_SIZE) {
		ODI_REPLAY_LOG_ERR("refusing SoC write to 0x%08x, outside the window\n",
				   (unsigned int)addr);
		return -EPERM;
	}
	return odi_soc_write(addr - ODI_SOC_KSEG1, val);
}

static void odi_replay_apply(const struct odi_replay_event *e, const struct odi_replay_opts *opts)
{
	uint32_t val;

	switch (e->kind) {
	case ODI_REPLAY_REG:
		val = opts->value ? opts->value(e, opts->ctx) : e->value;
		if (opts->rmw) {
			uint32_t reg = odi_reg_read(e->offset);

			val = (reg & 0U) | val;
		}
		odi_reg_write(e->offset, val);
		break;
	case ODI_REPLAY_TABLE:
		(void)odi_switch_table_write(e->table, e->offset, e->words, e->n_words);
		break;
	case ODI_REPLAY_SOC:
		(void)odi_replay_soc_write(e->offset, e->value);
		break;
	}
}

/* The records of one sdkinit verb are contiguous (odi_replay_blob_parse()
 * refuses a blob whose verbs are out of order), so a verb replay stops at
 * the first record past it.
 */
unsigned int odi_replay_run(const struct odi_replay_blob *blob,
			    const struct odi_replay_opts *opts)
{
	struct odi_replay_event e;
	unsigned int applied = 0;
	uint32_t i;

	for (i = 0; i < blob->count; i++) {
		odi_replay_blob_event(blob, i, &e);
		if (opts->verb != ODI_REPLAY_ALL_VERBS) {
			if (e.verb < opts->verb)
				continue;
			if (e.verb > opts->verb)
				break;
		}
		odi_replay_apply(&e, opts);
		applied++;
	}
	return applied;
}
