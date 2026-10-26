// SPDX-License-Identifier: GPL-2.0
/*
 * Counting rules for one multicast destination each, entering the PON or
 * the UNI port (odi_switch_mc_count.h says what they are for and why they
 * may do no more than count).
 *
 * The rule layout is odi_switch_mcast.c's and the action the probes' log
 * class (odi_switch_acl_probe.c has the bit positions, and the probe run of
 * docs/SWITCH.md measured a rule like this counting while the frames went
 * on as before). Template 1 is laid out for both kinds of match:
 *
 *   slot 0  destination MAC bits 15:0   (field code 0x01)
 *   slot 1  destination MAC bits 31:16  (0x02)
 *   slot 2  destination MAC bits 47:32  (0x03)
 *   slot 3  EtherType                   (0x07)
 *   slot 4  IPv4 destination bits 15:0  (0x12)
 *   slot 5  IPv4 destination bits 31:16 (0x13)
 *   slots 6, 7 compare nothing
 *
 * The three MAC codes are another open implementation's reading ("a MAC
 * address takes three consecutive codes"); that the first holds the low
 * bits follows the IPv4 pair, whose first code is the low half. Neither
 * order has been measured on a stick: count-proof.sh's UNI window, an exact
 * MAC rule on frames the host sends, is the check. A MAC rule compares the
 * three MAC slots alone, so a tagged frame, an IPv6 one and an IPv4 one to
 * the same MAC are all counted; an IPv4 rule compares the EtherType and
 * the address.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "odi_switch_mc_count.h"
#include "odi_switch_acl_probe.h"
#include "odi_switch_log.h"
#include "odi_switch_mcast.h"
#include "odi_switch_dal.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_tbl.h"

#define MCC_EBUSY	(-16)
#define MCC_EINVAL	(-22)

struct mcc_slot {
	uint32_t used;
	uint32_t port, match, bytes;
	uint8_t mac[6], mac_mask[6];
	uint32_t dst, dst_mask;
	uint32_t last;		/* the counter word at the last read */
	uint64_t total;		/* what the rule counted since it went in */
};

static struct mcc_slot slots[ODI_SW_MCC_SLOTS];
static uint32_t nused;
static uint32_t saved_tmpl[2];

/* ---- template 1 ----------------------------------------------------------- */

#define MCC_FIELD_DMAC0		0x01U
#define MCC_FIELD_DMAC1		0x02U
#define MCC_FIELD_DMAC2		0x03U
#define MCC_FIELD_DST_LO	0x12U
#define MCC_FIELD_DST_HI	0x13U
#define MCC_TMPL_FIRST		(8U * ODI_SW_MCC_TEMPLATE)
#define MCC_TMPL_WORD(n)	(ODI_SW_ACL_TMPL_FIELD_OFF + 4U * ((n) / 4U))
#define MCC_TMPL_SHIFT(n)	(7U * ((n) % 4U))

/* The top four bits of each word are not slots and are kept. Put back as
 * found when the last rule goes: the module-load replay wrote codes of its
 * own here, which nothing reads, and they go back all the same. */
static void mcc_template_set(void)
{
	uint32_t w0 = MCC_TMPL_WORD(MCC_TMPL_FIRST);
	uint32_t w1 = MCC_TMPL_WORD(MCC_TMPL_FIRST + 4U);

	saved_tmpl[0] = odi_reg_read(w0);
	saved_tmpl[1] = odi_reg_read(w1);
	odi_reg_write(w0, (saved_tmpl[0] & ~0x0fffffffU) |
		      (MCC_FIELD_DMAC0 << MCC_TMPL_SHIFT(0U)) |
		      (MCC_FIELD_DMAC1 << MCC_TMPL_SHIFT(1U)) |
		      (MCC_FIELD_DMAC2 << MCC_TMPL_SHIFT(2U)) |
		      (ODI_SW_ACL_FIELD_ETHERTYPE << MCC_TMPL_SHIFT(3U)));
	odi_reg_write(w1, (saved_tmpl[1] & ~0x0fffffffU) |
		      (MCC_FIELD_DST_LO << MCC_TMPL_SHIFT(0U)) |
		      (MCC_FIELD_DST_HI << MCC_TMPL_SHIFT(1U)));
}

static void mcc_template_restore(void)
{
	odi_reg_write(MCC_TMPL_WORD(MCC_TMPL_FIRST), saved_tmpl[0]);
	odi_reg_write(MCC_TMPL_WORD(MCC_TMPL_FIRST + 4U), saved_tmpl[1]);
}

/* ---- the rule --------------------------------------------------------------- */

#define MCC_PORT_SHIFT		10U
#define MCC_PORTS_ALL		0x3fU
#define MCC_TMPL_BITS		7U
#define MCC_ETHERTYPE_IPV4	0x0800U
#define MCC_ACT_LOG_COUNTER	1U
#define MCC_ACT_EN_LOG		0x08U

/* Compare slot k into both planes: ones = care & value, zeros = care &
 * ~value, slot k at bits 16k + 31 .. 16k + 16 of the entry. */
