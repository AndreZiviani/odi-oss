/* What reaches the driver.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"

/* omcid -- answer the OLT's OMCI opening.
 *
 * Phase 3 of replacing omci_app (see ../PLAN.md). Phase 2 captured what the OLT
 * says when an ONU re-registers, and it is short: a MIB reset, then five gets
 * across three managed entities.
 *
 *     mib-reset  class 2    OntData
 *     get        class 256  Ontg     mask 8000 / c000 / 2000
 *     get        class 257  Ont2g    mask a000
 *     get        class 7    SWImage  inst 0 and 1, mask f000
 *     get        class 2    OntData  mask 8000
 *
 * This answers exactly those and logs everything else, so what the OLT does
 * next is observed rather than predicted. It does not touch the MIB, configure
 * anything, or carry traffic -- the question it exists to settle is whether a
 * correct identification is enough for the OLT to proceed to MIB upload.
 *
 * Attribute widths come from generated/omci_mib.c, extracted from the vendor's
 * own plugins, so the response layout is the device's rather than a reading of
 * the standard. Identity comes from the driver at run time -- the serial number
 * is the stick's and does not belong in a source file.
 */
#include "sys.h"
#include "io.h"
#include "../nl.h"
#include "../omci.h"
#include "omci_mib.h"
#include "../omci_tmpfile.h"
#include "omci_autonomous.h"
#include "omci_drv.h"
#include "../omci_gemflow.h"
#include "../omci_caps.h"
#include "../omci_bdgconn.h"
#include "../omci_cli_proto.h"
#include "../omci_msgq.h"

#define NL_POLL_US     2000    /* see nl_open: this paces the CLI too */

/* How long cli_chunk will wait out a slow reader before giving up on it.
 * 100 x 5 ms = half a second, which is already a long time for a daemon whose
 * other job is answering the OLT inside its retry window. */
#define CLI_RETRIES    100
#define CLI_RETRY_NS   (5 * 1000 * 1000)
#define DRV_GET_SN     15
#define DRV_GET_DEVID  4

int apply_hw;           /* -a: actually program the switch */

static int bdgconn_probe_enabled(void)
{
	static int enabled = -1;
	long fd;

	if (enabled >= 0)
		return enabled;
	fd = sys_open("/etc/config/bdgconn-probe", O_RDONLY);
	enabled = fd >= 0;
	if (fd >= 0)
		sys_close((int)fd);
	return enabled;
}

static void bdgconn_probe_mark(const char *path)
{
	static const char mark[] = "1\n";
	long fd;

	if (!bdgconn_probe_enabled())
		return;
	fd = sys_create(path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0600);
	if (fd >= 0) {
		sys_write((int)fd, mark, sizeof mark - 1);
		sys_close((int)fd);
	}
}

static int regtrace_mark_enabled(void)
{
	static int enabled = -1;
	long fd;

	if (enabled >= 0)
		return enabled;
	fd = sys_open("/var/config/regtrace-mark", O_RDONLY);
	enabled = fd >= 0;
	if (fd >= 0)
		sys_close((int)fd);
	return enabled;
}

/* Full-stream capture mode: s14 closed the gap on command brackets and
 * on the plain register file, down
 * to differences that do not matter -- what is left must be either writes
 * the vendor stack makes OUTSIDE any command bracket during provisioning
 * (which armonly, by design, never records at all) or table rows no
 * capture has compared yet. Answering that needs every write from omcid
 * start onward, attributed to a bracket or not, which is exactly what
 * armonly exists to filter out. /var/config/regtrace-all, checked once and
 * cached the same way regtrace_mark_enabled() is, turns that filtering
 * off for this run -- regtrace_clear_once() below still clears the ring
 * and still installs the two skip ranges (the table-access polling noise
 * that has nothing to do with provisioning either way), it just leaves
 * armonly unset. Marks are unaffected: regtrace_mark_enabled() and
 * regtrace_mark() are a separate mechanism, gated on their own file, so a
 * regtrace-all capture still has command brackets in it for tools/
 * regtrace/compare.py's own bracket mode to use alongside --stream. */
static int regtrace_all_enabled(void)
{
	static int enabled = -1;
	long fd;

	if (enabled >= 0)
		return enabled;
	fd = sys_open("/var/config/regtrace-all", O_RDONLY);
	enabled = fd >= 0;
	if (fd >= 0)
		sys_close((int)fd);
	return enabled;
}

/* Writes a fixed "clear" control line to /proc/rtk_regtrace, once per
 * process, right before the very first mark, followed by "armonly 1" (skipped
 * under regtrace-all, see regtrace_all_enabled() above) and two "skip"
 * ranges. On this stick the OLT provisions every ME right after
 * O5 on every boot, and by the time omcid reaches its first command the
 * boot-time rtk_init writes have already filled some 12k of the 16k ring
 * -- those boot writes are captured separately (phase 0), and are not what
 * a command-attribution capture wants competing for ring space against the
 * OMCI commands. Clearing here keeps the ring free for the commands this
 * trace exists to attribute, without moving the clear into rc35/rcS where
 * it would also wipe the boot-phase capture. armonly is needed on top of
 * that: background register writers outside any command bracket (the
 * indirect-table handshake on 0x1d00c is the one seen in practice) run at
 * around 1600 writes/second and fill the whole ring in seconds -- faster
 * than OMCI provisioning even starts -- so without armonly the ring is
 * full of unattributable noise before the first mark's writes land. With
 * armonly on, only writes inside a command bracket (between a before-mark
 * and its matching after-mark) are recorded at all. That alone is still
 * not enough: a single long command can itself be noisy enough to wrap the
 * whole ring before its own after-mark lands -- cmd 51 (MIB reset) on
 * isp1 pushed over 16k writes, more than 16k of them the CPU-port NIC
 * interrupt registers 0x12000/0x12008 (its own IRQ handler firing while
 * the command runs), which wrapped a 16384-entry ring within that single
 * command and evicted its own before-mark before the after-mark was ever
 * written (isp1-260922-boot2.txt: the ring's oldest surviving M entry was
 * cmd 51's after-mark, with no before-mark anywhere in it). The two skip
 * ranges below are that NIC IMR pair and the switch interrupt-mask pair on
 * 0x1d00c/0x1d010 seen the same way in the v1 boot tail; regtrace_record
 * drops anything in either range before it ever reaches the ring, so a
 * long command's real writes no longer compete with its own interrupt
 * noise for ring space -- kept even under regtrace-all, since that noise
 * still has nothing to do with what a full-stream capture is trying to
 * see either. */
static void regtrace_clear_once(void)
{
	static const char clear[] = "clear\n";
	static const char armonly[] = "armonly 1\n";
	static const char skip1[] = "skip 00012000 00012fff\n";
	static const char skip2[] = "skip 0001d00c 0001d010\n";
	static int cleared;
	long fd;

	if (cleared)
		return;
	cleared = 1;
	fd = sys_open("/proc/rtk_regtrace", O_WRONLY);
	if (fd >= 0) {
		sys_write((int)fd, clear, sizeof clear - 1);
		if (!regtrace_all_enabled())
			sys_write((int)fd, armonly, sizeof armonly - 1);
		sys_write((int)fd, skip1, sizeof skip1 - 1);
		sys_write((int)fd, skip2, sizeof skip2 - 1);
		sys_close((int)fd);
	}
}

/* Brackets the ODI_OMCI_OP_CMD netlink call in omci_drv_call_netlink so a
 * regtrace capture (kernel side: the rtk_regtrace capture, kind M) can
 * attribute the writes a command produced to that command. Tag encoding:
 * bit 31 is the phase (0 before the call, 1
 * after), bits 0..30 are the driver command number as passed in --
 * that space is nowhere near 2^31, so the two never collide. decode.py
 * pairs a before/after tag to group the writes between them into one block.
 * Gated on file existence, checked once and cached, so a production image
 * without the file pays one cached branch per call; failure to write the
 * marker itself is silent, the same posture as bdgconn_probe_mark. */
static void regtrace_mark(uint32_t cmd, int after)
{
	char line[24];
	uint32_t tag = (cmd & 0x7fffffffu) | (after ? 0x80000000u : 0);
	int i, n = 5;
	long fd;

	if (!regtrace_mark_enabled())
		return;
	regtrace_clear_once();
	line[0] = 'm'; line[1] = 'a'; line[2] = 'r'; line[3] = 'k'; line[4] = ' ';
	for (i = 28; i >= 0; i -= 4)
		line[n++] = "0123456789abcdef"[(tag >> i) & 0xf];
	line[n++] = '\n';
	fd = sys_open("/proc/rtk_regtrace", O_WRONLY);
	if (fd >= 0) {
		sys_write((int)fd, line, n);
		sys_close((int)fd);
	}
}

/* The odi_switch.ko path, ODI_OMCI_OP_CMD on
 * its OWN netlink socket (cmd_nl_fd/cmd_nl_tid below), not the one
 * main.c holds for OMCI frame RX/TX (nl_fd/nl_tid, omcid.h). s1 shared
 * the one socket (s2 fixes): every
 * recvmsg() nl_cmd_call() issued read into one spillover slot, so a
 * legitimate OMCI RX frame arriving in that window and a cmd reply
 * arriving right after it could overwrite one another, and metricsd's own
 * periodic omcli command 13 (GetOnuState -> this same path) opened that
 * window every few seconds. A second socket removes the sharing (and the
 * spillover code) entirely: the kernel replies to whichever portid sent
 * the request -- NETLINK_CB(skb).portid, odi_omci.c -- so two sockets on
 * the same pid work exactly like one, no server-side change needed.
 * Opened once, lazily, on first use (autobind picks a fresh port the
 * same way nl_open() always has); apply.c owns this socket for the life
 * of the process, closed only at exit (never, in practice -- omcid runs
 * until a signal, main.c's own -d loop).
 */
static long cmd_nl_fd = -1;
static uint32_t cmd_nl_tid;

static int cmd_nl_ensure(void)
{
	if (cmd_nl_fd >= 0)
		return 0;
	/* nl_open_pid(0, ...), not nl_open(): this socket cannot ask for the
	 * calling thread's own tid as its pid, main.c's own socket (nl_fd) already
	 * holds that exact value in this same single-threaded process --
	 * s3 fix, see nl.h's own nl_open_pid() comment for the s2 failure this
	 * caused (every ODI_OMCI_OP_CMD send silently skipped, cmd_requests
	 * stuck at 0 the whole trial).
	 */
	cmd_nl_fd = nl_open_pid(0, &cmd_nl_tid, NL_POLL_US);
	if (cmd_nl_fd < 0) {
		out_fmt("omci_drv_call: ODI_OMCI_OP_CMD socket open/bind failed, rc %d\n",
			(long)cmd_nl_fd);
		return -1;
	}
	return 0;
}

/* omci_drv_netlink_state tracks whether the TRANSPORT answered at all,
 * not whether any one command succeeded: -1 unknown (nothing tried yet),
 * 1 a reply has been seen at least once (the kernel's own odi_switch netlink
 * responder is there and answering, whatever it said about this particular
 * command), 0 the first attempt got no reply within the retry budget at all
 * (no netlink responder for ODI_OMCI_OP_CMD -- this build's own only OMCI driver
 * path) and every later call fails fast without paying that timeout again.
 * A single command answering -EOPNOTSUPP (odi_switch_cmd()'s own verdict for
 * the slots not yet implemented from trace evidence) does NOT flip this to
 * 0 -- the transport is still up, only that one command lacks a handler.
 */
