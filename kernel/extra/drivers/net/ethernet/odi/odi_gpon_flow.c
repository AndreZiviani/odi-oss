// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_flow.c -- the per-flow GEM counters, read from the GPON MAC and
 * accumulated into 64-bit totals (odi_gpon_flow.h has the contract).
 *
 * Downstream, per flow: GEM frames and bytes behind one indirect pair
 * (RSEL picks which), Ethernet frames received and Ethernet frames
 * forwarded behind one pair each. Upstream: GEM frames and Ethernet frames
 * behind one pair (bit 7 of the index picks which), bytes in a 64-bit
 * array indexed by flow. All of them clear when read.
 */
#include "odi_gpon_flow.h"
#include "odi_gpon_hw.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#endif

/* One indirect count: write ind_val to the index register, wait (bounded)
 * for ACK, read the value register. 0 and *out, or -1 when ACK never came
 * and nothing was read: the count then stays in the register for the next
 * sample.
 */
static int flow_read(struct odi_gpon_flows *f, uint32_t ind_off, uint32_t ind_val,
		     uint32_t ack, uint32_t val_off, uint32_t *out)
{
	unsigned int i;

	odi_reg_write(ind_off, ind_val);
	for (i = 0; i < ODI_GPON_FLOW_ACK_TRIES; i++) {
		if (odi_reg_read(ind_off) & ack) {
			*out = odi_reg_read(val_off);
			if (*out == 0xffffffffU)
				f->saturated++;
			return 0;
		}
	}
	f->ack_timeouts++;
	return -1;
}

static void flow_add(struct odi_gpon_flows *f, uint64_t *total, uint32_t ind_off,
		     uint32_t ind_val, uint32_t ack, uint32_t val_off)
{
	uint32_t v;

	if (!flow_read(f, ind_off, ind_val, ack, val_off, &v))
		*total += v;
}

static void flow_ds_read(struct odi_gpon_flows *f, uint32_t idx, struct odi_gpon_flow_ds *d)
{
	flow_add(f, &d->gem_frames, ODI_GPON_DSF_FLOW_CNTR_IND_OFF, idx,
		 ODI_GPON_DSF_FLOW_CNTR_IND_ACK, ODI_GPON_DSF_FLOW_CNTR_VAL_OFF);
	flow_add(f, &d->bytes, ODI_GPON_DSF_FLOW_CNTR_IND_OFF,
		 idx | ODI_GPON_DSF_FLOW_CNTR_IND_RSEL,
		 ODI_GPON_DSF_FLOW_CNTR_IND_ACK, ODI_GPON_DSF_FLOW_CNTR_VAL_OFF);
	flow_add(f, &d->eth_rx, ODI_GPON_DS_GEM_RX_IND_OFF, idx,
		 ODI_GPON_DS_GEM_RX_IND_ACK, ODI_GPON_DS_GEM_RX_VAL_OFF);
	flow_add(f, &d->eth_fwd, ODI_GPON_DS_GEM_FWD_IND_OFF, idx,
		 ODI_GPON_DS_GEM_FWD_IND_ACK, ODI_GPON_DS_GEM_FWD_VAL_OFF);
}

static void flow_us_read(struct odi_gpon_flows *f, uint32_t idx, struct odi_gpon_flow_us *u)
{
	uint32_t lo, hi;

	flow_add(f, &u->gem_frames, ODI_GPON_US_FLOW_CNTR_IND_OFF, idx,
		 ODI_GPON_US_FLOW_CNTR_IND_ACK, ODI_GPON_US_FLOW_CNTR_VAL_OFF);
	flow_add(f, &u->eth_frames, ODI_GPON_US_FLOW_CNTR_IND_OFF,
		 idx | ODI_GPON_US_FLOW_CNTR_IND_IDX_ETH,
		 ODI_GPON_US_FLOW_CNTR_IND_ACK, ODI_GPON_US_FLOW_CNTR_VAL_OFF);
	/* Low word first. A second is far too short to carry into the high
	 * word at 2.5 Gbit/s (about 312 MB), so the high word is normally 0.
	 */
	lo = odi_reg_read(ODI_GPON_US_FLOW_BYTES(idx));
	hi = odi_reg_read(ODI_GPON_US_FLOW_BYTES(idx) + 4U);
	if (lo == 0xffffffffU && hi == 0xffffffffU)
		f->saturated++;
	u->bytes += ((uint64_t)hi << 32) | lo;
}

