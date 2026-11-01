/* The upstream and downstream QoS: priority queues, T-CONTs, the GEM flow
 * tables and flow ids, the reconciler that rebuilds the upstream graph
 * (us_qos_rebuild()), the downstream broadcast flow, and the class 277 and
 * class 268 handlers. See omcid.h.
 */
#include "omcid.h"
#include "../procparse.h"

/* ------------------------------------------------------- priority queues
 *
 * Six words of the GEM flow descriptor, and the whole of driver command 23,
 * come from three attributes of one class 277 row: RelatedPort, Weight and
 * DropPrecedenceColourMarking. RelatedPort is two values in one word: the
 * high half names the managed entity the queue hangs off, the low half the
 * priority within it. Word 8 of the flow descriptor is a boolean: weighted
 * round robin.
 *
 * Two priorities leave here, and they are not the same number. The flow
 * descriptor's ds_prio is the stock one, the bottom three bits reversed
 * against the per-UNI queue count (OMCI priority 0 is 7); the switch driver
 * does not read it. Driver command 23 carries the priority as the OLT sees
 * it, RelatedPort & 0xffff, and the driver takes that as the number of the
 * UNI's hardware queue, the one served first being the highest: the queues
 * an OLT gives weight 0 (strict) are the ones it names with the high
 * priority numbers (ISP1 and claro: entities 0 and 1, priorities 7 and 6),
 * which only makes sense if a larger number is served first.
 */
static uint32_t caps_uni_queues(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_UNIQ);

	return n ? n : 8;                /* the driver block says 8 per UNI */
}

/* How many T-CONT slots the device has; a driver index at or above this is
 * not a T-CONT. The driver block reports 16 (uapi/omci_caps.h); 8 is only
 * the answer when no capability block is available at all. */
static uint32_t tcont_capacity(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_TCONTS);

	return n ? n : 8;                /* no block: the conservative count */
}

/* The k-th UNI managed entity: PPTP Ethernet UNIs first, then VEIPs, an
 * order fitted to the one observation of ISP1 (one of each). UNIs are
 * ONU-created, so omci_autonomous[] is searched before the store, which
 * holds a row only if the OLT set one. */
static uint16_t nth_uni_entity(unsigned k)
{
	for (unsigned cls = 0; cls < 2; cls++) {
		uint16_t want = cls ? 329 : 11;

		for (unsigned i = 0; i < aut_count; i++)
			if (aut[i].classId == want) {
				if (!k)
					return aut[i].inst;
				k--;
			}
		for (int i = 0; i < MIB_ROWS; i++)
			if (mib[i].used && mib[i].classId == want
			    && !instance_is_autonomous(want, mib[i].inst)) {
				if (!k)
					return mib[i].inst;
				k--;
			}
	}
	return 0;
}

/* RelatedPort of an ONU-created priority queue, derived rather than
 * stored: the store holds a class 277 row only if the OLT set one. Matches
 * all 208 rows of ISP1 (omcicli mib get 277):
 *
 *   0x0000..0x000f   downstream, 8 per UNI.  high = entity id of the UNI
 *   0x8000..0x807f   upstream,   8 per T-CONT. high = 0x8000 + tcont
 *   0xff00..0xff3f   reserved,   8 per T-CONT. high = 0x8000 + tcont
 *
 * Within every block the priority counts DOWN with the entity id (7..0,
 * 15..8 for the reserved one), so the lowest entity id of a block is its
 * highest priority. Returns 0 for an id in none of the three ranges, which
 * takes the "not a UNI" path.
 */
uint32_t priq_related_port(uint16_t meId)
{
	if (meId < 0x10)
		return ((uint32_t)nth_uni_entity(meId >> 3) << 16)
		     | (7u - (meId & 7));
	if (meId >= 0x8000 && meId <= 0x807f)
		return ((uint32_t)(0x8000 + ((meId - 0x8000) >> 3)) << 16)
		     | (7u - (meId & 7));
	if (meId >= 0xff00 && meId <= 0xff3f)
		return ((uint32_t)(0x8000 + ((meId - 0xff00) >> 3)) << 16)
		     | (15u - (meId & 7));
	return 0;
}

/* The stock queue table starts every queue at QCfgOpt 1, Weight 1,
 * PktDropMaxP 255, QueueDropWQ 9, zero elsewhere. On ISP1 the upstream
 * weights run 4, 9, 14, 19, 24, 30, 0, 0 (PPTP downstream the mirror; VEIP
 * and reserved queues 1): that is provisioning, so it is not guessed here. */

/* RelatedPort, Weight and DropPrecedenceColourMarking of a downstream
 * queue as the OLT left them. attr_value() answers what the OLT wrote and,
 * for an attribute it did not, the stock default: RelatedPort derived from
 * the entity id, Weight 1, no marking. A row the OLT touched only to set
 * the weight therefore still has its RelatedPort; reading the row raw gave
 * 0 there, and every one of ISP1's eight queues went to the driver as
 * priority 0. Returns 0 when the entity id is in no queue range and the OLT
 * set no RelatedPort: nothing to send. */
static int priq_attrs(uint16_t meId, uint32_t *rp, uint32_t *weight,
		      uint32_t *dp)
{
	const struct omci_class *qc = find_class(OMCI_ME_PRIORITY_QUEUE);
	uint8_t b[4];
	uint16_t n;

	if (!qc)
		return 0;
	*rp = 0;
	n = attr_value(qc, meId, 6, b);      /* RelatedPort */
	for (uint16_t i = 0; i < n && i < 4; i++)
		*rp = (*rp << 8) | b[i];
	if (!*rp)
		return 0;
	*weight = 0;
	n = attr_value(qc, meId, 8, b);      /* Weight */
	for (uint16_t i = 0; i < n && i < 4; i++)
		*weight = (*weight << 8) | b[i];
	*dp = 0;
	n = attr_value(qc, meId, 16, b);     /* DropPrecedenceColourMarking */
	for (uint16_t i = 0; i < n && i < 4; i++)
		*dp = (*dp << 8) | b[i];
	return 1;
}

static void ds_queue_words(struct omci_gemflow *f, uint16_t priqMeId)
{
	uint32_t rp, weight, dp;
	int port;

	if (!priq_attrs(priqMeId, &rp, &weight, &dp))
		return;
	f->ds_dp_mark = dp;
	port   = uni_switch_port((uint16_t)(rp >> 16), OMCI_UNI_SLOT_PPTP);

	f->ds_pq_pri   = rp & 0xffff;
	/* The stock stack falls back to the PON port when the related entity is not
	 * a PPTP Ethernet UNI -- a VEIP queue, or an ANI-side one. */
	f->ds_port      = (port >= 0) ? (uint32_t)port
					: caps_u32(OMCI_CAPS_OFF_PONPORT);
	f->ds_wrr = (weight >= 2);
	/* Clamped: the queue count comes from the capability blob, and -c
	 * accepts any blob. On uint32_t a count below 8 would underflow an
	 * OMCI priority above n-1 to ~0xfffffffc and send that to the driver. */
	{
		uint32_t nq = caps_uni_queues(), pri = rp & 7;

		f->ds_prio = (pri + 1 >= nq) ? 0 : (nq - 1) - pri;
	}
	f->ds_weight      = weight;
}

/* Driver command 23 for a downstream queue. An ME id with bit 15 set is an
 * upstream queue; that path is programmed from the GEM CTP below, after its
 * driver T-CONT index is known.
 *
 * Returns 0 when there was nothing to send, so a caller can tell "no queue
 * provisioned" from "the driver refused". */
static int priq_program(uint16_t priqMeId, struct omci_priq *out)
{
	uint32_t rp, weight, dp;
	int port;

	if (priqMeId & 0x8000)
		return 0;
	if (!priq_attrs(priqMeId, &rp, &weight, &dp))
		return 0;
	for (unsigned i = 0; i < sizeof *out / 4; i++)
		((uint32_t *)out)[i] = 0;
	port   = uni_switch_port((uint16_t)(rp >> 16), OMCI_UNI_SLOT_PPTP);

	out->index     = (uint16_t)(rp & 0xffff);
	/* A queue of a VEIP, or of anything that is not a PPTP Ethernet UNI,
	 * names the PON port, as its flow does: the driver schedules the
	 * UNI's queues only, and a VEIP queue left on port 0 would overwrite
	 * the PPTP queue of the same priority. */
	out->owner     = (uint16_t)(port >= 0 ? (uint32_t)port
					       : caps_u32(OMCI_CAPS_OFF_PONPORT));
	out->weight    = (uint16_t)(weight < 1 ? 1 : weight);
	out->wrr     = (weight >= 2);
	out->dp_mark = (uint8_t)dp;
	out->dir       = OMCI_GEMFLOW_DS;
	return 1;
}

