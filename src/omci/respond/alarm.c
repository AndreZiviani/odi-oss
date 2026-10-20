/* Alarms and attribute value changes: what the ONU tells the OLT on its own.
 *
 * The stock stack sends two kinds of autonomous message (read out of
 * bin/omci_app and lib/libomci_mib.so of our image):
 *
 *   - an alarm notification (message type 16): the entity's whole 28-byte
 *     alarm bitmap, and an alarm sequence number in the last contents byte,
 *     1 to 255 and never 0 (omci_alarm_notify_xmit);
 *   - an attribute value change (type 17): one message per attribute, its
 *     one-bit mask and then its value (omci_AvcCallback).
 *
 * Both go out with transaction id 0 and no acknowledgement asked.
 *
 * What raises an alarm there, and here:
 *
 *   ANI-G 0 / 1  received power below / above the ANI-G thresholds (attributes
 *                11 and 12, in -0.5 dB steps; 0xff takes the ONU defaults
 *                17 and 18), from the DDM, once a second in O5 -- the stock
 *                threshold monitor (omci_tm_timer_init, timer interval 1).
 *   ANI-G 4 / 5  transmit power below / above 15 and 16 (signed 0.5 dB
 *                steps; 0x81 takes the defaults 19 and 20), the same way.
 *   ANI-G 2 / 3  signal fail / degrade, which the kernel evaluates from the
 *                downstream BER and lists in /proc/odi_gpon/status as
 *                "signal sf|sd <0|1> threshold <x>"; no line, no alarm.
 *   PPTP Eth UNI 0  LAN-LOS: the host link is down. The stock stack takes it
 *                from the link interrupt; here the link bit is read once a
 *                second, which also gives the right answer after a MIB reset
 *                (the stock one stays silent until the link moves again).
 *
 *   ANI-G 6      laser bias current: the module's own alarm flags for the bias
 *                current, high or low (A2h byte 112 bits 3 and 2, read with
 *                the DDM selector 7), against the limits it carries itself.
 *                G.988 gives the ANI-G no bias threshold to set, and the
 *                stock constant for this alarm is not decoded, so the module
 *                is the judge; it clears its flag with its own margin.
 *
 * Not raised: ONU-G 7 (dying gasp; nothing in our kernel raises one).
 *
 * Every alarm is a level, evaluated each second against what the entity's
 * bitmap holds, so the bitmap the OLT reads with Get All Alarms is exactly
 * what it was told. A change is notified unless the stock stack would
 * suppress it: the ONU-G or the entity's circuit pack is locked, the entity's
 * ARC is on, or (PPTP) the port itself is locked. The bitmap changes either
 * way. A received power sitting on a threshold would flap once a second, so
 * an optical alarm clears only 0.5 dB inside its threshold.
 *
 * AVCs are change-driven: once a second, every attribute the model flags as
 * raising an AVC, of every entity the ONU creates itself, is compared with
 * the value last seen, and a change the OLT did not write itself is sent:
 * the PPTP port's operational state as the link moves, the VEIP's and the
 * circuit pack's as the OLT locks and unlocks them, the software image flags
 * after a download, an activate or a commit, an ARC that expired, an
 * identity reread on SIGHUP. Not sent: an attribute the OLT wrote (G.988
 * gives no AVC for those; the stock stack sends one), the AVC per attribute
 * of every entity the stock stack sends right after a MIB reset (the same
 * values are in the MIB upload the OLT does next), and the operational state
 * of an entity the OLT has just created.
 *
 * Bounded: nothing is sent outside O5, and at most NOTIFY_PER_S messages a
 * second; what does not fit waits for the next second (alarms are levels, an
 * AVC compares against what was last sent, so nothing is lost but a value
 * that came and went).
 */
#include "omcid.h"

#define NOTIFY_PER_S     8
#define ALM_MAX          8
#define ALM_BYTES        28
#define AVC_MAX          96
#define AVC_VAL_MAX      25          /* the contents after the mask, less one */
#define ARC_MAX          8
#define OPT_HYST         250         /* 0.5 dB in 0.002 dB steps */

#define ALM_ANIG_LOW_RX   0
#define ALM_ANIG_HIGH_RX  1
#define ALM_ANIG_SF       2
#define ALM_ANIG_SD       3
#define ALM_ANIG_LOW_TX   4
#define ALM_ANIG_HIGH_TX  5
#define ALM_ANIG_BIAS     6
#define ALM_UNI_LAN_LOS   0

