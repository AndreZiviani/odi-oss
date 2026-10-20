/* The switch driver: omci_drv_call(), the one way omcid reaches it, and the
 * capability block the driver describes the device with.
 *
 * Every driver command goes over the odi_omci netlink transport of the
 * kernel (ODI_OMCI_OP_CMD, answered by odi_switch_cmd()); there is no
 * other OMCI driver path on this image, so a call that gets no reply
 * simply fails. See omcid.h.
 */
#include "omcid.h"

#define NL_POLL_US     2000    /* see nl_open: this paces the CLI too */

/* The command socket is its own, not the one main.c holds for OMCI frames
 * (nl_fd/nl_tid): a reply read on a shared socket could overwrite an OMCI
 * frame that arrived in the same window, and the periodic ONU state poll
 * of metricsd opened that window every few seconds. The kernel replies to
 * the port that sent the request, so two sockets in one process need
 * nothing on the kernel side. Opened on first use, kept for the life of
 * the process.
 */
static long cmd_nl_fd = -1;
static uint32_t cmd_nl_tid;

static int cmd_nl_ensure(void)
{
	if (cmd_nl_fd >= 0)
		return 0;
	/* Port 0 (autobind): the thread id is taken by main.c's socket, and
	 * binding a second socket to it fails every send without a word
	 * (nl_open_pid() in nl.h).
	 */
	cmd_nl_fd = nl_open_pid(0, &cmd_nl_tid, NL_POLL_US);
	if (cmd_nl_fd < 0) {
		out_fmt("omci_drv_call: ODI_OMCI_OP_CMD socket open/bind failed, rc %d\n",
			(long)cmd_nl_fd);
		return -1;
	}
	return 0;
}

/* Whether the transport answers at all, not whether a command succeeded:
 * -1 not tried yet, 1 a reply has been seen, 0 the first call got no reply
 * within its retries, after which every call fails at once instead of
 * paying the timeout again. A command answered with -EOPNOTSUPP leaves it
 * at 1: the transport is up, that one command has no handler.
 */
static int omci_drv_netlink_state = -1;

static int omci_drv_call_netlink(uint32_t cmd, void *buf, uint32_t len)
{
	uint8_t scratch[NLMSG_HDR + NL_CMD_REQ_HDR + NL_CMD_MAX_LEN];
	int status = -1;
	long rc, send_rc = 0;

	if (len > NL_CMD_MAX_LEN || cmd_nl_ensure() != 0)
		return -1;
	rc = nl_cmd_call((int)cmd_nl_fd, cmd_nl_tid, scratch, cmd, buf, len, &status, 5, &send_rc);
	if (rc != 0) {
		if (send_rc < 0) {
			/* The request never left the process. Rare on an
			 * open, bound socket, so it is logged every time.
			 */
			out_fmt("omci_drv_call: ODI_OMCI_OP_CMD send failed, rc %d (cmd %u)\n",
				(long)send_rc, cmd);
		} else if (omci_drv_netlink_state < 0) {
			omci_drv_netlink_state = 0;
			out_fmt("omci_drv_call: no reply to ODI_OMCI_OP_CMD -- every OMCI driver call will fail for the rest of this run\n");
		}
		return -1;
	}
	if (omci_drv_netlink_state != 1) {
		omci_drv_netlink_state = 1;
		out_fmt("omci_drv_call: odi_switch netlink path answering, using it\n");
	}
	return status;
}

#ifdef OMCI_DRV_TRACE
static int ddm_stub_word(unsigned t, uint16_t *w);
static void ddm_stub_alarms(uint8_t *b);

/* The driver-call golden (src/omci/drv-test.sh): every call is logged as
 * "drv <cmd> <len> <args hex>" and succeeds, with no netlink. The two
 * identity reads get fixed answers, so the serial poll stops at once; the
 * once-a-second ONU state poll (command 13) is answered but not logged,
 * since how many of those land in a run depends on the clock, not on the
 * session. */
