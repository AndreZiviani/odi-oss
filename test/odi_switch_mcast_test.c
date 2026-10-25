/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mcast_test.c -- the switch half of IGMP snooping
 * (odi_switch_mcast.c): the ACL rule that copies (or takes) the host port's
 * IGMP to the CPU, the drop of unregistered IPv4 multicast from the PON, the
 * lease that takes them back with the rows the holder wrote, and the way a
 * taken report goes up: what may be sent, by whom, on which flow.
 *
 * The rule is checked by what it MATCHES, not by comparing its words with
 * words typed in from the same reading: a small matcher built from the
 * plane semantics (ones plane: the bits that must be 1, zeros plane: the
 * bits that must be 0, a bit in neither not compared) is asked about every
 * kind of frame, and the rule must fire for IGMP entering the UNI port and
 * for nothing else, the PON port above all. The stock replay's own rule is
 * decoded by the same matcher as a check on the semantics.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"

/* What odi_switch_mcast.c calls beyond the register accessors: recorded
 * here instead of modelling the whole table engine.
 */
#define MAX_TW 32
static struct {
	uint32_t table, index, n;
	uint32_t w[5];
} tw[MAX_TW];
static unsigned int ntw;
static int tw_fail;

int odi_switch_table_write(uint32_t table, uint32_t index, const uint32_t *data, uint32_t n)
{
	if (tw_fail)
		return -5;
	if (ntw < MAX_TW) {
		tw[ntw].table = table;
		tw[ntw].index = index;
		tw[ntw].n = n;
		memcpy(tw[ntw].w, data, n * sizeof(uint32_t));
		ntw++;
	}
	return 0;
}

#define MAX_ROWS 256
static uint8_t rows[MAX_ROWS][6];
static unsigned int nrows;
static unsigned int ndel;

#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_l2.h"

int odi_switch_l2_mcast_add(const struct odi_sw_l2_mcast_req *req, uint32_t *index)
{
	unsigned int i;

	for (i = 0; i < nrows; i++)
		if (!memcmp(rows[i], req->mac, 6))
			goto done;
	memcpy(rows[nrows++], req->mac, 6);
done:
	*index = 7;
	return 0;
}

/* The next n deletes fail with a busy engine. */
static unsigned int del_fail;

int odi_switch_l2_mcast_del(const struct odi_sw_l2_mcast_req *req, int *found)
{
	unsigned int i;

	ndel++;
	if (del_fail) {
		del_fail--;
		return -1;
	}
	for (i = 0; i < nrows; i++)
		if (!memcmp(rows[i], req->mac, 6)) {
			memcpy(rows[i], rows[--nrows], 6);
			if (found)
				*found = 1;
			return 0;
		}
	if (found)
		*found = 0;
	return 0;
}

/* The classifier rows odi_switch_bdgconn.c answers from, for the way up:
 * one upstream row (row 70, flow 3) that adds C-VID 11, the flows it knows,
 * and what was asked. The lookups themselves are tested against real rows in
 * odi_switch_bdgconn_extvlan_test.c. */
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_mcast.h"

static struct odi_sw_us_tags last_tags;
static unsigned int n_classify, n_by_tag;
static int by_tag_hit;

void odi_switch_bdgconn_us_classify(const struct odi_sw_us_tags *t, struct odi_sw_us_treat *out)
{
	n_classify++;
	last_tags = *t;
	memset(out, 0, sizeof(*out));
	if (t->ctag && t->cvid == 99) {
		out->verdict = ODI_SW_US_DISCARD;
		return;
	}
	out->verdict = ODI_SW_US_FWD;
	out->row = 70;
	out->flow = 3;
	out->c_act = 1;
	out->c_vid = 11;
	out->c_src = ODI_SW_US_SRC_VID | ODI_SW_US_SRC_PRI;
	out->s_act = 4;
}

int odi_switch_bdgconn_us_by_tag(uint32_t vid, uint32_t pri, uint32_t *flow, uint32_t *row)
{
	n_by_tag++;
	(void)pri;
	if (!by_tag_hit || vid != 420)
		return -1;
	*flow = 5;
	*row = 66;
	return 0;
}

