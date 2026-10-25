/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_flow.h -- the per-flow GEM counters, accumulated, for
 * /proc/odi_gpon/flows.
 *
 * Every per-flow register here clears when it is read, so there is one
 * reader, odi_gpon_flows_sample(), run by odi_gpon.c under odi_gpon_lock
 * in the same cycle as odi_gpon_ctr_sample(), and it adds each read into a
 * 64-bit total. Only the flows the driver configured are read, never the
 * whole table: odi_switch_cmd.c tells odi_gpon.c when OMCI creates a flow
 * (odi_gpon_flows_set()), when it deletes one (odi_gpon_flows_del()) and
 * when the flow state is emptied (odi_gpon_flows_clear()). A flow created
 * again starts from zero: its totals are cleared, and so are its
 * registers, with a read nobody keeps, so counts from before the flow
 * existed never reach the total.
 *
 * The flow index is the driver's, not the GEM port id: a downstream flow
 * is a row of the GEM port CAM, an upstream flow a slot of the upstream
 * flow table. Each flow records the GEM port it was created with.
 *
 * Every register access is a bounded poll: the ACK bit of an indirect
 * read is waited for a fixed number of reads, and a count whose ACK never
 * came is skipped and tallied in ack_timeouts.
 *
 * Pure: register access through odi_reg_read()/odi_reg_write(), so
 * test/odi_gpon_flow_test.c runs it on the host against a clear-on-read
 * register model.
 */
#ifndef ODI_GPON_FLOW_H
#define ODI_GPON_FLOW_H

#include "odi_gpon_ctr.h"

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* The flows odi_switch_cmd.c can create (ODI_SW_CMD_GEM_DS_MAX and
 * ODI_SW_CMD_GEM_US_MAX); an index past these is refused.
 */
#define ODI_GPON_FLOWS_DS	64U
#define ODI_GPON_FLOWS_US	64U

/* Reads of the index register spent waiting for ACK before the count is
 * given up on. The ACK was already set on the first read on the stick.
 */
#define ODI_GPON_FLOW_ACK_TRIES	64U

struct odi_gpon_flow_ds {
	uint8_t active;
	uint16_t gem_port;
	uint64_t gem_frames;
	uint64_t bytes;
	uint64_t eth_rx;	/* Ethernet frames received */
	uint64_t eth_fwd;	/* Ethernet frames forwarded to the switch */
};

struct odi_gpon_flow_us {
	uint8_t active;
	uint16_t gem_port;
	uint64_t gem_frames;
	uint64_t eth_frames;
	uint64_t bytes;
};

struct odi_gpon_flows {
	struct odi_gpon_flow_ds ds[ODI_GPON_FLOWS_DS];
	struct odi_gpon_flow_us us[ODI_GPON_FLOWS_US];
	uint32_t ack_timeouts;	/* counts skipped: no ACK within the bound */
	uint32_t saturated;	/* counts read as all ones: those lost counts */
};

void odi_gpon_flows_reset(struct odi_gpon_flows *f);

/* Direction of a flow. */
enum odi_gpon_flow_dir {
	ODI_GPON_FLOW_DIR_DS,
	ODI_GPON_FLOW_DIR_US,
};

/* Flow idx of direction dir exists now, on GEM port gem_port: zero its
 * totals and its registers and start sampling it. A flow already there is
 * started over. Returns 0, or -1 for an index the table does not hold.
 * Called with odi_gpon_lock held.
 */
int odi_gpon_flows_set(struct odi_gpon_flows *f, enum odi_gpon_flow_dir dir, uint32_t idx,
		       uint32_t gem_port);

/* Flow idx of direction dir is gone (cmd 25 deleted it): no longer
 * sampled or shown, its totals dropped. Returns 0, or -1 for an index the
 * table does not hold. Lock held.
 */
int odi_gpon_flows_del(struct odi_gpon_flows *f, enum odi_gpon_flow_dir dir, uint32_t idx);

/* Every flow is gone: forget them all, totals included. Lock held. */
void odi_gpon_flows_clear(struct odi_gpon_flows *f);

/* Read every active flow once and add the counts in. Lock held. */
void odi_gpon_flows_sample(struct odi_gpon_flows *f);

/* The lines of /proc/odi_gpon/flows, each passed to put() whole, newline
 * included. docs/TOOLS.md, "/proc/odi_gpon/", has the format.
 */
void odi_gpon_flows_show(const struct odi_gpon_flows *f, odi_gpon_ctr_put_fn put, void *ctx);

#endif /* ODI_GPON_FLOW_H */