static int omci_drv_trace(uint32_t cmd, void *buf, uint32_t len)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'T', 0, 0, 0, 1 };
	static const uint8_t devid[8] = { 'R', 'T', 'L', '9', '6', '0', '2', 'C' };
	uint8_t *b = buf;

	if (cmd == DRV_GET_SN || cmd == DRV_GET_DEVID) {
		for (uint32_t i = 0; i < len && i < 8; i++)
			b[i] = cmd == DRV_GET_SN ? sn[i] : devid[i];
		return 0;
	}
	if (cmd == 13)
		return 0;
	out_fmt("drv %u %u ", cmd, len);
	for (uint32_t i = 0; i < len; i++)
		out_hex(b[i], 2);
	out_char('\n');
	/* Command 21 as the kernel answers it (odi_switch_cmd.c cmd_tcont()):
	 * an Alloc-ID that is not in the PLOAM list (-g) is refused. With no
	 * list at all (no -g file) every one is taken, as before. */
	if (cmd == OMCI_TCONT_CMD && len >= 8 && alloc_ids_known()) {
		const uint16_t *ids;
		unsigned n = alloc_ids_assigned(&ids);
		uint32_t a = nl_get32(b + 4);
		int found = 0;

		for (unsigned i = 0; i < n; i++)
			found |= ids[i] == a;
		if (!found) {
			out_fmt("drvtrace: cmd 21 refused, alloc %d not assigned\n", (long)a);
			return -1;
		}
	}
	/* The bridge table as the kernel pages it (odi_switch_cmd.c): the UNI
	 * holds 2 addresses, the PON port 35 (a second page), the CPU port 1,
	 * each 02:00:00:00:<port>:<40 + index>, learned, 294 s old. */
	if (cmd == OMCI_BRIDGE_TBL_CMD && len >= OMCI_BRIDGE_TBL_LEN) {
		struct omci_bridge_tbl *t = (struct omci_bridge_tbl *)buf;
		uint32_t port = t->port, first = t->first;
		uint32_t total = port == 0 ? 2 : port == 2 ? 35 : port == 3 ? 1 : 0;

		t->total = total;
		t->count = 0;
		for (uint32_t i = first; i < total && t->count < OMCI_BRIDGE_TBL_PAGE; i++) {
			uint8_t *e = t->entry + t->count * OMCI_BRIDGE_TBL_ENTRY_LEN;

			e[0] = 0x12; e[1] = 0x64;
			e[2] = 0x02; e[3] = e[4] = e[5] = 0;
			e[6] = (uint8_t)port; e[7] = (uint8_t)(0x40 + i);
			t->count++;
		}
		return 0;
	}
	/* The DDM stub: command 10 answers a fixed SFF-8472 word per type,
	 * after the call is logged (so the log keeps the request as sent).
	 * Rx power 1000 (100 uW, -10 dBm), Tx power 5000 (500 uW, about
	 * -3 dBm), supply 3300 mV, bias, temperature. */
	if (cmd == 10 && len >= 6) {
		static const uint16_t ddm[7] = { 0, 0, 0x1900, 33000, 5000, 5000, 1000 };
		unsigned t = b[3];

		if (t == 7 && len >= 12) {
			ddm_stub_alarms(b);
			return 0;
		}
		if (t < 2 || t > 6)
			return -1;
		uint16_t w = ddm[t];

		ddm_stub_word(t, &w);
		b[4] = (uint8_t)(w >> 8);
		b[5] = (uint8_t)w;
	}
	return 0;
}
#endif

#ifdef OMCI_DRV_TRACE
/* The DDM stub, unlogged, for the polls. The words can be set by a test:
 * "<rx> <tx> <flags>" (decimal; the powers in 0.1 uW, flags the module's
 * alarm byte, A2h 112) in ODI_DDM_STUB_PATH replaces the fixed Rx 1000,
 * Tx 5000 and no flags for this call and the logged one alike. */
#define ODI_DDM_STUB_PATH "/tmp/omcid-ddm"