#define ANIG_ARC          8
#define ANIG_LOW_RX_THR   11
#define ANIG_UPP_RX_THR   12
#define ANIG_LOW_TX_THR   15
#define ANIG_UPP_TX_THR   16
#define ANIG_LOW_RX_DFL   17         /* the ONU default each one selects */
#define ANIG_RX_THR_DEFAULT 0xff
#define ANIG_TX_THR_DEFAULT 0x81
#define UNI_ADMIN         5
#define UNI_ARC           12
#define CP_ADMIN          6
#define ONUG_ADMIN        7
#define ADMIN_LOCKED      1

struct alm {
	uint16_t cls, inst;
	uint8_t bits[ALM_BYTES];
	uint8_t pending;             /* changed, not yet notified */
};

struct avc {
	uint16_t cls, inst;
	uint8_t attr, len;
	uint8_t val[AVC_VAL_MAX];
};

struct arc {
	uint16_t cls, inst;
	uint8_t attr;                /* the ARC attribute; the interval follows */
	uint8_t armed;
	long deadline_s;             /* 0: never (interval 255) */
};

static struct alm alm[ALM_MAX];
static unsigned alm_n;
static struct avc avc[AVC_MAX];
static unsigned avc_n;
static struct arc arc[ARC_MAX];
static unsigned arc_n;
static uint8_t alarm_seq = 1;
static int was_o5;
static unsigned sent_this_s;
static uint8_t ntxbuf[2048];
static uint8_t nframe[OMCI_FRAME_LEN];

/* Get All Alarms: the entities that had an alarm when the OLT asked. */
static struct { uint16_t cls, inst; uint8_t bits[ALM_BYTES]; } snap[ALM_MAX];
static unsigned snap_n;

unsigned long notify_sent;

static long now_s(void)
{
	long s = 0, ns = 0;

	sys_clock_gettime(CLOCK_MONOTONIC, &s, &ns);
	return s;
}

/* One autonomous message: transaction id 0, no AR, the baseline device id.
 * Printed to the log as "=> <frame>" too, the way a reply without a line
 * side is printed as "-> ". */
static void notify_send(uint8_t mt, uint16_t cls, uint16_t inst,
			const uint8_t *contents)
{
	for (unsigned i = 0; i < OMCI_FRAME_LEN; i++)
		nframe[i] = 0;
	nframe[2] = mt;
	nframe[3] = OMCI_DEVICE_ID;
	nl_put16(nframe + 4, cls);
	nl_put16(nframe + 6, inst);
	for (unsigned i = 0; i < 32; i++)
		nframe[8 + i] = contents[i];
	nl_put32(nframe + 40, OMCI_TRAILER);
	nl_put32(nframe + 44, omci_crc(nframe, 44));
	sent_this_s++;
	notify_sent++;
	/* Logged either way: these are rare, and omcid.log is where anyone
	 * checks what the OLT was told. */
	out("=> ");
	for (unsigned i = 0; i < OMCI_FRAME_LEN; i++)
		out_hex(nframe[i], 2);
	out_char('\n');
	if (nl_fd >= 0)
		nl_send_frame(nl_fd, nl_tid, ntxbuf, nframe, OMCI_FRAME_LEN);
}

static uint8_t attr_u8(uint16_t cls, uint16_t inst, unsigned k)
{
	const struct omci_class *c = find_class(cls);
	uint8_t b[32];

	if (!c || attr_width(c, k) != 1)
		return 0;
	attr_value(c, inst, k, b);
	return b[0];
}

/* The stock omci_is_notify_suppressed_by_circuitpack: the ONU-G locked, or
 * the circuit pack of the slot locked (`slot` 0: the ONU-G alone). */
static int suppressed_by_lock(uint8_t slot)
{
	if (attr_u8(OMCI_ME_ONU_G, 0, ONUG_ADMIN) == ADMIN_LOCKED)
		return 1;
	if (slot && mib_find(OMCI_ME_CIRCUIT_PACK, (uint16_t)(0x0100 | slot)) &&
	    attr_u8(OMCI_ME_CIRCUIT_PACK, (uint16_t)(0x0100 | slot), CP_ADMIN) ==
	    ADMIN_LOCKED)
		return 1;
	return 0;
}

