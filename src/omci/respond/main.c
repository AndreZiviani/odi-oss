/* omcid -- answer the OLT's OMCI opening.
 *
 * See ../README.md for the design rationale. What the OLT says when an ONU
 * re-registers was captured once and is short: a MIB reset, then five gets
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
#include "omcid.h"
#include "../redirect_guard.h"

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
static void on_signal(int sig)
{
	(void)sig;
	/* Put the sink back FIRST. out_set_sink flushes through the current
	 * sink on the way out, and while a CLI command is in flight that sink
	 * is cli_sink -- which would spend the whole retry budget pushing the
	 * tail of a dump into a queue we are about to delete, before this
	 * handler got anywhere near exiting. */
	out_set_sink(0);
	if (nl_fd >= 0)
		nl_redirect(nl_fd, nl_tid, REDIRECT_TYPE, NL_ACT_DEREG, 0);
	/* Our queues outlive the process otherwise, and this kernel has 53 of
	 * them in total. Only ours: reply_q belongs to a client, which removes
	 * its own. */
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

	/* Every argument is matched whole, and anything unrecognised stops the
	 * program before it opens a socket. The loose prefix match used here
	 * before let `omcid --help` start a second daemon that registered for
	 * redirect type 1 and took the OLT channel from the running one. */
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-h") || str_eq(argv[i], "--help")) {
			usage();
			out_flush();
			return 0;
		} else if (str_eq(argv[i], "-f") || str_eq(argv[i], "--force")) {
			force = 1;
		} else if (str_eq(argv[i], "-a"))
			apply_hw = 1;
		/* -r: this omcid replaces one that was running. The switch still
		 * holds the bridge connections the old one activated, and this
		 * one starts with an empty table of them, so the first rebuild
		 * would add a second set beside the first. /etc/scripts/apply
		 * passes it; rcS, starting the first omcid of a boot, does not. */
		else if (str_eq(argv[i], "-r"))
			restart = 1;
		/* -d: run as the daemon. The 400-frame and 120 s idle limits are
		 * from this program's capture days; started from rcS it is the
		 * OMCI stack and stops only on a signal. (Boot 15, 2026-09-17:
		 * it answered the OLT's 400th frame and exited, mid-session.) */
		else if (str_eq(argv[i], "-d")) {
			want = 0x7fffffff;
			maxidle = 0x7fffffff / (5000000 / NL_POLL_US) - 1;
		}
		/* -c <120 bytes of hex>: use this capability blob instead of
		 * asking the driver for one.
		 *
		 * The blob is read-only device information -- port map, flow
		 * and T-CONT counts -- and everything that builds a data path
		 * depends on it. Being able to supply it means the whole
		 * construction can be exercised off the device, against a blob
		 * captured from a real one. Without this, anything downstream
		 * of caps is only testable on hardware.
		 *
		 * It does not enable -a and cannot make a hardware call
		 * succeed; it only fills in what those calls would be built
		 * from. */
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

	/* Opened here, before the capability/serial/device-id reads below,
	 * not at its previous position further down: those three reads go
	 * through omci_drv_call(), which tries the ODI_OMCI_OP_CMD netlink
	 * path first and needs nl_fd/nl_tid
	 * (omcid.h) already set to do that -- they used to stay at their
	 * -1/0 defaults until catch_signals() ran, well after these three
	 * calls, so a SWITCH=oss image always fell through to the (absent,
	 * on this image) sockopt path for its very first three driver calls
	 * and logged "could not read" for all of them even though the
	 * driver was answering everything from the fourth call on (s1 trial,
	 * s2 fixes). catch_signals()
	 * below still runs at its old position and re-sets the same two
	 * globals -- harmless, and keeps the signal-handler installation
	 * exactly where it was.
	 */
	fd = nl_open(&tid, NL_POLL_US);
	if (fd < 0) {
		out_fmt("%% netlink: %d -- no line side. The message queues are "
			"still served, so `omcli --inject` works and replies "
			"are logged rather than sent.\n", (long)fd);
		tid = (uint32_t)sys_gettid();
	}
	/* Before anything reads the driver or takes a queue: a second omcid
	 * must not get as far as registering over the first. Only with a
	 * line side -- under qemu there is no netlink and no /proc/odi_omci,
	 * and nothing to take. */
	if (fd >= 0 && redirect_guard("omcid", REDIRECT_TYPE, force, tid)) {
		sys_close((int)fd);
		out_flush();
		return 1;
	}
	nl_fd = fd;
	nl_tid = tid;

	/* Not gated on -a. Reading the capability blob is a GET: it allocates
	 * nothing and changes nothing, and without it a dry run maps no UNI at
	 * all, so every class that resolves a TP pointer through the slot table
	 * reports "in no capability slot" and the run says less than it could.
	 * Only the setters below are gated. */
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

	/* The config store, read once, here. The identity used to be loaded
	 * only when someone ran `omcli ident`, so until then the CTC LOID
	 * entity answered empty whatever the store said. */
	cfg_load_identity();
	cfg_load_vlan();
	cfg_load_report();
	out_fmt("store: loid %s, manual vlan %s, identity report %s\n",
		ident.loid[0] ? "set" : "none",
		vlanCfg.manual ? "on" : "off",
		report.on ? "on" : "off");

	if (restart && apply_hw && fd >= 0) {
		/* Deactivating a service id the driver does not hold is not an
		 * error there (odi_switch_cmd.c cmd 50), so every id is sent
		 * rather than guessing which ones the old daemon used. */
		for (int id = 0; id < SERV_MAX; id++)
			omci_deactiveBdgConn((uint32_t)id);
		out_fmt("restart: %d bridge connection ids cleared\n", (long)SERV_MAX);
	}

	/* Two milliseconds, not five seconds. The loop only looks at the two
	 * message queues between netlink timeouts, and the stock omcicli waits
	 * 12 ms for its answer and then gives up -- so the poll has to fit
	 * inside that with room to write the reply. It costs about 500
	 * iterations a second, each three non-blocking syscalls that usually
	 * return nothing. `-w` keeps meaning five-second units; the idle count
	 * is scaled rather than the flag's meaning changed. */
	/* No netlink is not fatal, and used to be.
	 *
	 * A frame reaches this daemon two ways: off the line through netlink,
	 * and injected on the vendor's message queue (msgType 0). The second
	 * exists precisely so the responder can be exercised with no OLT --
	 * and it needs no netlink at all, since the only thing netlink would
	 * carry is the reply. Refusing to start without it meant refusing to
	 * start in exactly the situation that feature was built for.
	 *
	 * It also makes the emulator usable. qemu-user translates only
	 * NETLINK_ROUTE and NETLINK_AUDIT and answers EPROTONOSUPPORT (-120 on
	 * MIPS) for NETLINK_USERSOCK, so under qemu this socket can never open,
	 * whatever is or is not behind it. Everything that is not a hardware
	 * call still runs there: the MIB store, both CLI protocols, the dump
	 * renderers, and the injection path.
	 *
	 * Answers to injected frames are composed and logged as usual; with no
	 * socket they are not transmitted, which is the honest behaviour when
	 * there is nowhere to transmit them to. fd/tid were already opened
	 * above (moved there, s2 fixes), not reopened here.
	 */
	/* Our own queue, not the vendor's 0x800: a client can tell the good
	 * protocol is available by asking whether this key exists, which beats
	 * guessing what an unknown message type does to a daemon that is not
	 * ours. */
	cliq = mq_open_fresh(OMCLI_KEY);
	if (cliq < 0)
		out_fmt("%% no cli queue (%d); omcli will not find this daemon\n",
			(long)cliq);

	/* The vendor's queue, for boa and the scripts that shell out to
	 * omcicli, is taken in the loop rather than here -- omci_app usually
	 * still holds it at this point. See vq_ensure. */
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
		/* Serving a queue is not idling. Without this the no-netlink
		 * branch below never resets the counter -- the only reset was
		 * on a received netlink frame -- so a daemon with no line side
		 * exited after -w units however busy its queues were, which is
		 * exactly the mode that branch exists to enable. */
		if (cli_poll() || vq_poll())
			idle = 0;
		/* One quiet second after the OLT last touched a bridge port, GEM
		 * CTP/IW TP, mapper or VLAN entity: derive the bridge connections
		 * from the MIB as it stands (apply.c bdgconn_rebuild). Keep this
		 * before the no-netlink branch so injected QEMU frames exercise the
		 * same deferred rebuild as frames received from the line. */
		{
			static unsigned tick;

			if (++tick % (1000000 / NL_POLL_US) == 0) {
				onu_state_sample();
				serial_refresh(1);
			}
		}
		if (qos_dirty && idle == 1000000 / NL_POLL_US) {
			qos_dirty = 0;
			us_qos_rebuild();
		}
		if (conn_dirty && idle == 1000000 / NL_POLL_US) {
			conn_dirty = 0;
			bdgconn_rebuild();
		}
		if (fd < 0) {
			/* No line side: the queues are the only input, and
			 * they are polled above. Pace the loop the way the
			 * netlink receive timeout would. */
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