int odi_switch_bdgconn_us_flow_known(uint32_t flow)
{
	return flow == 3 || flow == 5;
}

/* The transmit odi_omci.c would do: recorded. */
static unsigned int n_tx;
static uint32_t tx_len, tx_flow;
static int tx_rc;

static int tx_record(const uint8_t *frame, uint32_t len, uint32_t flow)
{
	(void)frame;
	n_tx++;
	tx_len = len;
	tx_flow = flow;
	return tx_rc;
}

#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_mcast.c"

static int failures;

#define COPY_DROP (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP)

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

/* ---- the matcher ------------------------------------------------------ */

/* The key a frame presents to the ACL, bit by bit, as the rule layout
 * defines it (160 bits, word 0 first): template number at 2:0, the ingress
 * port at 10 + port, compare slot k at 16k + 31 .. 16k + 16, valid at 144.
 */
static void key_of(uint32_t key[5], unsigned int tmpl, unsigned int port,
		   const uint16_t slot[8])
{
	unsigned int k;

	memset(key, 0, 5 * sizeof(uint32_t));
	key[4] = tmpl | (1U << (10 + port));
	for (k = 0; k < 8; k++) {
		unsigned int lsb = 16U * k + 16U;

		key[4 - lsb / 32U] |= (uint32_t)slot[k] << (lsb % 32U);
	}
	key[0] |= 1U << 16;
}

/* ones: bits that must be 1; zeros: bits that must be 0. */
static int rule_matches(const uint32_t ones[5], const uint32_t zeros[5], const uint32_t key[5])
{
	unsigned int i;

	for (i = 0; i < 5; i++)
		if ((ones[i] & ~key[i]) || (zeros[i] & key[i]))
			return 0;
	return 1;
}

static int frame_hits(const uint32_t ones[5], const uint32_t zeros[5], unsigned int tmpl,
		      unsigned int port, uint16_t ethertype, uint16_t tos_proto)
{
	uint16_t slot[8] = { 0 };
	uint32_t key[5];

	slot[0] = ethertype;
	slot[1] = tos_proto;
	key_of(key, tmpl, port, slot);
	return rule_matches(ones, zeros, key);
}

/* ---- the template the rule names -------------------------------------- */

/* Slot k of template t is the 7-bit code at bit 7(n % 4) of the word at
 * 0x015008 + 4(n / 4), n = 8t + k. */
static unsigned int slot_code(unsigned int t, unsigned int k)
{
	unsigned int n = 8U * t + k;

	return (reg(0x15008U + 4U * (n / 4U)) >> (7U * (n % 4U))) & 0x7fU;
}

static unsigned int rx_enable_calls, rx_enable_at_ntw;

static void rx_enable_cb(void)
{
	rx_enable_calls++;
	rx_enable_at_ntw = ntw;
}

static void boot_state(void)
{
	unsigned int w;

	odi_mock_reset();
	ntw = 0;
	nrows = 0;
	ndel = 0;
	tw_fail = 0;
	del_fail = 0;
	rx_enable_calls = 0;
	odi_sw_mcast_rx_enable = rx_enable_cb;
	held = 0;
	ntracked = 0;
	odi_sw_mcast_copy_on = 0;
	odi_sw_mcast_trap_on = 0;
	/* Templates as a stick reads them (junk in template 6 included, and
	 * the unused top bits of each word set), the ACL lookup on for every
	 * port, MC_UNKNOWN_CFG as claro reads it (reserved IPv4 groups not
	 * exempt). */
	for (w = 0; w < 14; w++)
		set_reg(0x15008U + 4U * w, 0xa0000000U | (0x01020304U * (w + 1U) & 0x0fffffffU));
	set_reg(0x15040U, 0xf);
	set_reg(0x15044U, 0xf);
	set_reg(0x1504cU, 0x4);
	set_reg(ODI_SW_MC_UNKNOWN_CFG_OFF, 0x20);
}

