/* OMCI frames: the OLT's questions and our answers.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"

uint8_t rxbuf[2048];

static uint8_t txbuf[2048];

uint8_t frame[OMCI_FRAME_LEN];

/* The netlink socket this process answers on, and the transaction id it
 * registered with. Set once, before the main loop.
 *
 * volatile because the signal handler and the main loop both touch them. Not
 * sig_atomic_t: <signal.h> is not one of the freestanding headers, so the type
 * is not available here. Both are word-sized, which on MIPS is what the
 * guarantee amounts to anyway.
 *
 * They are file-scope rather than main's locals because a frame can arrive by
 * two routes -- off the line, and injected on the message queue -- and the
 * queue path has no fd of its own to answer through. */
volatile int nl_fd = -1;

volatile uint32_t nl_tid;

static void send_resp(int fd, uint32_t tid, const uint8_t *req,
		      const uint8_t *contents, uint16_t clen)
{
	uint16_t i;

	for (i = 0; i < OMCI_FRAME_LEN; i++)
		frame[i] = 0;
	frame[0] = req[0];                       /* echo the transaction id */
	frame[1] = req[1];
	frame[2] = (uint8_t)(OMCI_MT(req[2]) | OMCI_AK);
	frame[3] = OMCI_DEVICE_ID;
	frame[4] = req[4];                       /* echo class and instance */
	frame[5] = req[5];
	frame[6] = req[6];
	frame[7] = req[7];
	for (i = 0; i < clen && 8 + i < 40; i++)
		frame[8 + i] = contents[i];
	nl_put32(frame + 40, OMCI_TRAILER);
	nl_put32(frame + 44, omci_crc(frame, 44));
	if (fd < 0) {
		/* No line side -- an injected frame on a bench, or qemu. The
		 * answer is the thing being tested, so print it rather than
		 * writing it to a socket that does not exist. */
		out("-> ");
		for (i = 0; i < OMCI_FRAME_LEN; i++)
			out_hex(frame[i], 2);
		out_char('\n');
		return;
	}
	nl_send_frame(fd, tid, txbuf, frame, OMCI_FRAME_LEN);
}


/* 10*log10(x) in units of 0.002 dB, integer only (this program has no libm):
 * log2 from the top bit plus a 33-point table for the mantissa, then the
 * 3.0103 dB per octave. Good to about 0.05 dB, far below what an OLT shows. */
static int32_t db_x500_of(uint32_t x)
{
	static const uint16_t frac[33] = {   /* 500*10*log10(1 + k/32) */
		0, 67, 133, 197, 260, 320, 380, 438, 494, 549, 603, 656, 707,
		758, 807, 855, 902, 949, 994, 1038, 1082, 1124, 1166, 1207,
		1248, 1287, 1326, 1365, 1402, 1439, 1476, 1512, 1547 };
	int e = 0;
	uint32_t m;

	if (x == 0)
		return -30000;                   /* -60 dB: nothing measurable */
	while (x >> (e + 1))
		e++;
	m = e >= 5 ? (x >> (e - 5)) & 31 : (x << (5 - e)) & 31;
	return e * 1505 + frac[m];               /* 3.0103 dB * 500 per octave */
}

/* The SFF-8472 word the driver holds for a DDM type (omcidrv command 10,
 * {type, data[32]}, big-endian word at data[0]), 0 when it cannot be read. */
static uint32_t ddm_word(unsigned type)
{
	uint8_t buf[36];

	for (unsigned i = 0; i < sizeof buf; i++)
		buf[i] = 0;
	buf[3] = (uint8_t)type;
	if (omci_getTransceiverStatus(buf) != 0)
		return 0;
	return ((uint32_t)buf[4] << 8) | buf[5];
}

/* Test (18) on ANI-G: the OLT asks for the optical measurements and shows
 * them as the ONU's line values. The vendor answers, and this OLT stopped its
 * provisioning at the unanswered Test. Reply
 * with a Test response, then the autonomous Test Result (27, same TCI) in
 * the ANI-G layout of G.988 I.1: type/value pairs, 1 power feed voltage in
 * 20 mV, 3 received optical power and 5 mean launch power in 0.002 dB(uW),
 * 9 laser bias in 2 uA, 12 temperature in 1/256 C. */
