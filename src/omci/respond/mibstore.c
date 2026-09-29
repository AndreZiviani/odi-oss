/* The MIB store: what the OLT has created and set.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"
#include "omci_defaults.h"
#include "../procparse.h"

uint8_t serial[9];      /* 4 ASCII vendor id + 4 binary */

/* Where serial[] came from: 0 nowhere yet (all zeroes), 1 the config store,
 * 2 the kernel. Anything below 2 is retried by serial_refresh(). */
static int serial_src;

/* The serial number, and why it is not simply read once at startup.
 *
 * rcS starts omcid at the "omcimods" pon step and programs the serial at the
 * "gponsn" step right after it, so the one read omcid used to make at startup
 * raced the step that sets it and, when it lost, answered 00000000 for the rest
 * of the run: ONU-G serial, `omcicli get sn`, `omcli state`, every MIB dump.
 *
 * So this is asked again until the kernel has a real one, from the places that
 * know it, in order of authority:
 *
 *   1. the driver, over the same command the OMCI stack uses for everything
 *      else (DRV_GET_SN, answered by odi_switch from the GPON block);
 *   2. /proc/odi_gpon, whose "sn" line is the same GPON block value, for when
 *      the command path is not answering;
 *   3. GPON_SN in the config store, which is what the gponsn step programs --
 *      used only until the kernel has one, and never over it.
 *
 * All zeroes means "not set yet" from every source. With none of the three
 * available the serial stays zero, which is what it was before, and omcid
 * runs regardless. */
