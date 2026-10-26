/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mc_count_test.c -- the multicast group counters of
 * odi_switch_mc_count.c: what each rule matches, that its action does
 * nothing but count, which counter it counts in and in what unit, the 64-bit
 * total folded from the 32-bit counter, what it refuses, and that taking
 * the rules out leaves the switch as it was, the probes' half of LOG_TYPE
 * included.
 *
 * As in odi_switch_acl_probe_test.c, a rule is checked by what it MATCHES:
 * a small matcher built from the plane semantics is asked about frames, the
 * slot contents following the field codes read back from the template
 * registers, and the action entry is decoded by absolute bit number.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"

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

/* The probes, as odi_switch_acl_probe.c would answer. */
static int probe_active;

int odi_sw_probe_active(void)
{
	return probe_active;
}

#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_log.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_mc_count.c"

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

static unsigned int bad_reads;

static void read_hook(uint32_t off)
{
	if (off == ODI_SW_LOG_TYPE_OFF)
		bad_reads++;
}

/* ---- the matcher ------------------------------------------------------ */

struct frame {
	unsigned int port;
	uint8_t dmac[6];
	uint16_t ethertype;	/* after any tag */
	uint32_t dst;		/* IPv4 destination, for an IPv4 frame */
};

static unsigned int slot_code(unsigned int t, unsigned int k)
{
	unsigned int n = 8U * t + k;

	return (reg(0x15008U + 4U * (n / 4U)) >> (7U * (n % 4U))) & 0x7fU;
}

/* What a field code reads from a frame. The three MAC codes in the order
 * odi_switch_mc_count.c takes them: 0x01 the low 16 bits. */
