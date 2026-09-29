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
 * priority within it. Only the bottom three bits of the priority reach the
 * driver, reversed against the per-UNI queue count, so OMCI priority 0 is
 * driver priority 7. Word 8 is a boolean: weighted round robin.
 */
static uint32_t caps_uni_queues(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_UNIQ);

	return n ? n : 8;                /* ISP1 reports 8 */
}

/* How many T-CONT slots the device has; a driver index at or above this is
 * not a T-CONT. */
static uint32_t tcont_capacity(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_TCONTS);

	return n ? n : 8;                /* ISP1 reports 8 */
}

/* The class 277 row a pointer names, or 0. Returns the row and the class
 * together because every caller needs both to read an attribute. */
static struct mib_row *priq_row(uint16_t meId, const struct omci_class **cp)
{
	*cp = find_class(OMCI_ME_PRIORITY_QUEUE);
	return *cp ? mib_find(OMCI_ME_PRIORITY_QUEUE, meId) : 0;
}

/* The k-th UNI managed entity: PPTP Ethernet UNIs first, then VEIPs, an
 * order fitted to the one observation of ISP1 (one of each). UNIs are
 * ONU-created, so omci_autonomous[] is searched before the store, which
 * holds a row only if the OLT set one. */
static uint16_t nth_uni_entity(unsigned k)
{
	for (unsigned cls = 0; cls < 2; cls++) {
		uint16_t want = cls ? 329 : 11;

		for (unsigned i = 0; i < omci_autonomous_count; i++)
			if (omci_autonomous[i].classId == want) {
				if (!k)
					return omci_autonomous[i].inst;
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

static void ds_queue_words(struct omci_gemflow *f, uint16_t priqMeId)
{
	const struct omci_class *qc;
	struct mib_row *q = priq_row(priqMeId, &qc);
	uint32_t rp, weight;
	int port;

	if (q) {
		rp     = row_u32(q, qc, 6);      /* RelatedPort */
		weight = row_u32(q, qc, 8);      /* Weight */
		f->ds_dp_mark = row_u32(q, qc, 16);
	} else {
		rp     = priq_related_port(priqMeId);
		weight = PRIQ_DEFAULT_WEIGHT;
		if (!rp)
			return;
	}
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
	const struct omci_class *qc;
	struct mib_row *q = priq_row(priqMeId, &qc);
	uint32_t rp, weight;
	int port;

	if (priqMeId & 0x8000)
		return 0;
	if (q) {
		rp     = row_u32(q, qc, 6);
		weight = row_u32(q, qc, 8);
	} else {
		rp     = priq_related_port(priqMeId);
		weight = PRIQ_DEFAULT_WEIGHT;
		if (!rp)
			return 0;
	}
	for (unsigned i = 0; i < sizeof *out / 4; i++)
		((uint32_t *)out)[i] = 0;
	port   = uni_switch_port((uint16_t)(rp >> 16), OMCI_UNI_SLOT_PPTP);

	out->index     = (uint16_t)(rp & 0xffff);
	out->owner     = (uint16_t)(port >= 0 ? port : 0);
	out->weight    = (uint16_t)(weight < 1 ? 1 : weight);
	out->wrr     = (weight >= 2);
	out->dp_mark = (uint8_t)(q ? row_u32(q, qc, 16) : 0);
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
};

struct us_flow_plan {
	uint16_t ctp;
	uint16_t port;
	uint16_t tcontMe;
	uint16_t pqMe;
	uint16_t flow_id;
};

static struct us_queue usq_hw[USQ_MAX];
static unsigned usq_hw_n;
int qos_dirty;

/* `omcicli dump qmap`, in the column shape of the stock dump: one row per
 * upstream queue programmed. QID is the dense ordinal, TCID the driver
 * T-CONT index; CIR/PIR are not modelled and print 0, as they do on ISP2. */
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
			0L, 0L, 1L);
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
	return q->tcontMe >= 0x8000;
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
 * the kernel keeps (/proc/odi_gpon, alloc_ids), so the Alloc-IDs are this
 * OLT's own rather than ones read off another line:
 *
 *   - a T-CONT the OLT set keeps what it set, 0x00FF included;
 *   - every other T-CONT, in entity-id order, takes the next assigned
 *     Alloc-ID that no set T-CONT claims, in assignment order;
 *   - the rest read 0x00FF, and tcont_apply() refuses them.
 *
 * Binding by position also keeps T-CONT k on the kernel's Alloc-ID CAM row
 * k, which is the order its upstream queue and flow words are laid out in
 * (odi_switch_cmd.c, cmd 23 and 25). */
#define ALLOC_ID_UNASSIGNED 0x00ffu

const char *gpon_proc_path = "/proc/odi_gpon";
static uint16_t ploam_alloc[TCONT_MAX];
static int ploam_alloc_n = -1;           /* -1: not read yet, or unreadable */

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

uint16_t tcont_alloc_id(uint16_t meId, int *src)
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
	for (unsigned i = 0; i < omci_autonomous_count && nt < TCONT_MAX; i++)
		if (omci_autonomous[i].classId == OMCI_ME_TCONT) {
			inst[nt] = omci_autonomous[i].inst;
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

/* Program one T-CONT: from a Set of its Alloc-ID, or on demand when a GEM
 * port CTP names it and the OLT never set it (the row already carries the
 * value). The driver hands back the index; the dry run uses allocation order. */
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
				return 0xffff;
			}
			if (rc == 0)
				tcont_remember(inst, (uint16_t)t.index);
			out_fmt("   [hw] tcont me %04x alloc %d queues %d -> "
				"index %d rc %d\n", (long)inst, (long)t.alloc_id,
				(long)nqueues, (long)t.index, (long)rc);
			if (rc != 0)
				return 0xffff;
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

void us_qos_rebuild(void)
{
	static struct us_flow_plan flows[FLOW_MAX];
	static struct us_queue queues[USQ_MAX];
	const struct omci_class *ctp = find_class(OMCI_ME_GEM_PORT_CTP);
	uint8_t keep[FLOW_MAX];
	unsigned nf = 0, nq = 0;

	if (!ctp)
		return;
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

	/* Queue deletion needs the old ordinals and T-CONT indices, so tear down
	 * hardware before replacing the runtime plan. */
	for (unsigned i = 0; i < caps_flows(); i++)
		if (flow_us[i].used) {
			struct omci_gemflow f;

			for (unsigned w = 0; w < sizeof f / 4; w++)
				((uint32_t *)&f)[w] = 0;
			f.flow_id = i;
			f.dir = OMCI_GEMFLOW_US;
			if (apply_hw)
				omci_drv_call(OMCI_GEMFLOW_CMD, &f, sizeof f);
			out_fmt("   [%s] us flow %d delete\n",
				apply_hw ? "hw" : "dry", (long)i);
		}
	for (unsigned i = 0; i < usq_hw_n; i++) {
		struct omci_priq pq;

		for (unsigned w = 0; w < sizeof pq / 4; w++)
			((uint32_t *)&pq)[w] = 0;
		pq.index = usq_hw[i].ordinal;
		pq.owner = usq_hw[i].tcontIndex;
		pq.dir = OMCI_GEMFLOW_US;
		if (apply_hw)
			omci_drv_call(OMCI_PRIQ_DEL_CMD, &pq, sizeof pq);
		out_fmt("   [%s] us priq %d ordinal %d tcont %d delete\n",
			apply_hw ? "hw" : "dry", (long)usq_hw[i].meId,
			(long)pq.index, (long)pq.owner);
	}
	usq_hw_n = 0;
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

	/* Only queues an upstream GEM CTP references: the unreferenced ones
	 * ISP1 Sets would fill the shared pool of 32 queues and push the fifth
	 * T-CONT to the out-of-range index 8. */
	for (unsigned i = 0; i < nf; i++)
		us_queue_add(queues, &nq, flows[i].pqMe);
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
		uint16_t tcontMe = queues[first].tcontMe;
		unsigned end = first + 1;
		uint16_t tcontIndex;

		while (end < nq && queues[end].tcontMe == tcontMe)
			end++;
		if (end - first > 8) {
			out_fmt("   [hw] tcont me %04x needs %d queues; max is 8\n",
				(long)tcontMe, (long)(end - first));
			first = end;
			continue;
		}
		tcontIndex = tcont_apply(tcontMe,
			(uint16_t)effective_u32(262, tcontMe, 1),
			(uint16_t)(end - first));
		if (tcontIndex == 0xffff) {
			first = end;
			continue;
		}
		for (unsigned i = first; i < end; i++) {
			struct omci_priq pq;
			long rc;

			queues[i].ordinal = (uint16_t)(i - first);
			queues[i].tcontIndex = tcontIndex;
			for (unsigned w = 0; w < sizeof pq / 4; w++)
				((uint32_t *)&pq)[w] = 0;
			pq.index = queues[i].ordinal;
			pq.owner = tcontIndex;
			pq.weight = queues[i].wrr && queues[i].weight > 1
				? queues[i].weight : 1;
			pq.wrr = queues[i].wrr;
			pq.dp_mark = queues[i].dp_mark;
			pq.dir = OMCI_GEMFLOW_US;
			rc = apply_hw
			   ? omci_drv_call(OMCI_PRIQ_CMD, &pq, sizeof pq) : 0;
			out_fmt("   [%s] us priq %d ordinal %d tcont %d weight %d "
				"wrr %d dp %d -> %d\n", apply_hw ? "hw" : "dry",
				(long)queues[i].meId, (long)pq.index, (long)pq.owner,
				(long)pq.weight, (long)pq.wrr,
				(long)pq.dp_mark, rc);
			if (rc == 0 && usq_hw_n < USQ_MAX)
				usq_hw[usq_hw_n++] = queues[i];
		}
		first = end;
	}

	for (unsigned i = 0; i < nf; i++) {
		struct omci_gemflow f;
		struct us_queue requested;
		struct us_queue *bound = 0;
		long rc;

		if (flows[i].flow_id == 0xffff)
			continue;
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

/* Class 268, a GEM port network CTP: the downstream flow now, the upstream
 * one through the reconciler, and the broadcast flow lookup. */
void apply_gem_ctp(const struct omci_class *c, const struct mib_row *r)
{
	int rc;
	struct omci_gemflow f;
	uint32_t dir;

	for (unsigned i = 0; i < sizeof f / 4; i++)
		((uint32_t *)&f)[i] = 0;
	f.gem_port      = row_u32(r, c, 1);
	/* Upstream T-CONT and queue words are filled by us_qos_rebuild(),
	 * after the complete queue graph is available. */
	f.tcont     = 0;
	f.queue     = 0;
	/* From the class 277 row the CTP points at, or priq_related_port().
	 * A DsPriQPtr of 0 is priority queue ME 0, a real queue, not a null
	 * pointer: every GEM CTP of ISP1 carries 0. */
	ds_queue_words(&f, (uint16_t)row_u32(r, c, 7));
	f.ena         = 1;               /* create, not update */
	dir = row_u32(r, c, 3);
	if (dir == 2)
		f.mcast_filter = 1;
	if (dir == 2 || dir == 3) {
		int id = flow_alloc(1, (uint16_t)f.gem_port);
		if (id < 0) {
			out("   [hw] no downstream flow left\n");
			return;
		}
		f.flow_id = (uint32_t)id;
		f.dir = OMCI_GEMFLOW_DS;
		rc = apply_hw
		   ? omci_drv_call(OMCI_GEMFLOW_CMD, &f, sizeof f) : 0;
		if (rc == 0)
			flow_claim(1, id, (uint16_t)f.gem_port);
		out_fmt("   [%s] gem %d ds flow %d priq %d pri %d "
			"wrr %d weight %d port %d -> %d\n",
			apply_hw ? "hw" : "dry",
			(long)f.gem_port, (long)id, (long)f.ds_pq_pri,
			(long)f.ds_prio, (long)f.ds_wrr,
			(long)f.ds_weight, (long)f.ds_port,
			(long)rc);
	}
	if (dir == 1 || dir == 3)
		qos_dirty = 1;   /* rebuilt OLT_QUIET_MS after the last frame */
	bc_gem_update();
	return;
}

/* The QoS state a MIB reset throws away (mib_reset_all()). */
void qos_reset(void)
{
	for (int i = 0; i < FLOW_MAX; i++) {
		flow_us[i].used = 0;
		flow_ds[i].used = 0;
	}
	for (int i = 0; i < TCONT_MAX; i++)
		tcont_map[i].used = 0;
	usq_hw_n = 0;
	qos_dirty = 0;
	bc_flow = -1;
}
