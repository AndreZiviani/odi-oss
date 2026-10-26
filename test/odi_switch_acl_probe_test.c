/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_acl_probe_test.c -- the hand-installed ACL rules of
 * odi_switch_acl_probe.c: what each rule matches, what its action does with
 * a frame, which counter it counts in and in what unit, what it refuses, and
 * that taking the rules out leaves the switch as it was.
 *
 * As in odi_switch_mcast_test.c, a rule is checked by what it MATCHES: a
 * small matcher built from the plane semantics (ones plane: bits that must
 * be 1, zeros plane: bits that must be 0) is asked about frames, and the
 * action entry is decoded by absolute bit number, not compared with words
 * typed in from the same reading.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"

/* The ACL tables, as the table engine would hold them, plus the order of
 * the writes. */
#define ENTRIES 96
static uint32_t pat[ENTRIES][5], msk[ENTRIES][5], act_tbl[ENTRIES][3];
#define MAX_TW 256
static struct {
	uint32_t table, index;
	uint32_t w0;
} tw[MAX_TW];
static unsigned int ntw;
static int tw_fail_at = -1;	/* the n-th write fails, -1 never */

int odi_switch_table_write(uint32_t table, uint32_t index, const uint32_t *data, uint32_t n)
{
	if (tw_fail_at >= 0 && (int)ntw == tw_fail_at) {
		ntw++;
		return -110;
	}
	if (index >= ENTRIES)
		abort();
	if (ntw < MAX_TW) {
		tw[ntw].table = table;
		tw[ntw].index = index;
		tw[ntw].w0 = data[0];
	}
	ntw++;
	if (table == ODI_SW_TBL_ACL_PATTERN)
		memcpy(pat[index], data, n * sizeof(uint32_t));
	else if (table == ODI_SW_TBL_ACL_PATTERN_MASK)
		memcpy(msk[index], data, n * sizeof(uint32_t));
	else if (table == ODI_SW_TBL_ACL_ACTIONS)
		memcpy(act_tbl[index], data, n * sizeof(uint32_t));
	else
		abort();
	return 0;
}

/* The snooping lease, as odi_switch_mcast.c would answer, and the group
 * counters, as odi_switch_mc_count.c would. */
static uint32_t snoop_flags;
static int mcc_active;

uint32_t odi_switch_mcast_flags(void)
{
	return snoop_flags;
}

int odi_sw_mc_count_active(void)
{
	return mcc_active;
}

#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_log.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_acl_probe.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

static void set_reg(uint32_t off, uint32_t v)
{
	odi_mock.regs[odi_mock_slot(off)] = v;
}

/* Reads of registers the probe must never read. */
static unsigned int bad_reads;

static void read_hook(uint32_t off)
{
	if (off == ODI_SW_LOG_TYPE_OFF || (off >= 0x026008U && off <= 0x026010U))
		bad_reads++;
}

/* ---- the matcher ------------------------------------------------------ */

struct frame {
	unsigned int port;
	uint16_t ethertype;
	uint8_t proto;
	uint32_t dst;
};

/* The key a frame presents on a template: the template number, the ingress
 * port, and slot k at bits 16k + 31 .. 16k + 16, word 0 holding bits
 * 159:128. The slot contents follow the field codes the template holds,
 * read back from the template registers, so a wrong code fails here. */
static unsigned int slot_code(unsigned int t, unsigned int k)
{
	unsigned int n = 8U * t + k;

	return (reg(0x15008U + 4U * (n / 4U)) >> (7U * (n % 4U))) & 0x7fU;
}

static uint16_t field(unsigned int code, const struct frame *f)
{
	switch (code) {
	case 0x07: return f->ethertype;
	case 0x14: return f->proto;	/* the TOS above it is 0 here */
	case 0x12: return (uint16_t)f->dst;
	case 0x13: return (uint16_t)(f->dst >> 16);
	default: return 0;
	}
}