static void test_copy_rule(void)
{
	uint32_t before12, before13;
	uint32_t *ones = 0, *zeros = 0, *act = 0;
	unsigned int i;
	unsigned int last_ones_at = 0;

	boot_state();
	before12 = reg(0x15038U);
	before13 = reg(0x1503cU);
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "copy installs");
	CHECK(odi_switch_mcast_flags() == ODI_SW_MC_SNOOP_COPY, "copy held");
	CHECK(odi_sw_mcast_copy_on == 1, "the RX hook flag is on");

	CHECK(slot_code(6, 0) == 0x07, "template 6 slot 0 is the EtherType");
	CHECK(slot_code(6, 1) == 0x14, "template 6 slot 1 is the IPv4 protocol");
	for (i = 2; i < 8; i++)
		CHECK(slot_code(6, i) == 0, "template 6 slots 2 to 7 compare nothing");
	CHECK((reg(0x15038U) & 0xf0000000U) == (before12 & 0xf0000000U) &&
	      (reg(0x1503cU) & 0xf0000000U) == (before13 & 0xf0000000U),
	      "the unused top bits of the template words are left alone");
	CHECK(reg(0x15008U + 4U * 11U) == (0xa0000000U | (0x01020304U * 12U & 0x0fffffffU)),
	      "other templates are not rewritten");

	CHECK(reg(0x15048U + 4U * 8U) == 0x10, "entry 8 applies only the forwarding class");
	CHECK(reg(0x1504cU) == 0x4, "entry 1, the replayed rule, is untouched");
	CHECK(reg(0x15040U) == 0xf && reg(0x15044U) == 0xf, "ACL lookup and permit stay on");

	/* Table writes: the ones plane (valid) is the LAST of them, and
	 * nothing is written to any entry but 8. */
	for (i = 0; i < ntw; i++) {
		CHECK(tw[i].index == ODI_SW_MC_ACL_ENTRY, "every ACL write is entry 8");
		if (tw[i].table == ODI_SW_TBL_ACL_PATTERN && (tw[i].w[0] & ODI_SW_MC_RULE_VALID)) {
			ones = tw[i].w;
			last_ones_at = i;
		}
		if (tw[i].table == ODI_SW_TBL_ACL_PATTERN_MASK)
			zeros = tw[i].w;
		if (tw[i].table == ODI_SW_TBL_ACL_ACTIONS)
			act = tw[i].w;
	}
	CHECK(ones && zeros && act, "the rule, its mask and its action were written");
	if (!ones || !zeros || !act)
		return;
	CHECK(last_ones_at == ntw - 1, "the valid bit is written last: the rule cannot match half-built");
	CHECK(tw[0].table == ODI_SW_TBL_ACL_PATTERN && !(tw[0].w[0] & ODI_SW_MC_RULE_VALID),
	      "the first write clears the entry's valid bit");

	/* What it matches: IGMP into the UNI port, on template 6. */
	CHECK(frame_hits(ones, zeros, 6, 0, 0x0800, 0x0002), "IGMP from the UNI port matches");
	CHECK(frame_hits(ones, zeros, 6, 0, 0x0800, 0x1002), "IGMP with a TOS set matches (TOS is not compared)");
	CHECK(!frame_hits(ones, zeros, 6, 2, 0x0800, 0x0002), "IGMP from the PON port does NOT match");
	CHECK(!frame_hits(ones, zeros, 6, 3, 0x0800, 0x0002), "IGMP from the CPU port does not match");
	CHECK(!frame_hits(ones, zeros, 6, 1, 0x0800, 0x0002), "IGMP from port 1 does not match");
	CHECK(!frame_hits(ones, zeros, 6, 4, 0x0800, 0x0002) && !frame_hits(ones, zeros, 6, 5, 0x0800, 0x0002),
	      "ports 4 and 5 do not match");
	CHECK(!frame_hits(ones, zeros, 6, 0, 0x0800, 0x0006), "TCP from the UNI port does not match");
	CHECK(!frame_hits(ones, zeros, 6, 0, 0x0800, 0x0011), "UDP from the UNI port does not match");
	CHECK(!frame_hits(ones, zeros, 6, 0, 0x0800, 0x0003), "protocol 3 does not match");
	CHECK(!frame_hits(ones, zeros, 6, 0, 0x86dd, 0x0002), "IPv6 does not match");
	CHECK(!frame_hits(ones, zeros, 6, 0, 0x0806, 0x0002), "ARP does not match");
	CHECK(!frame_hits(ones, zeros, 3, 0, 0x0800, 0x0002), "the same slots under another template do not match");
	CHECK(!frame_hits(ones, zeros, 1, 0, 0x0800, 0x0002), "nor under template 1");

	/* The plane arithmetic, against the one rule the stock replay writes
	 * (template 3: ones word 0x1c03, zeros word 0xe004): the template
	 * number is split the same way (ones = number, zeros = its inverse in
	 * three bits), and the ports outside the rule's set are in the zeros
	 * plane. Only the template bits are compared: the stock rule names
	 * three ports at once, which no one-port rule can say anything about. */
	CHECK((0x1c03U & 7U) == 3U && (0xe004U & 7U) == (~3U & 7U),
	      "stock rule: template bits are number / inverse");
	CHECK((ones[4] & 7U) == 6U && (zeros[4] & 7U) == (~6U & 7U),
	      "our rule splits the template bits the same way");
	CHECK((zeros[4] & 0xe000U) == 0xe000U && (zeros[4] & 0x1c00U) == 0x1800U,
	      "ports outside the rule's set (here 1 to 5) are in the zeros plane");

	/* Action: mirror (2) to the CPU port (3) alone. Bits 42:41 and 40:37
	 * are word 1 bits 10:9 and 8:5. */
	CHECK(act[0] == 0 && act[2] == 0, "no other action field is set");
	CHECK(((act[1] >> 9) & 3U) == 2U, "the decision is mirror, not trap");
	CHECK(((act[1] >> 5) & 0xfU) == 0x8U, "the port mask is the CPU port alone");
	CHECK((act[1] & ~((3U << 9) | (0xfU << 5))) == 0, "nothing else in the action word");

	/* Installing again changes nothing. */
	ntw = 0;
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "installing again is accepted");
	CHECK(ntw == 0, "and writes nothing");

	/* Removing: valid bit first, the template restored, the entry given
	 * the reset enable word back (never zero, which would chain it). */
	ntw = 0;
	CHECK(odi_switch_mcast_snoop_set(0) == 0, "removing succeeds");
	CHECK(odi_sw_mcast_copy_on == 0, "the RX hook flag is off");
	CHECK(ntw >= 3 && tw[0].table == ODI_SW_TBL_ACL_PATTERN && !(tw[0].w[0] & ODI_SW_MC_RULE_VALID),
	      "removal clears the valid bit first");
	CHECK(reg(0x15048U + 4U * 8U) == 0x7f, "the entry gets the reset enable word back");
	CHECK(reg(0x15038U) == before12 && reg(0x1503cU) == before13, "template 6 is restored");
}

