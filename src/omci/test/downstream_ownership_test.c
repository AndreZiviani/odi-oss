/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Who owns a downstream GEM flow, and when it is taken back: the real
 * responder code with the driver call wrapped (--wrap=omci_drv_call), so a
 * withdrawal (cmd 25, ena 0) or a clear of the broadcast flow (cmd 26) can
 * be counted and made to fail once. A CTP is deleted the way the OLT does
 * it, a Delete frame through handle(). No device or netlink is opened. */
#include "omcid.h"

#define CTP 268

static int fail_delete, fail_bc, dels, clears, bad;

int __wrap_omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)len;
	if (cmd == 26 && nl_get32(buf) == OMCI_BC_FLOW_NONE) {
		clears++;
		if (fail_bc) {
			fail_bc = 0;
			return -1;
		}
	}
	if (cmd == 25 && !((struct omci_gemflow *)buf)->ena) {
		/* A flow still selected for broadcast must never be freed. */
		if (bc_flow == (int)((struct omci_gemflow *)buf)->flow_id)
			bad = 1;
		dels++;
		if (fail_delete) {
			fail_delete = 0;
			return -1;
		}
	}
	return 0;
}

static void put(struct mib_row *r, unsigned k, uint32_t v)
{
	const struct omci_class *c = find_class(r->classId);
	unsigned n = attr_width(c, k), o = attr_offset(c, k);

	for (unsigned i = 0; i < n; i++)
		r->data[o + n - 1 - i] = (uint8_t)(v >> (8 * i));
}

/* A CTP on `port`, direction 1 upstream, 2 downstream, 3 both. */
static struct mib_row *ctp(unsigned inst, unsigned port, unsigned dir)
{
	struct mib_row *r = mib_add(CTP, inst);

	put(r, 1, port);
	put(r, 3, dir);
	apply_gem_ctp(find_class(CTP), r);
	return r;
}

/* Delete (0x46: Delete with the AR bit) of CTP `inst`, as a frame. */
static void del(unsigned inst)
{
	uint8_t f[OMCI_FRAME_LEN];

	for (unsigned i = 0; i < sizeof f; i++)
		f[i] = 0;
	f[2] = 0x46;
	f[3] = 0x0a;
	f[4] = CTP >> 8;
	f[5] = CTP & 0xff;
	f[6] = (uint8_t)(inst >> 8);
	f[7] = (uint8_t)inst;
	handle(-1, 0, f);
}

static int used_flows(void)
{
	int n = 0;

	for (int i = 0; i < FLOW_MAX; i++)
		n += flow_ds[i].used ? 1 : 0;
	return n;
}

static int failed(const char *what)
{
	out_fmt("FAIL  %s\n", what);
	return 1;
}

int main(void)
{
	struct mib_row *r;
	int id;

	apply_hw = 1;

	/* Two CTPs share port 100: the flow goes with the last of them. */
	ctp(1, 100, 2);
	ctp(2, 100, 2);
	id = flow_find(1, 100);
	if (id < 0)
		return failed("a downstream CTP gets a flow");
	del(1);
	if (flow_find(1, 100) != id || dels)
		return failed("a flow another CTP still owns is kept");

	/* A withdrawal the driver refuses keeps the slot; the next
	 * reconciliation sends it again. */
	fail_delete = 1;
	del(2);
	if (flow_find(1, 100) != id || dels != 1)
		return failed("a refused withdrawal keeps the slot");
	mcast_extra_refresh(-1, -1, -1);
	if (flow_find(1, 100) >= 0 || dels != 2)
		return failed("the next reconciliation withdraws it");

	/* The broadcast flow is cleared before its slot is freed, and a
	 * clear the driver refuses keeps both. */
	ctp(3, 100, 2);
	bc_flow = flow_find(1, 100);
	fail_bc = 1;
	del(3);
	if (flow_find(1, 100) != id || clears != 1 || dels != 2)
		return failed("a refused broadcast clear keeps the flow");
	fail_delete = 1;
	mcast_extra_refresh(-1, -1, -1);
	if (bc_flow != -1 || flow_find(1, 100) != id || clears != 2)
		return failed("broadcast is cleared before the withdrawal");
	mcast_extra_refresh(-1, -1, -1);
	if (flow_find(1, 100) >= 0 || bad)
		return failed("and the slot is freed after it");

	/* Two broadcast candidates (a class 47 on a class 266 with
	 * interworking option 6, each on its own CTP). Deleting the CTP that
	 * holds the broadcast flow moves it to the survivor: handle() runs
	 * bc_gem_update() after the row is gone. */
	ctp(4, 200, 2);
	ctp(5, 300, 2);
	for (unsigned i = 1; i <= 2; i++) {
		r = mib_add(266, i);
		put(r, 1, i + 3);
		put(r, 2, 6);
		r = mib_add(47, i);
		put(r, 3, 5);
		put(r, 4, i);
	}
	bc_gem_update();
	if (bc_flow != flow_find(1, 200))
		return failed("the first candidate is the broadcast flow");
	del(4);
	if (flow_find(1, 200) >= 0 || bc_flow < 0 ||
	    bc_flow != flow_find(1, 300) || bad)
		return failed("deleting its CTP moves broadcast to the survivor");
	del(5);
	if (used_flows() || bc_flow != -1 || bad)
		return failed("deleting the last CTP leaves no flow");

	/* Only a downstream or bidirectional CTP owns a downstream flow:
	 * an upstream-only one gets none, and a CTP that is Set to
	 * upstream-only gives its flow back. */
	ctp(6, 400, 1);
	if (flow_find(1, 400) >= 0)
		return failed("an upstream-only CTP gets no downstream flow");
	r = ctp(7, 500, 3);
	if (flow_find(1, 500) < 0)
		return failed("a bidirectional CTP gets a downstream flow");
	put(r, 3, 1);
	apply_gem_ctp(find_class(CTP), r);
	if (flow_find(1, 500) >= 0)
		return failed("a CTP set to upstream-only gives its flow back");
	del(6);
	del(7);

	/* More create and delete cycles than the table has slots, each on a
	 * port of its own (#307): every slot comes back. */
	for (unsigned i = 0; i < 3 * FLOW_MAX; i++) {
		ctp(100 + i, 1000 + i, i % 2 ? 2 : 3);
		if (flow_find(1, (uint16_t)(1000 + i)) < 0)
			return failed("a slot is free for every new CTP");
		del(100 + i);
		if (used_flows())
			return failed("the slot comes back with the delete");
	}
	if (bad)
		return failed("no broadcast flow was freed while selected");
	out("ok    downstream flow ownership: shared port, refused withdrawal, broadcast clear and move, direction, 192 cycles\n");
	return 0;
}