#define USQ_MAX 128

struct us_queue {
	uint16_t meId;
	uint16_t tcontMe;
	uint16_t tcontIndex;
	uint16_t ordinal;
	uint16_t priority;
	uint16_t weight;
	uint8_t wrr;
	uint8_t dp_mark;
	uint32_t cir;                    /* driver units (8 kbit/s), 0: none */
	uint32_t pir;
};

struct us_flow_plan {
	uint16_t ctp;
	uint16_t port;
	uint16_t tcontMe;
	uint16_t pqMe;
	uint16_t flow_id;
};

/* What the driver holds upstream, as the last rebuild left it: the queues
 * cmd 23 programmed, the T-CONTs cmd 21 answered for, and per flow slot the
 * GEM port and the T-CONT index and ordinal cmd 25 created it on. The next
 * rebuild changes only what differs from this (us_qos_rebuild()), and a
 * resume snapshot keeps it (snapshot.c), so a respawn does not forget what
 * is in the hardware. */
struct us_tcont_prog {
	uint16_t meId;
	uint16_t alloc;
	uint16_t index;
	uint8_t nq;
	uint8_t used;
};

struct us_flow_prog {
	uint16_t gem;
	uint16_t tcontMe;
	uint16_t tcont;
	uint16_t queue;
	uint8_t live;
};

static struct {
	struct us_queue q[USQ_MAX];
	uint32_t n;
	struct us_tcont_prog t[TCONT_MAX];
	struct us_flow_prog f[FLOW_MAX];
} prog;
#define usq_hw   prog.q
#define usq_hw_n prog.n

int qos_dirty;
int qos_full;

void *qos_prog_state(uint32_t *len)
{
	*len = (uint32_t)sizeof prog;
	return &prog;
}

/* `omcicli dump qmap`, in the column shape of the stock dump: one row per
 * upstream queue programmed. QID is the dense ordinal, TCID the driver
 * T-CONT index; CIR/PIR are the rates of its GEM ports' upstream traffic
 * descriptor (us_queue_rates()), 0 without one, as on ISP2. */
void qmap_dump(void)
{
	out("\n\n============= PQ ===============\n"
	    "   MEID\t   QID\tPolicy\t   P/W\t  TCID\t    DP\t Queue\t"
	    "CIR(8K)\tPIR(8K)\tUsedCnt\n"
	    "-------\t------\t------\t------\t------\t------\t------\t"
	    "-------\t-------\t-------\n");
	for (unsigned i = 0; i < usq_hw_n; i++) {
		const struct us_queue *q = &usq_hw[i];

		out_fmt(" 0x%04x\t%6d\t%6s\t%6d\t%6d\t%6d\t0x%04x\t%7d\t%7d\t%7d\n",
			(long)q->meId, (long)q->ordinal, q->wrr ? "WRR" : "SP",
			(long)(q->wrr ? q->weight : q->priority),
			(long)q->tcontIndex, (long)q->dp_mark, (long)q->tcontMe,
			(long)q->cir, (long)q->pir, 1L);
		out("-------\t------\t------\t------\t------\t------\t------\t"
		    "-------\t-------\t-------\n");
	}
}


static uint32_t effective_u32(uint16_t cls, uint16_t inst, unsigned attr)
{
	const struct omci_class *c = find_class(cls);
	uint8_t b[8];
	uint16_t n;
	uint32_t v = 0;

	if (!c)
		return 0;
	for (unsigned i = 0; i < sizeof b; i++)
		b[i] = 0;
	n = attr_value(c, inst, attr, b);
	if (n > 4)
		n = 4;
	for (unsigned i = 0; i < n; i++)
		v = (v << 8) | b[i];
	return v;
}

static int us_queue_decode(uint16_t meId, struct us_queue *q)
{
	uint32_t rp, scheduler;

	if (meId < 0x8000 || meId > 0x807f)
		return 0;
	rp = effective_u32(277, meId, 6);
	scheduler = effective_u32(277, meId, 7);
	q->meId = meId;
	q->tcontMe = (uint16_t)(scheduler >= 0x8000 ? scheduler : rp >> 16);
	q->wrr = (uint8_t)(scheduler >= 0x8000
		? effective_u32(278, (uint16_t)scheduler, 3) == 2
		: effective_u32(262, q->tcontMe, 3) == 2);
	q->priority = (uint16_t)rp;
	q->weight = (uint16_t)effective_u32(277, meId, 8);
	q->dp_mark = (uint8_t)effective_u32(277, meId, 16);
	q->tcontIndex = 0xffff;
	q->ordinal = 0xffff;
	q->cir = 0;
	q->pir = 0;
	return q->tcontMe >= 0x8000;
}

/* The upstream rates of a queue: the traffic descriptor (class 280) its GEM
 * port CTPs name as their upstream one (class 268 attribute 5, G.988
 * 9.2.3), applied as the stock stack applies it: CIR and PIR in byte/s,
 * PIR less CIR for a meter of type 1 (RFC 4115, where the second rate is
 * the excess), each divided by 1024 into the driver's 8 kbit/s units. Our
 * ONU-G reports traffic management option 2 (priority and rate
 * controlled), which tells the OLT it may provision these. A queue
 * without one keeps 0 and 0, which the driver takes as no limit; when
 * several CTPs on one queue name different descriptors, the last CTP of
 * the store wins, as the last flow set does in the stock stack. */
static void us_queue_rates(struct us_queue *q, unsigned nq,
			   const struct us_flow_plan *flows, unsigned nf)
{
	for (unsigned i = 0; i < nf; i++) {
		uint16_t td = (uint16_t)effective_u32(OMCI_ME_GEM_PORT_CTP,
						      flows[i].ctp, 5);
		uint32_t cir, pir;

		if (!td || !mib_find(OMCI_ME_TRAFFIC_DESCRIPTOR, td))
			continue;
		cir = effective_u32(OMCI_ME_TRAFFIC_DESCRIPTOR, td, 1);
		pir = effective_u32(OMCI_ME_TRAFFIC_DESCRIPTOR, td, 2);
		if (effective_u32(OMCI_ME_TRAFFIC_DESCRIPTOR, td, 8) == 1)
			pir = pir > cir ? pir - cir : 0;
		for (unsigned j = 0; j < nq; j++)
			if (q[j].meId == flows[i].pqMe) {
				q[j].cir = cir / 1024u;
				q[j].pir = pir / 1024u;
			}
	}
}

/* Does an upstream GEM port CTP name this class-277 queue? Only such a
 * queue is in the plan, so only its Set dirties it: ISP1 Sets 72 queues,
 * and each would otherwise rerun the delete-and-recreate transaction. */
int us_queue_referenced(uint16_t pqMe)
{
	const struct omci_class *ctp = find_class(OMCI_ME_GEM_PORT_CTP);

	if (!ctp)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t dir;

		if (!r || r->classId != OMCI_ME_GEM_PORT_CTP)
			continue;
		dir = row_u32(r, ctp, 3);
		if ((dir == 1 || dir == 3) && row_u32(r, ctp, 4) == pqMe)
			return 1;
	}
	return 0;
}

int us_scheduler_referenced(uint16_t ts)
{
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i);

		if (!r || r->classId != OMCI_ME_PRIORITY_QUEUE)
			continue;
		if (effective_u32(OMCI_ME_PRIORITY_QUEUE, r->inst, 7) == ts &&
		    us_queue_referenced(r->inst))
			return 1;
	}
	return 0;
}

int us_td_referenced(uint16_t td)
{
	const struct omci_class *ctp = find_class(OMCI_ME_GEM_PORT_CTP);

	if (!ctp || !td)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t dir;

		if (!r || r->classId != OMCI_ME_GEM_PORT_CTP)
			continue;
		dir = row_u32(r, ctp, 3);
		if ((dir == 1 || dir == 3) && row_u32(r, ctp, 5) == td)
			return 1;
	}
	return 0;
}

