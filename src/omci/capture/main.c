/* omcicap -- capture the OMCI channel between the OLT and this ONU.
 *
 * OMCI frames do not arrive on a netdev: the driver redirects them to
 * whichever userland app has registered for redirect type 1, over
 * NETLINK_USERSOCK. This makes the same three calls the responder does --
 * each one nlmsghdr and a twelve-byte body.
 *
 * There is exactly one receiver per type. Registering for a type that is
 * already registered overwrites that entry's pid rather than adding a second
 * receiver, and deregistering clears the type outright. So running this TAKES
 * the OMCI channel from omcid: the ONU stops answering the OLT, and when this
 * exits nobody holds the channel until omcid is restarted. That is why it
 * refuses to start while a live process holds type 1 (redirect_guard.h), and
 * why -f exists only as a deliberate override:
 *
 *     killall omcid
 *     ./omcicap -n 200
 *     /bin/omcid -a -d > /var/log/omcid.log 2>&1 &
 *
 * Losing OMCI makes the OLT re-range the ONU, which is the point: a re-ranging
 * is the whole conversation worth recording -- MIB reset, MIB upload, then the
 * create/set run that builds the service.
 */
#include "sys.h"
#include "io.h"
#include "omci_mib.h"
#include "redirect_guard.h"

#define AF_NETLINK        16
#define NETLINK_USERSOCK  2
#define SO_RCVTIMEO       0x1006      /* MIPS value, from asm/socket.h */
#define SOL_SOCKET        0xffff

#define REDIRECT_TYPE     1           /* what omci_wrapper_createMsgDev uses */
#define REDIRECT_MTU      1500

#define NLMSG_HDR         16
#define BODY_LEN          12
#define BUF_MAX           2048

/* libpr.so's message body. Opcode 1 both registers and deregisters; the action
 * byte is 1 for one and 2 for the other. */
/* libpr.so never initialises nlmsg_type -- it sends whatever malloc left in the
 * buffer -- and this used to set 0x10 on the theory that a type below
 * NLMSG_MIN_TYPE would be swallowed by netlink_rcv_skb() as a control message.
 *
 * That theory is wrong: the redirect driver takes the raw netlink input
 * itself, never passes it through netlink_rcv_skb, and dispatches on the
 * opcode in the payload, so nlmsg_type is read nowhere.
 *
 * `omcid` has always sent zero here -- nl.h zeroes the whole header -- and the
 * OLT answers it, which is the empirical half of the same conclusion. So the
 * 180-second run that registered and saw no frames failed for some other
 * reason, still unidentified, and this value is kept only because there is no
 * reason to churn a working capture path. Do not cite it as a cause. */
#define NLMSG_TYPE        0x10

#define OP_REG            1
#define ACT_REG           1
#define ACT_DEREG         2

struct sockaddr_nl {
	uint16_t family;
	uint16_t pad;
	uint32_t pid;
	uint32_t groups;
};

struct iovec {
	void *base;
	uint32_t len;
};

struct msghdr {
	void *name;
	uint32_t namelen;
	struct iovec *iov;
	uint32_t iovlen;
	void *control;
	uint32_t controllen;
	int flags;
};

static uint8_t rxbuf[BUF_MAX];
static uint8_t txbuf[NLMSG_HDR + BODY_LEN];

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Register or deregister for a redirect type. Both are opcode 1. */
static long redirect(int fd, uint32_t tid, int type, int action, int mtu)
{
	struct sockaddr_nl to = { AF_NETLINK, 0, 0, 0 };
	struct iovec iov = { txbuf, sizeof txbuf };
	struct msghdr msg = { &to, sizeof to, &iov, 1, 0, 0, 0 };
	uint8_t *b = txbuf + NLMSG_HDR;

	for (unsigned i = 0; i < sizeof txbuf; i++)
		txbuf[i] = 0;
	put32(txbuf + 0, sizeof txbuf);          /* nlmsg_len */
	put16(txbuf + 4, NLMSG_TYPE);            /* see below */
	put32(txbuf + 12, tid);                  /* nlmsg_pid */
	put16(b + 0, OP_REG);
	put16(b + 2, (uint16_t)type);
	put32(b + 4, tid);
	b[8] = (uint8_t)action;
	put16(b + 10, (uint16_t)mtu);
	return sys_sendmsg(fd, &msg, 0);
}

static const struct omci_class *find_class(uint16_t id)
{
	for (unsigned i = 0; i < omci_class_count; i++)
		if (omci_classes[i].classId == id)
			return &omci_classes[i];
	return 0;
}