static int omci_drv_netlink_state = -1;

static int omci_drv_call_netlink(uint32_t cmd, void *buf, uint32_t len)
{
	uint8_t scratch[NLMSG_HDR + NL_CMD_REQ_HDR + NL_CMD_MAX_LEN];
	int status = -1;
	long rc, send_rc = 0;

	if (len > NL_CMD_MAX_LEN || cmd_nl_ensure() != 0)
		return -1;
	regtrace_mark(cmd, 0);
	rc = nl_cmd_call((int)cmd_nl_fd, cmd_nl_tid, scratch, cmd, buf, len, &status, 5, &send_rc);
	regtrace_mark(cmd, 1);
	if (rc != 0) {
		if (send_rc < 0) {
			/* Distinct from "no reply": the request itself never left
			 * this process. Logged every time (not once, like the
			 * two states below) -- a send failure is rare enough on
			 * an already-open, already-bound socket that seeing it
			 * repeat is itself diagnostic, and NL_POLL_US-paced
			 * retries make it cheap.
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

/* No sockopt fallback any more: that path spoke to the vendor omcidrv.ko over
 * a legacy getsockopt, and no proprietary OMCI kernel driver is ever built
 * on this tree. The
 * odi_switch netlink responder is the only OMCI driver path a 6.18 image
 * can ever have, so a call that gets no reply from it simply fails.
 */
int omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	if (omci_drv_netlink_state == 0)
		return -1;
	return omci_drv_call_netlink(cmd, buf, len) == 0 ? 0 : -1;
}

/* The per-port setters hand the driver the caller's own buffer, and every one
 * of them that has been checked against hardware -- getPortLinkStatus and
 * getPortState both -- is {u32 port; u32 value}, the port echoed back and the
 * value in the second word. */
uint8_t pairbuf[8];

void *pair(uint32_t port, uint32_t value)
{
	nl_put32(pairbuf + 0, port);
	nl_put32(pairbuf + 4, value);
	return pairbuf;
}

static int ethuni_auto_ability(uint32_t cfg, uint32_t *ability)
{
	switch (cfg) {
	case 0x00: *ability = 0xfc000000u; return 0;
	case 0x01: *ability = 0x40000000u; return 0;
	case 0x02: *ability = 0x10000000u; return 0;
	case 0x03: *ability = 0x04000000u; return 0;
	case 0x04: *ability = 0x54000000u; return 0;
	case 0x10: *ability = 0xc0000000u; return 0;
	case 0x11: *ability = 0x80000000u; return 0;
	case 0x12: *ability = 0x20000000u; return 0;
	case 0x13: *ability = 0x08000000u; return 0;
	case 0x14: *ability = 0xa8000000u; return 0;
	case 0x20: *ability = 0x0c000000u; return 0;
	case 0x30: *ability = 0x30000000u; return 0;
	default: return -1;
	}
}

/* `mask` is what the OLT just wrote. Only those attributes are applied.
 *
 * The first version of this applied the whole row on any set, which meant
 * attributes the OLT had not written yet went to the switch as their defaults
 * -- setMaxFrameSize(port, 0) among them, which stops a port forwarding. An
 * attribute that was never set is not a value; it is an absence.
 *
 * Per-UNI settings used to be absent for a reason that no longer holds, and
 * the reason is worth keeping because it is what the fix had to satisfy. The
 * vendor maps an entity id to a switch port through
 * pptp_eth_uni_me_id_to_switch_port, which looks the slot up in a table built
 * at run time; the arithmetic that looks obvious -- (inst & 0xff) - 1 -- is a
 * guess, and on a device whose switch answers on ports 0, 2 and 3 a guessed
 * port is somebody else's port.
 *
 * That table has since been found: it is the first 64 bytes of
 * getDevCapabilities, 32 slots of {type, index}, and uni_switch_port() below
 * reads it live rather than guessing. So class 11 now does set admin state,
 * PHY power-down and max frame size -- on a port the device named, not one we
 * worked out.
 */
/* ------------------------------------------------------------- capabilities
 *
 * Two things this needed were only ever in memory on a running device: how
 * many GEM flows the driver has, and which switch port a UNI entity id names.
 * Both come out of one read-only command. OMCI_Init copies the 120-byte answer
 * into gInfo+112 and every offset the vendor's code quotes is relative to that,
 * so the blob is read here once and indexed the same way.
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

#define FLOW_MAX 64                      /* what this device reports, and the
					  * ceiling on what is honoured */

uint32_t caps_flows(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_FLOWS);

	return (n == 0 || n > FLOW_MAX) ? FLOW_MAX : n;
}

/* pptp_eth_uni_me_id_to_switch_port, reproduced.
 *
 * The switch port is the slot's index, not anything stored in it: the table is
 * searched for a slot whose second byte is the entity id's low byte less one,
 * and whose first byte is the slot type this kind of entity needs. Returns -1
 * when nothing matches -- where the vendor returns success and leaves the
 * caller's variable untouched, which is a trap rather than a behaviour to copy.
 *
 * The vendor reaches the slot type through the entity id's high byte and a
 * value in its own bss; the caller here passes it directly, which needs no
 * such lookup and cannot go stale.
 */
static int uni_switch_port(uint16_t meId, uint8_t slot_type)
{
	uint8_t want = (uint8_t)((meId - 1) & 0xff);

	if (!caps_ok)
		return -1;
	for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
		if (caps[i * 2 + 1] == want && caps[i * 2] == slot_type)
			return (int)i;
	return -1;
}

/* ------------------------------------------------------- priority queues
 *
 * Six words of the GEM flow descriptor, and the whole of driver command 23,
 * come from one class 277 row -- three of its attributes, in fact:
 * RelatedPort, Weight and DropPrecedenceColourMarking.
 *
 * RelatedPort is two values in one word. Its high half names the managed
 * entity the queue hangs off, and its low half is the priority within that
 * port; only the bottom three bits of the priority reach the driver, and they
 * are reversed against the per-UNI queue count, so OMCI priority 0 becomes
 * driver priority 7.
 *
 * Word 8 is a boolean -- use weighted round robin -- and the 48 this used
 * to send was the length argument of the MIB_Get two instructions earlier.
 */
static uint32_t caps_uni_queues(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_UNIQ);

	return n ? n : 8;                /* isp1 reports 8 */
}

/* How many T-CONT slots the device has; a driver index at or above this is
 * not a T-CONT. */
static uint32_t tcont_capacity(void)
{
	uint32_t n = caps_u32(OMCI_CAPS_OFF_TCONTS);

	return n ? n : 8;                /* isp1 reports 8 */
}

/* The class 277 row a pointer names, or 0. Returns the row and the class
 * together because every caller needs both to read an attribute. */
static struct mib_row *priq_row(uint16_t meId, const struct omci_class **cp)
{
	*cp = find_class(OMCI_ME_PRIORITY_QUEUE);
	return *cp ? mib_find(OMCI_ME_PRIORITY_QUEUE, meId) : 0;
}

/* The k-th UNI managed entity, in class order: PPTP Ethernet UNIs first, then
 * VEIPs. ISP1 has one of each and its downstream queue blocks point at them
 * in that order; with only one stick to look at, the ORDER BETWEEN THE TWO
 * CLASSES is fitted to that one observation. The priorities within a block are
 * not -- see priq_related_port.
 *
 * Search omci_autonomous[] BEFORE mib[], because that is where these live: the
 * ONU creates its own UNIs and the OLT never has to, so `11, 0x0101` and
 * `329, 0x0601` have a row in mib[] only if the OLT happened to Set one.
 * Scanning mib[] alone returned 0 for every downstream queue -- which is the
 * exact case this function exists for, and the qemu assertion covered an
 * upstream queue and so agreed with the bug. */
static int instance_is_autonomous(uint16_t cls, uint16_t inst)
{
	for (unsigned i = 0; i < omci_autonomous_count; i++)
		if (omci_autonomous[i].classId == cls
		    && omci_autonomous[i].inst == inst)
			return 1;
	return 0;
}

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

/* RelatedPort for a priority queue the ONU auto-created, derived rather than
 * stored. The vendor creates all 208 at startup and fills this in; we hold a
 * class 277 row only if the OLT set something on one, which on a line that
 * provisions no queues is never.
 *
 * Checked against every one of isp1's 208 rows -- `omcicli mib get 277`, kept
 * at ~/tmp/odi/ref/isp1-mib-277.txt -- with no mismatch:
 *
 *   0x0000..0x000f   downstream, 8 per UNI.  high = the UNI's entity id
 *   0x8000..0x807f   upstream,   8 per T-CONT. high = 0x8000 + tcont
 *   0xff00..0xff3f   reserved,   8 per T-CONT. high = 0x8000 + tcont
 *
 * and within every block the priority counts DOWN with the entity id: 7..0 for
 * the first two ranges, 15..8 for the reserved one. So the lowest entity id in
 * a block is its highest priority, which is the opposite of the obvious
 * reading and is why this is derived from a capture rather than assumed.
 *
 * Returns 0 when the id is in none of the three ranges, which makes the
 * caller take the vendor's own "not a UNI" path.
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

/* mib_PriQ.so's
 * mibTable_init stages QCfgOpt 1, Weight 1, PktDropMaxP 255, QueueDropWQ 9 and
 * zero everywhere else. ISP1's queues do NOT all carry 1: its upstream blocks
 * run 4, 9, 14, 19, 24, 30, 0, 0 and its PPTP downstream block the mirror of
 * that, while its VEIP block and every reserved queue are 1. Which queues get
 * a non-default weight is provisioning, not derivation, so it is not guessed
 * here. */

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
	/* The vendor falls back to the PON port when the related entity is not
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

/* `omcicli dump qmap`, in the vendor column shape (captured on isp2
 * 2026-09-21): one row per upstream queue this daemon has programmed. QID
 * is the dense ordinal, TCID the driver T-CONT index; CIR/PIR are not
 * modelled and print 0 as they do on isp2. */
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

/* Does any upstream GEM port CTP name this class-277 queue? Only such a
 * queue is in the reservation, so only its Set can change the plan. Boot 47
 * (p47) ran the whole delete-and-recreate transaction 69 times during
 * provisioning because ISP1 Sets 72 queues and every one of them used to
 * dirty the graph. */
static int us_queue_referenced(uint16_t pqMe)
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

static int flow_find(int ds, uint16_t port)
{
	struct flow_slot *t = ds ? flow_ds : flow_us;

	for (uint32_t i = 0; i < caps_flows(); i++)
		if (t[i].used && t[i].port == port)
			return (int)i;
	return -1;
}