static uint16_t field(unsigned int code, const struct frame *f)
{
	switch (code) {
	case 0x01: return (uint16_t)(f->dmac[4] << 8 | f->dmac[5]);
	case 0x02: return (uint16_t)(f->dmac[2] << 8 | f->dmac[3]);
	case 0x03: return (uint16_t)(f->dmac[0] << 8 | f->dmac[1]);
	case 0x07: return f->ethertype;
	case 0x12: return f->ethertype == 0x0800 ? (uint16_t)f->dst : 0;
	case 0x13: return f->ethertype == 0x0800 ? (uint16_t)(f->dst >> 16) : 0;
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

static int matches(unsigned int entry, const struct frame *f)
{
	return hits(entry, pat[entry][4] & 7U, f);
}

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

/* 239.1.1.1 from the PON, as a stream, and its neighbours. */
static const struct frame grp_pon = { 2, { 0x01, 0x00, 0x5e, 0x01, 0x01, 0x01 }, 0x0800, IP4(239, 1, 1, 1) };
static const struct frame grp_uni = { 0, { 0x01, 0x00, 0x5e, 0x01, 0x01, 0x01 }, 0x0800, IP4(239, 1, 1, 1) };
static const struct frame next_pon = { 2, { 0x01, 0x00, 0x5e, 0x01, 0x01, 0x02 }, 0x0800, IP4(239, 1, 1, 2) };
static const struct frame ra_pon = { 2, { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 }, 0x86dd, 0 };
static const struct frame ns_pon = { 2, { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 }, 0x86dd, 0 };
static const struct frame bcast_pon = { 2, { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 0x0806, 0 };
static const struct frame ucast_pon = { 2, { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 }, 0x0800, IP4(192, 168, 1, 10) };

static const uint8_t grp_mac[6] = { 0x01, 0x00, 0x5e, 0x01, 0x01, 0x01 };
static const uint8_t all48[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ---- state ------------------------------------------------------------- */

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
	probe_active = 0;
	memset(slots, 0, sizeof slots);
	nused = 0;
	log_type = 0;
	/* Templates as the module-load replay leaves template 1 (codes 1 to 6,
	 * 9, 10) and junk elsewhere with the unused top bits set; the ACL
	 * lookup and the permit on for the UNI and the CPU only, so the PON
	 * bits are seen going on; every class word at reset. */
	for (w = 0; w < 14; w++)
		set_reg(0x15008U + 4U * w, 0xa0000000U | (0x01020304U * (w + 1U) & 0x0fffffffU));
	set_reg(0x15010U, 0x0080c101U);
	set_reg(0x15014U, 0x01424305U);
	set_reg(0x15040U, 0x9);
	set_reg(0x15044U, 0x9);
	for (e = 0; e < ENTRIES; e++)
		set_reg(0x15048U + 4U * e, 0x7f);
	set_reg(0x1504cU, 0x4);		/* entry 1, the replayed rule */
	set_reg(ODI_SW_LOG_TYPE_OFF, 0x5a5a5a5aU);
}

static int mcc_set_mac(uint32_t slot, uint32_t port, uint32_t bytes, const uint8_t *mac,
		       const uint8_t *mask, struct odi_sw_mc_count *out)
{
	struct odi_sw_mc_count r;
	int rc;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_MCC_SET;
	r.slot = slot;
	r.port = port;
	r.match = ODI_SW_MCC_MATCH_MAC;
	r.bytes = bytes;
	memcpy(r.mac, mac, 6);
	memcpy(r.mac_mask, mask, 6);
	rc = odi_sw_mc_count_request(&r);
	if (out)
		*out = r;
	return rc;
}

static int mcc_set_ip(uint32_t slot, uint32_t port, uint32_t bytes, uint32_t dst, uint32_t mask)
{
	struct odi_sw_mc_count r;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_MCC_SET;
	r.slot = slot;
	r.port = port;
	r.match = ODI_SW_MCC_MATCH_IPV4;
	r.bytes = bytes;
	r.dst = dst;
	r.dst_mask = mask;
	return odi_sw_mc_count_request(&r);
}

static struct odi_sw_mc_count mcc_get(uint32_t slot)
{
	struct odi_sw_mc_count r;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_MCC_GET;
	r.slot = slot;
	(void)odi_sw_mc_count_request(&r);
	return r;
}

static int mcc_del(uint32_t slot)
{
	struct odi_sw_mc_count r;

	memset(&r, 0, sizeof r);
	r.op = ODI_SW_MCC_DEL;
	r.slot = slot;
	return odi_sw_mc_count_request(&r);
}

/* ---- tests ------------------------------------------------------------- */

static void test_group_from_the_pon(void)
{
	uint32_t before[14], w;
	struct odi_sw_mc_count r;
	unsigned int i, e = ODI_SW_MCC_ENTRY_BASE, last_valid = 0;

	boot_state();
	for (w = 0; w < 14; w++)
		before[w] = reg(0x15008U + 4U * w);
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, &r) == 0, "a byte counter for 239.1.1.1 from the PON");
	CHECK(r.entry == 48 && r.counter == 0 && r.in_use == 1, "slot 0 is entry 48, log counter 0");
	CHECK(slot_code(1, 0) == 0x01 && slot_code(1, 1) == 0x02 && slot_code(1, 2) == 0x03,
	      "template 1: the destination MAC in slots 0 to 2");
	CHECK(slot_code(1, 3) == 0x07 && slot_code(1, 4) == 0x12 && slot_code(1, 5) == 0x13,
	      "then the EtherType and the IPv4 destination halves");
	CHECK(slot_code(1, 6) == 0 && slot_code(1, 7) == 0, "slots 6 and 7 compare nothing");
	CHECK((reg(0x15010U) >> 28) == (before[2] >> 28) && (reg(0x15014U) >> 28) == (before[3] >> 28),
	      "the top bits of the template words are kept");
	for (w = 0; w < 14; w++)
		if (w != 2 && w != 3)
			CHECK(reg(0x15008U + 4U * w) == before[w], "no other template is rewritten");

	CHECK(matches(e, &grp_pon), "the group from the PON counts");
	CHECK(!matches(e, &grp_uni), "from the UNI it does not");
	CHECK(!matches(e, &next_pon), "the next group does not");
	CHECK(!matches(e, &ra_pon) && !matches(e, &bcast_pon) && !matches(e, &ucast_pon),
	      "nor IPv6 multicast, broadcast or unicast");
	{
		struct frame f = grp_pon;

		f.port = 3;
		CHECK(!matches(e, &f), "from the CPU port it does not");
		f.port = 1;
		CHECK(!matches(e, &f), "nor from port 1");
		f = grp_pon;
		f.dst = IP4(239, 129, 1, 1);	/* the same MAC: 23 bits */
		CHECK(matches(e, &f), "a group sharing the MAC counts with it, as it shares the row");
		f = grp_pon;
		f.dmac[3] = 0x81;
		CHECK(!matches(e, &f), "the MAC is compared whole: bit 23 differs");
		f = grp_pon;
		f.dmac[0] = 0x03;
		CHECK(!matches(e, &f), "the high octets are compared");
	}
	CHECK(reg(0x15048U + 4U * e) == 0x08, "the entry applies the log class only");
	CHECK(act_bits(e, 41, 2) == 0 && act_bits(e, 37, 4) == 0, "no forwarding decision");
	CHECK(act_bits(e, 35, 2) == 1 && act_bits(e, 30, 5) == 0, "log class: count in counter 0");
	CHECK(act_bits(e, 0, 30) == 0 && act_bits(e, 43, 32) == 0 && act_bits(e, 75, 21) == 0,
	      "nothing else in the action");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 1U, "LOG_TYPE: pair 0 counts bytes, nothing else set");
	CHECK(reg(0x15040U) == 0xd && reg(0x15044U) == 0xd,
	      "the PON gets its ACL lookup and its permit, the other ports keep theirs");
	CHECK(reg(0x1504cU) == 0x4, "entry 1, the replayed rule, is untouched");
	for (i = 0; i < ntw && i < MAX_TW; i++) {
		CHECK(tw[i].index == e, "every table write is entry 48");
		if (tw[i].table == ODI_SW_TBL_ACL_PATTERN && (tw[i].w0 & (1U << 16)))
			last_valid = i;
	}
	CHECK(tw[0].table == ODI_SW_TBL_ACL_PATTERN && !(tw[0].w0 & (1U << 16)),
	      "the first write clears the valid bit");
	CHECK(last_valid == ntw - 1, "the valid bit is written last");
	r = mcc_get(0);
	CHECK(r.port == 2 && r.match == ODI_SW_MCC_MATCH_MAC && r.bytes == 1 &&
	      !memcmp(r.mac, grp_mac, 6) && !memcmp(r.mac_mask, all48, 6), "GET echoes the rule");
	CHECK(bad_reads == 0, "LOG_TYPE is never read");
}