static void test_rx_hook_enabled_before_the_rule(void)
{
	boot_state();
	CHECK(rx_enable_calls == 0, "nothing registers the RX hook before a lease");
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "copy installs");
	CHECK(rx_enable_calls == 1, "the hook is registered when the copy is first installed");
	CHECK(rx_enable_at_ntw == 0, "and before any ACL write, so no copy arrives unhooked");
	CHECK(odi_switch_mcast_snoop_set(COPY_DROP) == 0 && rx_enable_calls == 1,
	      "changing the flags does not register it again");
	odi_switch_mcast_release();
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0 && rx_enable_calls == 2,
	      "a later lease asks again (the callback itself is idempotent)");
	odi_switch_mcast_release();
}

static void test_failed_install_rolls_back(void)
{
	boot_state();
	tw_fail = 1;
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == -5, "a table error is reported");
	CHECK(odi_switch_mcast_flags() == 0 && odi_sw_mcast_copy_on == 0, "nothing is held");
	CHECK(reg(0x15038U) == (0xa0000000U | (0x01020304U * 13U & 0x0fffffffU)), "the template is restored");
	CHECK(reg(0x15048U + 4U * 8U) != 0x10, "the entry does not enable anything");
}

static void test_pon_drop(void)
{
	boot_state();
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_PON_DROP) < 0,
	      "the drop without the copy is refused: no group would ever be registered");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == 0, "and nothing is written");
	CHECK(odi_switch_mcast_snoop_set(0x8) < 0, "an unknown flag is refused");
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_TRAP) < 0,
	      "the take without the copy is refused: it is a mode of the copy rule");
	CHECK(odi_switch_mcast_flags() == 0 && ntw == 0, "and nothing is written");

	set_reg(ODI_SW_MC_UNKNOWN_IP4_OFF, 0x0000000cU);	/* ports 1 and 3 flood-bits as they were */
	CHECK(odi_switch_mcast_snoop_set(COPY_DROP) == 0, "copy and drop together");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == (0x0cU | (1U << 4)),
	      "port 2 (the PON) drops, the other ports keep what they had");
	CHECK(reg(ODI_SW_MC_UNKNOWN_CFG_OFF) == 0x28,
	      "the reserved IPv4 groups are flooded again (the OLT's general queries go to 224.0.0.1)");
	CHECK(odi_switch_mcast_flags() == COPY_DROP, "both held");

	/* Back to the copy alone: the drop goes, the copy stays. */
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "drop released");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == 0x0cU, "the PON floods again");
	CHECK(reg(ODI_SW_MC_UNKNOWN_CFG_OFF) == 0x20, "the reserved-group setting is back as found");
	CHECK(odi_sw_mcast_copy_on == 1 && odi_switch_mcast_flags() == ODI_SW_MC_SNOOP_COPY,
	      "the copy is still on");

	/* Dropping the copy while the drop is held takes the drop first. */
	CHECK(odi_switch_mcast_snoop_set(COPY_DROP) == 0, "both again");
	CHECK(odi_switch_mcast_snoop_set(0) == 0, "all released");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == 0x0cU && reg(ODI_SW_MC_UNKNOWN_CFG_OFF) == 0x20,
	      "registers are as found");
}