/* ------------------------------------------------------- the broadcast port
 *
 * The downstream broadcast GEM port is not configured by cfgGemFlow -- the
 * driver refuses the reserved port id there. It is designated separately, by
 * flow id, with setDsBcGemFlow, and MacBriPortCfgDataDrvCfg is where the
 * vendor works out which flow that is:
 *
 *     a MAC bridge port whose TP is a GEM interworking TP
 *       -> that GemIwTp's interworking option is 6
 *       -> follow its GEM CTP pointer to the GEM port network CTP
 *       -> take its Port-ID, look up the downstream flow that carries it
 *       -> setDsBcGemFlow(that flow id)
 *
 * Interworking option 6 is the vendor's own numbering, read out of the
 * comparison rather than out of G.988. The lookup is repeated whenever any of
 * the three entities changes, because the OLT may create them in any order;
 * the driver is only told when the answer actually moves.
 */
int bc_flow = -1;
int conn_dirty;

static void bc_gem_update(void)
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

/* The other half of the same rule, and the one we did not have.
 *
 * MacBriPortCfgDataDrvCfg's delete arm walks every OTHER class 47 row looking
 * for a second bridge port whose TP is a downstream-broadcast GEM interworking
 * TP. If it finds one the flow stays; if it does not, it hands the driver
 * 0xffffffff, which is how the broadcast flow is withdrawn.
 *
 * `going` is the entity being deleted -- its row is still in the store when
 * this runs, so it has to be skipped explicitly, exactly as the vendor skips
 * the row whose entity id matches its own.
 */
static void bc_gem_withdraw(uint16_t going)
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

/* ------------------------------------------------------ class 47's own work
 *
 * Three things a MAC bridge port config data row drives directly, all of them
 * only when its TP is a PPTP Ethernet UNI; what matters here is which arm
 * sends what.
 */

/* The MAC learning limit has a three-step fallback: the port's own
 * NumOfAllowedMac, else the bridge service profile's MacLearningDepth, else a
 * global default the vendor keeps in gInfo[0xe4].
 *
 * That default is runtime state in omci_app's .bss and is NOT recovered -- no
 * store to that offset exists anywhere in omci_app's text. So the third step
 * is a refusal rather than a guess: -1 means send nothing. A wrong limit is a
 * silent forwarding fault, and zero is not a safe stand-in because the vendor
 * never sends zero.
 */
static int mbpcd_learn_limit(const struct mib_row *r, const struct omci_class *c)
{
	const struct omci_class *sp = find_class(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE);
	struct mib_row *pr;
	uint32_t v = row_u32(r, c, 13);          /* NumOfAllowedMac */

	if (v)
		return (int)v;
	if (!sp)
		return -1;
	pr = mib_find(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE, (uint16_t)row_u32(r, c, 1));
	if (!pr)
		return -1;
	v = row_u32(pr, sp, 9);                  /* MacLearningDepth */
	return v ? (int)v : -1;
}

/* omci_apply_traffic_descriptor_to_uni_port, whole: the rate is the traffic
 * descriptor's PIR in bytes per second turned into kbit/s, and a descriptor
 * that does not resolve is the same as no limit. */
static void mbpcd_uni_rate(uint16_t inst, int port, uint32_t dir, uint32_t tdPtr)
{
	const struct omci_class *td = find_class(OMCI_ME_TRAFFIC_DESCRIPTOR);
	struct mib_row *tdr = td ? mib_find(OMCI_ME_TRAFFIC_DESCRIPTOR, (uint16_t)tdPtr) : 0;
	struct omci_unirate u;
	int rc;

	u.port = (uint32_t)port;
	u.dir = dir;
	u.kbps = OMCI_UNIRATE_NOLIMIT;
	if (tdr) {
		uint32_t pir = row_u32(tdr, td, 2);

		u.kbps = (pir << 3) >> 10;
		if (u.kbps == 0)
			u.kbps = OMCI_UNIRATE_NOLIMIT;
	}
	rc = apply_hw ? omci_drv_call(OMCI_UNIRATE_CMD, &u, sizeof u) : 0;
	out_fmt("   [%s 47/%04x] uni port %d dir %d rate %d kbit/s (td %04x) -> %d\n",
		apply_hw ? "hw" : "dry", (long)inst, (long)u.port, (long)u.dir,
		(long)u.kbps, (long)tdPtr, (long)rc);
}

/* The DSCP-to-P-bit map, unpacked and sent -- command 52.
 *
 * TWO classes carry the same 24-byte attribute and both send it here, with
 * different guards:
 *
 *   171  ExtVlanTagOperCfgDataDrvCfg's tail -- sent when the attribute is in
 *        the mask and the map is not all zero (the vendor memcmps it against
 *        24 zero bytes and returns).
 *   130  Map8021pServProfConnCfg -- sent when the attribute is in the mask and
 *        the mapper's UnmarkFrmOpt is 0 (G.988: DSCP to P-bit), the
 *        "unmarked frames take their P-bit from DSCP" option. There is no
 *        all-zero test on this one.
 *
 * Three bits per code point, most significant first, one byte out per byte of
 * DSCP space: eight groups of three bytes become eight P-bits each.
 */
static void dscp_remap_send(const uint8_t *p, const char *who, uint16_t inst)
{
	uint8_t remap[64];
	int rc;

	for (unsigned g = 0; g < 8; g++) {
		uint32_t w = ((uint32_t)p[g * 3] << 16)
			   | ((uint32_t)p[g * 3 + 1] << 8)
			   | p[g * 3 + 2];

		for (unsigned j = 0; j < 8; j++) {
			remap[g * 8 + j] = (uint8_t)((w & 0x00e00000u) >> 21);
			w <<= 3;
		}
	}
	out_fmt("   dscp remap %d %d %d %d ... %d\n",
		(long)remap[0], (long)remap[1], (long)remap[2],
		(long)remap[3], (long)remap[63]);
	if (!apply_hw)
		return;
	/* The wrapper's buf[64..128] is its own shadow of the last map and a
	 * changed-bits mask; neither is sent, so 64 is the whole payload. */
	rc = omci_setDscpRemap(remap);
	out_fmt("   [hw %s/%04x] dscp remap -> %d\n", who, (long)inst, (long)rc);
}

/* Command 64. `enable` is the create-side test spelled out: the vendor's
 * `xori 1; sltu` on class 45's DiscardUnknow is (byte != 1), so a 2 floods. */
static void flood_mask(const char *who, uint16_t inst, uint32_t portMask,
		       uint32_t enable)
{
	struct omci_flood f;
	int rc;

	f.sel = 0;
	f.enable = enable;
	f.portMask = portMask;
	rc = apply_hw ? omci_drv_call(OMCI_FLOOD_CMD, &f, sizeof f) : 0;
	out_fmt("   [%s %s/%04x] flooding mask %x enable %d -> %d\n",
		apply_hw ? "hw" : "dry", who, (long)inst, (long)portMask,
		(long)enable, (long)rc);
}

/* Class 47 sends the one port it is adding or removing; class 45 sends the
 * bridge's whole membership. Same command, same descriptor. */
static void mbpcd_flood(uint16_t inst, int port, uint32_t enable)
{
	flood_mask("47", inst, 1u << (unsigned)port, enable);
}

/* Everything class 47 drives for a PPTP Ethernet UNI, in one place, because
 * create, set and delete differ only in which of these they call and with what.
 * Returns the switch port, or -1 when the TP pointer names no UNI. */
static int mbpcd_uni_port(const struct mib_row *r, const struct omci_class *c,
			  uint16_t inst)
{
	int port;

	if (row_u32(r, c, 3) != MBPCD_TP_PPTP_ETH_UNI)
		return -1;
	port = uni_switch_port((uint16_t)row_u32(r, c, 4), OMCI_UNI_SLOT_PPTP);
	if (port < 0)
		out_fmt("   [47/%04x] tp pointer %04x is in no capability slot\n",
			(long)inst, (long)row_u32(r, c, 4));
	return port;
}

/* ------------------------------------------------------- dot1 rate limiter
 *
 * Class 298 holds a parent bridge, a TP type and three traffic descriptor
 * pointers -- upstream unicast flood, broadcast and multicast payload -- and
 * programs one driver slot per pointer that resolves.
 *
 * The slot number is ours in the same way a GEM flow id is: the driver's table
 * is indexed by it and the descriptor's first two words are what identify the
 * entry, so all that has to hold is that a delete names the slot its set used.
 */
static struct {
	uint8_t used;
	uint32_t portMask;
	uint32_t kind;
} dot1rl[OMCI_DOT1RL_SLOTS];

/* The vendor searches for a slot already holding this (portMask, kind) and
 * falls back to the first free one only when setting. -1 is "no such slot" on
 * a delete and "table full" on a set, and the vendor refuses in both cases. */
static int dot1rl_slot(uint32_t portMask, uint32_t kind, int alloc)
{
	int free_slot = -1;

	for (int i = 0; i < OMCI_DOT1RL_SLOTS; i++) {
		if (dot1rl[i].used) {
			if (dot1rl[i].portMask == portMask &&
			    dot1rl[i].kind == kind)
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	return alloc ? free_slot : -1;
}

/* omci_get_pptp_eth_uni_port_mask_in_bridge, out of our own store: every class
 * 47 row that is in this bridge and whose TP is a PPTP Ethernet UNI, mapped
 * through the capability table to a switch port.
 *
 * The vendor picks between this and omci_get_eth_uni_port_mask_behind_veip on
 * gInfo[0], which is not recovered. This stick's UNI is a PPTP Ethernet UNI, so
 * only this half is built; a VEIP bridge would come out as mask 0 and be
 * refused with the vendor's own message rather than programmed wrongly.
 */
static uint32_t bridge_uni_port_mask(uint16_t bridge_id)
{
	const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	uint32_t mask = 0;

	if (!bp)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		int port;

		if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		if (row_u32(&mib[i], bp, 1) != bridge_id)
			continue;
		if (row_u32(&mib[i], bp, 3) != MBPCD_TP_PPTP_ETH_UNI)
			continue;
		port = uni_switch_port((uint16_t)row_u32(&mib[i], bp, 4),
				       OMCI_UNI_SLOT_PPTP);
		if (port >= 0)
			mask |= 1u << (unsigned)port;
	}
	return mask;
}

/* Defined further down, with the bridge-connection code that is their other
 * caller; declared here because class 298 needs them first. */
static uint32_t veip_uni_port_mask(uint16_t veipId);
static uint32_t all_eth_uni_mask(void);

/* The same, for a bridge whose ports are VEIPs. `omci_get_eth_uni_port_mask_
 * behind_veip` in the vendor; here, the union of the Ethernet UNIs each VEIP
 * port fronts. */
static uint32_t bridge_veip_port_mask(uint16_t bridge_id)
{
	const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	uint32_t mask = 0;

	if (!bp)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		if (row_u32(&mib[i], bp, 1) != bridge_id)
			continue;
		if (row_u32(&mib[i], bp, 3) != MBPCD_TP_VEIP)
			continue;
		mask |= veip_uni_port_mask((uint16_t)row_u32(&mib[i], bp, 4));
	}
	return mask;
}

/* One of the three rates. `drop` forces the delete arm; otherwise a traffic
 * descriptor that does not resolve is itself a delete, which is what the
 * vendor does with a pointer of 0 or one naming nothing. */