static int alarm_suppressed(const struct alm *a)
{
	if (suppressed_by_lock((uint8_t)(a->inst >> 8)))
		return 1;
	if (a->cls == OMCI_ME_ANI_G)
		return attr_u8(a->cls, a->inst, ANIG_ARC) == 1;
	if (a->cls == OMCI_ME_PPTP_ETH_UNI)
		return attr_u8(a->cls, a->inst, UNI_ARC) == 1 ||
		       attr_u8(a->cls, a->inst, UNI_ADMIN) == ADMIN_LOCKED;
	return 0;
}

static int bit_get(const struct alm *a, unsigned n)
{
	return (a->bits[n / 8] >> (7 - n % 8)) & 1;
}

static void bit_put(struct alm *a, unsigned n, int on)
{
	uint8_t m = (uint8_t)(0x80u >> (n % 8));

	if (on < 0 || bit_get(a, n) == !!on)
		return;
	if (on)
		a->bits[n / 8] |= m;
	else
		a->bits[n / 8] &= (uint8_t)~m;
	a->pending = 1;
}

/* ------------------------------------------------------------- the inputs */

/* The SFF-8472 word of a DDM type, as omcimsg.c reads it, through the call
 * the driver-call trace does not log (the poll runs every second). */
static int ddm_poll_dbm(unsigned type, int32_t *out)
{
	uint8_t buf[36];
	uint32_t w;
	int32_t v;

	for (unsigned i = 0; i < sizeof buf; i++)
		buf[i] = 0;
	buf[3] = (uint8_t)type;
	if (omci_drv_call_quiet(10, buf, sizeof buf) != 0)
		return -1;
	w = ((uint32_t)buf[4] << 8) | buf[5];
	v = db_x500_of(w) - 20000;           /* 0.1 uW to dBm */
	*out = v < -32768 ? -32768 : v;
	return 0;
}

/* The module's laser bias alarm: 1 when A2h byte 112 says the bias current
 * is above its high alarm limit (bit 3) or under its low one (bit 2), 0 when
 * neither, -1 when the flags cannot be read (nothing then changes). The
 * answer of DDM selector 7 holds A2h 110-117 from buffer byte 4, so byte
 * 112 is byte 6. */
#define DDM_ALARM_STATUS      7
#define A2_ALARM_BYTE         6
#define A2_ALARM_BIAS_HIGH    0x08
#define A2_ALARM_BIAS_LOW     0x04

static int ddm_poll_bias_alarm(void)
{
	uint8_t buf[36];

	for (unsigned i = 0; i < sizeof buf; i++)
		buf[i] = 0;
	buf[3] = DDM_ALARM_STATUS;
	if (omci_drv_call_quiet(10, buf, sizeof buf) != 0)
		return -1;
	return (buf[A2_ALARM_BYTE] & (A2_ALARM_BIAS_HIGH | A2_ALARM_BIAS_LOW)) != 0;
}

/* "signal sf <0|1> threshold <x>" in /proc/odi_gpon/status: 1 set, 0
 * clear, -1 no such line (a kernel without the evaluation). */
static int signal_state(const char *buf, int n, char which)
{
	static const char key[] = "signal s";

	for (int i = 0; i + 11 <= n; i++) {
		int j = 0;

		if (i && buf[i - 1] != '\n')
			continue;
		while (key[j] && buf[i + j] == key[j])
			j++;
		if (key[j] || buf[i + j] != which || buf[i + j + 1] != ' ')
			continue;
		if (buf[i + j + 2] == '1')
			return 1;
		if (buf[i + j + 2] == '0')
			return 0;
	}
	return -1;
}

