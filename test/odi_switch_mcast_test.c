/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mcast_test.c -- the switch half of IGMP snooping
 * (odi_switch_mcast.c): the ACL rule that copies the host port's IGMP to
 * the CPU, the drop of unregistered IPv4 multicast from the PON, and the
 * lease that takes both back with the rows the holder wrote.
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

#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_mcast.c"

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
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_ALL) == 0 && rx_enable_calls == 1,
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
	CHECK(odi_switch_mcast_snoop_set(0x4) < 0, "an unknown flag is refused");

	set_reg(ODI_SW_MC_UNKNOWN_IP4_OFF, 0x0000000cU);	/* ports 1 and 3 flood-bits as they were */
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_ALL) == 0, "copy and drop together");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == (0x0cU | (1U << 4)),
	      "port 2 (the PON) drops, the other ports keep what they had");
	CHECK(reg(ODI_SW_MC_UNKNOWN_CFG_OFF) == 0x28,
	      "the reserved IPv4 groups are flooded again (the OLT's general queries go to 224.0.0.1)");
	CHECK(odi_switch_mcast_flags() == ODI_SW_MC_SNOOP_ALL, "both held");

	/* Back to the copy alone: the drop goes, the copy stays. */
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_COPY) == 0, "drop released");
	CHECK(reg(ODI_SW_MC_UNKNOWN_IP4_OFF) == 0x0cU, "the PON floods again");
	CHECK(reg(ODI_SW_MC_UNKNOWN_CFG_OFF) == 0x20, "the reserved-group setting is back as found");
	CHECK(odi_sw_mcast_copy_on == 1 && odi_switch_mcast_flags() == ODI_SW_MC_SNOOP_COPY,
	      "the copy is still on");

	/* Dropping the copy while the drop is held takes the drop first. */
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_ALL) == 0, "both again");
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
	CHECK(odi_switch_mcast_snoop_set(ODI_SW_MC_SNOOP_ALL) == 0, "lease taken");
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
	CHECK(held == ODI_SW_MC_SNOOP_ALL, "still held");
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

int main(void)
{
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
