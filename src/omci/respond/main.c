/* omcid -- the OMCI stack: startup and the main loop.
 *
 * Takes redirect type 1 and answers the OLT (omcimsg.c), keeps the MIB
 * (mibstore.c), applies it to the switch driver (apply*.c, drv.c), and serves
 * the omcli and omcicli queues (clisrv.c, vqsrv.c). OLT_QUIET_MS after the
 * last OMCI frame, the loop rebuilds the upstream QoS and bridge connections.
 * See ../README.md for the design; omcid.h for what the modules share.
 */
#include "omcid.h"
#include "../redirect_guard.h"

/* wdt_ping() -- the odi_wdt watchdog rule this daemon owns (docs/SETTINGS.md,
 * "Watchdog rules"): a cheap write to /proc/odi_wdt/ping every few seconds
 * from this OWN main loop, not a separate probing process. rcS registers
 * "omcid" with its deadline at boot (/proc/odi_wdt/register); the kernel
 * arms it on the first ping here and resets the board if this stops
 * arriving. A missing /proc/odi_wdt (host, qemu, a stock kernel) makes the
 * open fail and the write a silent no-op -- exactly as intended off real
 * hardware.
 */
#define WDT_PING_PATH   "/proc/odi_wdt/ping"
#define WDT_PING_NAME   "omcid"

static void wdt_ping(void)
{
	long fd = sys_open(WDT_PING_PATH, O_WRONLY);

	if (fd < 0)
		return;
	sys_write((int)fd, WDT_PING_NAME, sizeof(WDT_PING_NAME) - 1);
	sys_close((int)fd);
}

/* A redirect type dropped without deregistering keeps the kernel delivering
 * to a dead netlink port, with an unthrottled printk per frame; the OLT
 * retries hard, and the console storm can starve the kernel thread that kicks
 * the watchdog. So deregister on a signal too. Without a libc there is no
 * sigreturn trampoline: this handler must exit, never return. */
static void on_signal(int sig)
{
	(void)sig;
	/* Reset the sink FIRST: mid-command it is cli_sink, and flushing through
	 * it would spend the whole retry budget on a queue about to be deleted. */
	out_set_sink(0);
	if (nl_fd >= 0)
		nl_redirect(nl_fd, nl_tid, REDIRECT_TYPE, NL_ACT_DEREG, 0);
	/* Queues outlive the process, and this kernel has 53 in total. Only
	 * ours: reply_q belongs to a client, which removes it. */
	if (cliq >= 0)
		mq_remove(cliq);
	if (vq >= 0 && vq_is_ours)
		mq_remove(vq);
	out("\nderegistered on signal\n");
	out_flush();
	sys_exit(0);
}

static void catch_signals(int fd, uint32_t tid)
{
	nl_fd = fd;
	nl_tid = tid;
	out_fmt("signal handlers: hup %d int %d term %d\n",
		sys_signal(1, on_signal),        /* the session going away */
		sys_signal(2, on_signal),
		sys_signal(15, on_signal));      /* what kill sends */
}

static void usage(void)
{
	out("usage: omcid [-a] [-r] [-d] [-f] [-w units] [-c caps-hex]\n"
	    "\n"
	    "The OMCI responder: registers for redirect type 1 and answers the\n"
	    "OLT, and serves the omcli and omcicli queues.\n"
	    "\n"
	    "  -a          program the switch (apply mode); without it, dry run\n"
	    "  -r          restart: first remove every bridge connection a\n"
	    "              previous omcid left in the switch (needs -a)\n"
	    "  -d          daemon: no frame or idle limit\n"
	    "  -w units    exit after this many idle 5 s units (default 24)\n"
	    "  -c hex      use this capability blob instead of the driver one\n"
	    "  -f          start even if a live process holds redirect type 1\n"
	    "  -h          this text; starts nothing\n");
}