static void mc(struct odi_sw_l2_mcast_req *r, unsigned int n)
{
	memset(r, 0, sizeof *r);
	r->mac[0] = 0x01;
	r->mac[1] = 0x00;
	r->mac[2] = 0x5e;
	r->mac[3] = (uint8_t)(n >> 16);
	r->mac[4] = (uint8_t)(n >> 8);
	r->mac[5] = (uint8_t)n;
	r->ports = 0x5;
}

static void test_rows_and_release(void)
{
	struct odi_sw_l2_mcast_req r;
	uint32_t idx;
	unsigned int n;
	int found;

	boot_state();
	CHECK(odi_switch_mcast_snoop_set(COPY_DROP) == 0, "lease taken");
	mc(&r, 0x101);
	CHECK(odi_switch_mcast_row_add(&r, &idx) == 0 && nrows == 1, "a row is added");
	CHECK(odi_switch_mcast_row_add(&r, &idx) == 0 && nrows == 1 && ntracked == 1,
	      "the same row again is one row and one track");
	mc(&r, 0x102);
	CHECK(odi_switch_mcast_row_add(&r, &idx) == 0 && ntracked == 2, "a second row");
	mc(&r, 0x101);
	found = 0;
	CHECK(odi_switch_mcast_row_del(&r, &found) == 0 && found == 1 && ntracked == 1 && nrows == 1,
	      "a delete takes the row out of the table and the track");
	CHECK(odi_switch_mcast_row_del(&r, &found) == 0 && found == 0 && ntracked == 1,
	      "a second delete finds nothing and tracks nothing");

	/* The end of the lease deletes what is left, after the flags. */
	CHECK(held == COPY_DROP, "still held");
	odi_switch_mcast_release();
	CHECK(held == 0 && odi_sw_mcast_copy_on == 0, "release takes every flag back");
	CHECK(nrows == 0 && ntracked == 0, "and the rows the holder added");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == 0 && reg(ODI_SW_MC_UNKNOWN_CFG_OFF) == 0x20,
	      "the PON floods as it did");

	/* A holder cannot outgrow the track: the 129th row is refused and
	 * the table is as it was. */
	boot_state();
	for (n = 0; n < ODI_SW_MC_TRACK_MAX; n++) {
		mc(&r, 0x200 + n);
		CHECK(odi_switch_mcast_row_add(&r, &idx) == 0, "row within the limit");
	}
	mc(&r, 0x400);
	CHECK(odi_switch_mcast_row_add(&r, &idx) == -28, "the row past the limit is ENOSPC");
	CHECK(nrows == ODI_SW_MC_TRACK_MAX, "and was not written");
	mc(&r, 0x200);
	CHECK(odi_switch_mcast_row_add(&r, &idx) == 0, "a row already tracked can still be rewritten");
	odi_switch_mcast_release();
	CHECK(nrows == 0 && ndel == ODI_SW_MC_TRACK_MAX, "release deletes every tracked row once");
}