static int hits(unsigned int entry, unsigned int tmpl, const struct frame *f)
{
	uint32_t key[5] = { 0 };
	unsigned int k, i;

	key[4] = tmpl | (1U << (10 + f->port));
	for (k = 0; k < 8; k++) {
		unsigned int lsb = 16U * k + 16U;

		key[4 - lsb / 32U] |= (uint32_t)field(slot_code(tmpl, k), f) << (lsb % 32U);
	}
	key[0] |= 1U << 16;
	if (!(pat[entry][0] & (1U << 16)))
		return 0;
	for (i = 0; i < 5; i++)
		if ((pat[entry][i] & ~key[i]) || (msk[entry][i] & key[i]))
			return 0;
	return 1;
}

/* A frame meets the rule on the template the rule names. */
static int matches(unsigned int entry, const struct frame *f)
{
	return hits(entry, pat[entry][4] & 7U, f);
}

/* Bits lsb .. lsb + width - 1 (width at most 32) of a 96-bit action entry,
 * word 0 holding bits 95:64. */
static uint32_t act_bits(unsigned int entry, unsigned int lsb, unsigned int width)
{
	uint32_t v = 0;
	unsigned int i;

	for (i = 0; i < width; i++) {
		unsigned int b = lsb + i;

		v |= ((act_tbl[entry][2 - b / 32U] >> (b % 32U)) & 1U) << i;
	}
	return v;
}

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static const struct frame igmp_uni = { 0, 0x0800, 2, IP4(224, 0, 0, 22) };
static const struct frame igmp_pon = { 2, 0x0800, 2, IP4(224, 0, 0, 1) };
static const struct frame udp_uni_group = { 0, 0x0800, 17, IP4(239, 255, 77, 1) };
static const struct frame udp_pon_group = { 2, 0x0800, 17, IP4(239, 255, 77, 1) };
static const struct frame udp_uni_other = { 0, 0x0800, 17, IP4(239, 255, 77, 2) };
static const struct frame udp_uni_ucast = { 0, 0x0800, 17, IP4(192, 168, 1, 1) };
static const struct frame v6_uni = { 0, 0x86dd, 2, IP4(224, 0, 0, 22) };

/* ---- state ------------------------------------------------------------- */

static unsigned int rx_enable_calls, rx_enable_at_ntw;

static void rx_enable_cb(void)
{
	rx_enable_calls++;
	rx_enable_at_ntw = ntw;
}

static void boot_state(void)
{
	unsigned int w, e;

	odi_mock_reset();
	odi_mock_read_hook = read_hook;
	bad_reads = 0;
	memset(pat, 0, sizeof pat);
	memset(msk, 0, sizeof msk);
	memset(act_tbl, 0, sizeof act_tbl);
	ntw = 0;
	tw_fail_at = -1;
	snoop_flags = 0;
	mcc_active = 0;
	rx_enable_calls = 0;
	odi_sw_probe_rx_enable = rx_enable_cb;
	memset(slots, 0, sizeof slots);
	nused = 0;
	log_type = 0;
	odi_sw_probe_on = 0;
	/* Templates with junk in them and the unused top bits set, the ACL
	 * lookup and the permit on for the UNI and the CPU only (so the PON
	 * bits are seen being turned on), every class word at reset. */
	for (w = 0; w < 14; w++)
		set_reg(0x15008U + 4U * w, 0xa0000000U | (0x01020304U * (w + 1U) & 0x0fffffffU));
	set_reg(0x15040U, 0x9);
	set_reg(0x15044U, 0x9);
	for (e = 0; e < ENTRIES; e++)
		set_reg(0x15048U + 4U * e, 0x7f);
	set_reg(0x1504cU, 0x4);		/* entry 1, the replayed rule */
	set_reg(ODI_SW_LOG_TYPE_OFF, 0x5a5a5a5aU);
}

static int probe_set(uint32_t slot, uint32_t port, uint32_t action, uint32_t bytes,
		     uint32_t proto, uint32_t dst, uint32_t mask, struct odi_sw_acl_probe *out)
{
	struct odi_sw_acl_probe r;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_PROBE_SET;
	r.slot = slot;
	r.port = port;
	r.action = action;
	r.bytes = bytes;
	r.proto = proto;
	r.dst = dst;
	r.dst_mask = mask;
	{
		int rc = odi_sw_probe_request(&r);

		if (out)
			*out = r;
		return rc;
	}
}

