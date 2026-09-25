/* omciprobe -- send one OMCI driver command and dump the answer.
 *
 * omcid programs the switch through one call, omci_drv_call(cmd, buf, len),
 * which on a 6.18 image travels as ODI_OMCI_OP_CMD over the odi_omci netlink
 * socket to odi_switch_cmd() (kernel/extra/drivers/net/ethernet/odi/
 * odi_switch_cmd.c). This speaks the same message (nl.h nl_cmd_call), on a
 * socket of its own with a kernel-chosen port, so it never registers for a
 * redirect type and cannot take the OLT channel from omcid.
 *
 * The command numbers are the ones tools/omci-drv-api.py lifted out of the
 * vendor wrappers (generated/omci_drv_cmds.h), and they are odi_switch_cmd()
 * own numbering too -- there is one numbering space, so no id map sits
 * between the table and the wire.
 *
 * Three outcomes are told apart, because they mean different things:
 *   - no socket, or no reply: this kernel has no odi_omci command path at all;
 *   - status -95 (EOPNOTSUPP): the path works, but odi_switch has no handler
 *     for this command -- it has no netlink equivalent on this kernel;
 *   - any other status: the handler ran and said so; the buffer is dumped.
 *
 * It runs beside a live omcid: by default it refuses every command whose
 * wrapper is not a `get`, because a setter here reconfigures the PON while
 * the OLT believes it owns the configuration. -f overrides that,
 * deliberately. Some commands are named `get` in the vendor table but make
 * register writes in odi_switch; those are treated as setters too. Note that
 * the first command of a boot, from anyone, fires odi_switch lazy platform
 * init (a no-op with the default module parameter); omcid normally sent it
 * long before this runs.
 */
#include "sys.h"
#include "io.h"
#include "nl.h"
#include "omci_drv_cmds.h"

#define ODI_EOPNOTSUPP  (-95)
#define PROBE_POLL_US   200000u      /* per recvmsg; five tries, one second */

/* `get` in the vendor table, register writes in odi_switch: cmd 10
 * (getTransceiverStatus) replays an I2C/GPIO write sequence each time it is
 * called (odi_switch_cmd.c cmd_transceiver_status). */
static int writes_on_odi_switch(unsigned cmd)
{
	return cmd == 10;
}

static int is_safe(const struct omci_drv_cmd *c)
{
	return c->safe && !writes_on_odi_switch(c->cmd);
}

/* One payload byte, base 16, with or without a 0x prefix. */
static int parse_hex8(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	int n = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (; *s; s++, n++) {
		uint32_t d;

		if (*s >= '0' && *s <= '9')      d = (uint32_t)(*s - '0');
		else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
		else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
		else return 0;
		v = v * 16 + d;
		if (n > 7)
			return 0;
	}
	if (!n)
		return 0;
	*out = v;
	return 1;
}


/* One command over ODI_OMCI_OP_CMD. Returns 0 with *status set when the
 * kernel answered; -1 with *why set when it did not: 1 no netlink socket,
 * 2 the send failed, 3 no reply. */
static int drv_call(uint32_t cmd, uint8_t *data, uint32_t len, int *status,
		    int *why, long *err)
{
	static uint8_t scratch[NLMSG_HDR + NL_CMD_REQ_HDR + NL_CMD_MAX_LEN];
	uint32_t tid = 0;
	long fd = nl_open_pid(0, &tid, PROBE_POLL_US), send_rc = 0;
	long rc;

	if (fd < 0) {
		*why = 1;
		*err = fd;
		return -1;
	}
	rc = nl_cmd_call((int)fd, tid, scratch, cmd, data, len, status, 5, &send_rc);
	sys_close((int)fd);
	if (rc == 0)
		return 0;
	*why = send_rc < 0 ? 2 : 3;
	*err = send_rc;
	return -1;
}

static int parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	int base = 10, any = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s; s++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return 0;
		v = v * (uint32_t)base + (uint32_t)d;
		any = 1;
	}
	*out = v;
	return any;
}

static int str_same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static const struct omci_drv_cmd *lookup(const char *s)
{
	uint32_t n;

	for (int i = 0; i < OMCI_DRV_CMD_COUNT; i++)
		if (str_same(omci_drv_cmds[i].name, s))
			return &omci_drv_cmds[i];
	if (!parse_u32(s, &n))
		return 0;
	for (int i = 0; i < OMCI_DRV_CMD_COUNT; i++)
		if (omci_drv_cmds[i].cmd == n)
			return &omci_drv_cmds[i];
	return 0;
}