static void test_release_retries(void)
{
	struct odi_sw_l2_mcast_req r;
	uint32_t idx;

	boot_state();
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "lease");
	mc(&r, 0x301);
	CHECK(odi_switch_mcast_row_add(&r, &idx) == 0, "a row");
	mc(&r, 0x302);
	CHECK(odi_switch_mcast_row_add(&r, &idx) == 0, "another");
	del_fail = 1;
	odi_switch_mcast_release();
	CHECK(nrows == 0 && ntracked == 0, "one busy answer is retried: both rows are gone");
	CHECK(ndel == 3, "three deletes for two rows");
}

/* The rule written by the last install: the ones plane (the write with the
 * valid bit), the zeros plane and the action, from the table-write log. */
static void last_rule(uint32_t **ones, uint32_t **zeros, uint32_t **act)
{
	unsigned int i;

	*ones = *zeros = *act = 0;
	for (i = 0; i < ntw; i++) {
		if (tw[i].table == ODI_SW_TBL_ACL_PATTERN && (tw[i].w[0] & ODI_SW_MC_RULE_VALID))
			*ones = tw[i].w;
		if (tw[i].table == ODI_SW_TBL_ACL_PATTERN_MASK)
			*zeros = tw[i].w;
		if (tw[i].table == ODI_SW_TBL_ACL_ACTIONS)
			*act = tw[i].w;
	}
}

/* The take: the same rule, whose action is "trap" (3 at bits 42:41) with no
 * port, as diag acl-probe measured it on a stick: the report reaches the CPU
 * and nothing leaves by the PON. */
static void test_trap_rule(void)
{
	uint32_t *ones, *zeros, *act;
	unsigned int i;

	boot_state();
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_TRAP) == 0,
	      "the copy installs as a take");
	CHECK(odi_switch_mcast_flags() == (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_TRAP), "both held");
	CHECK(odi_sw_mcast_copy_on == 1 && odi_sw_mcast_trap_on == 1, "the hook copies and marks taken");
	last_rule(&ones, &zeros, &act);
	CHECK(ones && zeros && act, "the rule, its mask and its action were written");
	if (!ones || !zeros || !act)
		return;
	CHECK(act[0] == 0 && act[2] == 0 && act[1] == (3U << 9),
	      "the action is trap (3 at bits 42:41), no port, nothing else");
	CHECK(reg(0x15048U + 4U * 8U) == 0x10, "the entry applies only the forwarding class");
	/* What it matches is the copy's: IGMP into the UNI, and from the PON
	 * nothing at all. */
	CHECK(frame_hits(ones, zeros, 6, 0, 0x0800, 0x0002), "IGMP from the UNI port is taken");
	CHECK(!frame_hits(ones, zeros, 6, 2, 0x0800, 0x0002), "IGMP from the PON port is NOT taken");
	CHECK(!frame_hits(ones, zeros, 6, 3, 0x0800, 0x0002), "nor from the CPU port");
	CHECK(!frame_hits(ones, zeros, 6, 0, 0x0800, 0x0011) && !frame_hits(ones, zeros, 6, 0, 0x86dd, 0x0002),
	      "nor anything but IPv4 IGMP");
	for (i = 0; i < ntw; i++)
		CHECK(tw[i].index == ODI_SW_MC_ACL_ENTRY, "every ACL write is entry 8");

	/* Back to a copy with the rule in: the action entry alone is rewritten,
	 * and the mark goes off after it. */
	ntw = 0;
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "take to copy");
	CHECK(ntw == 1 && tw[0].table == ODI_SW_TBL_ACL_ACTIONS && tw[0].index == ODI_SW_MC_ACL_ENTRY &&
	      tw[0].w[1] == ((2U << 9) | (0x8U << 5)),
	      "one write, the action entry: mirror to the CPU port again");
	CHECK(odi_sw_mcast_trap_on == 0 && odi_sw_mcast_copy_on == 1, "the mark is off, the copy on");
	ntw = 0;
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_ALL) == 0, "copy to take, with the drop");
	CHECK(ntw == 1 && tw[0].table == ODI_SW_TBL_ACL_ACTIONS && tw[0].w[1] == (3U << 9),
	      "one write, the action entry: trap");
	CHECK(odi_sw_mcast_trap_on == 1 && odi_switch_mcast_flags() == ODI_SW_MC_SNOOP_ALL,
	      "taken, and the drop on");

	/* A failed mode write keeps the old mode, mark included. */
	tw_fail = 1;
	CHECK(odi_switch_mcast_snoop_set(COPY_DROP) == -5, "a table error is reported");
	CHECK(odi_switch_mcast_flags() == ODI_SW_MC_SNOOP_ALL && odi_sw_mcast_trap_on == 1,
	      "and the take is still held and marked");
	tw_fail = 0;

	/* The end of the lease takes the rule out whatever its mode: the host's
	 * reports go up by the switch again. */
	ntw = 0;
	odi_switch_mcast_release();
	CHECK(odi_switch_mcast_flags() == 0 && odi_sw_mcast_copy_on == 0 && odi_sw_mcast_trap_on == 0,
	      "release: nothing held, nothing marked");
	CHECK(ntw >= 3 && tw[0].table == ODI_SW_TBL_ACL_PATTERN && !(tw[0].w[0] & ODI_SW_MC_RULE_VALID),
	      "the rule stops matching first");
	CHECK(reg(0x15048U + 4U * 8U) == 0x7f, "the entry gets the reset enable word back");
}