static void test_masks_and_ipv4(void)
{
	static const uint8_t v6[6] = { 0x33, 0x33, 0, 0, 0, 0 }, v6m[6] = { 0xff, 0xff, 0, 0, 0, 0 };
	static const uint8_t ra[6] = { 0x33, 0x33, 0, 0, 0, 1 };
	struct frame f;
	unsigned int e;

	boot_state();
	CHECK(mcc_set_mac(2, 2, 0, v6, v6m, NULL) == 0, "every IPv6 multicast MAC from the PON");
	e = ODI_SW_MCC_ENTRY_BASE + 2;
	CHECK(matches(e, &ra_pon) && matches(e, &ns_pon), "all-nodes and a solicited-node group count");
	CHECK(!matches(e, &grp_pon) && !matches(e, &bcast_pon), "IPv4 multicast and broadcast do not");
	CHECK(act_bits(e, 30, 5) == 2, "slot 2 counts in counter 2");

	CHECK(mcc_set_mac(3, 2, 0, ra, all48, NULL) == 0, "the all-nodes address alone");
	e = ODI_SW_MCC_ENTRY_BASE + 3;
	CHECK(matches(e, &ra_pon) && !matches(e, &ns_pon), "all-nodes counts, solicited-node does not");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0, "pair 1 counts frames");

	CHECK(mcc_set_ip(4, 2, 0, IP4(224, 0, 0, 0), 0xf0000000U) == 0, "IPv4 224.0.0.0/4 from the PON");
	e = ODI_SW_MCC_ENTRY_BASE + 4;
	CHECK(matches(e, &grp_pon) && matches(e, &next_pon), "any IPv4 multicast counts");
	CHECK(!matches(e, &ra_pon) && !matches(e, &ucast_pon) && !matches(e, &bcast_pon),
	      "IPv6, unicast and ARP do not");
	f = grp_pon;
	f.dst = IP4(240, 0, 0, 1);
	CHECK(!matches(e, &f), "240/4 does not");
	f = grp_pon;
	f.dmac[5] = 0x77;
	CHECK(matches(e, &f), "an IPv4 rule does not look at the MAC");
	CHECK(slots[4].dst == IP4(224, 0, 0, 0) && slots[4].mac_mask[0] == 0,
	      "the address kept masked, no MAC compared");

	/* The last slot: counter 15 straddles the two action words. */
	CHECK(mcc_set_mac(15, 0, 0, grp_mac, all48, NULL) == 0, "slot 15 on the UNI");
	e = ODI_SW_MCC_ENTRY_BASE + 15;
	CHECK(act_bits(e, 30, 5) == 15, "slot 15 counts in counter 15");
	CHECK(matches(e, &grp_uni) && !matches(e, &grp_pon), "from the UNI only");
	CHECK(bad_reads == 0, "LOG_TYPE is never read");
}

