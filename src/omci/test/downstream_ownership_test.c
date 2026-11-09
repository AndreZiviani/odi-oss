/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "omcid.h"
static int fail_delete, fail_bc, dels, clears, bad;
int __wrap_omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)len;
	if (cmd == 26 && nl_get32(buf) == OMCI_BC_FLOW_NONE) {
		clears++;
		if (fail_bc) { fail_bc = 0; return -1; }
	}
	if (cmd == 25 && !((struct omci_gemflow *)buf)->ena) {
		if (bc_flow == (int)((struct omci_gemflow *)buf)->flow_id) bad = 1;
		dels++;
		if (fail_delete) { fail_delete = 0; return -1; }
	}
	return 0;
}
static void put(struct mib_row *r, unsigned k, uint32_t v)
{
	const struct omci_class *c = find_class(r->classId);
	unsigned n = attr_width(c, k), o = attr_offset(c, k);
	for (unsigned i = 0; i < n; i++) r->data[o + n - 1 - i] = (uint8_t)(v >> (8 * i));
}
static struct mib_row *ctp(unsigned inst, unsigned port)
{
	struct mib_row *r = mib_add(268, inst);
	put(r, 1, port); put(r, 3, 2);
	apply_gem_ctp(find_class(268), r);
	return r;
}
static void del(unsigned inst)
{
	apply_delete(find_class(268), inst);
	mib_del(268, inst);
	bc_gem_update();
}
int main(void)
{
	apply_hw = 1;
	ctp(1, 100); ctp(2, 100);
	int id = flow_find(1, 100);
	if (id < 0) return 1;
	del(1);
	if (flow_find(1, 100) != id || dels) return 2;
	fail_delete = 1; del(2);
	if (flow_find(1, 100) != id || dels != 1) return 3;
	mcast_extra_refresh(-1, -1, -1);
	if (flow_find(1, 100) >= 0 || dels != 2) return 4;
	ctp(3, 100); bc_flow = flow_find(1, 100);
	fail_bc = 1; del(3);
	if (flow_find(1, 100) != id || clears != 1 || dels != 2) return 5;
	fail_delete = 1; mcast_extra_refresh(-1, -1, -1);
	if (bc_flow != -1 || flow_find(1, 100) != id || clears != 2) return 6;
	mcast_extra_refresh(-1, -1, -1);
	if (flow_find(1, 100) >= 0 || bad) return 7;
	ctp(4, 200); ctp(5, 300);
	for (unsigned i = 1; i <= 2; i++) {
		struct mib_row *r = mib_add(266, i);
		put(r, 1, i + 3); put(r, 2, 6);
		r = mib_add(47, i); put(r, 3, 5); put(r, 4, i);
	}
	bc_gem_update();
	if (bc_flow != flow_find(1, 200)) return 8;
	del(4);
	if (flow_find(1, 200) >= 0 || bc_flow != flow_find(1, 300) || bad) return 9;
	out("ok    downstream CTP ownership, sharing, failed withdrawal, broadcast clear and reselection\n");
	return 0;
}