static void handle_test(int fd, uint32_t tid, const uint8_t *f, uint16_t cls)
{
	uint8_t body[16];
	uint32_t volt = ddm_word(3), rx = ddm_word(6), tx = ddm_word(5);
	uint32_t bias = ddm_word(4), temp = ddm_word(2);
	int32_t rxdb, txdb;
	uint16_t i;

	body[0] = OMCI_OK;
	send_resp(fd, tid, f, body, 1);
	if (cls != OMCI_ME_ANI_G) {
		out("-> test accepted (no result for this class)\n");
		return;
	}
	/* SFF-8472 power words are 0.1 uW; dB(uW) needs uW, so 10 log10 of
	 * the word minus 10 dB (5000 in these units). */
	rxdb = db_x500_of(rx) - 5000;
	txdb = db_x500_of(tx) - 5000;
	for (i = 0; i < OMCI_FRAME_LEN; i++)
		frame[i] = 0;
	frame[0] = f[0];
	frame[1] = f[1];
	frame[2] = OMCI_MT_TEST_RESULT;          /* autonomous: no AR, no AK */
	frame[3] = OMCI_DEVICE_ID;
	frame[4] = f[4]; frame[5] = f[5]; frame[6] = f[6]; frame[7] = f[7];
	frame[8] = 1;  frame[9]  = (uint8_t)((volt / 200) >> 8); frame[10] = (uint8_t)(volt / 200);
	frame[11] = 3; frame[12] = (uint8_t)((uint16_t)rxdb >> 8); frame[13] = (uint8_t)rxdb;
	frame[14] = 5; frame[15] = (uint8_t)((uint16_t)txdb >> 8); frame[16] = (uint8_t)txdb;
	frame[17] = 9; frame[18] = (uint8_t)(bias >> 8); frame[19] = (uint8_t)bias;
	frame[20] = 12; frame[21] = (uint8_t)(temp >> 8); frame[22] = (uint8_t)temp;
	nl_put32(frame + 40, OMCI_TRAILER);
	nl_put32(frame + 44, omci_crc(frame, 44));
	if (fd >= 0)
		nl_send_frame(fd, tid, txbuf, frame, OMCI_FRAME_LEN);
	out_fmt("-> test result: vcc %d mV, rx %d tx %d (0.002 dBuW), bias %d uA, temp %d/256 C\n",
		(long)(volt / 10), (long)rxdb, (long)txdb, (long)(bias * 2), (long)temp);
}

static void handle_upload_next(int fd, uint32_t tid, const uint8_t *f)
{
	uint8_t body[32];

	uint16_t seq = nl_get16(f + 8);
	const struct omci_instance *e;
	const struct omci_class *ec;
	uint16_t mask = 0, n = 6;

	if ((unsigned)seq >= omci_autonomous_count) {
		int k = seq - (int)omci_autonomous_count, n = 0;

		for (int i = 0; i < MIB_ROWS; i++) {
			if (!mib[i].used)
				continue;
			if (n++ != k)
				continue;
			{
				const struct omci_class *rc = find_class(mib[i].classId);
				uint16_t mask = 0, len = 6;

				nl_put16(body + 0, mib[i].classId);
				nl_put16(body + 2, mib[i].inst);
				for (unsigned a = 1; rc && a < rc->nattr && a <= 16; a++) {
					uint16_t w = attr_width(rc, a);
					int off = attr_offset(rc, a);

					if (!w || off < 0 || len + w > (uint16_t)sizeof body)
						break;
					for (uint16_t b = 0; b < w; b++)
						body[len + b] = mib[i].data[off + b];
					len = (uint16_t)(len + w);
					mask |= (uint16_t)(1u << (16 - a));
				}
				nl_put16(body + 4, mask);
				send_resp(fd, tid, f, body, len);
				return;
			}
		}
	}
	if ((unsigned)seq >= omci_autonomous_count) {
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> sequence %d is past the end\n", (long)seq);
		return;
	}
	e = &omci_autonomous[seq];
	ec = find_class(e->classId);
	nl_put16(body + 0, e->classId);
	nl_put16(body + 2, e->inst);
	/* As many attributes as fit; the rest would need another
	 * response with a different mask, which no entity here has
	 * needed yet. */
	for (unsigned k = 1; ec && k < ec->nattr && k <= 16; k++) {
		uint16_t w = attr_width(ec, k);

		if (n + w > (uint16_t)sizeof body)
		break;
		attr_value(ec, e->inst, k, body + n);
		n = (uint16_t)(n + w);
		mask |= (uint16_t)(1u << (16 - k));
	}
	nl_put16(body + 4, mask);
	send_resp(fd, tid, f, body, n);
	if (seq < 3 || (unsigned)seq + 1 == omci_autonomous_count)
		out_fmt("-> [%d] class %d %s inst %04x mask %04x\n",
		(long)seq, (long)e->classId,
		ec ? ec->name : "?", (long)e->inst, (long)mask);
	return;
}