static void test_total_is_folded(void)
{
	struct odi_sw_mc_count r;

	boot_state();
	set_reg(ODI_SW_LOG_CNT_OFF + 4U * 5U, 0xfffffff0U);
	CHECK(mcc_set_mac(5, 2, 1, grp_mac, all48, &r) == 0, "installs");
	CHECK(r.counter == 5 && r.now == 0xfffffff0U && r.total_lo == 0 && r.total_hi == 0,
	      "the counter as found counts as zero");
	set_reg(ODI_SW_LOG_CNT_OFF + 4U * 5U, 0x00000010U);
	r = mcc_get(5);
	CHECK(r.now == 0x10 && r.total_lo == 0x20 && r.total_hi == 0, "a wrap of the word is folded in");
	set_reg(ODI_SW_LOG_CNT_OFF + 4U * 5U, 0x00000000U);
	r = mcc_get(5);
	CHECK(r.total_lo == 0x10 && r.total_hi == 1, "almost 2^32 more: the high word moves");
	set_reg(ODI_SW_LOG_CNT_OFF + 4U * 5U, 0x00000030U);
	r = mcc_get(5);
	CHECK(r.total_lo == 0x40 && r.total_hi == 1, "and goes on from there");
	r = mcc_get(5);
	CHECK(r.total_lo == 0x40 && r.total_hi == 1, "a read with nothing counted adds nothing");
	CHECK(mcc_del(5) == 0 && mcc_set_mac(5, 2, 1, grp_mac, all48, &r) == 0, "again");
	CHECK(r.total_lo == 0 && r.total_hi == 0, "a new rule starts its total at zero");
	r = mcc_get(6);
	CHECK(r.in_use == 0 && r.entry == 54 && r.counter == 6, "an empty slot says so, with its entry");
}

static void test_refusals(void)
{
	static const uint8_t none[6] = { 0 };

	boot_state();
	CHECK(mcc_set_mac(0, 1, 1, grp_mac, all48, NULL) == -22, "port 1 is refused");
	CHECK(mcc_set_mac(0, 3, 1, grp_mac, all48, NULL) == -22, "the CPU port is refused");
	CHECK(mcc_set_mac(0, 2, 2, grp_mac, all48, NULL) == -22, "an unknown unit is refused");
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, none, NULL) == -22, "a MAC rule comparing nothing is refused");
	CHECK(mcc_set_ip(0, 2, 1, 0, 0) == -22, "an IPv4 rule comparing nothing is refused");
	CHECK(mcc_set_mac(16, 2, 1, grp_mac, all48, NULL) == -22, "slot 16 is refused");
	{
		struct odi_sw_mc_count r;

		memset(&r, 0, sizeof r);
		r.op = ODI_SW_MCC_SET;
		r.port = 2;
		r.match = 3;
		r.dst_mask = 1;
		CHECK(odi_sw_mc_count_request(&r) == -22, "an unknown match is refused");
		r.op = 7;
		CHECK(odi_sw_mc_count_request(&r) == -22, "an unknown op is refused");
	}
	CHECK(ntw == 0 && reg(0x15010U) == 0x0080c101U, "and a refusal writes nothing");

	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, NULL) == 0, "installs");
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, NULL) == -16, "a slot in use is EBUSY");
	CHECK(mcc_set_mac(1, 2, 0, grp_mac, all48, NULL) == -16,
	      "frames beside bytes in one pair is EBUSY: the pair has one unit");
	CHECK(mcc_set_mac(1, 2, 1, grp_mac, all48, NULL) == 0, "bytes beside bytes goes in");
	CHECK(mcc_set_mac(2, 2, 0, grp_mac, all48, NULL) == 0, "frames in the next pair goes in");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 1U, "pair 0 bytes, pair 1 frames");
	odi_sw_mc_count_release();
	probe_active = 1;
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, NULL) == -16,
	      "nothing goes in while an ACL probe is in");
	CHECK(!odi_sw_mc_count_active(), "and nothing is held");
}