static void dot1rl_one(uint16_t inst, uint32_t portMask, uint32_t kind,
		       uint16_t tdPtr, int drop)
{
	const struct omci_class *td = find_class(OMCI_ME_TRAFFIC_DESCRIPTOR);
	struct mib_row *tdr = (!drop && td) ? mib_find(OMCI_ME_TRAFFIC_DESCRIPTOR, tdPtr) : 0;
	struct omci_dot1rl d;
	int slot = dot1rl_slot(portMask, kind, tdr != 0);
	int rc;

	if (slot < 0) {
		if (tdr)
			out_fmt("   [298/%04x] kind %d: no free rate limiter "
				"slot\n", (long)inst, (long)kind);
		return;                          /* a delete of nothing is not
						  * an error -- the vendor
						  * tolerates rc 15 here */
	}
	d.slot = (uint32_t)slot;
	d.portMask = portMask;
	d.kind = kind;
	d.cir = tdr ? row_u32(tdr, td, 1) : 0;
	d.cbs = tdr ? row_u32(tdr, td, 3) : 0;
	rc = apply_hw ? omci_drv_call(tdr ? OMCI_DOT1RL_SET_CMD
					  : OMCI_DOT1RL_DEL_CMD,
				      &d, sizeof d) : 0;
	out_fmt("   [%s 298/%04x] %s slot %d mask %x kind %d cir %d cbs %d -> %d\n",
		apply_hw ? "hw" : "dry", (long)inst, tdr ? "set" : "del",
		(long)slot, (long)portMask, (long)kind, (long)d.cir,
		(long)d.cbs, (long)rc);
	if (rc == 0) {
		dot1rl[slot].used = (uint8_t)(tdr != 0);
		dot1rl[slot].portMask = portMask;
		dot1rl[slot].kind = kind;
	}
}

/* The handler's body, for a create, a set or a delete. */
static void dot1rl_apply(const struct omci_class *c, uint16_t inst,
			 const struct mib_row *r, int drop, int all)
{
	uint32_t mask;

	if (row_u32(r, c, 2) == 2) {             /* TpType */
		out_fmt("   [298/%04x] rate limiter for 802.1p mapper is not "
			"supported\n", (long)inst);
		return;
	}
	mask = bridge_uni_port_mask((uint16_t)row_u32(r, c, 1));
	if (mask == 0) {
		/* The vendor's other branch: a bridge whose ports are VEIPs
		 * rather than PPTP UNIs, which is what isp2's OLT builds. It
		 * picks between the two on gInfo[0]; we try the PPTP shape
		 * first and fall back, which gives the same answer on both
		 * shapes without needing a field we cannot read. */
		mask = bridge_veip_port_mask((uint16_t)row_u32(r, c, 1));
	}
	if (mask == 0) {
		out_fmt("   [298/%04x] no associated pptp eth uni is found\n",
			(long)inst);
		return;
	}
	for (unsigned k = 0; k < 3; k++) {
		/* `all` on a create or a delete; on a Set only the pointers
		 * this Set moved, which is the vendor's own guard. */
		if (!all && !mib_changed(r, c, 3 + k))
			continue;
		dot1rl_one(inst, mask, k, (uint16_t)row_u32(r, c, 3 + k), drop);
	}
}

/* ------------------------------------------------- the bridge connection
 *
 * Command 51 takes a 160-byte bridge rule descriptor and is what actually makes
 * the stick forward: with the vendor's connections torn down by
 * deactiveBdgConn, upstream traffic stopped dead even with every GEM flow and
 * T-CONT still in place.
 *
 * Three pieces of the stock daemon's observable behaviour are reproduced here,
 * and each one was a way to get this wrong:
 *
 * 1. A rule is not built by zeroing the struct. The stock daemon sends six
 *    fields already set, two of them to "ignore" sentinels that are not zero
 *    (OMCI_PRI_ANY is 8, OMCI_VID_ANY is 4096). A zeroed
 *    descriptor is a rule that filters on VID 0 and priority 0 -- accepted by
 *    the driver, and silently dropping everything.
 * 2. Two ingresses that differ in nothing else share one service: the stock
 *    daemon ORs the new ingress port into the existing entry; on
 *    isp1 that is why all six services read UNIMASK=5 while the connection
 *    dump prints twelve rules. Sending the local copy instead of the stored
 *    entry provisions half the ingress.
 * 3. A refused descriptor gives its service id back. Without the rollback the
 *    table drifts out of step with the driver's on the first failure.
 */
#define SERV_MAX 256                     /* the service-id limit, and the number of
					  * rows omcicli dump srvflow prints */

struct omci_bdgconn servtab[SERV_MAX];

/* MBPCD TP types, from the MAC bridge port config data model. Only the three
 * that name an ingress entity are used here. */
#define MBPCD_TP_PPTP_ETH_UNI   1
#define MBPCD_TP_IP_HOST        4
#define MBPCD_TP_VEIP          11

static int serv_avail(void)
{
	for (int i = 0; i < SERV_MAX; i++)
		if (!servtab[i].in_use)
			return i;
	return -1;
}

/* Whether two rules are the same service apart from the ingress. The compared
 * set is "everything in vlan_op that the driver reads, plus both flow ids and
 * the direction", and the exclusions are what a merge is allowed to differ in:
 * uni_mask, service_id, in_use, latch, the two Dp fields, is_mcast and
 * ds_tag_op. Comparing the vlan_op as words says the same thing in one
 * line -- except for those last two, which live inside vlan_op, so they are
 * skipped explicitly. */
static int serv_same_except_uni(const struct omci_bdgconn *a,
				const struct omci_bdgconn *b)
{
	const uint32_t *x = (const uint32_t *)&a->vlan_op;
	const uint32_t *y = (const uint32_t *)&b->vlan_op;
	/* Word indices within vlan_op: out starts at +88, so is_mcast is
	 * at +92 and ds_tag_op at +108. */
	enum { W_IS_MC_RULE = 92 / 4, W_DS_TAG_OPER = 108 / 4 };

	if (a->us_flow != b->us_flow || a->ds_flow != b->ds_flow ||
	    a->dir != b->dir)
		return 0;
	for (unsigned i = 0; i < sizeof a->vlan_op / 4; i++) {
		if (i == W_IS_MC_RULE || i == W_DS_TAG_OPER)
			continue;
		if (x[i] != y[i])
			return 0;
	}
	return 1;
}

/* Merge into an existing service if one matches, else
 * store at the id the caller already allocated. Returns the service id. */
static int serv_update(const struct omci_bdgconn *r)
{
	for (int i = 0; i < SERV_MAX; i++)
		if (servtab[i].in_use && serv_same_except_uni(r, &servtab[i])) {
			servtab[i].uni_mask |= r->uni_mask;
			return i;
		}
	if (r->service_id < 0 || r->service_id >= SERV_MAX)
		return -1;
	servtab[r->service_id] = *r;
	return r->service_id;
}

/* A new rule's initial state, and the only part of it that is not
 * a memset. See the comment above: these six are why zeroing is wrong. */
static void rule_init(struct omci_bdgconn *r)
{
	for (unsigned i = 0; i < sizeof *r / 4; i++)
		((uint32_t *)r)[i] = 0;
	r->vlan_op.filter.outer_mode = OMCI_TAGF_ANY;
	r->vlan_op.filter.inner_mode = OMCI_TAGF_ANY;
	r->vlan_op.outer_act.tag_op = OMCI_TAGOP_PASS;
	r->vlan_op.inner_act.tag_op = OMCI_TAGOP_PASS;
	r->vlan_op.out.out_tag.pri = OMCI_PRI_ANY;
	r->vlan_op.out.out_tag.vid = OMCI_VID_ANY;
	r->vlan_op.out.is_mcast = 0;
}

/* The ingress entity id to a switch port.
 *
 * The vendor tests the VEIP by comparing against (VEIP slot id << 8) | 1, a
 * build-time constant it reaches through two feature_api calls -- 0x601 on
 * isp1. Asking the MIB whether this entity id is a VEIP is the same test
 * without the arithmetic, and it is a measurement rather than a constant.
 *
 * Returns 0xFFFF for "no particular UNI", which is the vendor's own sentinel
 * for the all-UNIs-plus-PON mask, and -1 when the entity names a UNI that is
 * in no capability slot -- which the vendor treats as fatal for the rule.
 */
/* Every PPTP Ethernet UNI slot in the capability blob -- which is what a VEIP
 * fronts. This answers class 298's question (which UNIs sit behind a VEIP, for
 * a rate limiter), NOT the bridge connection's (which ingress port a VEIP is,
 * which is the PON port). Conflating the two produced a wrong fix once.
 *
 * The vendor resolves a VEIP through `omci_get_eth_uni_port_mask_behind_veip`:
 * for each UNI-G whose NonOmciPointer names the VEIP, map that UNI-G's own
 * entity id through the PPTP slot lookup. We cannot follow that literally --
 * our autonomous table carries UNI-G ids with no attribute values, because the
 * OLT never sets them -- but the blob supports the equivalent, and the answer
 * is the same set: the Ethernet UNIs this VEIP sits in front of.
 *
 * If the blob ever carries a type-1 slot for the VEIP itself, that wins. The
 * pairing is inverted from the obvious reading -- PPTP wants type 2, a VEIP
 * type 1 -- and only the vendor's code settles it; see the KB note
 * rtl9601-omci-device-capabilities.
 */
static uint32_t veip_uni_port_mask(uint16_t veipId)
{
	int own = uni_switch_port(veipId, OMCI_UNI_SLOT_VEIP);

	if (own >= 0)
		return 1u << (unsigned)own;
	return all_eth_uni_mask();
}

/* Which switch port an ingress entity is: a VEIP is the PON port, an IP host
 * the CPU port, anything else the UNI table.
 *
 * `mib_find` alone is not the test, and that was the bug. A VEIP, an IP host
 * and the Ethernet UNI are created by the ONU, not by the OLT, so they live in
 * omci_autonomous[] and the store never sees them. Asking the store for a VEIP
 * therefore always said no, and the call fell through to the PPTP lookup --
 * which happened to give a working port on this board only because
 * (0x601-1)&0xff and (0x101-1)&0xff are both 0. A collision, not a rule.
 *
 * The mapping itself is the vendor's and is measured, not inferred: isp1's
 * `omcicli dump srvflow` reports UNIMASK=5 on every service, and 5 is only
 * reachable as the VEIP's PON port bit (1<<2) ORed with the Ethernet UNI's
 * (1<<0). An earlier version of this comment claimed the PON port was the wrong
 * side and cited isp2 as proof; isp2 proves only that uni_mask 1 works, and on a
 * single-UNI stick both masks reach the same wire.
 */
static int ingress_switch_port(int ingress)
{
	uint16_t id = (uint16_t)ingress;

	if (ingress < 0)
		return 0xFFFF;
	if (!caps_ok)
		return -1;       /* without the blob there is no port map, and
				  * guessing one puts a rule on the wrong port */
	if (mib_find(OMCI_ME_VEIP, id) || instance_is_autonomous(OMCI_ME_VEIP, id))
		return (int)caps_u32(OMCI_CAPS_OFF_PONPORT);
	if (mib_find(OMCI_ME_IP_HOST_CONFIG_DATA, id) ||
	    instance_is_autonomous(OMCI_ME_IP_HOST_CONFIG_DATA, id))
		return (int)caps_u32(OMCI_CAPS_OFF_CPUPORT);
	return uni_switch_port(id, OMCI_UNI_SLOT_PPTP);
}