/* ANI-G: the optical thresholds as stock evaluates them, in 0.002 dB. */
static void eval_anig(struct alm *a, const char *proc, int pn)
{
	int32_t rx, tx;
	uint8_t lo = attr_u8(a->cls, a->inst, ANIG_LOW_RX_THR);
	uint8_t hi = attr_u8(a->cls, a->inst, ANIG_UPP_RX_THR);
	uint8_t tlo = attr_u8(a->cls, a->inst, ANIG_LOW_TX_THR);
	uint8_t thi = attr_u8(a->cls, a->inst, ANIG_UPP_TX_THR);

	if (lo == ANIG_RX_THR_DEFAULT)
		lo = attr_u8(a->cls, a->inst, ANIG_LOW_RX_DFL);
	if (hi == ANIG_RX_THR_DEFAULT)
		hi = attr_u8(a->cls, a->inst, ANIG_LOW_RX_DFL + 1);
	if (tlo == ANIG_TX_THR_DEFAULT)
		tlo = attr_u8(a->cls, a->inst, ANIG_LOW_RX_DFL + 2);
	if (thi == ANIG_TX_THR_DEFAULT)
		thi = attr_u8(a->cls, a->inst, ANIG_LOW_RX_DFL + 3);
	if (ddm_poll_dbm(6, &rx) == 0) {
		int32_t l = -(int32_t)lo * 250, h = -(int32_t)hi * 250;

		bit_put(a, ALM_ANIG_LOW_RX, rx < l ? 1 :
			rx >= l + OPT_HYST ? 0 : -1);
		bit_put(a, ALM_ANIG_HIGH_RX, rx > h ? 1 :
			rx <= h - OPT_HYST ? 0 : -1);
	}
	if (ddm_poll_dbm(5, &tx) == 0) {
		int32_t l = (int32_t)(int8_t)tlo * 250, h = (int32_t)(int8_t)thi * 250;

		bit_put(a, ALM_ANIG_LOW_TX, tx < l ? 1 :
			tx >= l + OPT_HYST ? 0 : -1);
		bit_put(a, ALM_ANIG_HIGH_TX, tx > h ? 1 :
			tx <= h - OPT_HYST ? 0 : -1);
	}
	bit_put(a, ALM_ANIG_BIAS, ddm_poll_bias_alarm());
	bit_put(a, ALM_ANIG_SF, signal_state(proc, pn, 'f'));
	bit_put(a, ALM_ANIG_SD, signal_state(proc, pn, 'd'));
}

static void eval_uni(struct alm *a)
{
	int link = uni_link_state();

	if (link >= 0)
		bit_put(a, ALM_UNI_LAN_LOS, !link);
}

/* ------------------------------------------------------------------ ARC */

/* Each entity whose class has an ARC (alarm reporting control) attribute,
 * followed by its interval in minutes. */
static const struct { uint16_t cls; uint8_t attr; } arc_attrs[] = {
	{ OMCI_ME_ANI_G, ANIG_ARC },
	{ OMCI_ME_PPTP_ETH_UNI, UNI_ARC },
	{ 5, 8 },                                    /* cardholder */
};

/* ARC on starts a one-shot timer of the interval times 60 s (the stock
 * omci_arc_timer_processor); at its end ARC goes back off and an AVC says
 * so. 255 is "inhibited indefinitely" in G.988 and never ends here; the
 * stock stack counts 255 minutes. An ARC this process finds already on (a
 * respawn) gets a full interval from now. */
static void arc_tick(long t)
{
	for (unsigned i = 0; i < arc_n; i++) {
		struct arc *x = &arc[i];
		const struct omci_class *c = find_class(x->cls);
		struct mib_row *r = mib_find(x->cls, x->inst);
		uint8_t on = attr_u8(x->cls, x->inst, x->attr);
		uint8_t iv = attr_u8(x->cls, x->inst, (unsigned)x->attr + 1);
		uint8_t body[32];
		int off;

		if (!on) {
			x->armed = 0;
			continue;
		}
		if (!x->armed) {
			x->armed = 1;
			x->deadline_s = iv == 255 ? 0 : t + (long)iv * 60;
			continue;
		}
		if (!x->deadline_s || t < x->deadline_s || !c || !r ||
		    (off = attr_offset(c, x->attr)) < 0 || off >= MIB_ROW_MAX)
			continue;
		if (sent_this_s >= NOTIFY_PER_S)
			return;
		r->data[off] = 0;
		x->armed = 0;
		for (unsigned b = 0; b < sizeof body; b++)
			body[b] = 0;
		nl_put16(body, (uint16_t)(1u << (16 - x->attr)));
		notify_send(17, x->cls, x->inst, body);
		snapshot_save();
		out_fmt("   ARC interval over: class %d inst %04x, ARC off\n",
			(long)x->cls, (long)x->inst);
	}
}

/* ------------------------------------------------------------------ AVC */

/* The current value of one AVC attribute into v; its width, 0 if none. */
static uint8_t avc_value(const struct avc *e, uint8_t *v)
{
	const struct omci_class *c = find_class(e->cls);
	uint8_t b[64];
	uint16_t w;

	if (!c)
		return 0;
	w = attr_width(c, e->attr);
	if (!w || w > AVC_VAL_MAX)
		return 0;
	attr_value(c, e->inst, e->attr, b);
	for (uint16_t i = 0; i < w; i++)
		v[i] = b[i];
	return (uint8_t)w;
}