struct getnext_ctx getnext;

/* How many alarm records the last Get All Alarms promised. Always zero today --
 * nothing here raises an alarm -- but it is what Get All Alarms Next is
 * measured against, so the two cannot drift apart. 13 of the vendor's classes
 * raise alarms. */
static uint16_t alarm_snapshot;

/* A Get of a big attribute: report the byte length and remember the value.
 *
 * The length goes back as four bytes in the attribute's place, which is the
 * one part of this exchange NOT read off the vendor. The Get-Next half
 * below is measured instruction by instruction; this half is G.988's own
 * shape, and it is the first thing to doubt if an OLT refuses the
 * exchange.
 *
 * Only one attribute is answered, and the mask says which: the vendor stops
 * looking at the mask the moment it finds a big attribute, and mixing four
 * bytes of length in among inline attribute values would leave the OLT parsing
 * everything after it at the wrong offset. */
static void handle_get_big(int fd, uint32_t tid, const uint8_t *f,
			   const struct omci_class *c, uint16_t inst,
			   unsigned k)
{
	uint8_t body[32];

	getnext.len = tbl_serialise(c, inst, k, getnext.data, GETNEXT_MAX);
	getnext.classId = c->classId;
	getnext.inst = inst;
	getnext.attr = (uint16_t)k;
	getnext.chunks = (uint16_t)((getnext.len + GETNEXT_CHUNK - 1)
				    / GETNEXT_CHUNK);
	getnext.primed = 1;

	body[0] = OMCI_OK;
	nl_put16(body + 1, (uint16_t)(1u << (16 - k)));
	nl_put32(body + 3, getnext.len);
	send_resp(fd, tid, f, body, 7);
	out_fmt("-> table get, attribute %d, %d bytes in %d chunks\n",
		(long)k, (long)getnext.len, (long)getnext.chunks);
}

/* Get-Next, from the responder at 0x413400 in bin/omci_app.
 *
 * The request carries the attribute mask at contents+0 and the sequence number
 * at contents+2. The vendor checks the class id, the instance AND the attribute
 * index against what the Get cached, and refuses rather than answering another
 * entity's bytes -- an OLT that interleaves two table reads would otherwise get
 * a silently wrong answer. */
static void handle_get_next(int fd, uint32_t tid, const uint8_t *f,
			    const struct omci_class *c, uint16_t cls,
			    uint16_t inst)
{
	uint8_t body[32];
	uint16_t mask = nl_get16(f + 8);
	uint16_t seq = nl_get16(f + 10);
	uint16_t k = 0, off, n;

	for (unsigned i = 1; i <= 16; i++)
		if (mask & (1u << (16 - i))) {
			k = (uint16_t)i;
			break;
		}
	if (!c || !getnext.primed || getnext.classId != cls
	    || getnext.inst != inst || getnext.attr != k) {
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> get-next for class %d inst %04x attr %d, "
			"which is not what the last get primed\n",
			(long)cls, (long)inst, (long)k);
		return;
	}
	if (seq >= getnext.chunks) {
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> get-next %d is past the last chunk (%d)\n",
			(long)seq, (long)getnext.chunks);
		return;
	}
	off = (uint16_t)(seq * GETNEXT_CHUNK);
	/* The last chunk is the remainder, not a padded full one: the vendor
	 * computes len - 29*seq only on the final sequence number. */
	n = (uint16_t)(seq + 1 == getnext.chunks ? getnext.len - off
						 : GETNEXT_CHUNK);
	body[0] = OMCI_OK;
	nl_put16(body + 1, mask);
	for (uint16_t i = 0; i < n; i++)
		body[3 + i] = getnext.data[off + i];
	send_resp(fd, tid, f, body, (uint16_t)(3 + n));
	out_fmt("-> get-next %d of %d, %d bytes\n",
		(long)seq, (long)getnext.chunks, (long)n);
}