/* All Ethernet UNI switch ports, for the 0xFFFF case. The vendor calls
 * omci_get_all_eth_uni_port_mask; the capability table is the same answer. */
static uint32_t all_eth_uni_mask(void)
{
	uint32_t m = 0;

	if (!caps_ok)
		return 0;
	for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
		if (caps[i * 2] == OMCI_UNI_SLOT_PPTP)
			m |= 1u << i;
	return m;
}

/* omci_wrapper_activeBdgConn. `vr` is the generator's output, already filled
 * in; everything else here is the wrapper's own work. Returns the service id,
 * or -1. */
int bdgconn_add(int ingress, uint16_t gemPort, uint32_t dir,
		       const struct omci_vlan_oper *vr)
{
	struct omci_bdgconn r, scratch;
	uint32_t nflows = caps_flows();
	int uniPort, service_id, rc;

	rule_init(&r);
	r.vlan_op = *vr;
	r.dir = dir;

	uniPort = ingress_switch_port(ingress);
	if (uniPort < 0) {
		out_fmt("   [hw] no UNI for ingress %04x gem %d\n",
			(long)ingress, (long)gemPort);
		return -1;
	}

	/* The flow ids, and the not-found sentinel is GEM port count -- the
	 * same convention setDsBcGemFlow reports. Either one missing is fatal
	 * for the rule; a rule pointing at a flow that does not exist is worse
	 * than no rule. */
	if (dir == OMCI_DIR_US || dir == OMCI_DIR_BI) {
		int id = flow_find(0, gemPort);

		r.us_flow = id < 0 ? nflows : (uint32_t)id;
		/* Stock omci_UpdateUsDpFlowId starts here; zero means a real,
		 * different flow and makes the driver install a second rule. */
		r.us_dp_flow = r.us_flow;
	}
	if (dir == OMCI_DIR_DS || dir == OMCI_DIR_BI) {
		int id = flow_find(1, gemPort);

		r.ds_flow = id < 0 ? nflows : (uint32_t)id;
	}
	if (r.us_flow == nflows || r.ds_flow == nflows) {
		out_fmt("   [hw] no flow id for ingress %04x gem %d\n",
			(long)ingress, (long)gemPort);
		return -1;
	}

	r.uni_mask = (uniPort == 0xFFFF)
		  ? all_eth_uni_mask() | (1u << caps_u32(OMCI_CAPS_OFF_PONPORT))
		  : (1u << (unsigned)uniPort);
	/* A VEIP ingress stands in front of the Ethernet UNIs, so its mask
	 * carries them too. On isp1 the same value (5) arose by merging the
	 * UNI-ingress rule (1) with the VEIP one (4); on isp2 the bridge has
	 * no UNI port at all, the VEIP is the only ingress, and a mask of 4
	 * alone left host frames on port 0 unmatched: p61, boot 2, PON port
	 * counters at zero both ways while pppd waited for PADO. The vendor
	 * dump on isp2 reads UNIMASK=4 and forwards, so its driver reads the
	 * bit differently from the driver we load; our own earlier rule
	 * with mask 1 forwarded on isp2 (KB rtl9601-omci-bridge-connection). */
	if (mib_find(OMCI_ME_VEIP, (uint16_t)ingress) ||
	    instance_is_autonomous(OMCI_ME_VEIP, (uint16_t)ingress))
		r.uni_mask |= veip_uni_port_mask((uint16_t)ingress);

	service_id = serv_avail();
	if (service_id < 0) {
		out("   [hw] service table full\n");
		return -1;
	}
	r.service_id = service_id;
	r.in_use = 1;

	service_id = serv_update(&r);
	if (service_id < 0)
		return -1;

	/* Send the stored entry, not the local copy: a merge put another
	 * ingress in its uni_mask and the local copy does not have it. Through a
	 * scratch buffer, as the vendor does. */
	scratch = servtab[service_id];

	if (!apply_hw) {
		/* Dry run. The descriptor is 160 bytes that either make the
		 * stick forward or stop it forwarding, and the only way to
		 * check one before sending it was to send it. Print it
		 * instead, so the bytes can be read -- and diffed against a
		 * stick that is already provisioned -- with nothing at risk. */
		const uint8_t *b = (const uint8_t *)&scratch;

		out_fmt("   [dry] serv %d ingress %04x gem %d dir %d\n",
			(long)service_id, (long)ingress, (long)gemPort, (long)dir);
		for (unsigned i = 0; i < OMCI_BDGCONN_LEN; i += 16) {
			out("   ");
			out_hex(i, 3);
			out("  ");
			for (unsigned k = 0; k < 16; k++)
				out_hex(b[i + k], 2);
			out_char('\n');
		}
		return service_id;
	}

	bdgconn_probe_mark("/etc/config/bdgconn-before");
	rc = omci_activeBdgConn(&scratch);
	bdgconn_probe_mark("/etc/config/bdgconn-after");
	out_fmt("   [hw] bdgconn ingress %04x gem %d dir %d serv %d "
		"us %d ds %d unimask %d -> %d\n",
		(long)ingress, (long)gemPort, (long)dir, (long)service_id,
		(long)servtab[service_id].us_flow, (long)servtab[service_id].ds_flow,
		(long)servtab[service_id].uni_mask, (long)rc);
	if (rc != 0) {
		servtab[service_id].in_use = 0;      /* give the slot back */
		return -1;
	}
	sys_nanosleep(0, 1000 * 1000);            /* 1 ms between sends, as the stock daemon paces them */
	return service_id;
}

/* The no-VLAN-filter rule for one ingress, the N:1 all-pass case only.
 *
 * For a flow-based connection with no ANI VLAN tag operation this adds exactly
 * one field to the initialised rule: rule_gen = OMCI_VLAN_OPER_FORWARD_ALL.
 * The 802.1p and extended-VLAN branches are not here; they need the MIB tree
 * the vendor maintains, and guessing them would produce rules that are
 * accepted and wrong.
 */
void gen_no_vlan_filter_rule(struct omci_vlan_oper *vr)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FORWARD_ALL;
}

/* The rule a stick in manual VLAN mode actually runs.
 *
 * Every field below is pinned by `omcicli dump conn` on a working stick, and
 * the two we have disagree only in the VID: isp2 adds 10, isp1 adds 11, and
 * each matches its own `VLAN_MANU_TAG_VID`. The tag is NOT in class 171 --
 * every treatment field there carries the 4096 "no VID" sentinel -- so this
 * reads the config store instead.
 *
 * Upstream (`isMc` 0): accept untagged and ADD a C-TAG. Downstream multicast
 * (`isMc` 1): match priority/VID zero, remove both tags, and emit no tag,
 * exactly as the OEM's final service does.
 *
 * A zeroed descriptor is not a starting point -- rule_init sets the two
 * "ignore" sentinels that are not zero -- so this starts from it, as the
 * vendor's constructor does.
 */
void gen_manual_vlan_rule(struct omci_vlan_oper *vr, int vid, int pri, int isMc)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_EXTVLAN;
	/* Untagged in, both tags. NO_TAG rather than NO_CARE: the vendor's
	 * dump reads "filter S-TAG Mode: NO TAG". */
	vr->filter.outer_mode = OMCI_TAGF_UNTAGGED;
	vr->filter.inner_mode = isMc ?
		(OMCI_TAGF_VID | OMCI_TAGF_PRI) : OMCI_TAGF_UNTAGGED;
	vr->filter.outer.pri = 0;
	vr->filter.outer.vid = 0;
	vr->filter.outer.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.inner.pri = 0;
	vr->filter.inner.vid = isMc ? 0u : (uint32_t)vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = isMc ? OMCI_TAGOP_POP : OMCI_TAGOP_PASS;
	vr->outer_act.vid_op  = OMCI_VIDOP_SET;
	vr->outer_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.tag_op = isMc ? OMCI_TAGOP_POP : OMCI_TAGOP_PUSH;
	vr->inner_act.vid_op  = OMCI_VIDOP_SET;
	vr->inner_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.set_tag.pri  = (uint32_t)pri;
	vr->inner_act.set_tag.vid  = (uint32_t)vid;
	vr->inner_act.set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	vr->out.is_default = 0;
	vr->out.is_mcast = isMc ? 1u : 0u;
	vr->out.tag_count = isMc ? 0u : 1u;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	if (isMc) {
		vr->out.out_tag.pri  = 0;
		vr->out.out_tag.vid  = 0;
		vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	} else {
		vr->out.out_tag.pri  = (uint32_t)pri;
		vr->out.out_tag.vid  = (uint32_t)vid;
		vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	}
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

/* Everything a MIB reset throws away.
 *
 * The rows were the obvious part. The rest is state that DESCRIBES those rows:
 * tcont_map has 32 slots keyed on ME id and no eviction, so every re-ranging
 * added entries until it filled, after which tcont_remember silently dropped
 * new ones and every GEM flow naming a T-CONT was skipped with "was never
 * allocated here"; and the flow tables and bc_flow went on describing a
 * provisioning that no longer existed. Both reset paths call this, which is
 * also the only block the OMCI machine and the vendor CLI server had in
 * common. */
void mib_reset_all(void)
{
	for (int i = 0; i < MIB_ROWS; i++) {
		if (mib[i].used)
			tbl_free(&mib[i]);
		mib[i].used = 0;
	}
	for (int i = 0; i < FLOW_MAX; i++) {
		flow_us[i].used = 0;
		flow_ds[i].used = 0;
	}
	for (int i = 0; i < TCONT_MAX; i++)
		tcont_map[i].used = 0;
	usq_hw_n = 0;
	qos_dirty = 0;
	for (int i = 0; i < OMCI_DOT1RL_SLOTS; i++)
		dot1rl[i].used = 0;
	bc_flow = -1;
	mib_data_sync = 0;
}

/* What a Delete does to hardware, which for every class but one is nothing.
 *
 * Kept separate from apply_entity because the two have different inputs: a
 * delete has no attribute mask and the row is about to go away. Called before
 * mib_del so the row is still readable, though class 45's arm does not need it.
 */