int main(int argc, char **argv)
{
	uint32_t tid = 0, plen;
	long fd, rc;
	int idle = 0, maxidle = 24, n = 0, want = 400, force = 0, restart = 0;
	int quiet = 0;
	unsigned long frames_seen = 0;

	/* Every argument is matched whole, and anything unrecognised stops the
	 * program before it opens a socket: a mistyped flag must not start a
	 * second daemon that takes redirect type 1 from the running one. */
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-h") || str_eq(argv[i], "--help")) {
			usage();
			out_flush();
			return 0;
		} else if (str_eq(argv[i], "-f") || str_eq(argv[i], "--force")) {
			force = 1;
		} else if (str_eq(argv[i], "-a"))
			apply_hw = 1;
		/* -r: replacing a running omcid, whose bridge connections are
		 * still in the switch; without clearing them the first rebuild
		 * adds a second set. /etc/scripts/apply passes it, rcS does not. */
		else if (str_eq(argv[i], "-r"))
			restart = 1;
		/* -d: the daemon, stopped only by a signal. Without it the
		 * 400-frame and 120 s idle limits end a live session. */
		else if (str_eq(argv[i], "-d")) {
			want = 0x7fffffff;
			maxidle = 0x7fffffff / (5000000 / NL_POLL_US) - 1;
		}
		/* -c <120 bytes of hex>: a capability blob (port map, flow and
		 * T-CONT counts) captured from a real device, so data-path
		 * construction can run off the device. It does not imply -a. */
		else if (str_eq(argv[i], "-c") && i + 1 < argc) {
			const char *h = argv[++i];
			unsigned n = 0;

			while (*h && n < sizeof caps) {
				int hi = hex_nib(*h);
				int lo = h[1] ? hex_nib(h[1]) : -1;

				if (hi < 0 || lo < 0)
					break;
				caps[n++] = (uint8_t)((hi << 4) | lo);
				h += 2;
			}
			if (n == sizeof caps) {
				caps_ok = 1;
				caps_from_arg = 1;
			} else {
				out_fmt("%% -c needs %d bytes of hex, got %d\n",
					(long)sizeof caps, (long)n);
			}
		}
		else if (str_eq(argv[i], "-w") && i + 1 < argc) {
			maxidle = 0;
			for (const char *s = argv[++i]; *s >= '0' && *s <= '9'; s++)
				maxidle = maxidle * 10 + (*s - '0');
		} else {
			out_fmt("omcid: unknown argument %s\n", argv[i]);
			usage();
			out_flush();
			return 2;
		}
	}

	/* Opened, and nl_fd/nl_tid set, before the capability, serial and
	 * device-id reads: omci_drv_call() needs them for the ODI_OMCI_OP_CMD
	 * path, and the sockopt fallback does not exist on this image.
	 * catch_signals() sets the same two globals again later. */
	fd = nl_open(&tid, NL_POLL_US);
	if (fd < 0) {
		out_fmt("%% netlink: %d -- no line side. The message queues are "
			"still served, so `omcli --inject` works and replies "
			"are logged rather than sent.\n", (long)fd);
		tid = (uint32_t)sys_gettid();
	}
	/* Before any driver read or queue: a second omcid must not register
	 * over the first. Only with a line side; qemu has nothing to take. */
	if (fd >= 0 && redirect_guard("omcid", REDIRECT_TYPE, force, tid)) {
		sys_close((int)fd);
		out_flush();
		return 1;
	}
	nl_fd = fd;
	nl_tid = tid;

	/* Not gated on -a: the read changes nothing, and without it a dry run
	 * maps no UNI, so every TP pointer reports "in no capability slot". */
	if (!caps_from_arg) {
		caps_ok = omci_drv_call(OMCI_CAPS_CMD, caps, sizeof caps) == 0;
		if (!caps_ok)
			out("% could not read the device capabilities; "
			    "flow ids fall back to a 64-entry table and no "
			    "UNI is mapped\n");
		else {
			out_fmt("capabilities: %d gem flows, %d priority queues\n",
				(long)caps_flows(),
				(long)caps_u32(OMCI_CAPS_OFF_PRIQ));
			for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
				if (caps[i * 2 + 1] != 0xff)
					out_fmt("  uni slot %d: type %d, "
						"index %d -> switch port %d\n",
						(long)i, (long)caps[i * 2],
						(long)caps[i * 2 + 1], (long)i);
		}
	}

	if (serial_refresh(1) != 0)
		out("% no serial number yet; asking again once a second\n");
	if (omci_drv_call(DRV_GET_DEVID, devid, sizeof devid) != 0)
		out("% could not read the device id from the driver\n");
	devid[8] = 0;                            /* "RTL9602C" is NUL-padded */

	/* The config store, read once at startup, so the CTC LOID entity
	 * answers from it from the first frame. */
	cfg_load_identity();
	cfg_load_vlan();
	cfg_load_report();
	out_fmt("store: loid %s, manual vlan %s, identity report %s\n",
		ident.loid[0] ? "set" : "none",
		vlanCfg.manual ? "on" : "off",
		report.on ? "on" : "off");

	if (restart && apply_hw && fd >= 0) {
		/* Deactivating an id the driver does not hold is not an error
		 * (odi_switch_cmd.c cmd 50), so every id is sent. */
		for (int id = 0; id < SERV_MAX; id++)
			omci_deactiveBdgConn((uint32_t)id);
		out_fmt("restart: %d bridge connection ids cleared\n", (long)SERV_MAX);
	}

	/* The netlink poll is NL_POLL_US (2 ms): the queues are served between
	 * netlink timeouts, and the stock omcicli gives up after 12 ms. `-w`
	 * keeps its five-second units; the idle count is scaled instead.
	 *
	 * No netlink is not fatal: frames injected on the message queue
	 * (msgType 0) need none, and qemu-user answers EPROTONOSUPPORT (-120 on
	 * MIPS) for our own protocol number (NETLINK_ODI; the stock firmware
	 * used NETLINK_USERSOCK). The MIB store, both CLI protocols, the
	 * renderers and injection all run there. Answers are logged, not sent. */
	/* A queue of our own, not 0x800: a client detects the omcli protocol by
	 * whether this key exists, not by probing a daemon that may be stock. */
	cliq = mq_open_fresh(OMCLI_KEY);
	if (cliq < 0)
		out_fmt("%% no cli queue (%d); omcli will not find this daemon\n",
			(long)cliq);

	/* The 0x800 queue (boa and the omcicli scripts) is taken in the loop:
	 * omci_app usually still holds it here. See vq_ensure. */
	vq_ensure();
	out_fmt("cli queue %d (omcli); the omcicli queue is %s\n", (long)cliq,
		vq >= 0 ? "ours" : "still omci_app's");

	catch_signals((int)fd, tid);
	rc = fd < 0 ? -1
		    : nl_redirect((int)fd, tid, REDIRECT_TYPE, NL_ACT_REG,
				  REDIRECT_MTU);
	out_fmt("answering as %s, tid %d (register %d)%s\n", (const char *)devid,
		tid, rc, apply_hw ? ", programming the switch" : "");
	out_flush();

	while (n < want && idle < maxidle * (5000000 / NL_POLL_US)) {
		/* Serving a queue is not idling; with no line side it is the
		 * only thing that resets the -w count. */
		if (cli_poll() || vq_poll())
			idle = 0;
		/* One quiet second after the last OMCI frame, from the line or
		 * injected: rebuild the upstream QoS and the bridge connections
		 * from the MIB as it stands (us_qos_rebuild(), bdgconn_rebuild()).
		 * Quiet counts OMCI frames only: CLI and queue requests reset
		 * `idle` (the -w exit) but not this, or a client polling omcicli
		 * every few seconds, a metrics scrape, would hold the rebuild off
		 * indefinitely and leave the ONU in O5 with no service. */
		if (omci_frames_handled != frames_seen) {
			frames_seen = omci_frames_handled;
			quiet = 0;
		} else if (quiet < 1000000 / NL_POLL_US) {
			quiet++;
		}
		{
			/* CLOCK_MONOTONIC-gated, not loop-iteration-counted: this
			 * used to count NL_POLL_US-spaced ticks and call a
			 * counted 1000000/NL_POLL_US of them "one second", which
			 * only holds if every iteration takes exactly NL_POLL_US.
			 * One slow iteration -- a full apply on an OMCI frame, a
			 * write that blocks while /tmp is nearly full -- stretches
			 * a counted second past its real length, and 15 counted
			 * seconds can cover well more than 15 real ones: seen on
			 * hardware as a 27 s last_ping_age against the 15 s this
			 * comment used to promise. Wall-clock elapsed time keeps
			 * the real cadence regardless of how long any one
			 * iteration takes. */
			static long last_sample_s = -1;
			static long last_ping_s = -1;
			long now_s, now_ns;

			if (sys_clock_gettime(CLOCK_MONOTONIC, &now_s, &now_ns) == 0) {
				if (last_sample_s < 0)
					last_sample_s = now_s;
				if (last_ping_s < 0)
					last_ping_s = now_s;
				if (now_s - last_sample_s >= 1) {
					last_sample_s = now_s;
					onu_state_sample();
					serial_refresh(1);
				}
				/* Every 15 s: a quarter of the 60 s deadline
				 * odi_wdt.h/rcS register omcid with, so an
				 * occasional missed beat (a slow OLT frame, a
				 * stalled CLI client) never costs a reset. */
				if (now_s - last_ping_s >= 15) {
					last_ping_s = now_s;
					wdt_ping();
				}
			}
		}
		if (qos_dirty && quiet >= 1000000 / NL_POLL_US) {
			qos_dirty = 0;
			us_qos_rebuild();
		}
		if (conn_dirty && quiet >= 1000000 / NL_POLL_US) {
			conn_dirty = 0;
			bdgconn_rebuild();
		}
		if (fd < 0) {
			/* No line side: pace the loop as the netlink
			 * receive timeout would. */
			sys_nanosleep(0, NL_POLL_US * 1000);
			idle++;
			continue;
		}
		rc = nl_recv((int)fd, rxbuf, sizeof rxbuf, &plen);
		if (rc <= 0) {
			idle++;
			continue;
		}
		idle = 0;
		if (plen >= NL_FRAME_OFF + OMCI_FRAME_LEN) {
			n++;
			handle((int)fd, tid, rxbuf + NLMSG_HDR + NL_FRAME_OFF);
		}
		out_flush();
	}
	if (fd >= 0) {
		nl_redirect((int)fd, tid, REDIRECT_TYPE, NL_ACT_DEREG, 0);
		sys_close((int)fd);
	}
	if (cliq >= 0)
		mq_remove(cliq);
	if (vq >= 0 && vq_is_ours)
		mq_remove(vq);
	out_fmt("done after %d frames\n", (long)n);
	out_flush();
	return 0;
}