static struct odi_sw_acl_probe probe_get(uint32_t slot)
{
	struct odi_sw_acl_probe r;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_PROBE_GET;
	r.slot = slot;
	(void)odi_sw_probe_request(&r);
	return r;
}

static int probe_del(uint32_t slot)
{
	struct odi_sw_acl_probe r;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_PROBE_DEL;
	r.slot = slot;
	return odi_sw_probe_request(&r);
}

/* ---- tests ------------------------------------------------------------- */

static void test_count_igmp_from_the_pon(void)
{
	uint32_t before[14], w;
	struct odi_sw_acl_probe r;
	unsigned int i, e = ODI_SW_PROBE_ENTRY_BASE, last_valid = 0;

	boot_state();
	for (w = 0; w < 14; w++)
		before[w] = reg(0x15008U + 4U * w);
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, &r) == 0, "a count of IGMP from the PON installs");
	CHECK(r.entry == 88 && r.counter == 16, "slot 0 is entry 88, log counter 16");
	CHECK(rx_enable_calls == 1 && rx_enable_at_ntw == 0,
	      "the RX hook is asked for once, before any table write");

	CHECK(slot_code(4, 0) == 0x07 && slot_code(4, 1) == 0x14 &&
	      slot_code(4, 2) == 0x12 && slot_code(4, 3) == 0x13,
	      "template 4: EtherType, IPv4 protocol, destination low and high halves");
	for (i = 4; i < 8; i++)
		CHECK(slot_code(4, i) == 0, "template 4 slots 4 to 7 compare nothing");
	CHECK((reg(0x15028U) >> 28) == (before[8] >> 28) && (reg(0x1502cU) >> 28) == (before[9] >> 28),
	      "the top bits of the template words are kept");
	for (w = 0; w < 14; w++)
		if (w != 8 && w != 9)
			CHECK(reg(0x15008U + 4U * w) == before[w], "no other template is rewritten");

	CHECK(matches(e, &igmp_pon), "IGMP from the PON is counted");
	CHECK(!matches(e, &igmp_uni), "IGMP from the UNI is not");
	CHECK(!matches(e, &udp_pon_group), "UDP from the PON is not");
	{
		struct frame f = igmp_pon;

		f.port = 3;
		CHECK(!matches(e, &f), "IGMP from the CPU port is not");
		f.port = 1;
		CHECK(!matches(e, &f), "nor from port 1");
		f.ethertype = 0x86dd;
		f.port = 2;
		CHECK(!matches(e, &f), "nor IPv6");
		f = igmp_pon;
		f.dst = IP4(239, 1, 2, 3);
		CHECK(matches(e, &f), "any destination: a group-specific query counts too");
	}
	/* The action: the log class alone, counter 16, frames. */
	CHECK(reg(0x15048U + 4U * e) == 0x08, "the entry applies the log class only");
	CHECK(act_bits(e, 41, 2) == 0 && act_bits(e, 37, 4) == 0, "no forwarding decision");
	CHECK(act_bits(e, 35, 2) == 1 && act_bits(e, 30, 5) == 16, "log class: count in counter 16");
	CHECK(act_bits(e, 0, 30) == 0 && act_bits(e, 43, 32) == 0 &&
	      act_bits(e, 75, 21) == 0, "nothing else in the action");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0, "LOG_TYPE written: pair 8 counts frames, nothing else set");
	CHECK(reg(0x15040U) == 0xd && reg(0x15044U) == 0xd,
	      "the PON gets its ACL lookup and its permit, the other ports keep theirs");
	CHECK(reg(0x1504cU) == 0x4, "entry 1, the replayed rule, is untouched");

	for (i = 0; i < ntw && i < MAX_TW; i++) {
		CHECK(tw[i].index == e, "every table write is entry 88");
		if (tw[i].table == ODI_SW_TBL_ACL_PATTERN && (tw[i].w0 & (1U << 16)))
			last_valid = i;
	}
	CHECK(tw[0].table == ODI_SW_TBL_ACL_PATTERN && !(tw[0].w0 & (1U << 16)),
	      "the first write clears the valid bit");
	CHECK(last_valid == ntw - 1, "the valid bit is written last");
	CHECK(odi_sw_probe_on == 1, "the RX path is told");
	CHECK(bad_reads == 0, "LOG_TYPE and the DoS threshold words are never read");
}