void apply_delete(const struct omci_class *c, uint16_t inst)
{
	if (c && (c->classId == OMCI_ME_TCONT ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  (c->classId == OMCI_ME_PRIORITY_QUEUE &&
		   us_queue_referenced(inst))))
		qos_dirty = 1;
	if (c && (c->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA ||
		  c->classId == OMCI_ME_VLAN_TAGGING_FILTER_DATA ||
		  c->classId == OMCI_ME_GEM_IW_TP ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  c->classId == OMCI_ME_MCAST_GEM_IW_TP))
		conn_dirty = 1;
	if (!c)
		return;
	if (c->classId == OMCI_ME_DOT1_RATE_LIMITER) {
		const struct mib_row *r = mib_find(OMCI_ME_DOT1_RATE_LIMITER, inst);

		if (r)
			dot1rl_apply(c, inst, r, 1, 1);
		return;
	}
	if (c->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA) {
		/* The delete arm's own work, in the vendor's order. The row is
		 * still in the store, which is what both halves need. */
		const struct mib_row *r = mib_find(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA, inst);
		int port;

		if (!r)
			return;
		port = mbpcd_uni_port(r, c, inst);
		if (port >= 0) {
			/* The vendor puts the limit back to gInfo[0xe4], the
			 * global default we have not recovered. Rather than
			 * guess it the port keeps the limit it had -- stated
			 * here because it IS a divergence, not an omission. */
			out_fmt("   [47/%04x] delete: mac learn limit left as "
				"set, the global default is not recovered\n",
				(long)inst);
			mbpcd_flood(inst, port, 0);
			mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_IN,
				       0xffff);
			mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_OUT,
				       0xffff);
		}
		if (row_u32(r, c, 3) == MBPCD_TP_GEM_IWTP)
			bc_gem_withdraw(inst);
		return;
	}
	if (c->classId != OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE)
		return;
	/* MacBriServProfDrvCfg op 3, the whole of it: the ageing time goes back
	 * to the same 300-second default a zero attribute means (see the Set/
	 * Create arm below), and port bridging is set to 1. Both constants,
	 * neither from the row.
	 *
	 * cmd 62's own register, LUT_CFG.AGE_SPD, is in tenths of a second,
	 * not seconds: the stock boot writes 3000 there for its 300-second
	 * default -- cmd 62's own 3000 in boot3/boot5 is this driver-init
	 * default, not per-ME,
	 * and not an OMCI Set. odi_switch_cmd.c's own cmd_mac_age_time() already
	 * carries this right (its own default is 3000, matching the register
	 * unit); this call site did not -- `omci_setAgeingTime(300)` sent the
	 * *seconds* value straight through, landing at 30 real seconds on
	 * the register instead of 300 ("open stack by default").
	 * Fixed by scaling here, at the boundary between this file's own
	 * seconds-denominated constant and cmd 62's argument, rather than
	 * inside the generated omci_setAgeingTime() wrapper or cmd_mac_age_
	 * time() itself -- neither of those is where the unit actually
	 * changes.
	 */
	out_fmt("   [45/%04x] delete: ageing 300, port bridging 1\n", (long)inst);
	if (!apply_hw)
		return;
	omci_setAgeingTime(300 * 10);
	omci_setPortBridging(1);
}


/* Program one T-CONT: from a Set of its Alloc-ID, or on demand when a GEM
 * port CTP names it and the OLT never set it (the row already carries the
 * value). The driver hands back its own index; without a driver the
 * allocation order stands in, as before. */