static int us_queue_before(const struct us_queue *a, const struct us_queue *b)
{
	if (a->tcontMe != b->tcontMe)
		return a->tcontMe < b->tcontMe;
	if (a->wrr != b->wrr)
		return a->wrr > b->wrr;
	if (!a->wrr && a->priority != b->priority)
		return a->priority > b->priority;
	return a->meId < b->meId;
}

static int us_queue_add(struct us_queue *q, unsigned *n, uint16_t meId)
{
	for (unsigned i = 0; i < *n; i++)
		if (q[i].meId == meId)
			return (int)i;
	if (*n >= USQ_MAX || !us_queue_decode(meId, &q[*n]))
		return -1;
	return (int)(*n)++;
}
struct flow_slot flow_us[FLOW_MAX], flow_ds[FLOW_MAX];

static int flow_alloc(int ds, uint16_t port)
{
	struct flow_slot *t = ds ? flow_ds : flow_us;
	uint32_t n = caps_flows();

	for (uint32_t i = 0; i < n; i++)
		if (t[i].used && t[i].port == port)
			return (int)i;           /* this port already has one */
	for (uint32_t i = 0; i < n; i++)
		if (!t[i].used)
			return (int)i;
	return -1;
}

static void flow_claim(int ds, int id, uint16_t port)
{
	struct flow_slot *t = ds ? flow_ds : flow_us;

	if (id >= 0 && id < FLOW_MAX) {
		t[id].used = 1;
		t[id].port = port;
	}
}

int flow_find(int ds, uint16_t port)
{
	struct flow_slot *t = ds ? flow_ds : flow_us;

	for (uint32_t i = 0; i < caps_flows(); i++)
		if (t[i].used && t[i].port == port)
			return (int)i;
	return -1;
}

/* ------------------------------------------------------- the broadcast port
 *
 * The downstream broadcast GEM port is not configured by cmd 25 -- the
 * driver refuses the reserved port id there. It is designated separately,
 * by flow id, with setDsBcGemFlow, and the stock stack works out which flow
 * from class 47:
 *
 *     a MAC bridge port whose TP is a GEM interworking TP
 *       -> that GemIwTp's interworking option is 6
 *       -> follow its GEM CTP pointer to the GEM port network CTP
 *       -> take its Port-ID, look up the downstream flow that carries it
 *       -> setDsBcGemFlow(that flow id)
 *
 * Interworking option 6 is the stock numbering, not G.988's. The lookup is
 * repeated whenever any of the three entities changes, because the OLT
 * creates them in any order; the driver is told only when the answer moves.
 */
int bc_flow = -1;

void bc_gem_update(void)
{
	const struct omci_class *iw = find_class(OMCI_ME_GEM_IW_TP);
	const struct omci_class *miw = find_class(OMCI_ME_MCAST_GEM_IW_TP);
	const struct omci_class *ctp = find_class(OMCI_ME_GEM_PORT_CTP);
	const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);

	/* Gated on the driver call, not on the search: the dry run has to say
	 * which flow WOULD be programmed, the same way the flow and T-CONT
	 * tables are kept either way. */
	if (!iw || !ctp || !bp)
		return;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *iwr, *ctpr;
		uint32_t tpType;
		uint16_t port;
		int id;

		if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		tpType = row_u32(&mib[i], bp, 3);
		if (tpType != 6) {
			iwr = mib_find(OMCI_ME_GEM_IW_TP, (uint16_t)row_u32(&mib[i], bp, 4));
			if (!iwr || row_u32(iwr, iw, 2) != 6)
				continue;
			ctpr = mib_find(OMCI_ME_GEM_PORT_CTP, (uint16_t)row_u32(iwr, iw, 1));
		} else {
			if (!miw)
				continue;
			iwr = mib_find(OMCI_ME_MCAST_GEM_IW_TP, (uint16_t)row_u32(&mib[i], bp, 4));
			ctpr = iwr ? mib_find(OMCI_ME_GEM_PORT_CTP, (uint16_t)row_u32(iwr, miw, 1)) : 0;
		}
		if (!ctpr)
			continue;
		port = (uint16_t)row_u32(ctpr, ctp, 1);
		id = flow_find(1, port);
		if (id < 0 || id == bc_flow)
			continue;
		{
			int rc = apply_hw ? omci_setDsBcGemFlow((uint32_t)id) : 0;

			out_fmt("   [%s] broadcast gem %d is ds flow %d -> %d\n",
				apply_hw ? "hw" : "dry", (long)port, (long)id,
				(long)rc);
			if (rc == 0)
				bc_flow = id;
			/* A rebuild may already have skipped this bridge port while
			 * its downstream flow did not exist. Retry now that it does. */
			if (rc == 0)
				conn_dirty = 1;
		}
		return;
	}
}

/* The other half: a class 47 delete. If no OTHER class 47 row has a
 * downstream-broadcast GEM interworking TP, the flow is withdrawn with
 * 0xffffffff. `going` is the entity being deleted; its row is still in
 * the store, so it is skipped explicitly.
 */
void bc_gem_withdraw(uint16_t going)
{
	const struct omci_class *iw = find_class(OMCI_ME_GEM_IW_TP);
	const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);

	if (!iw || !bp || bc_flow < 0)
		return;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *iwr;

		if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		if (mib[i].inst == going)
			continue;
		if (row_u32(&mib[i], bp, 3) != MBPCD_TP_GEM_IWTP)
			continue;
		iwr = mib_find(OMCI_ME_GEM_IW_TP, (uint16_t)row_u32(&mib[i], bp, 4));
		if (iwr && row_u32(iwr, iw, 2) == 6)
			return;                  /* another broadcast port */
	}
	out_fmt("   [%s 47/%04x] last broadcast bridge port -> withdraw ds flow "
		"%d\n", apply_hw ? "hw" : "dry", (long)going, (long)bc_flow);
	if (!apply_hw || omci_setDsBcGemFlow(OMCI_BC_FLOW_NONE) == 0)
		bc_flow = -1;
}
struct tcont_slot tcont_map[TCONT_MAX];

/* How many T-CONTs are mapped. Used only by the dry run, to stand in for the
 * index the driver would have handed back. */
static uint16_t tcont_count(void)
{
	uint16_t n = 0;

	for (int i = 0; i < TCONT_MAX; i++)
		if (tcont_map[i].used)
			n++;
	return n;
}

static void tcont_remember(uint16_t meId, uint16_t index)
{
	for (int i = 0; i < TCONT_MAX; i++)
		if (tcont_map[i].used && tcont_map[i].meId == meId) {
			tcont_map[i].index = index;
			return;
		}
	for (int i = 0; i < TCONT_MAX; i++)
		if (!tcont_map[i].used) {
			tcont_map[i].meId = meId;
			tcont_map[i].index = index;
			tcont_map[i].used = 1;
			return;
		}
}

/* A T-CONT the driver refused (its Alloc-ID not assigned by PLOAM any
 * more, or not yet) has no index: kept, omcli tcont and the snapshot
 * would still claim the old one. */
static void tcont_forget(uint16_t meId)
{
	for (int i = 0; i < TCONT_MAX; i++)
		if (tcont_map[i].used && tcont_map[i].meId == meId)
			tcont_map[i].used = 0;
}

static uint32_t tcont_index(uint16_t meId)
{
	for (int i = 0; i < TCONT_MAX; i++)
		if (tcont_map[i].used && tcont_map[i].meId == meId)
			return tcont_map[i].index;
	return 0xffff;                   /* no T-CONT of that name */
}
/* ------------------------------------------------- the T-CONT Alloc-IDs
 *
 * G.988 9.2.2: T-CONT attribute 1 links the entity to an Alloc-ID the OLT
 * assigned by PLOAM (Assign_Alloc-ID, G.984.3 9.2.3.9), and the OLT sets
 * it; until then it reads 0x00FF, unassigned. ISP2 sets it. ISP1 never
 * does: it assigns five Alloc-IDs by PLOAM and points its GEM ports at
 * T-CONTs 0x8000..0x8004 it never wrote, and the stock stack serves that by
 * binding the PLOAM Alloc-IDs to its T-CONT entities itself, in the order
 * they were assigned (its MIB on ISP1: 0x8000..0x8004 hold the five in
 * PLOAM order, the other eleven 0x00FF). This does the same, from the list
 * the kernel keeps (/proc/odi_gpon/status, alloc_ids), so the Alloc-IDs are this
 * OLT's own rather than ones read off another line:
 *
 *   - a T-CONT the OLT set keeps what it set, 0x00FF included;
 *   - every other T-CONT, in entity-id order, takes the next assigned
 *     Alloc-ID that no set T-CONT claims, in assignment order;
 *   - the rest read 0x00FF, and tcont_apply() refuses them.
 *
 * The driver index of a T-CONT is the CAM row of its Alloc-ID, whatever the
 * order (odi_switch_cmd.c, cmd 21), so nothing here depends on positions
 * lining up. */