static int serial_from_kernel(uint8_t sn[8])
{
	uint8_t buf[9];
	char proc[128];
	long fd, n;

	if (omci_drv_call(DRV_GET_SN, buf, sizeof buf) == 0 && !pp_sn_is_zero(buf)) {
		for (int i = 0; i < 8; i++)
			sn[i] = buf[i];
		return 0;
	}
	fd = sys_open(gpon_proc_path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = sys_read((int)fd, proc, sizeof proc);
	sys_close((int)fd);
	if (n <= 0 || pp_gpon_sn(proc, (unsigned)n, sn) != 0 || pp_sn_is_zero(sn))
		return -1;
	return 0;
}

int serial_refresh(int log)
{
	static unsigned asked;
	uint8_t sn[8];

	if (serial_src == 2)
		return 0;
	/* Once a second from the main loop; a kernel that still has no serial
	 * after fifteen minutes is not going to get one from waiting. */
	if (asked >= 900)
		return serial_src ? 0 : -1;
	asked++;
	if (serial_from_kernel(sn) == 0) {
		for (int i = 0; i < 8; i++)
			serial[i] = sn[i];
		serial[8] = 0;
		serial_src = 2;
		if (log)
			out("serial number: from the kernel\n");
		return 0;
	}
	if (serial_src == 0) {
		char label[16];
		int len = cfg_get("/var/config/lastgood_hs.xml", "GPON_SN",
				  label, sizeof label);

		if (len > 0 && pp_label_sn(label, (unsigned)len, sn) == 0 &&
		    !pp_sn_is_zero(sn)) {
			for (int i = 0; i < 8; i++)
				serial[i] = sn[i];
			serial[8] = 0;
			serial_src = 1;
			if (log)
				out("serial number: from the config store "
				    "until the kernel has one\n");
		}
	}
	return serial_src ? 0 : -1;
}

uint8_t devid[40];      /* "RTL9602C", then the revision */

uint8_t mib_data_sync;

const struct omci_class *find_class(uint16_t id)
{
	for (unsigned i = 0; i < omci_class_count; i++)
		if (omci_classes[i].classId == id)
			return &omci_classes[i];
	return 0;
}

/* ONU2-G attribute 2: baseline OMCC only, what omcid has always answered. */
#define OMCC_VER_BASELINE 0x80

static void put_str(uint8_t *out, uint16_t n, const char *s)
{
	uint16_t i = 0;

	for (; s[i] && i < n; i++)
		out[i] = (uint8_t)s[i];
	for (; i < n; i++)
		out[i] = 0;
}

/* Attribute k (1-based, as the mask counts) of one managed entity. Returns its
 * width from the model, having written that many bytes. Anything this does not
 * know about answers as zeroes of the right size, which is a valid answer for
 * every attribute the OLT asks about here. */
/* The model's size is what the vendor stores, and it keeps strings
 * NUL-terminated -- ONU-G's vendor id is 5 bytes there and 4 on the wire. Every
 * field after a string would be shifted by one without this. */
uint16_t attr_width(const struct omci_class *c, unsigned k)
{
	uint16_t n;

	if (k >= c->nattr)
		return 0;
	n = c->attrs[k].size;
	if (c->attrs[k].type == OMCI_STRING && n)
		n--;
	return n;
}

struct mib_tblent tblpool[MIB_TBL_NODES];

struct mib_row mib[MIB_ROWS];

/* Offset of attribute k in our packed row, or -1 if it does not fit. */
int attr_offset(const struct omci_class *c, unsigned k)
{
	int off = 0;

	for (unsigned i = 1; i < k; i++) {
		off += attr_width(c, i);
		if (off > MIB_ROW_MAX)
			return -1;
	}
	return off;
}

/* The attributes a create carries: oltAcc bit 2 is set-by-create, bit 1 write,
 * bit 0 read. EntityId reads 5, an ordinary writable attribute 7, and a
 * read-only one like GemPortCtp's UNI counter reads 1. */
#define OLT_ACC_CREATE 4

uint16_t create_mask(const struct omci_class *c)
{
	uint16_t m = 0;

	for (unsigned k = 1; k < c->nattr && k <= 16; k++)
		if (c->attrs[k].oltAcc & OLT_ACC_CREATE)
			m |= (uint16_t)(1u << (16 - k));
	return m;
}

/* Which attribute of this class is a table, or 0 if none. Only class 171 is
 * known to be one, and the model records it as OMCI_OCTETS of 16 like any
 * other fixed-width attribute -- the type does not say "table". Rather than
 * guess from the width, name the one we have evidence for. */
static unsigned table_attr(const struct omci_class *c)
{
	return (c && c->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA) ? 6 : 0;
}

/* Whether a Get of this attribute must go through Get-Next instead of being
 * answered inline. This is the vendor's own test, read out of OMCI_OnGetMsg in
 * bin/omci_app at 0x413abc:
 *
 *     dt = MIB_GetAttrDataType(tblIdx, k)
 *     if (dt == 5) goto big;                        // OCTETS
 *     if (MIB_GetAttrLen(tblIdx, k) < 26) continue;  // sltiu v0,v0,26
 *     goto big;
 *
 * 26 rather than 29 because a baseline Get response body is one result byte,
 * two mask bytes and 25 attribute bytes.
 *
 * Deliberately NOT the same question as table_attr() above. That one asks which
 * attribute accumulates entries as a keyed list, which is a behaviour of class
 * 171's plugin specifically; this one asks which attribute is too big to send
 * inline, which is every OCTETS attribute in the model. Class 84's 24-byte VLAN
 * filter list is big by this test and is not a keyed list by that one. */
int attr_is_big(const struct omci_class *c, unsigned k)
{
	if (!c || k >= c->nattr)
		return 0;
	return c->attrs[k].type == OMCI_OCTETS || attr_width(c, k) >= 26;
}

/* The whole of a big attribute, laid out as the OLT will read it back.
 *
 * For a keyed-list attribute that is every entry, head to tail, so the order
 * matches what the vendor's dump prints -- newest first. For any other big
 * attribute the row holds the value flat and there is exactly one "entry".
 *
 * Returns the byte length, which is what the Get response reports and what the
 * chunk count is computed from. */
uint16_t tbl_serialise(const struct omci_class *c, uint16_t inst, unsigned k,
		       uint8_t *out, uint16_t max)
{
	struct mib_row *r = mib_find(c->classId, inst);
	uint16_t w = attr_width(c, k);
	uint16_t n = 0;

	if (!r || !w)
		return 0;
	if (k == table_attr(c) && w == MIB_TBL_ENTRY) {
		for (uint8_t i = r->tbl_head; i != MIB_TBL_NONE;
		     i = tblpool[i].next) {
			if (n + w > max)
				break;
			for (uint16_t b = 0; b < w; b++)
				out[n + b] = tblpool[i].data[b];
			n = (uint16_t)(n + w);
		}
		return n;
	}
	/* Not written is not zero: an attribute the OLT never set has no table
	 * to read back, and answering with a zero-filled entry would have the
	 * OLT walk chunks of nothing. */
	if (!(r->written & (1u << (16 - k))))
		return 0;
	if (w > max)
		return 0;
	n = attr_value(c, inst, k, out);
	return n;
}

void tbl_free(struct mib_row *r)
{
	uint8_t n = r->tbl_head;

	while (n != MIB_TBL_NONE) {
		uint8_t nx = tblpool[n].next;

		tblpool[n].used = 0;
		n = nx;
	}
	r->tbl_head = MIB_TBL_NONE;
	r->tbl_count = 0;
}

static int tbl_bytes_eq(const uint8_t *a, const uint8_t *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* One OLT set of the table attribute. See the rules above the pool. */
static void tbl_set(struct mib_row *r, const uint8_t *e)
{
	uint8_t *link = &r->tbl_head;
	int del = 1;

	for (int i = 8; i < MIB_TBL_ENTRY; i++)
		if (e[i] != 0xff) {
			del = 0;
			break;
		}
	for (uint8_t n = r->tbl_head; n != MIB_TBL_NONE; n = tblpool[n].next) {
		struct mib_tblent *t = &tblpool[n];

		if (!tbl_bytes_eq(t->data, e, 8)) {
			link = &t->next;
			continue;
		}
		if (del) {
			*link = t->next;
			t->used = 0;
			r->tbl_count--;
		} else {
			/* The vendor reassigns words 1..3; word 0 is part of
			 * the key and already equal. */
			for (int i = 4; i < MIB_TBL_ENTRY; i++)
				t->data[i] = e[i];
		}
		return;
	}
	if (del)
		return;                      /* deleting what is not there */
	for (uint8_t n = 0; n < MIB_TBL_NODES; n++) {
		if (tblpool[n].used)
			continue;
		tblpool[n].used = 1;
		for (int i = 0; i < MIB_TBL_ENTRY; i++)
			tblpool[n].data[i] = e[i];
		tblpool[n].next = r->tbl_head;
		r->tbl_head = n;
		r->tbl_count++;
		return;
	}
	r->truncated = 1;                    /* pool exhausted; say so */
}

struct mib_row *mib_find(uint16_t cls, uint16_t inst)
{
	for (int i = 0; i < MIB_ROWS; i++)
		if (mib[i].used && mib[i].classId == cls && mib[i].inst == inst)
			return &mib[i];
	return 0;
}

struct mib_row *mib_add(uint16_t cls, uint16_t inst)
{
	struct mib_row *r = mib_find(cls, inst);

	if (r)
		return r;
	for (int i = 0; i < MIB_ROWS; i++)
		if (!mib[i].used) {
			r = &mib[i];
			r->classId = cls;
			r->inst = inst;
			r->used = 1;
			r->truncated = 0;
			r->written = 0;
			r->tbl_head = MIB_TBL_NONE;
			r->tbl_count = 0;
			for (int j = 0; j < MIB_ROW_MAX; j++)
				r->data[j] = r->prev[j] = 0;
			return r;
		}
	return 0;
}

void mib_del(uint16_t cls, uint16_t inst)
{
	struct mib_row *r = mib_find(cls, inst);

	if (r) {
		tbl_free(r);
		r->used = 0;
	}
}

/* The i-th slot of the store, for callers that walk a class: 0 for a slot
 * that holds no row. A free slot keeps the class and data of the row it
 * last held, so handing it out made a deleted entity -- or, after a MIB
 * reset, the whole previous MIB -- part of every rebuild again. */
struct mib_row *mib_row_at(int i)
{
	return (i >= 0 && i < MIB_ROWS && mib[i].used) ? &mib[i] : 0;
}

/* Every managed entity this ONU holds, for the CLI: the ones it creates for
 * itself (omci_autonomous[], which a MIB upload reports) and the ones the OLT
 * created, one entry each, sorted by class and then instance.
 *
 * The store alone is not the MIB. An autonomous entity has a row only once
 * the OLT sets one of its attributes, and most never are -- SWImage, ONT2-G,
 * the T-CONTs of an OLT that assigns Alloc-IDs by PLOAM -- so a dump that
 * walked the rows answered "0 rows" for exactly the entities a user looks
 * up first. `cls` 0 is every class; `want_inst` limits it to one instance.
 * Returns the count; the entries stay valid until the next call. */
static struct mib_ent ents[MIB_ENTS_MAX];

static void ent_add(int *n, uint16_t cls, uint16_t inst)
{
	int i;

	for (i = 0; i < *n; i++)
		if (ents[i].cls == cls && ents[i].inst == inst)
			return;
	if (*n >= MIB_ENTS_MAX)
		return;
	/* Insertion sort: a few hundred entries, once per CLI request. */
	for (i = *n; i > 0 && (ents[i - 1].cls > cls ||
			       (ents[i - 1].cls == cls && ents[i - 1].inst > inst)); i--)
		ents[i] = ents[i - 1];
	ents[i].cls = cls;
	ents[i].inst = inst;
	(*n)++;
}

int mib_entities(uint16_t cls, int want_inst, uint16_t inst,
		 const struct mib_ent **out)
{
	int n = 0;

	for (unsigned i = 0; i < omci_autonomous_count; i++) {
		const struct omci_instance *e = &omci_autonomous[i];

		if ((cls && e->classId != cls) || (want_inst && e->inst != inst))
			continue;
		ent_add(&n, e->classId, e->inst);
	}
	for (int i = 0; i < MIB_ROWS; i++) {
		if (!mib[i].used || (cls && mib[i].classId != cls) ||
		    (want_inst && mib[i].inst != inst))
			continue;
		ent_add(&n, mib[i].classId, mib[i].inst);
	}
	*out = ents;
	return n;
}

/* One entity as the OLT would read it: every attribute through attr_value(),
 * so what the OLT wrote wins and everything else is the built-in answer (the
 * defaults, the identity, ONT data's MIB data sync) -- the same bytes a Get
 * returns, where the row itself holds zeros for every attribute nobody
 * wrote. The table attribute, `written` and `truncated` come from the row
 * when there is one. The result is one static row, valid until the next
 * call; the renderers of show.c take it exactly as they take a stored row. */
const struct mib_row *mib_view(const struct omci_class *c, uint16_t inst)
{
	static struct mib_row v;
	const struct mib_row *r = mib_find(c->classId, inst);
	uint8_t tmp[MIB_ROW_MAX];

	for (unsigned i = 0; i < sizeof v; i++)
		((uint8_t *)&v)[i] = 0;
	v.classId = c->classId;
	v.inst = inst;
	v.used = 1;
	v.tbl_head = r ? r->tbl_head : MIB_TBL_NONE;
	v.tbl_count = r ? r->tbl_count : 0;
	v.truncated = r ? r->truncated : 0;
	v.written = r ? r->written : 0;
	for (unsigned k = 1; k < c->nattr && k <= 16; k++) {
		uint16_t w = attr_width(c, k);
		int off = attr_offset(c, k);

		if (!w || off < 0 || off + w > MIB_ROW_MAX)
			continue;
		attr_value(c, inst, k, tmp);
		for (uint16_t b = 0; b < w; b++)
			v.data[off + b] = tmp[b];
	}
	return &v;
}

int mib_count(void)
{
	int n = 0;

	for (int i = 0; i < MIB_ROWS; i++)
		n += mib[i].used;
	return n;
}

/* Write the attributes named by `mask` from `src` into the row. Returns the
 * number of bytes consumed, so a caller can tell a short frame from a full
 * one. */
uint16_t mib_write(struct mib_row *r, const struct omci_class *c,
			  uint16_t mask, const uint8_t *src, uint16_t srclen)
{
	uint16_t used = 0;

	/* Snapshot first: the handlers that run after this need the row as it
	 * was, and taking the copy per attribute would miss the ones this Set
	 * does not touch. */
	for (unsigned j = 0; j < MIB_ROW_MAX; j++)
		r->prev[j] = r->data[j];

	for (unsigned k = 1; k <= 16; k++) {
		uint16_t w;
		int off;

		if (!(mask & (1u << (16 - k))))
			continue;
		w = attr_width(c, k);
		if (!w || used + w > srclen)
			break;
		off = attr_offset(c, k);
		if (off < 0 || off + w > MIB_ROW_MAX) {
			r->truncated = 1;
		} else {
			for (uint16_t i = 0; i < w; i++)
				r->data[off + i] = src[used + i];
			r->written |= (uint16_t)(1u << (16 - k));
		}
		/* A table attribute also accumulates. The flat copy above is
		 * still made -- it is what a get of this attribute answers, and
		 * it is what the vendor's own store keeps beside its list. */
		if (k == table_attr(c) && w == MIB_TBL_ENTRY)
			tbl_set(r, src + used);
		used = (uint16_t)(used + w);
	}
	return used;
}

int mib_changed(const struct mib_row *r, const struct omci_class *c, unsigned k)
{
	uint16_t w = attr_width(c, k);
	int off = attr_offset(c, k);

	if (!r || !w || off < 0 || off + w > MIB_ROW_MAX)
		return 0;
	for (uint16_t i = 0; i < w; i++)
		if (r->data[off + i] != r->prev[off + i])
			return 1;
	return 0;
}

/* The Weight mib_PriQ.so's own default row carries. See the longer note beside
 * ds_queue_words: isp1's live weights are not all 1, and which queues get a
 * non-default one is provisioning rather than derivation. */
#define PRIQ_DEFAULT_WEIGHT 1

uint16_t attr_value(const struct omci_class *c, uint16_t inst,
			   unsigned k, uint8_t *out)
{
	uint16_t n = attr_width(c, k);
	struct mib_row *r = mib_find(c->classId, inst);
	int off = attr_offset(c, k);

	for (uint16_t i = 0; i < n; i++)
		out[i] = 0;
	/* An attribute the OLT has written wins: a get after a set has to return
	 * what was set. `written`, not merely `r`, because a row exists as soon
	 * as any one attribute is set and the rest of it is still zero -- the
	 * built-in answers below have to keep serving the attributes nobody
	 * wrote. */
	if (r && (r->written & (1u << (16 - k))) && off >= 0
	    && off + n <= MIB_ROW_MAX) {
		for (uint16_t i = 0; i < n; i++)
			out[i] = r->data[off + i];
		return n;
	}
	/* What the vendor stack answers for the same instance on this device
	 * (generated/omci_defaults.c, from its `omcicli mib get all`): the
	 * capability picture -- ONT2-G with 128 queues, 64 GEM ports,
	 * connectivity 0x7f; ANI-G with 16 T-CONTs -- that an OLT builds its
	 * service model from. Zeros here read as an ONU with nothing to
	 * provision. The identity cases below still
	 * override. */
	for (unsigned d = 0; d < omci_defaults_count; d++) {
		const struct omci_default *df = &omci_defaults[d];
		if (df->cls == c->classId && df->inst == inst && df->attr == k) {
			for (uint16_t i = 0; i < n && i < df->width; i++)
				out[i] = df->data[i];
			break;
		}
	}
	if (c->classId == OMCI_ME_ONU_G) { /* ONU-G */
		serial_refresh(0);
		if (k == 1) { out[0] = serial[0]; out[1] = serial[1];
			      out[2] = serial[2]; out[3] = serial[3]; }
		else if (k == 2) put_str(out, n, (const char *)devid);
		else if (k == 3) for (int i = 0; i < 8; i++) out[i] = serial[i];
	} else if (c->classId == OMCI_ME_ONU2_G) { /* ONU2-G */
		/* Attribute 2 is the OMCC version, per the vendor's own table
		 * and G.988 -- not attribute 3, which is the vendor product
		 * code. The three identity keys override only when reported
		 * (cfgstore.c: report); otherwise the answer is what it always
		 * was, the device id, 0x80 and the captured product code. */
		if (!report.loaded)
			cfg_load_report();
		if (k == 1)
			put_str(out, n, report.on && report.modelLen > 0
				? report.model : (const char *)devid);
		else if (k == 2)
			out[0] = report.on && report.omccVer >= 0
				? (uint8_t)report.omccVer : OMCC_VER_BASELINE;
		else if (k == 3 && report.on && report.productCode >= 0 && n >= 2) {
			out[0] = (uint8_t)(report.productCode >> 8);
			out[1] = (uint8_t)report.productCode;
		}
	} else if (c->classId == OMCI_ME_SOFTWARE_IMAGE) { /* software image */
		if (k == 1) put_str(out, n, report_sw_ver(inst));
		else if (k == 2) out[0] = (inst == 0);  /* is committed */
		else if (k == 3) out[0] = (inst == 0);  /* is active */
		else if (k == 4) out[0] = 1;            /* is valid */
	} else if (c->classId == OMCI_ME_CTC_LOID_AUTH) { /* CTC LOID authentication */
		/* The LOID and its password from the config store (LOID and
		 * LOID_PASSWD in lastgood.xml, both empty on isp1), AuthStatus 0
		 * until the OLT sets it; OperationId is left zero, nothing on
		 * this device says what the vendor puts there. */
		if (k == 2) put_str(out, n, ident.loid);
		else if (k == 3) put_str(out, n, ident.loidPwd);
	} else if (c->classId == OMCI_ME_ONT_DATA) { /* ONT data */
		if (k == 1) out[0] = mib_data_sync;
	} else if (c->classId == OMCI_ME_PRIORITY_QUEUE) { /* priority queue */
		/* All 208 of these are in omci_autonomous[], so a MIB upload
		 * has always reported them -- with every attribute zero, which
		 * tells an OLT that this ONU has 208 identical queues. The
		 * values are known: RelatedPort is derivable from the entity
		 * id (checked against all 208 of isp1's rows) and the rest is
		 * mib_PriQ.so's own default row, plus the two sizes isp1
		 * reports. */
		uint32_t rp = priq_related_port(inst);

		if (k == 1) out[0] = 1;                /* QCfgOpt */
		else if (k == 2 || k == 3) {           /* Max/AllocQSize */
			out[0] = 3276 >> 8; out[1] = 3276 & 0xff;
		} else if (k == 6) {                   /* RelatedPort */
			out[0] = (uint8_t)(rp >> 24); out[1] = (uint8_t)(rp >> 16);
			out[2] = (uint8_t)(rp >> 8);  out[3] = (uint8_t)rp;
		} else if (k == 8) out[0] = PRIQ_DEFAULT_WEIGHT;
		else if (k == 14) { out[0] = 0; out[1] = 0xff; } /* PktDropMaxP */
		else if (k == 15) out[0] = 9;          /* QueueDropWQ */
	} else if (c->classId == OMCI_ME_TCONT && k == 1 && n >= 2) { /* T-CONT */
		/* AllocID, when the OLT has not set it: the PLOAM assignment
		 * bound to this T-CONT, or 0x00FF (apply_qos.c). */
		uint16_t a = tcont_alloc_id(inst, 0);

		out[0] = (uint8_t)(a >> 8);
		out[1] = (uint8_t)a;
	} else if (c->classId == OMCI_ME_TRAFFIC_SCHEDULER) { /* traffic scheduler */
		/* ISP1's sixteen are identical apart from their identity:
		 * TcontPtr is the entity id, SchedulerPtr 0, Policy 2,
		 * PriWeight 0. */
		if (k == 1) { out[0] = (uint8_t)(inst >> 8); out[1] = (uint8_t)inst; }
		else if (k == 3) out[0] = 2;           /* Policy */
	}
	return n;
}

/* `contents` is everything from byte 8 on -- for a get that is the result code,
 * the attribute mask and the values, but a MIB upload response carries a count
 * and no result code at all, so this does not impose one. */
/* ------------------------------------------------------------------- apply
 *
 * Only 21 of the 81 managed entities reach the driver at all, and each does so
 * through wrappers that tools/omci-drv-abi.py has reduced to one command and a
 * payload. This is that map, for the entities whose payload came back whole.
 *
 * It is off unless -a is given. Everything above this point only answers
 * questions; from here the program reconfigures the switch, and it does so on
 * a device whose real configuration belongs to omci_app.
 */
uint32_t row_u32(const struct mib_row *r, const struct omci_class *c, unsigned k)
{
	int off = attr_offset(c, k);
	uint16_t w = attr_width(c, k);
	uint32_t v = 0;

	if (off < 0 || !w || off + w > MIB_ROW_MAX)
		return 0;
	for (uint16_t i = 0; i < w && i < 4; i++)
		v = (v << 8) | r->data[off + i];
	return v;
}