static uint16_t tcont_apply(uint16_t inst, uint32_t alloc_id, uint16_t nqueues)
{
	struct omci_tcont t;
	long rc;

	/* Two drivers, one 8-byte payload. The stock driver fills the first
	 * word with the index it picked. Other drivers of this command read it
	 * as the number of queues to reserve on a T-CONT the GPON application
	 * already holds (the OLT assigned the Alloc-ID by PLOAM), and return the
	 * index in the same word. Reserve exactly the number of active queues in
	 * the normalized plan. On the stock driver the word is overwritten anyway. */
	t.index = nqueues;
	t.alloc_id = alloc_id;
	if (t.alloc_id >= 4096 || t.alloc_id == 0xff)
		return 0xffff;           /* what TcontDrvCfg refuses */
	if (apply_hw) {
			rc = omci_drv_call(OMCI_TCONT_CMD, &t, sizeof t);
			/* Boot 46: an earlier driver build answered rc 0 with index 8
			 * once its queue pool was exhausted, and that flow then
			 * sat on scheduler 8 -- the same invalid slot as the
			 * scheduler 8 / queue 31 sentinel. The index is only
			 * good when it is below the T-CONT count the device
			 * reports (isp1: 8). */
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
			/* The driver hands back its own index; with no driver
			 * the allocation order is the best stand-in, and it is
			 * what the driver produces for a fresh table anyway. */
			/* tcont_index first: tcont_remember updates an existing
			 * entry in place and tcont_count() already counts it,
			 * so a repeated Set of the same Alloc-ID would move
			 * this ME to a higher index and every later GEM CTP
			 * would name a T-CONT that does not exist. */
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

	/* A queue participates only when an upstream GEM CTP references it.
	 * Boot 46 (p46) disproved the wider rule that every class-277 row the
	 * OLT has Set is active: ISP1 Sets several queues per T-CONT, the
	 * shared pool of 32 queues filled with unreferenced ones (physical ids
	 * 6/13/20/24 instead of dense ones) and the fifth T-CONT was handed the
	 * out-of-range index 8. The referenced set is what the flows need and is
	 * the model p45 forwarded traffic with. */
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


/* ------------------------------------------------ connections from the MIB
 *
 * What the vendor's MIB_TreeConnUpdate produces, read off its `omcicli dump
 * conn` on isp1 (backups/isp1/vendor-dump-conn-260917.txt, 2026-09-17):
 * one connection per (ingress UNI-side bridge port) x (GEM-side bridge
 * port), direction Both, ingress 0x0101 (PPTP Ethernet UNI) and 0x0601
 * (VEIP), egress the GEM port of the bridge port's GEM IW TP, and the VLAN
 * rule from the VlanTagFilterData (class 84) hung on the GEM-side bridge
 * port: FILTER VID <tci> for the tagged services, and for the one GEM whose
 * bridge port has no filter entries, untagged in and the manual VLAN added
 * (the vendor's service_id 0: NO TAG, assign VID 11). Multicast (direction 2
 * CTPs) is left to bc_gem_update. Rebuilt whole -- tear down, derive, add
 * -- when the OLT has been silent for a second after touching any of the
 * classes involved; the OLT sends these entities in an order that is not
 * ours to rely on, and a whole rebuild is what the vendor does too. */
static int conn_servs[SERV_MAX];
static int conn_nservs;

static void gen_vid_filter_rule(struct omci_vlan_oper *vr, unsigned vid, int pbit)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FILTER_SINGLETAG;
	vr->filter.outer_mode = OMCI_TAGF_ANY;
	vr->filter.inner_mode = OMCI_TAGF_VID |
		(pbit >= 0 ? OMCI_TAGF_PRI : 0);
	vr->filter.inner.pri = pbit >= 0 ? (uint32_t)pbit : 0;
	vr->filter.inner.vid = vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = OMCI_TAGOP_PASS;
	vr->outer_act.vid_op  = OMCI_VIDOP_SET;
	vr->outer_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.tag_op = OMCI_TAGOP_PASS;
	vr->inner_act.vid_op  = OMCI_VIDOP_SET;
	vr->inner_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	vr->out.is_default = 0;
	vr->out.is_mcast = 0;
	vr->out.tag_count = 1;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	/* The vendor dump (backups/isp1/vendor-dump-conn-260917.txt) carries
	 * the p-bit in the out-style tag as well as in the filter for the
	 * 802.1P-mapped services (PRI 4 for VID 13, 5 for VID 12) and the
	 * ignore sentinel 8 only for the VID-only ones. Ours wrote 8 for all of
	 * them until 2026-09-21. */
	vr->out.out_tag.pri  = pbit >= 0 ? (uint32_t)pbit : OMCI_PRI_ANY;
	vr->out.out_tag.vid  = vid;
	vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
}

void bdgconn_rebuild(void)
{
	const struct omci_class *c47 = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	const struct omci_class *c266 = find_class(OMCI_ME_GEM_IW_TP);
	const struct omci_class *c268 = find_class(OMCI_ME_GEM_PORT_CTP);
	const struct omci_class *c281 = find_class(OMCI_ME_MCAST_GEM_IW_TP);
	const struct omci_class *c84 = find_class(OMCI_ME_VLAN_TAGGING_FILTER_DATA);
	const struct omci_class *c130 = find_class(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE);
	int ingress[8], ingress_bridge[8], ning = 0, added = 0, mvid;

	for (int i = 0; i < conn_nservs; i++) {
		if (apply_hw)
			omci_deactiveBdgConn((uint32_t)conn_servs[i]);
		if (conn_servs[i] >= 0 && conn_servs[i] < SERV_MAX)
			servtab[conn_servs[i]].in_use = 0;
	}
	conn_nservs = 0;
	if (!c47 || !c266 || !c268 || !c281 || !c84)
		return;
	for (int i = 0; i < MIB_ROWS && ning < 8; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t tp;

		if (!r || r->classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		tp = row_u32(r, c47, 3);
		if ((tp == 1 || tp == 11) && ning < 8) { /* Ethernet UNI, VEIP */
			ingress_bridge[ning] = (int)row_u32(r, c47, 1);
			ingress[ning++] = (int)row_u32(r, c47, 4);
		}
	}
	/* The manual tag, gated on VLAN_CFG_TYPE and VLAN_MANU_MODE as the
	 * vendor stack gates it (cfgstore.c); -1 builds no manual tag. */
	mvid = cfg_manual_vid();
	/* Service ids are classifier priority. OEM installs the configured
	 * untagged service first, the other unicast services in reverse MIB order,
	 * and the broad downstream multicast rule last. */
	for (int pass = 0; pass < 3; pass++) {
		for (int i = 0; i < MIB_ROWS; i++) {
			int row = pass == 1 ? MIB_ROWS - 1 - i : i;
			struct mib_row *r = mib_row_at(row), *iw, *ctp, *vt, *mapper;
			struct mib_row *tgt_iw[8];
			int tgt_pbit[8], ntgt = 0;
			uint32_t tpType;

			if (!r || r->classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
				continue;
			tpType = row_u32(r, c47, 3);
			if ((pass < 2 && tpType != 3) || (pass == 2 && tpType != 6))
				continue;
			/* TPType 3 is "802.1p mapper" in G.988, and two OLTs read it
			 * two ways. ISP1 points TPPointer at the GEM IW TP itself and
			 * hangs the mapper off the IW TP (ServProPtr); isp2 (2026-09-21,
			 * FHTT OLT) points it at the mapper, whose eight p-bit slots
			 * name the IW TPs. Follow whichever it is: one connection per
			 * distinct IW TP the mapper names, with its p-bit when exactly
			 * one p-bit lands on it and VID-only when several do. */
			if (tpType == 3) {
				uint16_t ptr = (uint16_t)row_u32(r, c47, 4);

				iw = mib_find(OMCI_ME_GEM_IW_TP, ptr);
				if (iw) {
					int pbit = -1;

					mapper = mib_find(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE, (uint16_t)row_u32(iw, c266, 3));
					if (mapper) {
						int matches = 0;

						for (int pri = 0; pri < 8; pri++)
							if (row_u32(mapper, c130, 2 + pri) == iw->inst) {
								pbit = pri;
								matches++;
							}
						if (matches != 1)
							pbit = -1;
					}
					tgt_iw[0] = iw; tgt_pbit[0] = pbit; ntgt = 1;
				} else if ((mapper = mib_find(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE, ptr)) != 0) {
					for (int pri = 0; pri < 8; pri++) {
						uint32_t id = row_u32(mapper, c130, 2 + pri);
						int k;

						if (id == 0 || id == 0xffff)
							continue;
						for (k = 0; k < ntgt; k++)
							if (tgt_iw[k]->inst == id)
								break;
						if (k < ntgt) {
							tgt_pbit[k] = -1;    /* several p-bits: VID only */
							continue;
						}
						iw = mib_find(OMCI_ME_GEM_IW_TP, (uint16_t)id);
						if (!iw || ntgt >= 8)
							continue;
						tgt_iw[ntgt] = iw; tgt_pbit[ntgt] = pri; ntgt++;
					}
					/* All eight on one IW TP is the plain service, not a
					 * p-bit 0 service. */
					if (ntgt == 1)
						tgt_pbit[0] = -1;
				}
			} else {
				iw = mib_find(OMCI_ME_MCAST_GEM_IW_TP, (uint16_t)row_u32(r, c47, 4));
				if (iw) {
					tgt_iw[0] = iw; tgt_pbit[0] = -1; ntgt = 1;
				}
			}
			for (int t = 0; t < ntgt; t++) {
			struct omci_vlan_oper vr;
			uint32_t port, dir, nent = 0, vid = 0;
			int serviceVid, pbit = tgt_pbit[t];
			uint8_t tbl[24];

			iw = tgt_iw[t];
			ctp = mib_find(OMCI_ME_GEM_PORT_CTP, (uint16_t)row_u32(iw, tpType == 3 ? c266 : c281, 1));
			if (!ctp)
				continue;
			port = row_u32(ctp, c268, 1);
			dir = row_u32(ctp, c268, 3);
			vt = mib_find(OMCI_ME_VLAN_TAGGING_FILTER_DATA, r->inst);
			if (vt) {
				nent = row_u32(vt, c84, 3);
				for (unsigned b = 0; b < sizeof tbl; b++)
					tbl[b] = 0;
				attr_value(c84, r->inst, 1, tbl);
				vid = ((unsigned)tbl[0] << 8 | tbl[1]) & 0xfff;
			}
			serviceVid = vt && nent && mvid >= 0 &&
				vid == (unsigned)mvid;
			if (tpType == 3 && ((pass == 0 && !serviceVid) ||
					    (pass == 1 && serviceVid)))
				continue;
			if (dir == 2 && mvid >= 0) {
				gen_manual_vlan_rule(&vr, mvid, vlanCfg.pri, 1);
			} else if (vt && nent) {
				/* The service VID is the untagged handoff. OEM still has a
				 * class-84 row for it, but builds the manual add-tag rule. */
				if (vid == (unsigned)mvid)
					gen_manual_vlan_rule(&vr, mvid, vlanCfg.pri, 0);
				else
					gen_vid_filter_rule(&vr, vid, pbit);
			} else if (mvid >= 0) {
				gen_manual_vlan_rule(&vr, mvid, vlanCfg.pri, 0);
			} else {
				gen_no_vlan_filter_rule(&vr);
			}
			for (int g = 0; g < ning; g++) {
				int id;

				/* Only the UNIs and VEIPs of the SAME bridge: with two
				 * bridges in the MIB every GEM used to get every
				 * ingress. One bridge per stick so far, so untested
				 * on hardware; caught by the isp2 fixture in QEMU. */
				if (ingress_bridge[g] != (int)row_u32(r, c47, 1))
					continue;
				id = bdgconn_add(ingress[g], (uint16_t)port, dir, &vr);
				if (id >= 0 && conn_nservs < SERV_MAX) {
					conn_servs[conn_nservs++] = id;
					added++;
				}
			}
			}                        /* targets of this bridge port */
		}
	}
	out_fmt("   [%s] bridge connections rebuilt: %d ingress x gem = %d\n",
		apply_hw ? "hw" : "dry", (long)ning, (long)added);
	bdgconn_probe_mark("/etc/config/bdgconn-done");
}

void apply_entity(const struct omci_class *c, uint16_t inst,
			 const struct mib_row *r, uint16_t mask, int creating)
{
	if (c && (c->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA ||
		  c->classId == OMCI_ME_VLAN_TAGGING_FILTER_DATA ||
		  c->classId == OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE ||
		  c->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA ||
		  c->classId == OMCI_ME_GEM_IW_TP ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  c->classId == OMCI_ME_MCAST_GEM_IW_TP))
		conn_dirty = 1;
	if (c && (c->classId == OMCI_ME_TCONT ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  (c->classId == OMCI_ME_PRIORITY_QUEUE &&
		   us_queue_referenced(inst))))
		qos_dirty = 1;
	int rc = 0;
	const char *what = 0;
	unsigned k;

	/* Without -a this used to return here, which meant the daemon tracked
	 * nothing either: no flow ids, no T-CONT map. But that bookkeeping is
	 * OURS, not the driver's -- flow ids are ids we choose -- and keeping
	 * it is what lets `flows`, `tcont` and a dry-run `bridge` say what
	 * would be programmed. So the tables are maintained either way and only
	 * the driver calls are gated, individually, below.
	 *
	 * With -a the behaviour is unchanged. Without it, tables that used to
	 * stay empty now fill in, and nothing is sent. */
	switch (c->classId) {
	case OMCI_ME_TCONT: { /* T-CONT */
		/* Queue count and normalized ordinals are properties of the whole
		 * upstream graph. The deferred reconciler applies this T-CONT after
		 * class-277 and class-268 writes have settled. */
		return;
	}
	case OMCI_ME_PRIORITY_QUEUE: { /* priority queue */
		/* Driver command 23, where the vendor sends it: PriQDrvCfg in
		 * mib_PriQ.so calls omci_wrapper_setPriQueue at the end of a
		 * class 277 Set. It is NOT sent when a flow that uses the queue
		 * is created -- cfgGemFlow's call is on the upstream queue,
		 * where setPriQueue returns before reaching the driver because
		 * bit 15 of the entity id is set.
		 *
		 * So the queue is programmed when the queue changes. Hanging
		 * this off the downstream GEM flow instead, which is what the
		 * first version did, sent a switch write on every flow. */
		struct omci_priq pq;
		int prc;

		if (!priq_program(inst, &pq)) {
			/* An upstream Set can reorder every queue on its T-CONT --
			 * but only a queue an upstream CTP names is in the plan,
			 * and the rebuild waits for the quiet second so a burst
			 * of Sets costs one transaction, not one each (boots 47
			 * and 48 ran it 69 and 68 times). */
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
	case OMCI_ME_GEM_PORT_CTP: { /* GEM port network CTP */
		struct omci_gemflow f;
		uint32_t dir;

		for (unsigned i = 0; i < sizeof f / 4; i++)
			((uint32_t *)&f)[i] = 0;
		f.gem_port      = row_u32(r, c, 1);
		/* Upstream T-CONT and queue words are filled by us_qos_rebuild(),
		 * after the complete queue graph is available. */
		f.tcont     = 0;
		f.queue     = 0;
		/* The six downstream-queue words come from the class 277 row
		 * the CTP points at, not from the pointer -- and when the OLT
		 * has set nothing on that queue, from priq_related_port(),
		 * because the ONU auto-creates all 208 and the vendor always
		 * has a row to read.
		 *
		 * Note a DsPriQPtr of 0 is priority queue ME 0, a real queue,
		 * not a null pointer: isp1's GEM CTPs all carry 0 and the
		 * vendor looks up queue 0x0000 for them. */
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
			qos_dirty = 1;   /* rebuilt after the quiet second */
		bc_gem_update();
		return;
	}
	case OMCI_ME_PPTP_ETH_UNI: { /* PPTP Ethernet UNI */
		/* Now that the UNI table is read from the driver rather than
		 * guessed, a port here is the port the vendor would use. */
		int port = uni_switch_port(inst, OMCI_UNI_SLOT_PPTP);

		if (port < 0) {
			out_fmt("   [%s] uni %04x is in no capability slot\n",
				apply_hw ? "hw" : "dry",
				(long)inst);
			return;
		}
		if (!creating && (mask & (1u << (16 - 3)))) { /* AutoDetectCfg */
			uint32_t cfg = row_u32(r, c, 3), ability;

			if (ethuni_auto_ability(cfg, &ability) != 0) {
				out_fmt("   [%s] uni %04x port %d auto %x unsupported\n",
					apply_hw ? "hw" : "dry", (long)inst,
					(long)port, (long)cfg);
			} else {
				rc = apply_hw
				   ? omci_setPortAutoNegoAbility(pair((uint32_t)port, ability)) : 0;
				out_fmt("   [%s] uni %04x port %d auto %x ability %x -> %d\n",
					apply_hw ? "hw" : "dry", (long)inst,
					(long)port, (long)cfg, (long)ability, (long)rc);
			}
		}
		if (!creating && (mask & (1u << (16 - 4)))) { /* LoopbackCfg */
			uint32_t cfg = row_u32(r, c, 4);

			if (cfg != 0 && cfg != 3) {
				out_fmt("   [%s] uni %04x port %d loopback %d unsupported\n",
					apply_hw ? "hw" : "dry", (long)inst,
					(long)port, (long)cfg);
			} else {
				rc = apply_hw
				   ? omci_setPhyLoopback(pair((uint32_t)port, cfg == 3)) : 0;
				out_fmt("   [%s] uni %04x port %d loopback %d -> %d\n",
					apply_hw ? "hw" : "dry", (long)inst,
					(long)port, (long)(cfg == 3), (long)rc);
			}
		}
		if (mask & (1u << (16 - 5))) {           /* AdminState */
			uint32_t lock = row_u32(r, c, 5) == 1;

			rc = apply_hw
			   ? omci_setPortState(pair((uint32_t)port, !lock)) : 0;
			out_fmt("   [%s] uni %04x port %d state %d -> %d\n",
				apply_hw ? "hw" : "dry",
				(long)inst, (long)port, (long)!lock, (long)rc);
			if (apply_hw)
				omci_setPhyPwrDown(pair((uint32_t)port, lock));
		}
		if (mask & (1u << (16 - 8))) {           /* MaxFrameSize */
			uint32_t v = row_u32(r, c, 8);

			rc = apply_hw
			   ? omci_setMaxFrameSize(pair((uint32_t)port, v)) : 0;
			out_fmt("   [%s] uni %04x port %d max frame %d -> %d\n",
				apply_hw ? "hw" : "dry",
				(long)inst, (long)port, (long)v, (long)rc);
		}
		if (!creating && (mask & (1u << (16 - 10)))) { /* PauseTime */
			uint32_t pause = row_u32(r, c, 10);

			rc = apply_hw
			   ? omci_setPauseControl(pair((uint32_t)port, pause)) : 0;
			out_fmt("   [%s] uni %04x port %d pause %d -> %d\n",
				apply_hw ? "hw" : "dry", (long)inst,
				(long)port, (long)pause, (long)rc);
		}
		return;
	}
	case OMCI_ME_GEM_IW_TP: /* GEM interworking TP */
		/* Does not drive the switch directly: it is a step on the way
		 * to naming the downstream broadcast flow, and the OLT creates
		 * it and class 47 in no fixed order. */
		bc_gem_update();
		return;
	case OMCI_ME_MCAST_GEM_IW_TP: /* multicast GEM interworking TP */
		bc_gem_update();
		return;
	case OMCI_ME_DOT1_RATE_LIMITER: /* dot1 rate limiter */
		/* The vendor's set arm fires only for a pointer that CHANGED,
		 * against the row as it was before the Set. That used to be
		 * inexpressible -- the store kept one row and overwrote it --
		 * so the guard was dropped and every Set was sent. `prev` makes
		 * it expressible, so it is honoured: a create sends all three,
		 * a set sends only what moved. */
		(void)mask;
		dot1rl_apply(c, inst, r, 0, creating);
		return;
	case OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA: { /* MAC bridge port config data */
		int port;

		bc_gem_update();
		port = mbpcd_uni_port(r, c, inst);
		if (port < 0)
			return;
		/* The create arm sends the learning limit unconditionally; the
		 * set arm sends it only when attribute 13 is in the mask. */
		if (creating || (mask & (1u << (16 - 13)))) {
			int lim = mbpcd_learn_limit(r, c);

			if (lim < 0) {
				out_fmt("   [47/%04x] no mac learn limit: neither "
					"attribute set and the global default is "
					"not recovered\n", (long)inst);
			} else {
				int rc2 = apply_hw
					? omci_setMacLearnLimit((uint32_t)port,
								(uint32_t)lim)
					: 0;

				out_fmt("   [%s 47/%04x] mac learn limit port %d "
					"= %d -> %d\n", apply_hw ? "hw" : "dry",
					(long)inst, (long)port, (long)lim,
					(long)rc2);
			}
		}
		/* Flooding is create-side only. The enable comes from the
		 * bridge service profile, not from this row. */
		if (creating) {
			const struct omci_class *sp = find_class(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE);
			struct mib_row *pr = sp
				? mib_find(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE, (uint16_t)row_u32(r, c, 1)) : 0;
			uint32_t discard = pr ? row_u32(pr, sp, 8) : 0;

			mbpcd_flood(inst, port, discard != 1);
		}
		/* Traffic descriptors are set-side only, one attribute each.
		 * Direction 1 is the INBOUND descriptor here -- see
		 * omci_bridgeport.h. */
		if (!creating && (mask & (1u << (16 - 12))))
			mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_IN,
				       row_u32(r, c, 12));
		if (!creating && (mask & (1u << (16 - 11))))
			mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_OUT,
				       row_u32(r, c, 11));
		return;
	}
	case OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA: { /* extended VLAN tagging op cfg */
		/* The VLAN rules themselves do NOT go to the driver from here
		 * -- they are carried inside the bridge connection, and the
		 * plugin that owns this class cannot reach the driver at all
		 * except through this one call; the earlier
		 * reading of "op 1 builds the driver payload" was wrong.
		 *
		 * What does go, straight off a Set, is the DSCP-to-P-bit map,
		 * and the rules come from ExtVlanTagOperCfgDataDrvCfg's tail:
		 * only when that attribute is in the mask, and only when it is
		 * not all zero. The vendor memcmps it against 24 zero bytes and
		 * returns rather than programming an all-zero map -- which is
		 * also why isp1, whose map is all zero, has never exercised
		 * this path. */
		const uint8_t *p;
		int off, any = 0;

		k = 8;                           /* DscpToPbitMapping */
		if (!(mask & (1u << (16 - k))))
			return;
		off = attr_offset(c, k);
		if (off < 0 || off + 24 > MIB_ROW_MAX)
			return;
		p = r->data + off;
		for (int i = 0; i < 24; i++)
			if (p[i])
				any = 1;
		if (!any)
			return;
		dscp_remap_send(p, "171", inst);
		return;
	}
	case OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE: { /* 802.1p mapper service profile */
		/* Map8021pServProfConnCfg reaches the driver from exactly one
		 * arm, and this is it. Everything else that function does is
		 * MIB-tree bookkeeping -- AVL membership and the GEM
		 * interworking links -- which this stack expresses by walking
		 * the store instead of by keeping a tree.
		 *
		 * The class's OTHER handler, Map8021pServProfDrvCfg, reaches
		 * the driver twice and neither survives on this hardware:
		 * through omci_apply_traffic_descriptor_to_gem_port, which
		 * needs a class 280 traffic descriptor (neither of our OLTs
		 * provisions a single one), and through setUsVeipPriQ, which
		 * is the command-65 bug documented in omci_gemflow.h.
		 *
		 * So this arm is the whole of class 130's contact with the
		 * hardware -- and on our sticks it is dormant too: all five of
		 * isp1's mappers carry UnmarkFrmOpt 1 (default P-bit) and an
		 * empty map. It is here because it is exact and cheap, not
		 * because anything we can observe exercises it. */
		const uint8_t *p;
		int off;

		k = 11;                          /* DscpMap2Pbit */
		if (!(mask & (1u << (16 - k))))
			return;
		if (row_u32(r, c, 10) != MAP8021P_UNMARKED_DSCP_TO_PBIT)
			return;
		off = attr_offset(c, k);
		if (off < 0 || off + 24 > MIB_ROW_MAX)
			return;
		p = r->data + off;
		dscp_remap_send(p, "130", inst);
		return;
	}
	case OMCI_ME_OLT_G: { /* OLT-G */
		/* The whole of what this class does to hardware, from
		 * OltGDrvCfg: a Set that carries ToDInfo, and nothing else.
		 * The attribute is fourteen bytes straight through to command
		 * 70 -- no packing, no check on the contents.
		 *
		 * This is how an OLT distributes time of day, so it arrives
		 * unsolicited on a working line and answering it is part of
		 * being a drop-in. */
		int off;

		k = 4;                           /* ToDInfo */
		if (!(mask & (1u << (16 - k))))
			return;
		off = attr_offset(c, k);
		if (off < 0 || off + 14 > MIB_ROW_MAX)
			return;
		out_fmt("   tod info %02x%02x%02x%02x ...\n",
			(long)r->data[off], (long)r->data[off + 1],
			(long)r->data[off + 2], (long)r->data[off + 3]);
		if (!apply_hw)
			return;
		{
			uint8_t tod[14];

			for (int i = 0; i < 14; i++)
				tod[i] = r->data[off + i];
			rc = omci_setTodInfo(tod);
		}
		what = "tod info";
		break;
	}
	case OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE: /* MAC bridge service profile */
		/* This used to send setPortBridging(attribute 3) on a Set.
		 * MacBriServProfDrvCfg does not do that and never has: it
		 * sends setPortBridging(1) on ME DELETE, and routes G.988's
		 * port-bridging indicator to feature_api(46, &byte) instead.
		 * Wrong trigger and wrong argument.
		 *
		 * Of the vendor's four arms, this is the one that needs
		 * nothing we lack. The other three want feature_api's
		 * dispatch and two bridge-membership helpers, and two of them
		 * are also gated on the value having CHANGED, which this
		 * store cannot express -- it keeps one row and overwrites it.
		 * Sending them unguarded would send more than the vendor
		 * does.
		 *
		 * Two of those three are now in, below: `prev` expresses the
		 * CHANGED guard, and both helpers turned out to be things the
		 * class 47 work already built.
		 *
		 * The fourth needs nothing. feature_api(46, &byte) dispatches
		 * to a plugin loaded from /lib/features/internal/, and index
		 * 46 in THIS userland selects the me_00004000 plugin, and
		 * me_00004000.so is not in this image. With no module
		 * registered the call returns FAL_ERR_NOT_REGISTER and does
		 * nothing, so the arm is a no-op on this device and class 45
		 * is complete. (Index 46 is a different member in newer
		 * trees; the enum's membership moved, so it has to be read
		 * from the version that matches the binary.) */
		if (!creating) {
			unsigned kk = 8;         /* DiscardUnknow */

			if ((mask & (1u << (16 - kk))) && mib_changed(r, c, kk)) {
				uint32_t m = bridge_uni_port_mask(inst);

				if (m)
					flood_mask("45", inst, m,
						   row_u32(r, c, kk) != 1);
				else
					out_fmt("   [45/%04x] flooding: no pptp "
						"eth uni in this bridge\n",
						(long)inst);
			}
			kk = 9;                  /* MacLearningDepth */
			if ((mask & (1u << (16 - kk))) && mib_changed(r, c, kk)) {
				/* omci_is_one_pptp_eth_uni_number_in_bridge:
				 * the command names ONE port, so the vendor
				 * only sends it when the bridge has one. */
				const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
				int port = -1, n = 0;

				for (int i = 0; bp && i < MIB_ROWS; i++) {
					if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
						continue;
					if (row_u32(&mib[i], bp, 1) != inst)
						continue;
					if (row_u32(&mib[i], bp, 3)
					    != MBPCD_TP_PPTP_ETH_UNI)
						continue;
					n++;
					port = uni_switch_port((uint16_t)
						row_u32(&mib[i], bp, 4),
						OMCI_UNI_SLOT_PPTP);
				}
				if (n == 1 && port >= 0) {
					uint32_t lim = row_u32(r, c, kk);
					int rc2 = apply_hw
						? omci_setMacLearnLimit(
							(uint32_t)port, lim) : 0;

					out_fmt("   [%s 45/%04x] mac learn "
						"limit port %d = %d -> %d\n",
						apply_hw ? "hw" : "dry",
						(long)inst, (long)port,
						(long)lim, (long)rc2);
				} else {
					out_fmt("   [45/%04x] mac learn limit: "
						"bridge has %d pptp eth uni, "
						"not one\n", (long)inst, (long)n);
				}
			}
		}
		k = 10;                          /* DynamicFilteringAgeingTime */
		if (!(mask & (1u << (16 - k))))
			return;
		{
			uint32_t age = row_u32(r, c, k);

			/* `li v0,300` then `movz a0,v0,a0` -- zero means the
			 * default, not zero. objdump prints that delay slot as
			 * .word 0x0044200a, which is how it hides. This "300"
			 * is the OMCI attribute's own unit, seconds, same as
			 * the delete-arm default above.
			 *
			 * cmd 62's register (LUT_CFG.AGE_SPD) is tenths of a
			 * second, not seconds (rtk/l2.h RTK_L2_DEFAULT_AGING_
			 * TIME == 300 * 10) -- see the longer note at the
			 * class-45 delete arm. Scale the substituted default
			 * the same way. A real, non-zero attribute value from
			 * the OLT is passed through unscaled below: no vendor
			 * evidence yet pins its unit, and "open stack by
			 * default" leaves that case unresolved on
			 * purpose rather than guess. */
			if (age == 0)
				age = 300 * 10;
			out_fmt("   ageing time %d\n", (long)age);
			if (!apply_hw)
				return;
			rc = omci_setAgeingTime(age);
		}
		what = "ageing time";
		break;
	case OMCI_ME_ANI_G: /* ANI-G */
		if (!apply_hw)
			return;
		k = 12;                          /* GEM block length */
		if (!(mask & (1u << (16 - k))))
			return;
		rc = omci_setGemBlkLen(row_u32(r, c, k));
		what = "gem block length";
		break;
	default:
		return;
	}
	out_fmt("   [hw] %s -> %d\n", what, (long)rc);
}