static int ddm_stub_word(unsigned t, uint16_t *w)
{
	char b[32];
	long fd = sys_open(ODI_DDM_STUB_PATH, O_RDONLY), n;
	unsigned v[3] = { 0, 0, 0 }, k = 0;

	if (fd < 0)
		return -1;
	n = sys_read((int)fd, b, sizeof b - 1);
	sys_close((int)fd);
	for (long i = 0; i < n && k < 3; i++) {
		if (b[i] >= '0' && b[i] <= '9')
			v[k] = v[k] * 10 + (unsigned)(b[i] - '0');
		else if (b[i] == ' ')
			k++;
	}
	if (t == 6)
		*w = (uint16_t)v[0];
	else if (t == 5)
		*w = (uint16_t)v[1];
	else if (t == 7)
		*w = (uint16_t)v[2];
	else
		return -1;
	return 0;
}

/* Selector 7, the module's alarm and warning flags: data[0..7] is A2h
 * 110-117, so the alarm byte 112 is data[2], buffer byte 6. */
static void ddm_stub_alarms(uint8_t *b)
{
	uint16_t w = 0;

	for (unsigned i = 4; i < 12; i++)
		b[i] = 0;
	if (ddm_stub_word(7, &w) == 0)
		b[6] = (uint8_t)w;
}
#endif

int omci_drv_call_quiet(uint32_t cmd, void *buf, uint32_t len)
{
#ifdef OMCI_DRV_TRACE
	static const uint16_t ddm[7] = { 0, 0, 0x1900, 33000, 5000, 5000, 1000 };
	uint8_t *b = buf;
	uint16_t w;

	if (cmd == 10 && len >= 12 && b[3] == 7) {
		ddm_stub_alarms(b);
		return 0;
	}
	if (cmd != 10 || len < 6 || b[3] < 2 || b[3] > 6)
		return cmd == 10 ? -1 : 0;
	w = ddm[b[3]];
	ddm_stub_word(b[3], &w);
	b[4] = (uint8_t)(w >> 8);
	b[5] = (uint8_t)w;
	return 0;
#else
	return omci_drv_call(cmd, buf, len);
#endif
}

int omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
#ifdef OMCI_DRV_TRACE
	return omci_drv_trace(cmd, buf, len);
#endif
	if (omci_drv_netlink_state == 0)
		return -1;
	return omci_drv_call_netlink(cmd, buf, len) == 0 ? 0 : -1;
}

/* The per-port commands take {u32 port; u32 value}, as the two checked
 * against hardware (the port link status and port state reads) answer. */
uint8_t pairbuf[8];

void *pair(uint32_t port, uint32_t value)
{
	nl_put32(pairbuf + 0, port);
	nl_put32(pairbuf + 4, value);
	return pairbuf;
}

/* ------------------------------------------------------------- capabilities
 *
 * The 120-byte answer of driver command 3 (uapi/omci_caps.h): how many GEM
 * flows, T-CONTs and queues the driver has, and which switch port each UNI
 * slot is. Read once, at start, and indexed by the offsets of omci_caps.h.
 */
uint8_t caps[OMCI_CAPS_LEN];

int caps_ok;

int caps_from_arg;       /* -c: supplied, not read from the driver */

int hex_nib(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

uint32_t caps_u32(unsigned off)
{
	if (!caps_ok || off + 4 > sizeof caps)
		return 0;
	return ((uint32_t)caps[off] << 24) | ((uint32_t)caps[off + 1] << 16) |
	       ((uint32_t)caps[off + 2] << 8) | caps[off + 3];
}

uint32_t caps_flows(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_FLOWS);

	return (n == 0 || n > FLOW_MAX) ? FLOW_MAX : n;
}

/* The switch port of a UNI entity: the index of the capability slot whose
 * type is slot_type and whose second byte is the low byte of meId less
 * one. The port is the slot index, not anything stored in the slot.
 * Returns -1 when no slot matches; the stock driver leaves its caller
 * variable untouched then, which is a trap rather than a behaviour.
 */
int uni_switch_port(uint16_t meId, uint8_t slot_type)
{
	uint8_t want = (uint8_t)((meId - 1) & 0xff);

	if (!caps_ok)
		return -1;
	for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
		if (caps[i * 2 + 1] == want && caps[i * 2] == slot_type)
			return (int)i;
	return -1;
}