/* G.988 11.2.2: the low five bits of byte 2 are the message type, and the three
 * above them are the destination-bit, acknowledge-request and acknowledgement
 * flags. */
static const char *msg_name(uint8_t mt)
{
	switch (mt & 0x1f) {
	case 4:  return "create";
	case 5:  return "create-complete";
	case 6:  return "delete";
	case 8:  return "set";
	case 9:  return "get";
	case 11: return "get-all-alarms";
	case 12: return "get-all-alarms-next";
	case 13: return "mib-upload";
	case 14: return "mib-upload-next";
	case 15: return "mib-reset";
	case 16: return "alarm";
	case 17: return "avc";
	case 18: return "test";
	case 19: return "start-sw-download";
	case 20: return "download-section";
	case 21: return "end-sw-download";
	case 22: return "activate-sw";
	case 23: return "commit-sw";
	case 24: return "sync-time";
	case 25: return "reboot";
	case 26: return "get-next";
	case 27: return "test-result";
	case 28: return "get-current-data";
	case 29: return "set-table";
	default: return "?";
	}
}

static void hexdump(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 16) {
		out("   ");
		out_hex(i, 3);
		out(" ");
		for (uint32_t j = 0; j < 16 && i + j < n; j++) {
			out_hex(p[i + j], 2);
			out_char(' ');
		}
		out_char('\n');
	}
}

/* The netlink payload is the driver's buffer verbatim, padded to the registered
 * MTU: four bytes of its own -- the redirect port, always zero here -- and then
 * the OMCI frame. A baseline frame is 48 bytes with the 0x00000028 trailer at
 * offset 40, which is what finds it without having to trust the prefix length.
 */
static int omci_offset(const uint8_t *p, uint32_t n)
{
	static const int cand[] = { 4, 0, 16 };

	for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
		int o = cand[i];

		if ((uint32_t)o + 44 <= n && get32(p + o + 40) == 0x28)
			return o;
	}
	return -1;
}

static void decode(const uint8_t *p, uint32_t n, int raw)
{
	int o = omci_offset(p, n);

	if (o < 0 || raw) {
		out_fmt("  %d bytes%s\n", (long)n, o < 0 ? ", no OMCI trailer found" : "");
		hexdump(p, n > 64 ? 64 : n);
		if (o < 0)
			return;
	}
	{
		const uint8_t *f = p + o;
		uint16_t tci = get16(f);
		uint8_t mt = f[2];
		uint16_t cls = get16(f + 4), inst = get16(f + 6);
		const struct omci_class *c = find_class(cls);

		out_fmt("  %-16s %s%s%s tci %-5d  class %-5d %-24s inst %-5d mask %04x\n",
			msg_name(mt),
			(mt & 0x40) ? "AR" : "  ",
			(mt & 0x20) ? "AK" : "  ",
			(mt & 0x80) ? "DB" : "  ",
			(long)tci, (long)cls, c ? c->name : "(unknown)",
			(long)inst, (long)get16(f + 8));
	}
}

/* Registering for a redirect type takes it from whoever held it, and dropping
 * it without giving it back is worse than not taking it: the kernel keeps
 * delivering to a netlink port that no longer exists, and its failure path is
 * an unthrottled printk once per frame. The OLT retries hard when unanswered,
 * so a killed run leaves a console storm behind it -- on a device whose
 * hardware watchdog is kicked from a kernel thread, that is a way to reset the
 * board rather than merely to make noise.
 *
 * So deregister on the way out, including on a signal. Without a libc there is
 * no sigreturn trampoline, so this handler must not return; deregistering and
 * exiting is exactly the shape that allows. */
static int sig_fd = -1;
static uint32_t sig_tid;

static void on_signal(int sig)
{
	(void)sig;
	if (sig_fd >= 0)
		redirect(sig_fd, sig_tid, REDIRECT_TYPE, ACT_DEREG, 0);
	out("\nderegistered on signal\n");
	out_flush();
	sys_exit(0);
}

static void catch_signals(int fd, uint32_t tid)
{
	sig_fd = fd;
	sig_tid = tid;
	sys_signal(1, on_signal);        /* HUP  -- the session going away */
	sys_signal(2, on_signal);        /* INT  */
	sys_signal(15, on_signal);       /* TERM -- what kill sends */
}