/* The slot whose circuit pack can silence an entity: the cardholder and
 * circuit pack are their own (0x01ss), a port entity is in slot ss of its
 * id ss01, and the ONU-wide entities have none (the ONU-G alone). */
static uint8_t avc_slot(const struct avc *e)
{
	if (e->cls == 5 || e->cls == OMCI_ME_CIRCUIT_PACK)
		return (uint8_t)e->inst;
	if (e->cls == OMCI_ME_SOFTWARE_IMAGE || e->cls == OMCI_ME_ONU_G ||
	    e->cls == OMCI_ME_ONU2_G || (e->inst >> 8) == 0)
		return 0;
	return (uint8_t)(e->inst >> 8);
}

static int avc_olt_written(const struct avc *e)
{
	const struct mib_row *r = mib_find(e->cls, e->inst);

	return r && (r->written & (1u << (16 - e->attr)));
}

/* Take every value as it stands, sending nothing: at start, on reaching O5
 * and after a MIB reset. */
static void avc_baseline(void)
{
	for (unsigned i = 0; i < avc_n; i++)
		avc[i].len = avc_value(&avc[i], avc[i].val);
}

static void avc_tick(void)
{
	for (unsigned i = 0; i < avc_n; i++) {
		struct avc *e = &avc[i];
		uint8_t v[AVC_VAL_MAX], body[32];
		uint8_t n = avc_value(e, v);
		int same = n == e->len;

		for (uint8_t b = 0; same && b < n; b++)
			same = v[b] == e->val[b];
		if (same)
			continue;
		/* The OLT's own write, or a change nobody may hear about:
		 * remembered, not sent. */
		if (!avc_olt_written(e) &&
		    !suppressed_by_lock(avc_slot(e)) &&
		    !(e->cls == OMCI_ME_PPTP_ETH_UNI &&
		      attr_u8(e->cls, e->inst, UNI_ADMIN) == ADMIN_LOCKED)) {
			if (sent_this_s >= NOTIFY_PER_S)
				return;
			for (unsigned b = 0; b < sizeof body; b++)
				body[b] = 0;
			nl_put16(body, (uint16_t)(1u << (16 - e->attr)));
			for (uint8_t b = 0; b < n; b++)
				body[2 + b] = v[b];
			notify_send(17, e->cls, e->inst, body);
		}
		e->len = n;
		for (uint8_t b = 0; b < n; b++)
			e->val[b] = v[b];
	}
}

/* --------------------------------------------------------------- the API */

void alarm_init(void)
{
	alm_n = avc_n = arc_n = 0;
	for (unsigned i = 0; i < aut_count; i++) {
		uint16_t cls = aut[i].classId, inst = aut[i].inst;
		const struct omci_class *c = find_class(cls);

		if ((cls == OMCI_ME_ANI_G || cls == OMCI_ME_PPTP_ETH_UNI) &&
		    alm_n < ALM_MAX) {
			struct alm *a = &alm[alm_n++];

			a->cls = cls;
			a->inst = inst;
			for (unsigned b = 0; b < ALM_BYTES; b++)
				a->bits[b] = 0;
			a->pending = 0;
		}
		for (unsigned k = 0; k < sizeof arc_attrs / sizeof arc_attrs[0]; k++)
			if (arc_attrs[k].cls == cls && arc_n < ARC_MAX) {
				arc[arc_n].cls = cls;
				arc[arc_n].inst = inst;
				arc[arc_n].attr = arc_attrs[k].attr;
				arc[arc_n].armed = 0;
				arc_n++;
			}
		for (unsigned k = 1; c && k < c->nattr && k <= 16; k++)
			if (c->attrs[k].avc && avc_n < AVC_MAX) {
				avc[avc_n].cls = cls;
				avc[avc_n].inst = inst;
				avc[avc_n].attr = (uint8_t)k;
				avc_n++;
			}
	}
	avc_baseline();
	was_o5 = 0;
}

/* MIB reset: every alarm bitmap starts clear, as the stock stack recreates
 * the tables, and the sequence number starts again at 1 (G.988; the stock
 * stack resets it on Get All Alarms only). The conditions that still hold
 * are declared again at the next evaluation. The AVC values are taken as
 * they now stand, without the stock flood. The kernel's SF/SD thresholds go
 * back to their defaults with the ANI-G's, as the stock ANI-G create sends
 * them (AnigDrvCfg). */
