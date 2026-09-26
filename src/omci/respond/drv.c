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
	return 0;
}
#endif

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