#define ALLOC_ID_UNASSIGNED 0x00ffu

const char *gpon_proc_path = "/proc/odi_gpon/status";
static uint16_t ploam_alloc[TCONT_MAX];
static int ploam_alloc_n = -1;           /* -1: not read yet, or unreadable */
static int ploam_alloc_readable;         /* the last read found the line */

/* Read the kernel's list again. Returns 1 when it changed since the last
 * read, 0 otherwise; the first read is no change, since whatever was
 * programmed before it -- by a resumed snapshot -- was built from the same
 * list. An unreadable file or line (no kernel of ours, a truncated read)
 * counts as no Alloc-IDs: nothing to bind to. */
int alloc_ids_refresh(void)
{
	char buf[1024];
	uint16_t ids[TCONT_MAX];
	long fd, n;
	int got = -1, changed, readable, first = ploam_alloc_n < 0;

	fd = sys_open(gpon_proc_path, O_RDONLY);
	if (fd >= 0) {
		/* One read: the line is the sixth of the file, well inside the
		 * first kilobyte, and a seq_file read never blocks. */
		n = sys_read((int)fd, buf, sizeof buf);
		sys_close((int)fd);
		if (n > 0)
			got = pp_gpon_alloc_ids(buf, (unsigned)n, ids, TCONT_MAX);
	}
	readable = got >= 0;
	ploam_alloc_readable = readable;
	if (got < 0)
		got = 0;
	changed = ploam_alloc_n != got;
	for (int i = 0; i < got; i++)
		if (changed || ploam_alloc[i] != ids[i]) {
			changed = 1;
			ploam_alloc[i] = ids[i];
		}
	ploam_alloc_n = got;
	/* No line at all (qemu, a stock kernel) is not an event. */
	if (changed && readable)
		ev_alloc_ids(ploam_alloc, (unsigned)got);
	return changed && !first;
}

/* Whether alloc_ids_assigned() is a list the kernel printed (1), or no
 * list at all (0: no kernel of ours, as under qemu). */
int alloc_ids_known(void)
{
	if (ploam_alloc_n < 0)
		alloc_ids_refresh();
	return ploam_alloc_readable;
}

unsigned alloc_ids_assigned(const uint16_t **ids)
{
	if (ploam_alloc_n < 0)
		alloc_ids_refresh();
	*ids = ploam_alloc;
	return (unsigned)ploam_alloc_n;
}

/* The Alloc-ID the OLT wrote into T-CONT `meId`, or -1 if it wrote none. */
static int tcont_olt_alloc(uint16_t meId)
{
	const struct omci_class *c = find_class(OMCI_ME_TCONT);
	struct mib_row *r = mib_find(OMCI_ME_TCONT, meId);

	if (!c || !r || !(r->written & (1u << (16 - 1))))
		return -1;
	return (int)row_u32(r, c, 1);
}

/* The Alloc-ID T-CONT `meId` is bound to in the datapath: what the OLT set,
 * else the PLOAM assignment in order, else 0x00FF. Whatever
 * OMCI_TCONT_ALLOC_UNSET says: that key changes only what is reported. */
uint16_t tcont_bound_alloc_id(uint16_t meId, int *src)
{
	const uint16_t *ids;
	unsigned n = alloc_ids_assigned(&ids), nt = 0;
	uint16_t inst[TCONT_MAX];
	int set[TCONT_MAX], pos = -1, k = 0;

	if (src)
		*src = TCONT_ALLOC_NONE;
	/* The T-CONTs are ONU-created (omci_autonomous[], in entity-id
	 * order), sixteen here: what each was set to, gathered once, since a
	 * MIB upload asks this for every one of them. An id that is not
	 * among them is no T-CONT of this ONU. */
	for (unsigned i = 0; i < aut_count && nt < TCONT_MAX; i++)
		if (aut[i].classId == OMCI_ME_TCONT) {
			inst[nt] = aut[i].inst;
			set[nt] = tcont_olt_alloc(inst[nt]);
			nt++;
		}
	for (unsigned t = 0; t < nt; t++) {
		if (inst[t] == meId) {
			if (set[t] >= 0) {
				if (src)
					*src = TCONT_ALLOC_OLT;
				return (uint16_t)set[t];
			}
			pos = k;
			break;
		}
		if (set[t] < 0)
			k++;
	}
	if (pos < 0)
		return ALLOC_ID_UNASSIGNED;
	for (unsigned i = 0; i < n; i++) {
		int claimed = 0;

		for (unsigned t = 0; t < nt && !claimed; t++)
			claimed = set[t] == (int)ids[i];
		if (claimed)
			continue;
		if (pos-- == 0) {
			if (src)
				*src = TCONT_ALLOC_PLOAM;
			return ids[i];
		}
	}
	return ALLOC_ID_UNASSIGNED;
}

/* OMCI_TCONT_ALLOC_UNSET in CFG_ODI_PATH: "unassigned" makes a T-CONT the
 * OLT never Set report 0x00FF; anything else, or absent, is "ploam", the
 * PLOAM-bound value. Read at every call, so a change is live. */
int tcont_alloc_unset_mode(void)
{
	static const char want[] = "unassigned";
	char v[16];

	if (cfg_odi_get(CFG_ODI_PATH, "OMCI_TCONT_ALLOC_UNSET", v, sizeof v) !=
	    (int)sizeof want - 1)
		return 0;
	for (unsigned i = 0; i < sizeof want; i++)
		if (v[i] != want[i])
			return 0;
	return 1;
}

/* The Alloc-ID T-CONT `meId` reports (Get, MIB upload, show): the bound one,
 * except that in "unassigned" mode a T-CONT the OLT never Set reads 0x00FF
 * (G.988 9.2.2 initial value) although the datapath keeps its PLOAM binding. */
uint16_t tcont_alloc_id(uint16_t meId, int *src)
{
	int s;
	uint16_t a = tcont_bound_alloc_id(meId, &s);

	if (s == TCONT_ALLOC_PLOAM && tcont_alloc_unset_mode()) {
		s = TCONT_ALLOC_NONE;
		a = ALLOC_ID_UNASSIGNED;
	}
	if (src)
		*src = s;
	return a;
}

/* Program one T-CONT: from a Set of its Alloc-ID, or on demand when a GEM
 * port CTP names it and the OLT never set it (the row already carries the
 * value). The driver hands back the index, the CAM row of the Alloc-ID, and
 * refuses an Alloc-ID PLOAM has not assigned in this activation, whatever
 * the OLT set over OMCI (odi_switch_cmd.c cmd 21); the T-CONT is then left
 * unbound until the kernel list changes. The dry run uses allocation
 * order. */
static uint16_t tcont_apply(uint16_t inst, uint32_t alloc_id, uint16_t nqueues)
{
	struct omci_tcont t;
	long rc;

	/* Word 0 goes in as the number of queues to reserve (the active
	 * queues in the plan) on a T-CONT the OLT assigned by PLOAM, and comes
	 * back as the driver index. The stock driver only fills it in. */
	t.index = nqueues;
	t.alloc_id = alloc_id;
	if (t.alloc_id >= 4096 || t.alloc_id == 0xff)
		return 0xffff;           /* what the stock stack refuses */
	if (apply_hw) {
			rc = omci_drv_call(OMCI_TCONT_CMD, &t, sizeof t);
			/* rc 0 with an index at or past the T-CONT count (ISP1: 8)
			 * is not a T-CONT: index 8 comes back when the queue pool
			 * is exhausted, the scheduler 8 / queue 31 sentinel. */
			if (rc == 0 && t.index >= tcont_capacity()) {
				out_fmt("   [hw] tcont me %04x alloc %d queues %d -> "
					"index %d out of range (max %d)\n",
					(long)inst, (long)t.alloc_id, (long)nqueues,
					(long)t.index, (long)tcont_capacity());
				tcont_forget(inst);
				return 0xffff;
			}
			if (rc == 0)
				tcont_remember(inst, (uint16_t)t.index);
			out_fmt("   [hw] tcont me %04x alloc %d queues %d -> "
				"index %d rc %d\n", (long)inst, (long)t.alloc_id,
				(long)nqueues, (long)t.index, (long)rc);
			if (rc != 0) {
				tcont_forget(inst);
				return 0xffff;
			}
		} else {
			/* Allocation order, which is what the driver produces for
			 * a fresh table. An ME already mapped keeps its index: a
			 * repeated Set must not move it past tcont_count(). */
			{
				uint32_t had = tcont_index(inst);

				tcont_remember(inst, had == 0xffff
					       ? (uint16_t)tcont_count()
					       : (uint16_t)had);
			}
			out_fmt("   [dry] tcont me %04x alloc %d queues %d -> "
				"index %d\n", (long)inst, (long)t.alloc_id,
				(long)nqueues, (long)tcont_index(inst));
		}
	return (uint16_t)tcont_index(inst);
}