/* An IGMPv2 report frame for 239.1.1.1, untagged, padded to 60 bytes. */
static uint32_t report(uint8_t *f)
{
	memset(f, 0, 64);
	memcpy(f, "\x01\x00\x5e\x01\x01\x01\x02\x00\x00\x00\x00\x09\x08\x00", 14);
	f[14] = 0x46;
	f[23] = 2;
	return 60;
}

static void test_send(void)
{
	uint8_t f[64];
	uint32_t n = report(f);

	boot_state();
	odi_sw_mcast_tx = tx_record;
	n_tx = 0;
	tx_rc = 0;
	CHECK(odi_switch_mcast_send(f, n, 3) == -1, "no lease: nothing is sent (EPERM)");
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "the copy alone");
	CHECK(odi_switch_mcast_send(f, n, 3) == -1,
	      "with the copy alone the switch sent the report itself: nothing is sent");
	CHECK(n_tx == 0, "the transmit was never called");

	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_TRAP) == 0, "the take");
	CHECK(odi_switch_mcast_send(f, n, 3) == 0 && n_tx == 1 && tx_len == 60 && tx_flow == 3,
	      "a report on a flow an upstream row queues on: sent, once");
	CHECK(odi_switch_mcast_send(f, n, 4) == -22 && n_tx == 1,
	      "a flow no upstream row queues on: refused");
	CHECK(odi_switch_mcast_send(f, n, 0x80) == -22, "a flow past seven bits: refused");
	CHECK(odi_switch_mcast_send(f, 59, 3) == -22, "shorter than a minimum frame: refused");
	CHECK(odi_switch_mcast_send(f, 1537, 3) == -22, "longer than the largest: refused");
	f[23] = 17;
	CHECK(odi_switch_mcast_send(f, n, 3) == -22, "UDP, not IGMP: refused");
	f[23] = 2;
	f[0] = 0x02;
	CHECK(odi_switch_mcast_send(f, n, 3) == -22, "to a unicast MAC: refused");
	f[0] = 0x01;
	f[3] = 0x81;
	CHECK(odi_switch_mcast_send(f, n, 3) == -22, "to a MAC outside the IPv4 multicast block: refused");
	f[3] = 0x01;
	f[12] = 0x86;
	f[13] = 0xdd;
	CHECK(odi_switch_mcast_send(f, n, 3) == -22, "IPv6: refused");
	f[12] = 0x08;
	f[13] = 0x00;
	CHECK(n_tx == 1, "none of the refused ones reached the transmit");
	tx_rc = -105;
	CHECK(odi_switch_mcast_send(f, n, 5) == -105, "the transmit's error is passed back");
	tx_rc = 0;

	odi_switch_mcast_release();
	CHECK(odi_switch_mcast_send(f, n, 3) == -1, "after the lease: nothing is sent");
	odi_sw_mcast_tx = 0;
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_TRAP) == 0 &&
	      odi_switch_mcast_send(f, n, 3) == -1, "with no transmit registered: refused, not a crash");
	odi_switch_mcast_release();
}