static void test_count_a_group_and_a_range(void)
{
	struct frame f;
	unsigned int e1 = ODI_SW_PROBE_ENTRY_BASE + 1, e2 = ODI_SW_PROBE_ENTRY_BASE + 2;

	boot_state();
	CHECK(probe_set(1, 0, ODI_SW_PROBE_COUNT, 1, ODI_SW_PROBE_ANY,
			IP4(239, 255, 77, 1), 0xffffffffU, NULL) == 0, "a count of one group from the UNI");
	CHECK(matches(e1, &udp_uni_group), "the group from the UNI counts");
	CHECK(!matches(e1, &udp_uni_other), "the next group does not");
	CHECK(!matches(e1, &udp_pon_group), "the group from the PON does not");
	f = udp_uni_group;
	f.proto = 1;
	CHECK(matches(e1, &f), "any protocol to the group counts (a ping to it)");
	f.dst = IP4(239, 255, 76, 1);
	CHECK(!matches(e1, &f), "a group differing in the low half does not");
	f.dst = IP4(238, 255, 77, 1);
	CHECK(!matches(e1, &f), "a group differing in the high half does not");
	CHECK(!matches(e1, &v6_uni), "IPv6 does not");
	CHECK(act_bits(e1, 30, 5) == 18 && act_bits(e1, 35, 2) == 1, "slot 1 counts in counter 18");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == (1U << 9), "pair 9 counts bytes");

	CHECK(probe_set(2, 2, ODI_SW_PROBE_COUNT, 0, ODI_SW_PROBE_ANY,
			IP4(224, 0, 0, 0), 0xf0000000U, NULL) == 0, "a count of 224.0.0.0/4 from the PON");
	CHECK(matches(e2, &igmp_pon) && matches(e2, &udp_pon_group), "any IPv4 multicast from the PON");
	f = udp_pon_group;
	f.dst = IP4(192, 168, 1, 1);
	CHECK(!matches(e2, &f), "unicast does not");
	f.dst = IP4(240, 0, 0, 1);
	CHECK(!matches(e2, &f), "240/4 does not");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == (1U << 9), "pair 10 counts frames, pair 9 still bytes");
	CHECK(slots[2].dst == IP4(224, 0, 0, 0), "the address is kept masked");

	/* The slot's last counter, slot 7: counter 30 straddles the two
	 * action words. */
	CHECK(probe_set(7, 0, ODI_SW_PROBE_COUNT, 0, 17, 0, 0, NULL) == 0, "slot 7");
	CHECK(act_bits(ODI_SW_PROBE_ENTRY_BASE + 7, 30, 5) == 30, "slot 7 counts in counter 30");
	CHECK(matches(ODI_SW_PROBE_ENTRY_BASE + 7, &udp_uni_ucast) &&
	      !matches(ODI_SW_PROBE_ENTRY_BASE + 7, &igmp_uni), "slot 7: UDP from the UNI");
	CHECK(bad_reads == 0, "LOG_TYPE never read");
}

static void test_counter_is_a_difference(void)
{
	struct odi_sw_acl_probe r;

	boot_state();
	set_reg(ODI_SW_LOG_CNT_OFF + 4U * 20U, 0xfffffff0U);
	CHECK(probe_set(2, 0, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, &r) == 0, "installs");
	CHECK(r.base == 0xfffffff0U && r.counter == 20, "the counter as found is the base");
	set_reg(ODI_SW_LOG_CNT_OFF + 4U * 20U, 0x00000010U);
	r = probe_get(2);
	CHECK(r.in_use == 1 && r.now == 0x10 && (uint32_t)(r.now - r.base) == 0x20,
	      "now - base, modulo 2^32, is what the rule counted");
	CHECK(r.port == 0 && r.action == ODI_SW_PROBE_COUNT && r.proto == 2,
	      "GET echoes the rule");
	r = probe_get(3);
	CHECK(r.in_use == 0 && r.entry == 91 && r.counter == 22, "an empty slot says so, with its entry");
	r = probe_get(8);
	CHECK(r.in_use == 0, "slot 8 does not exist");
	{
		struct odi_sw_acl_probe q;

		memset(&q, 0, sizeof q);
		q.op = ODI_SW_PROBE_GET;
		q.slot = 8;
		CHECK(odi_sw_probe_request(&q) == -22, "and is refused");
	}
}