/* The programmed T-CONT of ME `meId`, or 0. */
static struct us_tcont_prog *tc_prog_find(uint16_t meId)
{
	for (int i = 0; i < TCONT_MAX; i++)
		if (prog.t[i].used && prog.t[i].meId == meId)
			return &prog.t[i];
	return 0;
}

static int tc_prog_dropped(uint16_t meId, const uint8_t *drop)
{
	for (int i = 0; i < TCONT_MAX; i++)
		if (prog.t[i].used && prog.t[i].meId == meId)
			return drop[i];
	return 1;                        /* no such T-CONT programmed: gone */
}

/* The queue programmed at ordinal `ord` of T-CONT ME `meId`, or 0. */
static struct us_queue *prog_queue(uint16_t meId, uint16_t ord)
{
	for (unsigned i = 0; i < usq_hw_n; i++)
		if (usq_hw[i].tcontMe == meId && usq_hw[i].ordinal == ord)
			return &usq_hw[i];
	return 0;
}

/* The weight cmd 23 carries for a queue. */
static uint16_t us_queue_weight(const struct us_queue *q)
{
	return q->wrr && q->weight > 1 ? q->weight : 1;
}

static long us_priq_send(const struct us_queue *q, uint16_t tcontIndex, uint16_t ord,
			 const char *what)
{
	struct omci_priq pq;
	long rc;

	for (unsigned w = 0; w < sizeof pq / 4; w++)
		((uint32_t *)&pq)[w] = 0;
	pq.index = ord;
	pq.owner = tcontIndex;
	pq.weight = us_queue_weight(q);
	pq.wrr = q->wrr;
	pq.dp_mark = q->dp_mark;
	pq.cir = q->cir;
	pq.pir = q->pir;
	pq.dir = OMCI_GEMFLOW_US;
	rc = apply_hw ? omci_drv_call(OMCI_PRIQ_CMD, &pq, sizeof pq) : 0;
	out_fmt("   [%s] us priq %d ordinal %d tcont %d weight %d "
		"wrr %d dp %d cir %d pir %d%s -> %d\n",
		apply_hw ? "hw" : "dry",
		(long)q->meId, (long)pq.index, (long)pq.owner,
		(long)pq.weight, (long)pq.wrr,
		(long)pq.dp_mark, (long)pq.cir, (long)pq.pir, what, rc);
	return rc;
}

static void us_flow_send_delete(unsigned slot)
{
	struct omci_gemflow f;

	for (unsigned w = 0; w < sizeof f / 4; w++)
		((uint32_t *)&f)[w] = 0;
	f.flow_id = slot;
	f.dir = OMCI_GEMFLOW_US;
	if (apply_hw) {
		int rc = omci_drv_call(OMCI_GEMFLOW_CMD, &f, sizeof f);

		if (rc)
			out_fmt("   [hw] us flow %d delete -> %d\n", (long)slot, (long)rc);
	}
	out_fmt("   [%s] us flow %d delete\n", apply_hw ? "hw" : "dry", (long)slot);
	prog.f[slot].live = 0;
}

/* The upstream graph from the MIB, as a change to what the driver holds.
 *
 * The plan: every upstream GEM CTP, the class 277 queues they name, sorted
 * into each T-CONT's ordinals (us_queue_before()), and each T-CONT's
 * Alloc-ID. Against what the last rebuild programmed (prog):
 *
 *   - a T-CONT with the same Alloc-ID and the same queue at every ordinal
 *     is kept: its index and queue block stay, and a queue whose weight,
 *     type, marking or rates moved is sent again in place (cmd 23 on a
 *     queue already in the T-CONT's mask writes no drain);
 *   - any other programmed T-CONT is torn down (its flows, then its queues)
 *     and, if the plan still has it, built again (cmd 21, cmd 23);
 *   - a flow is sent only when its GEM port, T-CONT or queue changed (cmd
 *     25 on a slot in use updates it in place), and deleted only when its
 *     CTP is gone or its T-CONT was torn down.
 *
 * So a Set that changes nothing upstream sends nothing, and one that
 * changes one T-CONT interrupts that T-CONT only, where the old rebuild
 * deleted and re-created every queue and flow on every rebuild. The stock
 * stack re-applies per entity too. Everything is torn down only after a
 * change of the PLOAM Alloc-ID list (qos_full): the CAM rows, hence the
 * T-CONT indexes, may then have moved. */