static void mcc_slot_put(uint32_t ones[ODI_SW_MC_RULE_WORDS], uint32_t zeros[ODI_SW_MC_RULE_WORDS],
			 unsigned int k, uint32_t value, uint32_t care)
{
	unsigned int lsb = 16U * k + 16U;
	unsigned int w = 4U - lsb / 32U, sh = lsb % 32U;

	ones[w] |= (value & care & 0xffffU) << sh;
	zeros[w] |= (~value & care & 0xffffU) << sh;
}

static uint32_t be16(const uint8_t *p)
{
	return ((uint32_t)p[0] << 8) | p[1];
}

static void mcc_planes(const struct mcc_slot *s, uint32_t ones[ODI_SW_MC_RULE_WORDS],
		       uint32_t zeros[ODI_SW_MC_RULE_WORDS])
{
	memset(ones, 0, ODI_SW_MC_RULE_WORDS * sizeof(uint32_t));
	memset(zeros, 0, ODI_SW_MC_RULE_WORDS * sizeof(uint32_t));
	ones[0] = ODI_SW_MC_RULE_VALID;
	/* The template, all three bits, and the ingress port: its own bit in
	 * the ones plane, every other port in the zeros plane. */
	ones[4] = ((1U << s->port) << MCC_PORT_SHIFT) | ODI_SW_MCC_TEMPLATE;
	zeros[4] = ((MCC_PORTS_ALL & ~(1U << s->port)) << MCC_PORT_SHIFT) |
		   (MCC_TMPL_BITS & ~ODI_SW_MCC_TEMPLATE);
	if (s->match == ODI_SW_MCC_MATCH_MAC) {
		mcc_slot_put(ones, zeros, 0U, be16(s->mac + 4), be16(s->mac_mask + 4));
		mcc_slot_put(ones, zeros, 1U, be16(s->mac + 2), be16(s->mac_mask + 2));
		mcc_slot_put(ones, zeros, 2U, be16(s->mac), be16(s->mac_mask));
	} else {
		mcc_slot_put(ones, zeros, 3U, MCC_ETHERTYPE_IPV4, 0xffffU);
		mcc_slot_put(ones, zeros, 4U, s->dst, s->dst_mask);
		mcc_slot_put(ones, zeros, 5U, s->dst >> 16, s->dst_mask >> 16);
	}
}

static uint32_t mcc_counter(uint32_t n)
{
	return ODI_SW_MCC_COUNTER_BASE + n;
}

/* The log class alone: count in the slot's counter, decide nothing. */
static void mcc_action(uint32_t counter, uint32_t act[ODI_SW_MC_ACTION_WORDS])
{
	act[0] = 0;
	act[1] = (MCC_ACT_LOG_COUNTER << 3) | (counter >> 2);
	act[2] = (counter & 3U) << 30;
}

static void mcc_entry_clear(uint32_t entry)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	static const uint32_t zact[ODI_SW_MC_ACTION_WORDS] = { 0, 0, 0 };

	/* The valid bit first, so the rule stops matching before anything
	 * under it changes; the class word back to its reset value, never
	 * zero, which would chain the entry to the one before it. */
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, zero, ODI_SW_MC_RULE_WORDS);
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, entry, zero, ODI_SW_MC_RULE_WORDS);
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, entry, zact, ODI_SW_MC_ACTION_WORDS);
	odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * entry, ODI_SW_ACL_ACT_EN_RESET);
}

/* The other slot of a counter pair, which shares its unit. */
static uint32_t mcc_partner(uint32_t n)
{
	return n ^ 1U;
}

/* A slot gone: its pair back to frames, the reset value, unless the other
 * slot of the pair still counts; the template as found with the last. */
static void mcc_gone(uint32_t n)
{
	if (!slots[mcc_partner(n)].used)
		odi_sw_log_unit(mcc_counter(n), 0);
	if (!nused)
		mcc_template_restore();
}

static int mcc_install(uint32_t n, struct mcc_slot *s)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	uint32_t ones[ODI_SW_MC_RULE_WORDS], zeros[ODI_SW_MC_RULE_WORDS];
	uint32_t act[ODI_SW_MC_ACTION_WORDS], permit, port_en;
	uint32_t entry = ODI_SW_MCC_ENTRY_BASE + n, counter = mcc_counter(n);
	uint32_t pbit = 1U << s->port;
	int rc;

	mcc_planes(s, ones, zeros);
	mcc_action(counter, act);

	if (!nused)
		mcc_template_set();
	/* A permit for a frame no rule matches before the lookup, or a port
	 * with the lookup on drops everything; neither is taken away (the
	 * stock init has both on for every port). */
	permit = odi_reg_read(ODI_SW_ACL_DEFAULT_PERMIT_OFF);
	if (!(permit & pbit))
		odi_reg_write(ODI_SW_ACL_DEFAULT_PERMIT_OFF, permit | pbit);
	port_en = odi_reg_read(ODI_SW_MC_ACL_PORT_EN_OFF);
	if (!(port_en & pbit))
		odi_reg_write(ODI_SW_MC_ACL_PORT_EN_OFF, port_en | pbit);

	/* What the pair counts, before anything can count in it. */
	odi_sw_log_unit(counter, s->bytes);

	rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, zero, ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, entry, zeros,
					    ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, entry, act,
					    ODI_SW_MC_ACTION_WORDS);
	if (!rc) {
		odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * entry, MCC_ACT_EN_LOG);
		/* The counter is never cleared: what the rule counts is the
		 * difference from here, the last moment before it can match. */
		s->last = odi_sw_log_read(counter);
		s->total = 0;
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, ones,
					    ODI_SW_MC_RULE_WORDS);
	}
	if (rc) {
		mcc_entry_clear(entry);
		mcc_gone(n);
		return rc;
	}
	s->used = 1;
	nused++;
	return 0;
}