static void test_mirror_and_trap(void)
{
	unsigned int e3 = ODI_SW_PROBE_ENTRY_BASE + 3, e4 = ODI_SW_PROBE_ENTRY_BASE + 4;

	boot_state();
	CHECK(probe_set(3, 0, ODI_SW_PROBE_MIRROR, 0, 2, 0, 0, NULL) == 0, "a mirror of the UNI's IGMP");
	CHECK(matches(e3, &igmp_uni) && !matches(e3, &igmp_pon) && !matches(e3, &udp_uni_group),
	      "it matches IGMP from the UNI only");
	CHECK(act_bits(e3, 41, 2) == 2 && act_bits(e3, 37, 4) == 0x8, "mirror to the CPU port alone");
	CHECK(act_bits(e3, 35, 2) == 1 && act_bits(e3, 30, 5) == 22, "and counted in counter 22");
	CHECK(reg(0x15048U + 4U * e3) == 0x18, "forwarding and log classes");
	CHECK(probe_del(3) == 0, "removed");

	CHECK(probe_set(4, 0, ODI_SW_PROBE_TRAP, 0, 2, 0, 0, NULL) == 0, "a trap of the UNI's IGMP");
	CHECK(matches(e4, &igmp_uni) && !matches(e4, &igmp_pon), "IGMP from the UNI only");
	CHECK(act_bits(e4, 41, 2) == 3 && act_bits(e4, 37, 4) == 0, "trap, no port in the mask");
	CHECK(reg(0x15048U + 4U * e4) == 0x18, "forwarding and log classes");

	/* Never anything else to the CPU. */
	CHECK(probe_set(5, 2, ODI_SW_PROBE_TRAP, 0, 2, 0, 0, NULL) == -22, "a trap from the PON is refused");
	CHECK(probe_set(5, 2, ODI_SW_PROBE_MIRROR, 0, 2, 0, 0, NULL) == -22, "a mirror from the PON is refused");
	CHECK(probe_set(5, 0, ODI_SW_PROBE_TRAP, 0, 17, 0, 0, NULL) == -22, "a trap of UDP is refused");
	CHECK(probe_set(5, 0, ODI_SW_PROBE_MIRROR, 0, ODI_SW_PROBE_ANY, 0, 0, NULL) == -22,
	      "a mirror of all IPv4 is refused");
	CHECK(probe_set(5, 1, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == -22, "port 1 is refused");
	CHECK(probe_set(5, 3, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == -22, "the CPU port is refused");
	CHECK(probe_set(5, 0, 4, 0, 2, 0, 0, NULL) == -22, "an unknown action is refused");
	CHECK(probe_set(5, 0, ODI_SW_PROBE_COUNT, 2, 2, 0, 0, NULL) == -22, "an unknown counter mode is refused");
	CHECK(probe_set(5, 0, ODI_SW_PROBE_COUNT, 0, 0x101, 0, 0, NULL) == -22, "a protocol past 255 is refused");
	CHECK(probe_set(8, 0, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == -22, "slot 8 is refused");
	CHECK(!slots[5].used && reg(0x15048U + 4U * (ODI_SW_PROBE_ENTRY_BASE + 5)) == 0x7f,
	      "and a refusal writes nothing");
}

static void test_busy(void)
{
	boot_state();
	CHECK(probe_set(0, 0, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == 0, "installs");
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == -16, "a slot in use is EBUSY");
	CHECK(slots[0].port == 0, "and keeps its rule");
	odi_sw_probe_release();
	snoop_flags = 1;
	CHECK(probe_set(0, 0, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == -16,
	      "nothing goes in while the snooping lease is held");
	CHECK(ntw == 0 || !(pat[ODI_SW_PROBE_ENTRY_BASE][0] & (1U << 16)), "and nothing was written valid");
	snoop_flags = 0;
	mcc_active = 1;
	CHECK(probe_set(0, 0, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == -16,
	      "nor while a multicast group counter is in: the lower entry would take its count");
	CHECK(!slots[0].used, "and nothing is held");
}

/* LOG_TYPE has two owners: the probes (pairs 8 to 15) and the group
 * counters (pairs 0 to 7, odi_switch_mc_count.c). The probes change their
 * own bits and no other, whatever they do. */
static void test_log_type_is_shared(void)
{
	uint32_t c;

	boot_state();
	/* The group counters' pairs counting bytes, as igmpd leaves them. */
	for (c = 0; c < 16; c += 2)
		odi_sw_log_unit(c, 1);
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0xffU, "pairs 0 to 7 count bytes");
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 1, 2, 0, 0, NULL) == 0, "a byte probe");
	CHECK(probe_set(1, 0, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == 0, "a frame probe");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0x1ffU, "pair 8 bytes, pair 9 frames, pairs 0 to 7 kept");
	CHECK(probe_del(0) == 0, "the byte probe goes");
	CHECK((reg(ODI_SW_LOG_TYPE_OFF) & 0xffU) == 0xffU, "pairs 0 to 7 kept");
	CHECK(probe_del(ODI_SW_PROBE_ALL) == 0, "the last probe goes");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0xffU,
	      "the last probe leaves the group counters' pairs as they were");

	/* A failed install leaves them too. */
	boot_state();
	odi_sw_log_unit(4, 1);
	tw_fail_at = 2;
	CHECK(probe_set(3, 2, ODI_SW_PROBE_COUNT, 1, 2, 0, 0, NULL) == -110, "a failed probe");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == (1U << 2), "pair 2 still counts bytes, pair 11 is back to frames");
	CHECK(bad_reads == 0, "LOG_TYPE never read");
}