void us_qos_rebuild(void)
{
	static struct us_flow_plan flows[FLOW_MAX];
	static struct us_queue queues[USQ_MAX];
	static struct {
		uint16_t tcontMe;
		uint16_t first, end;
		uint16_t alloc;
		uint8_t keep;
	} grp[USQ_MAX];
	const struct omci_class *ctp = find_class(OMCI_ME_GEM_PORT_CTP);
	uint8_t keep[FLOW_MAX], drop[TCONT_MAX];
	unsigned nf = 0, nq = 0, ng = 0, kept = 0, built = 0, same = 0;
	int full = qos_full;

	if (!ctp)
		return;
	qos_full = 0;
	for (unsigned i = 0; i < sizeof keep; i++)
		keep[i] = 0;

	/* The logical graph is the MIB. Keep existing flow ids by GEM Port-ID;
	 * new flows take the lowest free id after stale slots are released. */
	for (int i = 0; i < MIB_ROWS && nf < FLOW_MAX; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t dir;
		int id;

		if (!r || r->classId != OMCI_ME_GEM_PORT_CTP)
			continue;
		dir = row_u32(r, ctp, 3);
		if (dir != 1 && dir != 3)
			continue;
		flows[nf].ctp = r->inst;
		flows[nf].port = (uint16_t)row_u32(r, ctp, 1);
		flows[nf].tcontMe = (uint16_t)row_u32(r, ctp, 2);
		flows[nf].pqMe = (uint16_t)row_u32(r, ctp, 4);
		id = flow_find(0, flows[nf].port);
		flows[nf].flow_id = id < 0 ? 0xffff : (uint16_t)id;
		if (id >= 0)
			keep[id] = 1;
		nf++;
	}

	/* Only queues an upstream GEM CTP references: the unreferenced ones
	 * ISP1 Sets would fill the shared pool of 32 queues and push the fifth
	 * T-CONT to the out-of-range index 8. */
	for (unsigned i = 0; i < nf; i++)
		us_queue_add(queues, &nq, flows[i].pqMe);
	us_queue_rates(queues, nq, flows, nf);
	for (unsigned i = 1; i < nq; i++) {
		struct us_queue q = queues[i];
		unsigned j = i;

		while (j && us_queue_before(&q, &queues[j - 1])) {
			queues[j] = queues[j - 1];
			j--;
		}
		queues[j] = q;
	}
	for (unsigned first = 0; first < nq;) {
		unsigned end = first + 1;

		while (end < nq && queues[end].tcontMe == queues[first].tcontMe)
			end++;
		for (unsigned i = first; i < end; i++)
			queues[i].ordinal = (uint16_t)(i - first);
		if (end - first > 8) {
			out_fmt("   [hw] tcont me %04x needs %d queues; max is 8\n",
				(long)queues[first].tcontMe, (long)(end - first));
		} else {
			grp[ng].tcontMe = queues[first].tcontMe;
			grp[ng].first = (uint16_t)first;
			grp[ng].end = (uint16_t)end;
			grp[ng].alloc = tcont_bound_alloc_id(queues[first].tcontMe, 0);
			grp[ng].keep = 0;
			ng++;
		}
		first = end;
	}

	/* Which programmed T-CONTs stay as they are. */
	for (int t = 0; t < TCONT_MAX; t++) {
		const struct us_tcont_prog *tp = &prog.t[t];

		drop[t] = 0;
		if (!tp->used)
			continue;
		drop[t] = 1;
		if (full)
			continue;
		for (unsigned g = 0; g < ng; g++) {
			int same_q = 1;

			if (grp[g].tcontMe != tp->meId)
				continue;
			if (grp[g].alloc != tp->alloc || grp[g].end - grp[g].first != tp->nq)
				break;
			for (unsigned i = grp[g].first; i < grp[g].end && same_q; i++) {
				const struct us_queue *h = prog_queue(tp->meId, queues[i].ordinal);

				same_q = h && h->meId == queues[i].meId;
			}
			if (same_q) {
				grp[g].keep = 1;
				drop[t] = 0;
			}
			break;
		}
	}

	/* Teardown, flows first: a flow whose CTP is gone, or whose T-CONT
	 * goes. Then the queues of the T-CONTs that go (the deletes need the
	 * old ordinals and indexes). */
	for (unsigned i = 0; i < caps_flows() && i < FLOW_MAX; i++)
		if (prog.f[i].live &&
		    (!keep[i] || tc_prog_dropped(prog.f[i].tcontMe, drop)))
			us_flow_send_delete(i);
	for (unsigned i = 0; i < usq_hw_n;) {
		const struct us_tcont_prog *tp = tc_prog_find(usq_hw[i].tcontMe);

		if (tp && !drop[tp - prog.t]) {
			i++;
			continue;
		}
		{
			struct omci_priq pq;

			for (unsigned w = 0; w < sizeof pq / 4; w++)
				((uint32_t *)&pq)[w] = 0;
			pq.index = usq_hw[i].ordinal;
			pq.owner = usq_hw[i].tcontIndex;
			pq.dir = OMCI_GEMFLOW_US;
			if (apply_hw) {
				int rc = omci_drv_call(OMCI_PRIQ_DEL_CMD, &pq, sizeof pq);

				if (rc)
					out_fmt("   [hw] us priq ordinal %d tcont %d delete -> %d\n",
						(long)pq.index, (long)pq.owner, (long)rc);
			}
			out_fmt("   [%s] us priq %d ordinal %d tcont %d delete\n",
				apply_hw ? "hw" : "dry", (long)usq_hw[i].meId,
				(long)pq.index, (long)pq.owner);
		}
		usq_hw[i] = usq_hw[--usq_hw_n];
	}
	for (int t = 0; t < TCONT_MAX; t++)
		if (prog.t[t].used && drop[t]) {
			/* The driver answers again for one rebuilt; the dry
			 * run keeps its allocation-order index (tcont_apply()). */
			if (apply_hw)
				tcont_forget(prog.t[t].meId);
			prog.t[t].used = 0;
		}

	for (unsigned i = 0; i < caps_flows(); i++)
		if (!keep[i])
			flow_us[i].used = 0;
	for (unsigned i = 0; i < nf; i++)
		if (flows[i].flow_id == 0xffff) {
			int id = flow_alloc(0, flows[i].port);

			if (id < 0)
				continue;
			flows[i].flow_id = (uint16_t)id;
			flow_claim(0, id, flows[i].port);
		}

	/* The T-CONTs: kept ones get only their changed queues again; the
	 * others are built. */
	for (unsigned g = 0; g < ng; g++) {
		uint16_t tcontIndex;
		int slot = -1;

		if (grp[g].keep) {
			const struct us_tcont_prog *tp = tc_prog_find(grp[g].tcontMe);

			kept++;
			for (unsigned i = grp[g].first; i < grp[g].end; i++) {
				struct us_queue *h = prog_queue(grp[g].tcontMe, queues[i].ordinal);

				if (!h || !tp)
					continue;
				if (h->wrr == queues[i].wrr &&
				    us_queue_weight(h) == us_queue_weight(&queues[i]) &&
				    h->dp_mark == queues[i].dp_mark &&
				    h->cir == queues[i].cir && h->pir == queues[i].pir) {
					h->priority = queues[i].priority;
					h->weight = queues[i].weight;
					continue;
				}
				queues[i].tcontIndex = tp->index;
				if (us_priq_send(&queues[i], tp->index, queues[i].ordinal,
						 " (in place)") == 0)
					*h = queues[i];
			}
			continue;
		}
		if (tcont_alloc_unset_mode() && tcont_olt_alloc(grp[g].tcontMe) < 0)
			out_fmt("   warning: tcont me %04x carries GEM traffic but the OLT "
				"never Set its Alloc-ID (OMCI_TCONT_ALLOC_UNSET=unassigned): "
				"reported 0x00ff, bound to alloc %d from PLOAM\n",
				(long)grp[g].tcontMe, (long)grp[g].alloc);
		tcontIndex = tcont_apply(grp[g].tcontMe, grp[g].alloc,
					 (uint16_t)(grp[g].end - grp[g].first));
		if (tcontIndex == 0xffff)
			continue;
		built++;
		for (int t = 0; t < TCONT_MAX && slot < 0; t++)
			if (!prog.t[t].used)
				slot = t;
		if (slot >= 0) {
			prog.t[slot].meId = grp[g].tcontMe;
			prog.t[slot].alloc = grp[g].alloc;
			prog.t[slot].index = tcontIndex;
			prog.t[slot].nq = (uint8_t)(grp[g].end - grp[g].first);
			prog.t[slot].used = 1;
		}
		for (unsigned i = grp[g].first; i < grp[g].end; i++) {
			queues[i].tcontIndex = tcontIndex;
			if (us_priq_send(&queues[i], tcontIndex, queues[i].ordinal, "") == 0 &&
			    usq_hw_n < USQ_MAX)
				usq_hw[usq_hw_n++] = queues[i];
		}
	}

	for (unsigned i = 0; i < nf; i++) {
		struct omci_gemflow f;
		struct us_queue requested;
		struct us_queue *bound = 0;
		struct us_flow_prog *fp;
		long rc;

		if (flows[i].flow_id == 0xffff || flows[i].flow_id >= FLOW_MAX)
			continue;
		fp = &prog.f[flows[i].flow_id];
		for (unsigned q = 0; q < usq_hw_n; q++)
			if (usq_hw[q].meId == flows[i].pqMe &&
			    usq_hw[q].tcontMe == flows[i].tcontMe)
				bound = &usq_hw[q];
		if (!bound && us_queue_decode(flows[i].pqMe, &requested))
			for (unsigned q = 0; q < usq_hw_n; q++)
				if (usq_hw[q].tcontMe == flows[i].tcontMe &&
				    usq_hw[q].wrr == requested.wrr &&
				    (requested.wrr
				     ? usq_hw[q].weight == requested.weight
				     : usq_hw[q].priority == requested.priority)) {
					bound = &usq_hw[q];
					break;
				}
		if (!bound) {
			out_fmt("   [hw] gem %d has no queue %04x on tcont %04x\n",
				(long)flows[i].port, (long)flows[i].pqMe,
				(long)flows[i].tcontMe);
			if (fp->live)
				us_flow_send_delete(flows[i].flow_id);
			continue;
		}
		if (fp->live && fp->gem == flows[i].port && fp->tcontMe == bound->tcontMe &&
		    fp->tcont == bound->tcontIndex && fp->queue == bound->ordinal) {
			same++;
			continue;
		}
		for (unsigned w = 0; w < sizeof f / 4; w++)
			((uint32_t *)&f)[w] = 0;
		f.flow_id = flows[i].flow_id;
		f.gem_port = flows[i].port;
		f.tcont = bound->tcontIndex;
		f.queue = bound->ordinal;
		f.ena = 1;
		f.dir = OMCI_GEMFLOW_US;
		rc = apply_hw ? omci_drv_call(OMCI_GEMFLOW_CMD, &f, sizeof f) : 0;
		out_fmt("   [%s] gem %d us flow %d tcont me %d index %d "
			"queue %d -> %d\n", apply_hw ? "hw" : "dry",
			(long)f.gem_port, (long)f.flow_id, (long)flows[i].tcontMe,
			(long)f.tcont, (long)f.queue, rc);
		if (rc == 0) {
			fp->gem = flows[i].port;
			fp->tcontMe = bound->tcontMe;
			fp->tcont = bound->tcontIndex;
			fp->queue = bound->ordinal;
			fp->live = 1;
		} else {
			fp->live = 0;
		}
	}
	out_fmt("   [%s] us qos%s: %d T-CONTs kept, %d built, %d flows unchanged\n",
		apply_hw ? "hw" : "dry", full ? " (full, the Alloc-IDs changed)" : "",
		(long)kept, (long)built, (long)same);
}

