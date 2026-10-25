// SPDX-License-Identifier: GPL-2.0
/*
 * Hand-installed ACL rules that count multicast, or copy or take the host's
 * IGMP to the CPU, for measuring the switch (odi_switch_acl_probe.h says what
 * each may do and why no more).
 *
 * The rule layout is odi_switch_mcast.c's (two planes, the valid bit in the
 * ones plane, written last). The action entry, 96 bits, word 0 first (bits
 * 95:64), from another open implementation's measurements:
 *
 *   bits 42:41  forwarding decision: 2 mirror, 3 trap (word 1 bits 10:9)
 *   bits 40:37  its port mask (word 1 bits 8:5)
 *   bits 36:35  log class type, 1 = count in a log counter (word 1 bits 4:3)
 *   bits 34:30  the log counter (word 1 bits 2:0 and word 2 bits 31:30)
 *
 * and the entry's action-class word applies the log class with bit 3 and
 * the forwarding decision with bit 4. A counting rule applies the log class
 * alone, which leaves the frame's forwarding to the switch: the same
 * measurements have a rule like it counting a stream that still arrived.
 * Each class is decided by the lowest matching entry that has it, so two
 * rules here that match the same frame count it once, in the lower slot.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "odi_switch_acl_probe.h"
#include "odi_switch_mcast.h"
#include "odi_switch_dal.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_tbl.h"

#define PROBE_EBUSY	(-16)
#define PROBE_EINVAL	(-22)

volatile uint32_t odi_sw_probe_on;
void (*odi_sw_probe_rx_enable)(void);

struct probe_slot {
	uint32_t used;
	uint32_t port, action, bytes, proto, dst, dst_mask;
	uint32_t base;
	uint32_t cpu_hits, cpu_ports, cpu_bytes;
};

static struct probe_slot slots[ODI_SW_PROBE_SLOTS];
static uint32_t nused;
static uint32_t saved_tmpl[2];
static uint32_t log_type;
static uint32_t uni_igmp_other, other_reason;

/* ---- template 4 --------------------------------------------------------- */

#define PROBE_FIELD_DST_LO	0x12U	/* IPv4 destination, bits 15:0 */
#define PROBE_FIELD_DST_HI	0x13U	/* bits 31:16 */
#define PROBE_TMPL_FIRST	(8U * ODI_SW_PROBE_TEMPLATE)
#define PROBE_TMPL_WORD(n)	(ODI_SW_ACL_TMPL_FIELD_OFF + 4U * ((n) / 4U))
#define PROBE_TMPL_SHIFT(n)	(7U * ((n) % 4U))

/* Slots 0..3 hold the EtherType, the protocol and the two halves of the
 * destination; 4..7 compare nothing. The top four bits of each word are
 * not slots and are kept. Put back as found when the last rule goes. */
static void probe_template_set(void)
{
	uint32_t w0 = PROBE_TMPL_WORD(PROBE_TMPL_FIRST);
	uint32_t w1 = PROBE_TMPL_WORD(PROBE_TMPL_FIRST + 4U);

	saved_tmpl[0] = odi_reg_read(w0);
	saved_tmpl[1] = odi_reg_read(w1);
	odi_reg_write(w0, (saved_tmpl[0] & ~0x0fffffffU) |
		      (ODI_SW_ACL_FIELD_ETHERTYPE << PROBE_TMPL_SHIFT(0U)) |
		      (ODI_SW_ACL_FIELD_IPV4_PROTO << PROBE_TMPL_SHIFT(1U)) |
		      (PROBE_FIELD_DST_LO << PROBE_TMPL_SHIFT(2U)) |
		      (PROBE_FIELD_DST_HI << PROBE_TMPL_SHIFT(3U)));
	odi_reg_write(w1, saved_tmpl[1] & ~0x0fffffffU);
}

static void probe_template_restore(void)
{
	odi_reg_write(PROBE_TMPL_WORD(PROBE_TMPL_FIRST), saved_tmpl[0]);
	odi_reg_write(PROBE_TMPL_WORD(PROBE_TMPL_FIRST + 4U), saved_tmpl[1]);
}

/* ---- the rule ------------------------------------------------------------ */

#define PROBE_PORT_SHIFT	10U
#define PROBE_PORTS_ALL		0x3fU
#define PROBE_TMPL_BITS		7U
#define PROBE_ETHERTYPE_IPV4	0x0800U
#define PROBE_PROTO_IGMP	2U
#define PROBE_CPU_PORT		3U

/* One compare slot k into both planes: ones = care & value, zeros = care
 * & ~value, slot k at bits 16k + 31 .. 16k + 16 of the entry (word 0 holds
 * bits 159:128). */