static void dump(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 16) {
		out("  ");
		out_hex(i, 3);
		out(" ");
		for (uint32_t j = 0; j < 16; j++) {
			if (i + j < n) {
				out_hex(p[i + j], 2);
				out_char(' ');
			} else {
				out("   ");
			}
		}
		out(" |");
		for (uint32_t j = 0; j < 16 && i + j < n; j++) {
			uint8_t c = p[i + j];

			out_char(c >= 0x20 && c < 0x7f ? (char)c : '.');
		}
		out("|\n");
	}
}

static void list(void)
{
	out("cmd  len  wrapper\n");
	for (int i = 0; i < OMCI_DRV_CMD_COUNT; i++) {
		const struct omci_drv_cmd *c = &omci_drv_cmds[i];

		out_fmt("%3d  %3d  %s%s\n", (long)c->cmd, (long)c->len, c->name,
			is_safe(c) ? "" : "   (write -- needs -f)");
	}
}

static void usage(void)
{
	out("usage: omciprobe [-f] <cmd|name> [hexbyte ...]\n"
	    "       omciprobe -l            list the known commands\n"
	    "       omciprobe -h            this text\n"
	    "\n"
	    "Sends one OMCI driver command to odi_switch over the odi_omci\n"
	    "netlink socket and dumps the reply. Payload bytes are given in hex\n"
	    "and default to zero; the length is the one the vendor wrapper uses.\n"
	    "Only read-only commands run without -f. Never registers for the\n"
	    "OMCI channel, so omcid keeps it.\n");
}

int main(int argc, char **argv)
{
	static uint8_t data[OMCI_DRV_MAX];
	const struct omci_drv_cmd *c;
	int force = 0, i = 1, status = 0, why = 0;
	uint32_t n = 0;
	long err = 0;

	if (argc > 1 && (str_same(argv[1], "-h") || str_same(argv[1], "--help"))) {
		usage();
		out_flush();
		return 0;
	}
	if (argc > 1 && str_same(argv[1], "-l")) {
		list();
		out_flush();
		return 0;
	}
	if (argc > 1 && str_same(argv[1], "-f")) {
		force = 1;
		i = 2;
	}
	if (i >= argc) {
		usage();
		out_flush();
		return 2;
	}

	c = lookup(argv[i++]);
	if (!c) {
		out("% no such command; -l lists them\n");
		out_flush();
		return 2;
	}
	if (!is_safe(c) && !force) {
		out_fmt("%% %s writes; pass -f if that is really what you want\n", c->name);
		out_flush();
		return 2;
	}
	if (c->len > NL_CMD_MAX_LEN) {
		out_fmt("%% %s takes %d bytes, more than the netlink command carries (%d)\n",
			c->name, (long)c->len, (long)NL_CMD_MAX_LEN);
		out_flush();
		return 2;
	}

	for (; i < argc && n < c->len; i++, n++) {
		uint32_t b;

		/* Base 16 unconditionally: usage() says these are hex, the
		 * error below says they are hex, and parse_u32 treated an
		 * unprefixed byte as decimal -- so `omciprobe -f setDevMode 20`
		 * quietly wrote 20, not 0x20, into a driver command. A 0x
		 * prefix is still accepted. */
		if (!parse_hex8(argv[i], &b) || b > 0xff) {
			out("% payload bytes are hex, 00..ff\n");
			out_flush();
			return 2;
		}
		data[n] = (uint8_t)b;
	}
	if (i < argc) {
		out_fmt("%% %s takes %d bytes; extra arguments\n", c->name, (long)c->len);
		out_flush();
		return 2;
	}

	out_fmt("cmd %d (%s) len %d", (long)c->cmd, c->name, (long)c->len);
	if (n)
		out_fmt(", %d byte%s in", (long)n, n == 1 ? "" : "s");
	out_char('\n');

	if (drv_call(c->cmd, data, c->len, &status, &why, &err) != 0) {
		if (why == 1)
			out_fmt("%% no netlink socket (%d): this kernel has no odi_omci "
				"driver, so no OMCI driver command path\n", err);
		else if (why == 2)
			out_fmt("%% netlink send failed (%d)\n", err);
		else
			out("% no reply from odi_omci: this kernel does not answer "
			    "OMCI driver commands over netlink\n");
		out_flush();
		return 1;
	}
	if (status == ODI_EOPNOTSUPP) {
		out_fmt("%% %s has no netlink equivalent on this kernel: odi_switch "
			"answered EOPNOTSUPP (no handler for cmd %d)\n",
			c->name, (long)c->cmd);
		out_flush();
		return 3;
	}
	if (status != 0)
		out_fmt("%% odi_switch answered status %d; buffer as returned:\n",
			(long)status);
	dump(data, c->len);
	out_flush();
	return status != 0;
}