static void mcc_remove(uint32_t n)
{
	if (!slots[n].used)
		return;
	mcc_entry_clear(ODI_SW_MCC_ENTRY_BASE + n);
	slots[n].used = 0;
	nused--;
	mcc_gone(n);
}

/* What the rule counted, folded: the counter word now less the word at the
 * last read, modulo 2^32, added to the total. One wrap between two reads is
 * folded in; two are lost, which a reader avoids by reading often enough. */
static void mcc_fold(struct mcc_slot *s, uint32_t counter, uint32_t *now)
{
	uint32_t w = odi_sw_log_read(counter);

	s->total += (uint32_t)(w - s->last);
	s->last = w;
	*now = w;
}

static int mcc_mac_any(const uint8_t *m)
{
	return m[0] | m[1] | m[2] | m[3] | m[4] | m[5];
}

static int mcc_check(const struct odi_sw_mc_count *r)
{
	if (r->port != ODI_SW_MCC_PORT_UNI && r->port != ODI_SW_MCC_PORT_PON)
		return MCC_EINVAL;
	if (r->bytes > 1U)
		return MCC_EINVAL;
	if (r->match == ODI_SW_MCC_MATCH_MAC)
		return mcc_mac_any(r->mac_mask) ? 0 : MCC_EINVAL;
	if (r->match == ODI_SW_MCC_MATCH_IPV4)
		return r->dst_mask ? 0 : MCC_EINVAL;
	return MCC_EINVAL;
}

static void mcc_fill(uint32_t n, struct odi_sw_mc_count *r)
{
	struct mcc_slot *s = &slots[n];
	uint32_t i;

	r->in_use = s->used;
	r->entry = ODI_SW_MCC_ENTRY_BASE + n;
	r->counter = mcc_counter(n);
	if (!s->used)
		return;
	r->port = s->port;
	r->match = s->match;
	r->bytes = s->bytes;
	for (i = 0; i < 6; i++) {
		r->mac[i] = s->mac[i];
		r->mac_mask[i] = s->mac_mask[i];
	}
	r->dst = s->dst;
	r->dst_mask = s->dst_mask;
	mcc_fold(s, r->counter, &r->now);
	r->total_lo = (uint32_t)s->total;
	r->total_hi = (uint32_t)(s->total >> 32);
}

int odi_sw_mc_count_request(struct odi_sw_mc_count *r)
{
	struct mcc_slot *s, *p;
	uint32_t n, i;
	int rc;

	if (r->op == ODI_SW_MCC_DEL && r->slot == ODI_SW_MCC_ALL) {
		odi_sw_mc_count_release();
		return 0;
	}
	if (r->slot >= ODI_SW_MCC_SLOTS)
		return MCC_EINVAL;
	n = r->slot;
	s = &slots[n];

	switch (r->op) {
	case ODI_SW_MCC_GET:
		mcc_fill(n, r);
		return 0;
	case ODI_SW_MCC_DEL:
		mcc_remove(n);
		return 0;
	case ODI_SW_MCC_SET:
		break;
	default:
		return MCC_EINVAL;
	}

	rc = mcc_check(r);
	if (rc)
		return rc;
	p = &slots[mcc_partner(n)];
	if (s->used || odi_sw_probe_active() || (p->used && p->bytes != r->bytes))
		return MCC_EBUSY;
	s->port = r->port;
	s->match = r->match;
	s->bytes = r->bytes;
	for (i = 0; i < 6; i++) {
		s->mac_mask[i] = r->match == ODI_SW_MCC_MATCH_MAC ? r->mac_mask[i] : 0;
		s->mac[i] = r->mac[i] & s->mac_mask[i];
	}
	s->dst_mask = r->match == ODI_SW_MCC_MATCH_IPV4 ? r->dst_mask : 0;
	s->dst = r->dst & s->dst_mask;
	rc = mcc_install(n, s);
	if (rc)
		return rc;
	mcc_fill(n, r);
	return 0;
}

int odi_sw_mc_count_active(void)
{
	return nused != 0;
}

void odi_sw_mc_count_release(void)
{
	uint32_t n;

	for (n = 0; n < ODI_SW_MCC_SLOTS; n++)
		mcc_remove(n);
}