static void plane_slot(uint32_t ones[ODI_SW_MC_RULE_WORDS], uint32_t zeros[ODI_SW_MC_RULE_WORDS],
		       unsigned int k, uint32_t value, uint32_t care)
{
	unsigned int lsb = 16U * k + 16U;
	unsigned int w = 4U - lsb / 32U, sh = lsb % 32U;

	ones[w] |= (value & care & 0xffffU) << sh;
	zeros[w] |= (~value & care & 0xffffU) << sh;
}

static void probe_planes(const struct probe_slot *s, uint32_t ones[ODI_SW_MC_RULE_WORDS],
			 uint32_t zeros[ODI_SW_MC_RULE_WORDS])
{
	memset(ones, 0, ODI_SW_MC_RULE_WORDS * sizeof(uint32_t));
	memset(zeros, 0, ODI_SW_MC_RULE_WORDS * sizeof(uint32_t));
	ones[0] = ODI_SW_MC_RULE_VALID;
	/* The template, all three bits, and the ingress port: its own bit in
	 * the ones plane, every other port in the zeros plane. */
	ones[4] = ((1U << s->port) << PROBE_PORT_SHIFT) | ODI_SW_PROBE_TEMPLATE;
	zeros[4] = ((PROBE_PORTS_ALL & ~(1U << s->port)) << PROBE_PORT_SHIFT) |
		   (PROBE_TMPL_BITS & ~ODI_SW_PROBE_TEMPLATE);
	plane_slot(ones, zeros, 0U, PROBE_ETHERTYPE_IPV4, 0xffffU);
	/* The protocol is the low octet; the TOS above it is never compared. */
	if (s->proto != ODI_SW_PROBE_ANY)
		plane_slot(ones, zeros, 1U, s->proto, 0x00ffU);
	plane_slot(ones, zeros, 2U, s->dst, s->dst_mask);
	plane_slot(ones, zeros, 3U, s->dst >> 16, s->dst_mask >> 16);
}

#define PROBE_ACT_FWD_MIRROR	2U
#define PROBE_ACT_FWD_TRAP	3U
#define PROBE_ACT_LOG_COUNTER	1U
#define PROBE_ACT_EN_LOG	0x08U

static uint32_t probe_counter(uint32_t slot)
{
	return ODI_SW_PROBE_COUNTER_BASE + 2U * slot;
}

static void probe_action(const struct probe_slot *s, uint32_t counter,
			 uint32_t act[ODI_SW_MC_ACTION_WORDS], uint32_t *en)
{
	act[0] = 0;
	act[1] = (PROBE_ACT_LOG_COUNTER << 3) | (counter >> 2);
	act[2] = (counter & 3U) << 30;
	*en = PROBE_ACT_EN_LOG;
	if (s->action == ODI_SW_PROBE_MIRROR) {
		act[1] |= (PROBE_ACT_FWD_MIRROR << 9) | ((1U << PROBE_CPU_PORT) << 5);
		*en |= ODI_SW_ACL_ACT_EN_FWD;
	} else if (s->action == ODI_SW_PROBE_TRAP) {
		/* No port: another implementation measured the trap with an
		 * empty mask taking frames to the CPU. */
		act[1] |= PROBE_ACT_FWD_TRAP << 9;
		*en |= ODI_SW_ACL_ACT_EN_FWD;
	}
}

static void probe_entry_clear(uint32_t entry)
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

/* The last rule gone: the template and the counter modes as found. */
static void probe_all_gone(void)
{
	odi_sw_probe_on = 0;
	probe_template_restore();
	log_type = 0;
	odi_reg_write(ODI_SW_LOG_TYPE_OFF, log_type);
}

static int probe_install(uint32_t n, struct probe_slot *s)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	uint32_t ones[ODI_SW_MC_RULE_WORDS], zeros[ODI_SW_MC_RULE_WORDS];
	uint32_t act[ODI_SW_MC_ACTION_WORDS], en, permit, port_en;
	uint32_t entry = ODI_SW_PROBE_ENTRY_BASE + n, counter = probe_counter(n);
	uint32_t pbit = 1U << s->port;
	int rc;

	probe_planes(s, ones, zeros);
	probe_action(s, counter, act, &en);

	if (!nused) {
		if (odi_sw_probe_rx_enable)
			odi_sw_probe_rx_enable();
		uni_igmp_other = 0;
		other_reason = 0;
		probe_template_set();
	}
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
	if (s->bytes)
		log_type |= 1U << (counter / 2U);
	else
		log_type &= ~(1U << (counter / 2U));
	odi_reg_write(ODI_SW_LOG_TYPE_OFF, log_type);

	rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, zero, ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, entry, zeros,
					    ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, entry, act,
					    ODI_SW_MC_ACTION_WORDS);
	if (!rc) {
		odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * entry, en);
		/* The counter is never cleared: what the rule counts is the
		 * difference from here, the last moment before it can match. */
		s->base = odi_reg_read(ODI_SW_LOG_CNT_OFF + 4U * counter);
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, ones,
					    ODI_SW_MC_RULE_WORDS);
	}
	if (rc) {
		probe_entry_clear(entry);
		log_type &= ~(1U << (counter / 2U));
		if (nused)
			odi_reg_write(ODI_SW_LOG_TYPE_OFF, log_type);
		else
			probe_all_gone();
		return rc;
	}
	s->cpu_hits = 0;
	s->cpu_ports = 0;
	s->cpu_bytes = 0;
	s->used = 1;
	nused++;
	odi_sw_probe_on = 1;
	return 0;
}