void odi_gpon_flows_reset(struct odi_gpon_flows *f)
{
	memset(f, 0, sizeof(*f));
}

int odi_gpon_flows_set(struct odi_gpon_flows *f, enum odi_gpon_flow_dir dir, uint32_t idx,
		       uint32_t gem_port)
{
	/* The first read only clears the registers: what it returns built up
	 * before this flow existed, and is dropped, saturation included.
	 */
	uint32_t saturated = f->saturated;

	if (dir == ODI_GPON_FLOW_DIR_DS) {
		struct odi_gpon_flow_ds *d;

		if (idx >= ODI_GPON_FLOWS_DS)
			return -1;
		d = &f->ds[idx];
		flow_ds_read(f, idx, d);
		memset(d, 0, sizeof(*d));
		d->active = 1;
		d->gem_port = (uint16_t)gem_port;
	} else if (dir == ODI_GPON_FLOW_DIR_US) {
		struct odi_gpon_flow_us *u;

		if (idx >= ODI_GPON_FLOWS_US)
			return -1;
		u = &f->us[idx];
		flow_us_read(f, idx, u);
		memset(u, 0, sizeof(*u));
		u->active = 1;
		u->gem_port = (uint16_t)gem_port;
	} else {
		return -1;
	}
	f->saturated = saturated;
	return 0;
}

void odi_gpon_flows_clear(struct odi_gpon_flows *f)
{
	memset(f->ds, 0, sizeof(f->ds));
	memset(f->us, 0, sizeof(f->us));
}

void odi_gpon_flows_sample(struct odi_gpon_flows *f)
{
	unsigned int i;

	for (i = 0; i < ODI_GPON_FLOWS_DS; i++)
		if (f->ds[i].active)
			flow_ds_read(f, i, &f->ds[i]);
	for (i = 0; i < ODI_GPON_FLOWS_US; i++)
		if (f->us[i].active)
			flow_us_read(f, i, &f->us[i]);
}

/* /proc/odi_gpon/flows: the two tallies, then one line per active flow. */
void odi_gpon_flows_show(const struct odi_gpon_flows *f, odi_gpon_ctr_put_fn put, void *ctx)
{
	char line[160];
	unsigned int i;

	snprintf(line, sizeof(line), "flow_ack_timeouts %u\n", f->ack_timeouts);
	put(ctx, line);
	snprintf(line, sizeof(line), "flow_saturated %u\n", f->saturated);
	put(ctx, line);
	for (i = 0; i < ODI_GPON_FLOWS_DS; i++) {
		const struct odi_gpon_flow_ds *d = &f->ds[i];

		if (!d->active)
			continue;
		snprintf(line, sizeof(line),
			 "ds %u gem %u gem_frames %llu bytes %llu eth_rx %llu eth_fwd %llu\n",
			 i, d->gem_port, (unsigned long long)d->gem_frames,
			 (unsigned long long)d->bytes, (unsigned long long)d->eth_rx,
			 (unsigned long long)d->eth_fwd);
		put(ctx, line);
	}
	for (i = 0; i < ODI_GPON_FLOWS_US; i++) {
		const struct odi_gpon_flow_us *u = &f->us[i];

		if (!u->active)
			continue;
		snprintf(line, sizeof(line),
			 "us %u gem %u gem_frames %llu eth_frames %llu bytes %llu\n",
			 i, u->gem_port, (unsigned long long)u->gem_frames,
			 (unsigned long long)u->eth_frames, (unsigned long long)u->bytes);
		put(ctx, line);
	}
}