static void test_route(void)
{
	struct odi_sw_mc_route r;

	memset(&r, 0, sizeof r);
	r.mode = ODI_SW_MC_ROUTE_FRAME;
	n_classify = n_by_tag = 0;
	CHECK(odi_switch_mcast_route(&r) == 0 && r.verdict == ODI_SW_US_FWD && r.via == ODI_SW_MC_ROUTE_FRAME &&
	      r.row == 70 && r.flow == 3 && r.c_act == 1 && r.c_vid == 11 && r.s_act == 4 &&
	      r.c_src == (ODI_SW_US_SRC_VID | ODI_SW_US_SRC_PRI),
	      "FRAME: the row the frame matches, its flow and its treatment");
	CHECK(n_classify == 1 && n_by_tag == 0 && !last_tags.ctag && !last_tags.stag,
	      "classified once, by the frame's own tags");

	memset(&r, 0, sizeof r);
	r.mode = ODI_SW_MC_ROUTE_FRAME;
	r.ctag = 1;
	r.cvid = 99;
	r.cpri = 3;
	CHECK(odi_switch_mcast_route(&r) == 0 && r.verdict == ODI_SW_US_DISCARD && r.flow == 0,
	      "a frame the extended VLAN rules discard: no flow");
	CHECK(last_tags.ctag == 1 && last_tags.cvid == 99 && last_tags.cpri == 3, "the C-tag was passed on");

	memset(&r, 0, sizeof r);
	r.mode = ODI_SW_MC_ROUTE_TAG;
	r.want_vid = 420;
	r.want_pri = 5;
	by_tag_hit = 1;
	CHECK(odi_switch_mcast_route(&r) == 0 && r.verdict == ODI_SW_US_FWD && r.via == ODI_SW_MC_ROUTE_TAG &&
	      r.flow == 5 && r.row == 66 && r.c_act == 0,
	      "TAG: the row that carries VLAN 420 up, and no treatment to apply");
	by_tag_hit = 0;
	n_classify = 0;
	CHECK(odi_switch_mcast_route(&r) == 0 && r.verdict == ODI_SW_US_FWD && r.via == ODI_SW_MC_ROUTE_FRAME &&
	      r.flow == 3 && n_classify == 1,
	      "TAG with no row for that VLAN: the flow the report itself would take");

	memset(&r, 0, sizeof r);
	CHECK(odi_switch_mcast_route(&r) == -22, "mode 0: refused");
	r.mode = 3;
	CHECK(odi_switch_mcast_route(&r) == -22, "mode 3: refused");
	r.mode = ODI_SW_MC_ROUTE_FRAME;
	r.cvid = 4096;
	CHECK(odi_switch_mcast_route(&r) == -22, "a VID past 12 bits: refused");
	r.cvid = 0;
	r.ctag = 2;
	CHECK(odi_switch_mcast_route(&r) == -22, "a tag flag not 0 or 1: refused");
	r.ctag = 0;
	r.mode = ODI_SW_MC_ROUTE_TAG;
	r.want_vid = 4095;
	CHECK(odi_switch_mcast_route(&r) == -22, "TAG of VID 4095: refused");

	/* The ABI the userland copy (src/diag/src/odi_sw_ioctl.h) restates. */
	CHECK(sizeof(struct odi_sw_mc_route) == 88 && sizeof(struct odi_sw_mc_send) == 1544,
	      "the route request is 88 bytes, the send 1544");
}

int main(void)
{
	test_trap_rule();
	test_send();
	test_route();
	test_rx_hook_enabled_before_the_rule();
	test_release_retries();
	test_copy_rule();
	test_failed_install_rolls_back();
	test_pon_drop();
	test_rows_and_release();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_mcast_test: all checks passed\n");
	return 0;
}