static void probe_remove(uint32_t n)
{
	if (!slots[n].used)
		return;
	probe_entry_clear(ODI_SW_PROBE_ENTRY_BASE + n);
	slots[n].used = 0;
	if (--nused == 0)
		probe_all_gone();
}

static int probe_check(const struct odi_sw_acl_probe *r)
{
	if (r->port != ODI_SW_PROBE_PORT_UNI && r->port != ODI_SW_PROBE_PORT_PON)
		return PROBE_EINVAL;
	if (r->bytes > 1U || r->proto > ODI_SW_PROBE_ANY)
		return PROBE_EINVAL;
	if (r->action == ODI_SW_PROBE_COUNT)
		return 0;
	if (r->action != ODI_SW_PROBE_MIRROR && r->action != ODI_SW_PROBE_TRAP)
		return PROBE_EINVAL;
	/* To the CPU: IGMP from the UNI, nothing else. */
	if (r->port != ODI_SW_PROBE_PORT_UNI || r->proto != PROBE_PROTO_IGMP)
		return PROBE_EINVAL;
	return 0;
}

static void probe_fill(uint32_t n, struct odi_sw_acl_probe *r)
{
	const struct probe_slot *s = &slots[n];

	r->in_use = s->used;
	r->entry = ODI_SW_PROBE_ENTRY_BASE + n;
	r->counter = probe_counter(n);
	r->uni_igmp_other = uni_igmp_other;
	r->other_reason = other_reason;
	if (!s->used)
		return;
	r->port = s->port;
	r->action = s->action;
	r->bytes = s->bytes;
	r->proto = s->proto;
	r->dst = s->dst;
	r->dst_mask = s->dst_mask;
	r->base = s->base;
	r->now = odi_reg_read(ODI_SW_LOG_CNT_OFF + 4U * r->counter);
	r->cpu_hits = s->cpu_hits;
	r->cpu_ports = s->cpu_ports;
	r->cpu_bytes = s->cpu_bytes;
}

int odi_sw_probe_request(struct odi_sw_acl_probe *r)
{
	struct probe_slot *s;
	uint32_t n;
	int rc;

	if (r->op == ODI_SW_PROBE_DEL && r->slot == ODI_SW_PROBE_ALL) {
		odi_sw_probe_release();
		return 0;
	}
	if (r->slot >= ODI_SW_PROBE_SLOTS)
		return PROBE_EINVAL;
	n = r->slot;
	s = &slots[n];

	switch (r->op) {
	case ODI_SW_PROBE_GET:
		probe_fill(n, r);
		return 0;
	case ODI_SW_PROBE_DEL:
		probe_remove(n);
		return 0;
	case ODI_SW_PROBE_SET:
		break;
	default:
		return PROBE_EINVAL;
	}

	rc = probe_check(r);
	if (rc)
		return rc;
	if (s->used || odi_switch_mcast_flags())
		return PROBE_EBUSY;
	s->port = r->port;
	s->action = r->action;
	s->bytes = r->bytes;
	s->proto = r->proto;
	s->dst_mask = r->dst_mask;
	s->dst = r->dst & r->dst_mask;
	rc = probe_install(n, s);
	if (rc)
		return rc;
	probe_fill(n, r);
	return 0;
}

int odi_sw_probe_active(void)
{
	return nused != 0;
}

void odi_sw_probe_release(void)
{
	uint32_t n;

	for (n = 0; n < ODI_SW_PROBE_SLOTS; n++)
		probe_remove(n);
}

void odi_sw_probe_rx(uint32_t reason, uint32_t src_port, uint32_t len, int igmp)
{
	uint32_t n = reason - (ODI_SW_PROBE_REASON_ACL0 + ODI_SW_PROBE_ENTRY_BASE);

	/* A count that races a SET or a DEL of the same slot may be lost or
	 * land in the old rule's total; a measurement over seconds does not
	 * notice. */
	if (n < ODI_SW_PROBE_SLOTS && slots[n].used) {
		slots[n].cpu_hits++;
		slots[n].cpu_ports |= 1U << (src_port & 0x1fU);
		slots[n].cpu_bytes += len;
		return;
	}
	if (src_port == ODI_SW_PROBE_PORT_UNI && igmp) {
		uni_igmp_other++;
		other_reason = reason;
	}
}