/* The queue each downstream flow was last sent with, as ds_queue_key():
 * 0 for a flow not sent since omcid started (also after a resume, where the
 * flow table came from the snapshot), so the first change of its queue is
 * always sent. Not part of the snapshot: the snapshot layout is
 * flow_slot's. */
static uint32_t flow_ds_queue[FLOW_MAX];

/* What the driver is told about the queue of a downstream flow that decides
 * where its frames go: the UNI port and the priority, never 0. */
static uint32_t ds_queue_key(const struct omci_gemflow *f)
{
	return ((f->ds_port & 0xffU) << 16 | (f->ds_pq_pri & 0xffffU)) + 1U;
}

static void ds_flow_send(const struct omci_class *c, const struct mib_row *r,
			 int skip281);

/* A downstream queue (class 277) changed: every downstream flow whose CTP
 * points at it is sent again if the port or the priority it was sent with
 * is not the queue's now. The driver sends each flow's frames to the queue
 * of its priority, and the CTP may have been created before the OLT wrote
 * the queue's RelatedPort. A flow that is not yet sent is left to the CTP's
 * own create. */
static void ds_flows_queue_refresh(uint16_t priq)
{
	const struct omci_class *c = find_class(OMCI_ME_GEM_PORT_CTP);

	if (!c)
		return;
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);
		struct omci_gemflow f;
		uint32_t dir;
		int id;

		if (!r || r->classId != OMCI_ME_GEM_PORT_CTP ||
		    (uint16_t)row_u32(r, c, 7) != priq)
			continue;
		dir = row_u32(r, c, 3);
		if (dir != 2 && dir != 3)
			continue;
		id = flow_find(1, (uint16_t)row_u32(r, c, 1));
		if (id < 0 || id >= FLOW_MAX)
			continue;
		for (unsigned k = 0; k < sizeof f / 4; k++)
			((uint32_t *)&f)[k] = 0;
		ds_queue_words(&f, priq);
		if (flow_ds_queue[id] == ds_queue_key(&f))
			continue;
		ds_flow_send(c, r, -1);
	}
}

/* Class 277, a priority queue: driver command 23, sent when the queue is
 * Set (the stock stack does the same), not when a flow using it is
 * created, which would write the switch on every flow. An upstream queue
 * (bit 15 of the entity id) goes through the reconciler instead. */
void apply_priq(uint16_t inst)
{
	struct omci_priq pq;
	int prc;

	if (!(inst & 0x8000))
		ds_flows_queue_refresh(inst);
	if (!priq_program(inst, &pq)) {
		/* An upstream queue named by a CTP dirties the plan; the
		 * rebuild runs OLT_QUIET_MS after the last OMCI frame, so a
		 * burst of Sets costs one transaction. */
		if (us_queue_referenced(inst))
			qos_dirty = 1;
		return;
	}
	prc = apply_hw ? omci_drv_call(OMCI_PRIQ_CMD, &pq, sizeof pq) : 0;
	out_fmt("   [%s] priq %d port %d pri %d weight %d wrr %d "
		"dp %d -> %d\n", apply_hw ? "hw" : "dry",
		(long)inst, (long)pq.owner, (long)pq.index,
		(long)pq.weight, (long)pq.wrr, (long)pq.dp_mark,
		(long)prc);
	return;
}

/* Take a downstream flow back: cmd 25 with ena 0, which clears the CAM row
 * and reads it back clear (odi_switch_cmd.c). A delete the kernel did not
 * complete leaves the slot claimed (and flagged), so that nothing else is
 * sent over a row the CAM still holds, and the next refresh tries again. */
static void ds_flow_withdraw(int id)
{
	struct omci_gemflow f;
	int rc;

	for (unsigned i = 0; i < sizeof f / 4; i++)
		((uint32_t *)&f)[i] = 0;
	f.gem_port = flow_ds[id].port;
	f.flow_id = (uint32_t)id;
	f.dir = OMCI_GEMFLOW_DS;
	rc = apply_hw ? omci_drv_call(OMCI_GEMFLOW_CMD, &f, sizeof f) : 0;
	out_fmt("   [%s] gem %d ds flow %d withdrawn -> %d\n",
		apply_hw ? "hw" : "dry", (long)f.gem_port, (long)id, (long)rc);
	if (rc == 0) {
		flow_ds[id].used = 0;
		flow_ds[id].mcast = 0;
		flow_ds_queue[id] = 0;
	}
}

/* The multicast flag of a downstream GEM flow (mcast_filter of cmd 25).
 *
 * The stock stack sets it for a GEM port when a multicast GEM interworking
 * TP (class 281) that points at its CTP holds rows in its IPv4 or IPv6
 * multicast address table, and sends the flow again when that changes:
 * MultiGemIwTpDrvCfg re-sends cfgGemFlow with isFilterMcast set when a table
 * reaches one row, and clear when both are empty; a plain CTP create sends it
 * clear (mib_GemPortCtp). Keyed on the address table, not on the direction
 * or the port number: ISP1's multicast GEM 4095 and ISP2's 4094 both get it
 * once their OLT writes the 224/4 row. `skip` is a class 281 instance to
 * leave out, the one being deleted. */
static int ctp_mcast_rows(uint16_t ctp, int skip)
{
	const struct omci_class *c = find_class(OMCI_ME_MCAST_GEM_IW_TP);

	if (!c)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);

		if (!r || r->classId != OMCI_ME_MCAST_GEM_IW_TP ||
		    (int)r->inst == skip || row_u32(r, c, 1) != ctp)
			continue;
		if (tbl_rows(r, 9) || tbl_rows(r, 10))
			return 1;
	}
	return 0;
}

/* Send (or send again) the downstream flow of `port`, with the queue the
 * CTP row `r` points at. The CTP's own port takes its flag from the CTP's
 * address tables (`mcast` < 0); an extra multicast GEM port, which has no
 * CTP, is sent flagged (mcast_extra_refresh) with the queue of the CTP its
 * class 281 points at. */
static void ds_flow_send_port(const struct omci_class *c,
			      const struct mib_row *r, uint16_t port,
			      int mcast, int skip281)
{
	int rc, id;
	struct omci_gemflow f;

	for (unsigned i = 0; i < sizeof f / 4; i++)
		((uint32_t *)&f)[i] = 0;
	f.gem_port      = port;
	/* From the class 277 row the CTP points at, or priq_related_port().
	 * A DsPriQPtr of 0 is priority queue ME 0, a real queue, not a null
	 * pointer: every GEM CTP of ISP1 carries 0. */
	ds_queue_words(&f, (uint16_t)row_u32(r, c, 7));
	f.ena         = 1;               /* create, not update */
	f.mcast_filter = (uint32_t)(mcast >= 0 ? mcast
				    : ctp_mcast_rows(r->inst, skip281));
	id = flow_alloc(1, (uint16_t)f.gem_port);
	if (id < 0) {
		out("   [hw] no downstream flow left\n");
		return;
	}
	f.flow_id = (uint32_t)id;
	f.dir = OMCI_GEMFLOW_DS;
	rc = apply_hw ? omci_drv_call(OMCI_GEMFLOW_CMD, &f, sizeof f) : 0;
	if (rc == 0) {
		flow_claim(1, id, (uint16_t)f.gem_port);
		/* flow_alloc() stays below caps_flows(), which drv.c caps at
		 * FLOW_MAX; the check says so where the compiler can see it. */
		if (id < FLOW_MAX) {
			flow_ds[id].mcast = (uint8_t)f.mcast_filter;
			flow_ds_queue[id] = ds_queue_key(&f);
		}
	}
	out_fmt("   [%s] gem %d ds flow %d priq %d pri %d "
		"wrr %d weight %d port %d mcast %d -> %d\n",
		apply_hw ? "hw" : "dry",
		(long)f.gem_port, (long)id, (long)f.ds_pq_pri,
		(long)f.ds_prio, (long)f.ds_wrr,
		(long)f.ds_weight, (long)f.ds_port,
		(long)f.mcast_filter, (long)rc);
}

/* Send (or send again) the downstream flow of a CTP. */
static void ds_flow_send(const struct omci_class *c, const struct mib_row *r,
			 int skip281)
{
	ds_flow_send_port(c, r, (uint16_t)row_u32(r, c, 1), -1, skip281);
}