static void test_removal_restores(void)
{
	uint32_t t2, t3;
	unsigned int e0 = ODI_SW_MCC_ENTRY_BASE, e1 = ODI_SW_MCC_ENTRY_BASE + 1, first;

	boot_state();
	/* The probes' pairs, as a probe run would leave them while in. */
	odi_sw_log_unit(16, 1);
	odi_sw_log_unit(30, 1);
	t2 = reg(0x15010U);
	t3 = reg(0x15014U);
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, NULL) == 0, "first");
	CHECK(mcc_set_mac(1, 2, 1, grp_mac, all48, NULL) == 0, "second, the same pair");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == ((1U << 8) | (1U << 15) | 1U), "pair 0 bytes beside the probes' pairs");

	first = ntw;
	CHECK(mcc_del(0) == 0, "the first goes");
	CHECK(tw[first].table == ODI_SW_TBL_ACL_PATTERN && tw[first].index == e0 &&
	      !(tw[first].w0 & (1U << 16)), "valid bit first");
	CHECK(!matches(e0, &grp_pon), "it matches nothing");
	CHECK(reg(0x15048U + 4U * e0) == 0x7f, "its class word is back at reset, never zero");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) & 1U, "the pair keeps bytes while the second slot counts");
	CHECK(reg(0x15010U) != t2, "the template stays while a rule uses it");
	CHECK(odi_sw_mc_count_active(), "still active");
	CHECK(mcc_del(0) == 0, "a second delete is harmless");

	CHECK(mcc_del(ODI_SW_MCC_ALL) == 0, "delete all");
	CHECK(!matches(e1, &grp_pon) && reg(0x15048U + 4U * e1) == 0x7f, "the second is gone");
	CHECK(reg(0x15010U) == t2 && reg(0x15014U) == t3, "template 1 is as the replay left it");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == ((1U << 8) | (1U << 15)),
	      "pair 0 back to frames, the probes' pairs untouched");
	CHECK(!odi_sw_mc_count_active(), "nothing active");
	CHECK(reg(0x15040U) == 0xd && reg(0x15044U) == 0xd,
	      "lookup and permit stay on: they are never taken away");
	CHECK(bad_reads == 0, "LOG_TYPE is never read");

	/* Release, as the holder's close does. */
	boot_state();
	CHECK(mcc_set_mac(7, 2, 1, grp_mac, all48, NULL) == 0, "one");
	odi_sw_mc_count_release();
	CHECK(!matches(ODI_SW_MCC_ENTRY_BASE + 7, &grp_pon) && !odi_sw_mc_count_active(),
	      "release takes it out");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 0 && reg(0x15010U) == 0x0080c101U, "and puts things back");
}

static void test_failed_install_rolls_back(void)
{
	boot_state();
	tw_fail_at = 2;		/* the action write */
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, NULL) == -110, "a table error is returned");
	CHECK(!slots[0].used && !odi_sw_mc_count_active(), "nothing is held");
	CHECK(!matches(ODI_SW_MCC_ENTRY_BASE, &grp_pon), "the entry matches nothing");
	CHECK(reg(0x15048U + 4U * ODI_SW_MCC_ENTRY_BASE) == 0x7f, "its class word is at reset");
	CHECK(reg(0x15010U) == 0x0080c101U && reg(ODI_SW_LOG_TYPE_OFF) == 0,
	      "template and LOG_TYPE as left by no rule");

	boot_state();
	CHECK(mcc_set_mac(0, 2, 1, grp_mac, all48, NULL) == 0, "first");
	tw_fail_at = (int)ntw + 3;	/* the second rule's valid write */
	CHECK(mcc_set_mac(1, 2, 1, grp_mac, all48, NULL) == -110, "second fails");
	CHECK(slots[0].used && matches(ODI_SW_MCC_ENTRY_BASE, &grp_pon), "the first still counts");
	CHECK(reg(ODI_SW_LOG_TYPE_OFF) == 1U, "and its pair still counts bytes");
	CHECK(slot_code(1, 0) == 0x01, "the template stays");
}

int main(void)
{
	test_group_from_the_pon();
	test_masks_and_ipv4();
	test_total_is_folded();
	test_refusals();
	test_removal_restores();
	test_failed_install_rolls_back();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_mc_count_test: all checks passed\n");
	return 0;
}