static void test_removal_restores(void)
{
	uint32_t t8, t9;
	unsigned int e0 = ODI_SW_PROBE_ENTRY_BASE, e1 = ODI_SW_PROBE_ENTRY_BASE + 1;
	unsigned int first;

	boot_state();
	t8 = reg(0x15028U);
	t9 = reg(0x1502cU);
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 1, 2, 0, 0, NULL) == 0, "first");
	CHECK(probe_set(1, 0, ODI_SW_PROBE_TRAP, 0, 2, 0, 0, NULL) == 0, "second");
	CHECK(rx_enable_calls == 1, "the RX hook is asked for once");

	first = ntw;
	CHECK(probe_del(0) == 0, "the first goes");
	CHECK(tw[first].table == ODI_SW_TBL_ACL_PATTERN && tw[first].index == e0 &&
	      !(tw[first].w0 & (1U << 16)), "valid bit first");
	CHECK(!matches(e0, &igmp_pon), "it matches nothing");
	CHECK(reg(0x15048U + 4U * e0) == 0x7f, "its class word is back at reset, never zero");
	CHECK(reg(0x15028U) != t8, "the template stays while a rule uses it");
	CHECK(odi_sw_probe_active() && odi_sw_probe_on, "still active");
	CHECK(probe_del(0) == 0, "a second delete is harmless");

	CHECK(probe_del(ODI_SW_PROBE_ALL) == 0, "delete all");
	CHECK(!matches(e1, &igmp_uni) && reg(0x15048U + 4U * e1) == 0x7f, "the trap is gone");
	CHECK(reg(0x15028U) == t8 && reg(0x1502cU) == t9, "template 4 is as found");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0, "LOG_TYPE is left at 0, frames everywhere");
	CHECK(!odi_sw_probe_active() && odi_sw_probe_on == 0, "nothing active, the RX path told");
	CHECK(reg(0x15040U) == 0xd && reg(0x15044U) == 0xd,
	      "lookup and permit stay on: they are never taken away");
	CHECK(bad_reads == 0, "LOG_TYPE never read");

	/* Release, as the holder's close does, from a fresh start. */
	boot_state();
	CHECK(probe_set(6, 0, ODI_SW_PROBE_MIRROR, 0, 2, 0, 0, NULL) == 0, "a mirror");
	odi_sw_probe_release();
	CHECK(!matches(ODI_SW_PROBE_ENTRY_BASE + 6, &igmp_uni) && !odi_sw_probe_active(),
	      "release takes it out");
}