static void usage(void)
{
	out("usage: omcicap [-f] [-x] [-n frames] [-w ticks]\n"
	    "\n"
	    "Registers for OMCI redirect type 1 and prints every frame the OLT\n"
	    "sends, then deregisters. Only one process can hold the channel:\n"
	    "stop omcid first, and restart it afterwards.\n"
	    "\n"
	    "  -n frames  stop after this many frames (default 100)\n"
	    "  -w ticks   stop after this many idle 5 s ticks (default 12)\n"
	    "  -x         hex-dump the head of every frame\n"
	    "  -f         take the channel even from a live holder (omcid)\n"
	    "  -h         this text; registers nothing\n");
}

/* A non-negative decimal, or -1. */
static int parse_count(const char *s)
{
	int v = 0;

	if (!*s)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || v > 100000000)
			return -1;
		v = v * 10 + (*s - '0');
	}
	return v;
}

int main(int argc, char **argv)
{
	struct sockaddr_nl me = { AF_NETLINK, 0, 0, 0 };
	struct sockaddr_nl from;
	struct iovec iov = { rxbuf, sizeof rxbuf };
	struct msghdr msg = { &from, sizeof from, &iov, 1, 0, 0, 0 };
	uint32_t tv[2] = { 5, 0 };                 /* struct timeval, 5 s */
	long fd, rc, tid;
	int want = 100, raw = 0, got = 0, idle = 0, maxidle = 12, force = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (str_eq(a, "-h") || str_eq(a, "--help")) {
			usage();
			out_flush();
			return 0;
		} else if (str_eq(a, "-x")) {
			raw = 1;
		} else if (str_eq(a, "-f") || str_eq(a, "--force")) {
			force = 1;
		} else if (str_eq(a, "-n") || str_eq(a, "-w")) {
			int v;

			if (i + 1 >= argc || (v = parse_count(argv[i + 1])) < 0) {
				out_fmt("omcicap: %s needs a number\n", a);
				usage();
				out_flush();
				return 2;
			}
			i++;
			if (a[1] == 'n')
				want = v;
			else
				maxidle = v;        /* five-second ticks to wait */
		} else {
			out_fmt("omcicap: unknown argument %s\n", a);
			usage();
			out_flush();
			return 2;
		}
	}

	tid = sys_gettid();
	/* Before the socket, so a refusal leaves nothing behind. */
	if (redirect_guard("omcicap", REDIRECT_TYPE, force, (uint32_t)tid)) {
		out_flush();
		return 1;
	}
	fd = sys_socket(AF_NETLINK, SOCK_RAW, NETLINK_USERSOCK);
	if (fd < 0) {
		out_fmt("%% socket: %d\n", (long)fd);
		out_flush();
		return 1;
	}
	me.pid = (uint32_t)tid;
	rc = sys_bind((int)fd, &me, sizeof me);
	if (rc != 0) {
		out_fmt("%% bind: %d\n", (long)rc);
		out_flush();
		return 1;
	}
	sys_setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO, tv, sizeof tv);

	catch_signals((int)fd, (uint32_t)tid);
	rc = redirect((int)fd, (uint32_t)tid, REDIRECT_TYPE, ACT_REG, REDIRECT_MTU);
	out_fmt("registered tid %d for redirect type %d (sendmsg %d)\n",
		tid, (long)REDIRECT_TYPE, rc);
	out_fmt("omcid (if running) has lost the OMCI channel; restart it when done\n");
	out_flush();

	while (got < want && idle < maxidle) {
		rc = sys_recvmsg((int)fd, &msg, 0);
		if (rc <= 0) {
			idle++;
			out_fmt("[idle %d, recvmsg %d]\n", (long)idle, rc);
			out_flush();
			continue;
		}
		idle = 0;
		got++;
		{
			uint32_t nlen = get32(rxbuf);
			uint32_t plen = (uint32_t)rc;

			if (nlen >= NLMSG_HDR && nlen <= plen)
				plen = nlen;
			out_fmt("#%d  netlink %d bytes\n", (long)got, (long)plen);
			decode(rxbuf + NLMSG_HDR,
			       plen > NLMSG_HDR ? plen - NLMSG_HDR : 0, raw);
		}
		out_flush();
	}

	redirect((int)fd, (uint32_t)tid, REDIRECT_TYPE, ACT_DEREG, 0);
	sys_close((int)fd);
	out_fmt("deregistered after %d frame%s\n", (long)got, got == 1 ? "" : "s");
	out_flush();
	return 0;
}