void alarm_mib_reset(void)
{
	for (unsigned i = 0; i < alm_n; i++) {
		for (unsigned b = 0; b < ALM_BYTES; b++)
			alm[i].bits[b] = 0;
		alm[i].pending = 0;
	}
	for (unsigned i = 0; i < arc_n; i++)
		arc[i].armed = 0;
	snap_n = 0;
	alarm_snapshot = 0;
	alarm_seq = 1;
	avc_baseline();
	anig_signal_thresholds(5, 9);
}

/* Driver command 11: {4, SF exponent} and {5, SD exponent}. */
void anig_signal_thresholds(int sf, int sd)
{
	if (!apply_hw)
		return;
	if (sf >= 0)
		out_fmt("   [hw] signal fail threshold %d -> %d\n", (long)sf,
			(long)omci_setSignalParameter(pair(4, (uint32_t)sf)));
	if (sd >= 0)
		out_fmt("   [hw] signal degrade threshold %d -> %d\n", (long)sd,
			(long)omci_setSignalParameter(pair(5, (uint32_t)sd)));
}

void alarm_tick(void)
{
	char proc[2048];
	long fd, n = 0;
	int st = onu_state_now();
	long t = now_s();

	sent_this_s = 0;
	if (st != 5) {
		was_o5 = 0;
		return;
	}
	if (!was_o5) {
		was_o5 = 1;
		avc_baseline();
	}
	fd = sys_open(gpon_proc_path, O_RDONLY);
	if (fd >= 0) {
		n = sys_read((int)fd, proc, sizeof proc);
		sys_close((int)fd);
		if (n < 0)
			n = 0;
	}
	/* AVCs first: the stock stack sends the operational state of a port
	 * before the LAN-LOS it goes with (the handler's MIB_Set runs the AVC
	 * callback before the notification goes out). */
	arc_tick(t);
	avc_tick();
	for (unsigned i = 0; i < alm_n; i++) {
		struct alm *a = &alm[i];
		uint8_t body[32];

		if (a->cls == OMCI_ME_ANI_G)
			eval_anig(a, proc, (int)n);
		else
			eval_uni(a);
		if (!a->pending)
			continue;
		if (alarm_suppressed(a)) {
			a->pending = 0;
			continue;
		}
		if (sent_this_s >= NOTIFY_PER_S)
			continue;
		for (unsigned b = 0; b < sizeof body; b++)
			body[b] = b < ALM_BYTES ? a->bits[b] : 0;
		body[31] = alarm_seq;
		alarm_seq = (uint8_t)(alarm_seq == 255 ? 1 : alarm_seq + 1);
		a->pending = 0;
		notify_send(16, a->cls, a->inst, body);
	}
}

/* Get All Alarms: the sequence number back to 1, and a snapshot of every
 * entity with an alarm, whatever its ARC (the stock stack ignores the
 * retrieval mode too). Returns the count. A new Get All Alarms replaces the
 * snapshot; the stock stack refuses one for 60 s instead. */
uint16_t alarm_get_all(void)
{
	snap_n = 0;
	for (unsigned i = 0; i < alm_n; i++) {
		int any = 0;

		for (unsigned b = 0; b < ALM_BYTES; b++)
			any |= alm[i].bits[b];
		if (!any)
			continue;
		snap[snap_n].cls = alm[i].cls;
		snap[snap_n].inst = alm[i].inst;
		for (unsigned b = 0; b < ALM_BYTES; b++)
			snap[snap_n].bits[b] = alm[i].bits[b];
		snap_n++;
	}
	alarm_seq = 1;
	alarm_snapshot = (uint16_t)snap_n;
	return (uint16_t)snap_n;
}

/* Get All Alarms Next `seq`: class, instance and bitmap into body (32
 * bytes); -1 past the end of the snapshot. */
int alarm_get_next(uint16_t seq, uint8_t *body)
{
	if (seq >= snap_n)
		return -1;
	nl_put16(body, snap[seq].cls);
	nl_put16(body + 2, snap[seq].inst);
	for (unsigned b = 0; b < ALM_BYTES; b++)
		body[4 + b] = snap[seq].bits[b];
	return 0;
}