static void test_failed_install_rolls_back(void)
{
	uint32_t t8;

	boot_state();
	t8 = reg(0x15028U);
	tw_fail_at = 2;		/* the action write */
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 1, 2, 0, 0, NULL) == -110, "a table error is returned");
	CHECK(!slots[0].used && !odi_sw_probe_active() && odi_sw_probe_on == 0, "nothing is held");
	CHECK(!matches(ODI_SW_PROBE_ENTRY_BASE, &igmp_pon), "the entry matches nothing");
	CHECK(reg(0x15048U + 4U * ODI_SW_PROBE_ENTRY_BASE) == 0x7f, "its class word is at reset");
	CHECK(reg(0x15028U) == t8 && reg(ODI_SW_LOG_TYPE_OFF) == 0, "template and LOG_TYPE as left by no rule");

	/* A failure on a second rule leaves the first alone. */
	boot_state();
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 1, 2, 0, 0, NULL) == 0, "first");
	tw_fail_at = (int)ntw + 3;	/* the second rule's valid write */
	CHECK(probe_set(1, 0, ODI_SW_PROBE_COUNT, 1, 2, 0, 0, NULL) == -110, "second fails");
	CHECK(slots[0].used && matches(ODI_SW_PROBE_ENTRY_BASE, &igmp_pon), "the first still counts");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == (1U << 8), "its bytes bit kept, the failed one cleared");
	CHECK(slot_code(4, 0) == 0x07, "the template stays");
}

static void test_rx_accounting(void)
{
	struct odi_sw_acl_probe r;

	boot_state();
	CHECK(probe_set(2, 2, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == 0, "a count in slot 2");
	CHECK(probe_set(1, 0, ODI_SW_PROBE_TRAP, 0, 2, 0, 0, NULL) == 0, "a trap in slot 1");
	odi_sw_probe_rx(128 + 89, 0, 64, 1);
	odi_sw_probe_rx(128 + 89, 0, 64, 1);
	odi_sw_probe_rx(128 + 88, 0, 64, 1);	/* slot 0, empty: not a hit */
	odi_sw_probe_rx(7, 0, 64, 1);		/* IGMP from the UNI, another reason */
	odi_sw_probe_rx(9, 0, 64, 0);		/* not IGMP */
	odi_sw_probe_rx(11, 2, 64, 1);		/* from the PON */
	r = probe_get(1);
	CHECK(r.cpu_hits == 2 && r.cpu_ports == 0x1, "two frames with the entry's reason, from the UNI");
	CHECK(r.cpu_bytes == 128, "and their lengths");
	CHECK(r.uni_igmp_other == 2 && r.other_reason == 7,
	      "IGMP from the UNI with another reason is counted apart, with the reason");
	odi_sw_probe_rx(128 + 89, 2, 64, 1);
	r = probe_get(1);
	CHECK(r.cpu_hits == 3 && r.cpu_ports == 0x5, "a hit from the PON would show in the port mask");
	CHECK(probe_del(1) == 0 && probe_set(1, 0, ODI_SW_PROBE_TRAP, 0, 2, 0, 0, NULL) == 0, "again");
	r = probe_get(1);
	CHECK(r.cpu_hits == 0 && r.cpu_ports == 0 && r.cpu_bytes == 0, "a new rule starts its counts at zero");
	CHECK(r.uni_igmp_other == 2, "the session count stays while any rule is in");
	odi_sw_probe_release();
	CHECK(probe_set(0, 2, ODI_SW_PROBE_COUNT, 0, 2, 0, 0, NULL) == 0, "a new session");
	r = probe_get(0);
	CHECK(r.uni_igmp_other == 0 && r.other_reason == 0, "starts its counts at zero");
}

int main(void)
{
	test_count_igmp_from_the_pon();
	test_count_a_group_and_a_range();
	test_counter_is_a_difference();
	test_mirror_and_trap();
	test_busy();
	test_removal_restores();
	test_failed_install_rolls_back();
	test_rx_accounting();
	test_log_type_is_shared();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_acl_probe_test: all checks passed\n");
	return 0;
}