unsigned long omci_frames_handled;

void handle(int fd, uint32_t tid, const uint8_t *f)
{
	uint8_t mt = OMCI_MT(f[2]);
	uint16_t cls = nl_get16(f + 4), inst = nl_get16(f + 6);
	const struct omci_class *c = find_class(cls);
	uint8_t body[32];

	omci_frames_handled++;
	if (mt == OMCI_MT_MIB_UPLOAD_NEXT && nl_get16(f + 8) > 2 &&
	    (unsigned)nl_get16(f + 8) + 1 < omci_autonomous_count) {
		handle_upload_next(fd, tid, f);   /* 301 of these; do not log */
		return;
	}
	out_fmt("<- %-16s tci %-5d class %-5d %-22s inst %d\n",
		mt == OMCI_MT_GET ? "get" :
		mt == OMCI_MT_MIB_RESET ? "mib-reset" :
		mt == OMCI_MT_MIB_UPLOAD ? "mib-upload" :
		mt == OMCI_MT_MIB_UPLOAD_NEXT ? "mib-upload-next" :
		mt == OMCI_MT_CREATE ? "create" :
		mt == OMCI_MT_DELETE ? "delete" :
		mt == OMCI_MT_SET ? "set" :
		mt == OMCI_MT_GET_NEXT ? "get-next" :
		mt == OMCI_MT_GET_ALL_ALARMS ? "get-all-alarms" :
		mt == OMCI_MT_GET_ALL_ALARMS_NEXT ? "get-all-alarms-next" :
		mt == OMCI_MT_SYNC_TIME ? "sync-time" :
		mt == OMCI_MT_TEST ? "test" : "?",
		(long)nl_get16(f), (long)cls, c ? c->name : "(unknown)", (long)inst);
	/* Anything unnamed is what phase 4 has to learn, so show the type and
	 * enough of the contents to recognise it -- except where the contents
	 * are the line's own secrets. The tail of this OLT's provisioning sets
	 * the TR-069 ACS credentials into AuthSecMethod and the ACS URL into
	 * LargeString; dumping those puts them in a log file and, from there,
	 * anywhere the log is pasted. The shape is what matters here, not the
	 * value. */
	if (mt != OMCI_MT_GET && mt != OMCI_MT_GET_NEXT &&
	    mt != OMCI_MT_MIB_RESET && mt != OMCI_MT_MIB_UPLOAD &&
	    mt != OMCI_MT_MIB_UPLOAD_NEXT && mt != OMCI_MT_GET_ALL_ALARMS &&
	    mt != OMCI_MT_GET_ALL_ALARMS_NEXT && mt != OMCI_MT_SYNC_TIME) {
		if (cls == OMCI_ME_AUTH_SECURITY_METHOD ||
		    cls == OMCI_ME_LARGE_STRING ||
		    cls == OMCI_ME_TR069_MGMT_SERVER ||
		    cls == OMCI_ME_IP_HOST_CONFIG_DATA) {
			out_fmt("   type %d contents withheld (credentials)\n", (long)mt);
		} else {
			out_fmt("   type %d contents", (long)mt);
			for (int i = 8; i < 24; i++) {
				out_char(' ');
				out_hex(f[i], 2);
			}
			out_char('\n');
		}
	}

	if (!(f[2] & OMCI_AR)) {                 /* nothing to answer */
		out("   (no ack requested)\n");
		return;
	}
	if (mt == OMCI_MT_MIB_RESET) {
		mib_reset_all();
		body[0] = OMCI_OK;
		send_resp(fd, tid, f, body, 1);
		out("-> ok, mib data sync reset\n");
		return;
	}
	if (mt == OMCI_MT_GET_ALL_ALARMS) {
		/* Same shape as MIB upload: a count of the Next commands to
		 * follow, no result byte. Saying zero stops the OLT walking all
		 * 256 alarm entries, which is what it did when this answered
		 * "not supported".
		 *
		 * The vendor takes a snapshot here and hangs a 60-second timer
		 * off it (OMCI_OnGetAllAlarms, 0x412eac), deleting the timer
		 * immediately when the count is zero. Nothing in this daemon
		 * raises an alarm yet, so the count is always zero and the
		 * snapshot is always empty -- but the sequence counter is reset
		 * for the same reason the vendor resets it, so a walk that the
		 * OLT starts anyway is answered consistently. */
		alarm_snapshot = 0;
		body[0] = 0;
		body[1] = 0;
		send_resp(fd, tid, f, body, 2);
		out("-> no alarms\n");
		return;
	}
	if (mt == OMCI_MT_GET_ALL_ALARMS_NEXT) {
		/* Unreachable in a conforming exchange while the count above is
		 * zero, which is why this was allowed to fall through to "not
		 * supported" for so long. It is still the wrong answer: the OLT
		 * asked a question this ME understands and got "I do not know
		 * that command".
		 *
		 * The response is class id, instance id and a 28-byte alarm
		 * bitmap -- 32 bytes, the whole contents field (0x412e14). With
		 * an empty snapshot every sequence number is past the end, and
		 * the vendor's own out-of-range answer is a failure result, not
		 * a zero-filled record. */
		uint16_t seq = nl_get16(f + 8);

		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> get-all-alarms-next %d, but the snapshot holds %d\n",
			(long)seq, (long)alarm_snapshot);
		return;
	}
	if (mt == OMCI_MT_SYNC_TIME) {
		body[0] = OMCI_OK;
		body[1] = 0;                     /* success, no offset */
		send_resp(fd, tid, f, body, 2);
		out("-> time accepted\n");
		return;
	}
	if (mt == OMCI_MT_MIB_UPLOAD) {
		/* A count of the Next commands to follow, no result byte. After
		 * a MIB reset the ONU's MIB is not empty: it holds what the ONU
		 * built for itself, and that is how the OLT learns what
		 * hardware it has. Reporting zero left the OLT with nothing to
		 * work with, and it simply started the cycle again. */
		nl_put16(body, (uint16_t)(omci_autonomous_count + mib_count()));
		send_resp(fd, tid, f, body, 2);
		out_fmt("-> %d managed entities to upload (%d autonomous, %d held)\n",
			(long)(omci_autonomous_count + mib_count()),
			(long)omci_autonomous_count, (long)mib_count());
		return;
	}
	if (mt == OMCI_MT_MIB_UPLOAD_NEXT) {
		handle_upload_next(fd, tid, f);
		return;
	}
	if (mt == OMCI_MT_GET_NEXT) {
		handle_get_next(fd, tid, f, c, cls, inst);
		return;
	}
	if (mt == OMCI_MT_GET && c) {
		uint16_t mask = nl_get16(f + 8);
		uint16_t got = 0;
		uint16_t n = 0;

		/* A big attribute takes the whole message: the vendor breaks
		 * out of the mask walk at the first one it finds and hands the
		 * message to the Get-Next state machine. Doing this before the
		 * inline loop, not inside it, is what keeps the answer's shape
		 * single-valued. */
		for (unsigned k = 1; k <= 16; k++)
			if ((mask & (1u << (16 - k))) && attr_is_big(c, k)) {
				handle_get_big(fd, tid, f, c, inst, k);
				return;
			}

		body[0] = OMCI_OK;
		n = 3;
		for (unsigned k = 1; k <= 16; k++) {
			uint16_t w;

			if (!(mask & (1u << (16 - k))))
				continue;
			w = attr_width(c, k);
			if (!w)
				continue;       /* past nattr, or unsized */
			if (n + w > (uint16_t)sizeof body)
				break;          /* would not fit the frame */
			attr_value(c, inst, k, body + n);
			got |= (uint16_t)(1u << (16 - k));
			n = (uint16_t)(n + w);
		}
		/* The mask that goes back is the one actually answered, not the
		 * one asked for. Echoing the request while skipping an
		 * attribute -- because it is past nattr, unsized, or would not
		 * fit -- tells the OLT bytes are present that are not, and it
		 * then parses every attribute after the gap at the wrong
		 * offset. handle_upload_next has always built its mask this
		 * way; this is the same discipline on the get path. */
		nl_put16(body + 1, got);
		/* NOT OMCI_ERR_ATTR_FAILED, tempting as it is. G.988 result 9
		 * requires an unsupported-attributes mask and a failed-
		 * attributes mask in the last four bytes of the 32-byte
		 * response body, and sending the code without them would leave
		 * the OLT parsing a body that does not match its result. The
		 * corrected mask above is the part that stops it misreading
		 * the attributes; saying 9 properly is separate work. */
		send_resp(fd, tid, f, body, n);
		out_fmt("-> ok, mask %04x", (long)mask);
		if (got != mask)
			out_fmt(" (answered %04x)", (long)got);
		out_fmt(", %d bytes\n", (long)(n - 3));
		return;
	}
	/* Accept what the OLT provisions without acting on it. Nothing here
	 * configures hardware or keeps a MIB: the point is to see the whole
	 * script the line runs, which only appears if each step is
	 * acknowledged. A refusal stops the conversation at that step. */
	if (mt == OMCI_MT_CREATE) {
		struct mib_row *r = c ? mib_add(cls, inst) : 0;
		uint16_t mask;

		if (!r) {
			body[0] = c ? OMCI_ERR_CMD : OMCI_ERR_UNKNOWN_ME;
			send_resp(fd, tid, f, body, 1);
			out(c ? "-> no room in the MIB\n" : "-> unknown entity\n");
			return;
		}
		/* Create carries, in order and with no mask, only the
		 * attributes the OLT may set at creation -- which the model
		 * records as bit 2 of oltAcc. GemPortCtp's UNI counter is
		 * read-only and therefore absent, so taking every attribute in
		 * order shifts everything after it: the port id came out as
		 * 4095 before this. */
		mask = create_mask(c);
		mib_write(r, c, mask, f + 8, 32);
		if (cls == OMCI_ME_GEM_PORT_CTP || cls == OMCI_ME_TCONT) {
			out_fmt("   create mask %04x raw", (long)mask);
			for (int i = 8; i < 28; i++) {
				out_char(' ');
				out_hex(f[i], 2);
			}
			out_char('\n');
			out_fmt("   parsed port %d tcont %d dir %d usTM %d dsPQ %d\n",
				(long)row_u32(r, c, 1), (long)row_u32(r, c, 2),
				(long)row_u32(r, c, 3), (long)row_u32(r, c, 4),
				(long)row_u32(r, c, 7));
		}
		body[0] = OMCI_OK;
		body[1] = 0;                     /* attribute execution mask */
		body[2] = 0;
		send_resp(fd, tid, f, body, 3);
		mib_data_sync++;
		apply_entity(c, inst, r, mask, 1);
		out_fmt("-> created (%d rows held)\n", (long)mib_count());
		return;
	}
	if (mt == OMCI_MT_SET) {
		struct mib_row *r = c ? mib_add(cls, inst) : 0;
		uint16_t mask = nl_get16(f + 8);

		if (r)
			mib_write(r, c, mask, f + 10, 30);
		body[0] = r ? OMCI_OK : OMCI_ERR_UNKNOWN_ME;
		send_resp(fd, tid, f, body, 1);
		mib_data_sync++;
		if (r)
			apply_entity(c, inst, r, mask, 0);
		out_fmt("-> set mask %04x%s\n", (long)mask,
			r && r->truncated ? " (row truncated)" : "");
		return;
	}
	if (mt == OMCI_MT_DELETE) {
		/* Before mib_del, so the row is still there for anything that
		 * needs it. Class 45 is the only entity whose delete reaches
		 * the driver at all. */
		apply_delete(c, inst);
		mib_del(cls, inst);
		body[0] = OMCI_OK;
		send_resp(fd, tid, f, body, 1);
		mib_data_sync++;
		out("-> deleted\n");
		return;
	}
	if (mt == OMCI_MT_TEST) {
		handle_test(fd, tid, f, cls);
		return;
	}
	body[0] = c ? OMCI_ERR_CMD : OMCI_ERR_UNKNOWN_ME;
	send_resp(fd, tid, f, body, 1);
	out("-> not supported yet\n");
}