/* The extra multicast GEM ports. G.988 9.2.5 lets an OLT spread the channels
 * over several multicast GEM ports by writing more rows, with other GEM port
 * ids, into the address table of one class 281 (and its access control
 * lists, class 309, name the port a channel arrives on); only the first
 * port has a GEM port network CTP, "no additional GEM network CTPs need be
 * created". The ports named only by rows have no CTP to give them a flow,
 * so they get one here, flagged multicast and never upstream, and lose it
 * when the last row naming them goes. Only while some class 281 points at a
 * CTP the OLT created (class 268, downstream capable): a port that is in no
 * multicast GEM IW TP's reach is not a multicast GEM port, and a port a CTP
 * already owns keeps the CTP's flow. `skip281`, `skip309` and `skip268`
 * are the instances being deleted (-1 for none); a class 268 being deleted
 * is no CTP to hang the ports on, though its own flow is not this code's
 * to take back. */
#define MCAST_XPORTS 16
#define CLS_MCAST_OPER_PROFILE 309

static int is_ctp_port(uint16_t port)
{
	const struct omci_class *c = find_class(OMCI_ME_GEM_PORT_CTP);

	for (int i = 0; c && i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);

		if (r && r->classId == OMCI_ME_GEM_PORT_CTP &&
		    (uint16_t)row_u32(r, c, 1) == port)
			return 1;
	}
	return 0;
}

static void xport_add(uint16_t *ports, int *n, uint16_t port)
{
	/* 12-bit GEM port ids (4095 is the highest, and the CAM's width);
	 * 0 and 0xffff are not ports. */
	if (!port || port > 4095 || is_ctp_port(port))
		return;
	for (int i = 0; i < *n; i++)
		if (ports[i] == port)
			return;
	if (*n < MCAST_XPORTS)
		ports[(*n)++] = port;
}

/* The ports named by rows of the live class 281 and 309 instances, and the
 * CTP row whose queue they use (null when there is no class 281 on a
 * downstream CTP, and then no port). */
static const struct mib_row *xports_wanted(uint16_t *ports, int *n,
					   int skip281, int skip309,
					   int skip268)
{
	const struct omci_class *c281 = find_class(OMCI_ME_MCAST_GEM_IW_TP);
	const struct omci_class *c268 = find_class(OMCI_ME_GEM_PORT_CTP);
	const struct mib_row *ctp = 0;

	*n = 0;
	if (!c281 || !c268)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);
		const struct mib_row *p;
		uint32_t dir;

		if (!r || r->classId != OMCI_ME_MCAST_GEM_IW_TP ||
		    (int)r->inst == skip281)
			continue;
		p = mib_find(OMCI_ME_GEM_PORT_CTP, (uint16_t)row_u32(r, c281, 1));
		if (p && (int)p->inst == skip268)
			p = 0;
		dir = p ? row_u32(p, c268, 3) : 0;
		if (!ctp && p && (dir == 2 || dir == 3))
			ctp = p;
		for (uint8_t t = r->tbl_head; t != MIB_TBL_NONE; t = tblpool[t].next)
			if (tblpool[t].attr == 9 || tblpool[t].attr == 10)
				xport_add(ports, n, (uint16_t)(tblpool[t].data[0] << 8 |
							       tblpool[t].data[1]));
	}
	if (!ctp) {
		*n = 0;
		return 0;
	}
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);

		if (!r || r->classId != CLS_MCAST_OPER_PROFILE ||
		    (int)r->inst == skip309)
			continue;
		/* Part 0 of a row (bits 13-11 of the control word) is the one
		 * with the GEM port; the other parts are prefixes. */
		for (uint8_t t = r->tbl_head; t != MIB_TBL_NONE; t = tblpool[t].next)
			if ((tblpool[t].attr == 7 || tblpool[t].attr == 8) &&
			    !((tblpool[t].data[0] >> 3) & 7))
				xport_add(ports, n, (uint16_t)(tblpool[t].data[2] << 8 |
							       tblpool[t].data[3]));
	}
	return ctp;
}

void mcast_extra_refresh(int skip281, int skip309, int skip268)
{
	const struct omci_class *c = find_class(OMCI_ME_GEM_PORT_CTP);
	uint16_t ports[MCAST_XPORTS];
	const struct mib_row *ctp;
	int n;

	if (!c)
		return;
	ctp = xports_wanted(ports, &n, skip281, skip309, skip268);
	/* Withdraw first, so the slots are free for a new port. A flow that
	 * is flagged multicast and has no CTP is one of these. */
	for (int id = 0; id < FLOW_MAX && id < (int)caps_flows(); id++) {
		int keep = 0;

		if (!flow_ds[id].used || !flow_ds[id].mcast ||
		    is_ctp_port(flow_ds[id].port))
			continue;
		for (int i = 0; i < n; i++)
			keep |= ports[i] == flow_ds[id].port;
		if (keep)
			continue;
		ds_flow_withdraw(id);
	}
	for (int i = 0; ctp && i < n; i++) {
		int id = flow_find(1, ports[i]);
		struct omci_gemflow q;

		for (unsigned k = 0; k < sizeof q / 4; k++)
			((uint32_t *)&q)[k] = 0;
		ds_queue_words(&q, (uint16_t)row_u32(ctp, c, 7));
		/* Send again only when the flow is new, not flagged, or its
		 * queue moved. */
		if (id >= 0 && flow_ds[id].mcast &&
		    flow_ds_queue[id] == ds_queue_key(&q))
			continue;
		ds_flow_send_port(c, ctp, ports[i], 1, -1);
	}
}

/* After a class 281 Create, Set or Delete (`skip` is the instance being
 * deleted, else -1): every downstream flow whose multicast flag no longer
 * matches its CTP's address tables is sent again. */
void mcast_flag_refresh(int skip)
{
	const struct omci_class *c = find_class(OMCI_ME_GEM_PORT_CTP);

	if (!c)
		return;
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);
		int id;

		if (!r || r->classId != OMCI_ME_GEM_PORT_CTP)
			continue;
		id = flow_find(1, (uint16_t)row_u32(r, c, 1));
		if (id < 0 || flow_ds[id].mcast == ctp_mcast_rows(r->inst, skip))
			continue;
		ds_flow_send(c, r, skip);
	}
	mcast_extra_refresh(skip, -1, -1);
}

/* Class 268, a GEM port network CTP: the downstream flow now, the upstream
 * one through the reconciler, and the broadcast flow lookup. */
void apply_gem_ctp(const struct omci_class *c, const struct mib_row *r)
{
	uint32_t dir = row_u32(r, c, 3);

	if (dir == 2 || dir == 3)
		ds_flow_send(c, r, -1);
	if (dir == 1 || dir == 3)
		qos_dirty = 1;   /* rebuilt OLT_QUIET_MS after the last frame */
	/* A multicast GEM IW TP pointing here may already name more ports. */
	mcast_extra_refresh(-1, -1, -1);
	bc_gem_update();
}

/* The QoS state a MIB reset throws away (mib_reset_all()). */
void qos_reset(void)
{
	for (int i = 0; i < FLOW_MAX; i++) {
		flow_us[i].used = 0;
		flow_ds[i].used = 0;
		flow_ds_queue[i] = 0;
	}
	for (int i = 0; i < TCONT_MAX; i++)
		tcont_map[i].used = 0;
	/* What the MIB had programmed goes with it, in the driver too:
	 * command 0, the stock resetMib, empties every upstream flow, queue
	 * and T-CONT block and deletes every downstream flow, the extra
	 * multicast ports' included (odi_switch_cmd.c), so the next
	 * provisioning starts from what the module load left, as omcid's
	 * tables do. */
	if (apply_hw) {
		uint8_t none[4] = { 0, 0, 0, 0 };
		long rc = omci_drv_call(OMCI_MIB_RESET_CMD, none, sizeof none);

		out_fmt("   [hw] flow reset (mib reset) -> %d\n", rc);
	}
	for (int i = 0; i < USQ_MAX; i++)
		usq_hw[i].meId = 0;
	usq_hw_n = 0;
	for (int i = 0; i < TCONT_MAX; i++)
		prog.t[i].used = 0;
	for (int i = 0; i < FLOW_MAX; i++)
		prog.f[i].live = 0;
	qos_dirty = 0;
	qos_full = 0;
	bc_flow = -1;
}
