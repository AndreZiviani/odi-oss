/* The vendor's 45-command protocol, on queue 0x800.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"

/* ---------------------------------------------------- the vendor's protocol
 *
 * `boa` and five scripts on this device drive OMCI through /bin/omcicli, so a
 * daemon that replaces omci_app has to answer on the vendor's queue too, or
 * the web UI's OMCI pages stop working. That queue is 0x800 and the protocol
 * is the 45-id one in ../omci_msgq.h.
 *
 * Its answers come back a file, because the vendor's dump functions printf and
 * the handler dup2s a temp file over stdout. Nothing forces that on us -- but
 * the *client* is the stock one, and it sleeps 12 ms and then reads whatever
 * is in /tmp/temp_omcicli*. So the file is reproduced here: the daemon removes
 * the stale one, writes the answer, and the client finds it. That deadline is
 * also why the main loop polls at 2 ms.
 *
 * This does not reproduce the vendor's dump *format*, which differs between
 * tables in the same firmware.
 * A script that parses `mib get 256` by column will not be satisfied by this
 * yet; one that just wants the data will.
 */
long vq = -1;

int vq_is_ours;

static int vq_replied;

static struct omci_msg vreq, vrep;

long vfile = -1;

static void vfile_sink(const char *s, int n)
{
	if (vfile >= 0)
		sys_write((int)vfile, s, (unsigned long)n);
}

/* The stock client scans /tmp and prints the first temp_omcicli* it finds, and
 * never removes it -- the next command's handler does. Same contract here. */
static void vfile_open(void)
{
	static char dbuf[2048];
	long fd = sys_open(OMCI_TMP_DIR, 0), n;

	if (fd >= 0) {
		while ((n = sys_getdents64((int)fd, dbuf, sizeof dbuf)) > 0)
			for (long off = 0; off < n;) {
				struct dirent64 *d = (struct dirent64 *)(dbuf + off);
				char path[128];

				if (str_has_prefix(d->d_name, OMCI_TMP_PREFIX)) {
					omci_tmp_path(path, (int)sizeof path,
						      d->d_name);
					sys_unlink(path);
				}
				off += d->d_reclen;
			}
		sys_close((int)fd);
	}
	vfile = sys_create(OMCI_TMP_OURS,
			   O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (vfile >= 0)
		out_set_sink(vfile_sink);
}

static void vfile_close(void)
{
	out_flush();
	out_set_sink(0);
	if (vfile >= 0)
		sys_close((int)vfile);
	vfile = -1;
}

/* A queue-answering command: the vendor's handlers build a 240-byte reply and
 * send it to the key in the request. */
static void vq_reply(uint32_t id, uint32_t word1, uint32_t cls, uint16_t ent,
		     const char *text)
{
	long rq = mq_open(vreq.replyKey, 0);
	uint8_t *b = (uint8_t *)&vrep;

	vq_replied = 1;
	if (rq < 0)
		return;
	/* Zero first, then fill. The vendor's own OMCI_MibAttrGet_Cmd does it
	 * the other way round and memsets its command id back to zero; the
	 * stock client does not check, but there is no reason to copy a bug. */
	for (unsigned i = 0; i < sizeof vrep; i++)
		b[i] = 0;
	vrep.mtype   = OMCI_MQ_MTYPE;
	vrep.msgType = OMCI_MQ_TYPE_CLI;
	vrep.len     = OMCI_MQ_LEN;
	mq_put32(vrep.p + OMCI_P_ID, id);
	mq_put32(vrep.p + 4, word1);
	mq_put32(vrep.p + OMCI_P_CLASS, cls);
	vrep.p[OMCI_P_ENTITY] = (uint8_t)(ent >> 8);
	vrep.p[OMCI_P_ENTITY + 1] = (uint8_t)ent;
	if (text)
		str_copy((char *)vrep.p + OMCI_P_VALUE, text, 80);
	/* IPC_NOWAIT, and a short bounded retry.
	 *
	 * rq is a key the CLIENT chose, and the stock omcicli hardcodes 0x6868
	 * -- one queue shared by every run this device ever does. Replies
	 * nobody drains accumulate there (a client killed after sending, or one
	 * that gave up waiting), msgmnb is 16384 and a message is 260 bytes, so
	 * about 63 orphans fill it. A blocking send would then park this
	 * daemon's only thread inside msgsnd forever: no netlink, no CLI, no
	 * OMCI. That is the mirror of the trap omci_SendCmdAndGet sets for the
	 * client, and there is no reason to fall into our own version of it. */
	for (int tries = 0; tries < CLI_RETRIES; tries++) {
		long rc = mq_snd_flags(rq, &vrep, OMCI_MQ_LEN - 4, IPC_NOWAIT);

		if (rc >= 0)
			return;
		if (rc != -MQ_EAGAIN)
			return;          /* never going to be deliverable */
		sys_nanosleep(0, CLI_RETRY_NS);
	}
}

/* How long the ONU has been in O5, for `get authuptime` ("PON duration
 * time"). Sampled from the main loop about once a second rather than on
 * demand, so a request costs no driver call. */
static long o5_since = -1;

void onu_state_sample(void)
{
	uint32_t st = 0;
	long sec = 0, nsec = 0;

	if (omci_getOnuState(&st) != 0)
		return;
	if (st != 5) {
		o5_since = -1;
		return;
	}
	if (o5_since < 0 && sys_clock_gettime(1, &sec, &nsec) == 0)
		o5_since = sec;             /* CLOCK_MONOTONIC */
}

static long onu_o5_seconds(void)
{
	long sec = 0, nsec = 0;

	if (o5_since < 0 || sys_clock_gettime(1, &sec, &nsec) != 0)
		return 0;
	return sec - o5_since;
}

static void vq_dispatch(void)
{
	uint32_t id = mq_get32(vreq.p + OMCI_P_ID);
	uint32_t cls = mq_get32(vreq.p + OMCI_P_CLASS);
	uint32_t arg = mq_get32(vreq.p + OMCI_P_ARG);
	uint16_t ent = (uint16_t)((vreq.p[OMCI_P_ENTITY] << 8) |
				  vreq.p[OMCI_P_ENTITY + 1]);

	switch (id) {
	case 13:                                 /* loid auth status */
		vq_reply(id, 0, 0, 0, 0);
		return;
	case 23:                                 /* auth uptime: seconds in O5 */
		vq_reply(id, (uint32_t)onu_o5_seconds(), 0, 0, 0);
		return;
	case 33: {                               /* mib getattr */
		const struct omci_class *c = find_class((uint16_t)cls);
		char v[48];
		int k = 0;

		/* Any entity this ONU holds, autonomous ones included, with
		 * the value a Get returns (attr_value), not only the rows the
		 * OLT created -- the same rule as mib_dump(). */
		if (c && (mib_find((uint16_t)cls, ent) ||
			  instance_is_autonomous((uint16_t)cls, ent)) &&
		    arg && arg < c->nattr && arg <= 16) {
			uint8_t val[MIB_ROW_MAX];
			uint16_t w = attr_width(c, (unsigned)arg);

			if (w > MIB_ROW_MAX)
				w = 0;
			if (w)
				attr_value(c, ent, (unsigned)arg, val);
			for (uint16_t b = 0; b < w && k < 40; b++) {
				static const char hex[] = "0123456789abcdef";

				v[k++] = hex[(val[b] >> 4) & 15];
				v[k++] = hex[val[b] & 15];
			}
		}
		v[k] = 0;
		/* The vendor writes its id into the reply and then memsets the
		 * buffer, so the stock client reads a zero there and does not
		 * mind. Echo it properly; nothing breaks either way. */
		vq_reply(id, arg, cls, ent, v);
		return;
	}
	case 32:                                 /* mib reset */
		mib_reset_all();
		return;
	case 4:                                  /* set logfile -- nothing to do */
	case 41:                                 /* detect iot vlan */
		return;
	}

	/* Everything else answers through the file. */
	vfile_open();
	switch (id) {
	case 1: {
		/* The vendor prints the four binary bytes as UPPERCASE hex
		 * (e.g. SerialNumber: FHTT01020304), and the UI and the
		 * exporter read that line. */
		static const char hexu[] = "0123456789ABCDEF";

		serial_refresh(0);
		out("SerialNumber: ");
		out_n((const char *)serial, 4);
		for (int i = 4; i < 8; i++) {
			out_char(hexu[serial[i] >> 4]);
			out_char(hexu[serial[i] & 15]);
		}
		out_char('\n');
		break;
	}
	case 3:                                  /* get log */
		out("Usr Log Lvl: Error\nDrv Log Lvl: Error\n");
		break;
	case 5:                                  /* get logfile */
		out("Mode:\nWithout logging to file\nActionMask:\n0x8150\n"
		    "Log File:\n/tmp/omcilog\n");
		break;
	case 8:                                  /* get devmode */
		out("DevMode: bridge\n");
		break;
	case 10: {                               /* get dmmode */
		char v[16];
		int n = cfg_get(CFG_CS_PATH, "DUAL_MGMT_MODE", v, sizeof v);

		/* DUAL_MGMT_MODE=1 prints as wan_queue on isp2; the other
		 * spellings are not captured, so they print as the number. */
		if (n > 0 && str_eq(v, "1"))
			out("dmMode = wan_queue\n");
		else if (n > 0 && str_eq(v, "0"))
			out("dmMode = off\n");
		else
			out_fmt("dmMode = %s\n", n > 0 ? v : "unknown");
		break;
	}
	case 22: {                               /* get cflag */
		static const struct { const char *key, *label; } f[] = {
			{ "OMCI_CUSTOM_BDP",   "BDP: 0x" },
			{ "OMCI_CUSTOM_RDP",   "RDP: 0x" },
			{ "OMCI_CUSTOM_MCAST", " MC: 0x" },
			{ "OMCI_CUSTOM_ME",    " ME: 0x" },
		};

		for (unsigned i = 0; i < 4; i++) {
			char v[16];
			uint32_t x = 0;
			int n = cfg_get(CFG_CS_PATH, f[i].key, v, sizeof v);

			for (int k = 0; n > 0 && v[k] >= '0' && v[k] <= '9'; k++)
				x = x * 10 + (uint32_t)(v[k] - '0');
			out(f[i].label);
			out_hex(x, 8);
			out_char('\n');
		}
		break;
	}
	case 19:
		out_fmt("OMCI DRV version : %s\n", (const char *)devid);
		break;
	case 6:
		/* `omcicli get tables`. In the vendor this command DEADLOCKS:
		 * MIB_ShowAll locks gOmciTableMutex, printfs each registered
		 * table, then locks it again where the unlock belongs -- both
		 * calls read gp-32544, and pthread_mutex_unlock is gp-32356.
		 * The listing never flushes (zero bytes) and omci_app's only
		 * message-loop thread never reaches msgrcv again, so every
		 * later command of any kind goes unanswered -- OMCI frames
		 * from the line included.
		 *
		 * Here there is no mutex and nothing to deadlock on. The
		 * format is the vendor's own, read out of libomci_mib.so:
		 * "TableId [%d] Name: %s!" per line.
		 *
		 * The ids are OURS -- the index into the model -- where the
		 * vendor's come from the order it happened to load
		 * the plugin directory. Nothing parses them: the one consumer ever
		 * seen is a UI populating a picker, which reads the names. */
		for (unsigned i = 0; i < omci_class_count; i++)
			out_fmt("TableId [%d] Name: %s!\n", (long)i,
				omci_classes[i].name);
		break;
	case 29: {                               /* mib get */
		const char *name = (const char *)vreq.p + OMCI_P_NAME;
		uint32_t want = 0;

		if (name[0]) {
			for (const char *p = name; *p; p++)
				want = (*p >= '0' && *p <= '9')
					? want * 10 + (uint32_t)(*p - '0') : 0;
			if (!want)
				for (unsigned i = 0; i < omci_class_count; i++)
					if (str_eq(omci_classes[i].name, name)) {
						want = omci_classes[i].classId;
						break;
					}
			/* A name that is no class used to leave `want` at
			 * zero, and zero is "every class": a typo dumped the
			 * whole MIB. "0" itself still means every class. */
			if (!want && !(name[0] == '0' && !name[1])) {
				out_fmt("no managed entity called %s\n", name);
				out("0 rows\n");
				break;
			}
		}
		/* The vendor's format for the classes that have a renderer
		 * (scripts on the stick parse those by key), our own dump for
		 * the rest, over every entity -- autonomous ones included --
		 * and always closed by the row count. See mib_dump(). */
		mib_dump((uint16_t)want, arg != 0xffffffffu, (uint16_t)arg, 1);
		break;
	}
	case 35:                                 /* dump qmap */
		qmap_dump();
		break;
	case 36:                                 /* dump conn: the vendor prints
						  * its tree pointers; ours prints
						  * the connections themselves */
	case 37:                                 /* dump srvflow: SERVID rows */
		cli_conn();
		break;
	case 38:
		cli_state();
		break;
	default:
		out_fmt("omcid does not implement command %d\n", (long)id);
		break;
	}
	vfile_close();
}

/* Take the vendor's queue as soon as nobody else has it.
 *
 * omci_app removes 0x800 when it stops -- the queue id changes across every
 * restart, which is how you can tell -- so attaching to its queue at startup
 * buys an id that goes dead the moment omci_app is killed, and then neither we
 * nor omcicli can use the key: our msgrcv fails forever and the client's
 * msgget finds nothing. Create it instead, and keep trying until it is ours.
 * Checked about once a second, since until it succeeds there is nothing to
 * serve anyway.
 *
 * omci_app never runs on this image at all (../README.md: this replaces it
 * outright), so a queue already sitting at 0x800 here is never a stock
 * daemon's -- it can only be a previous omcid's, left behind because a
 * respawn goes through kill -9 (inittab has no graceful stop), which skips
 * on_signal()'s mq_remove(). IPC_EXCL then fails EEXIST forever and this
 * daemon waits out a queue nobody but a dead instance of itself ever held,
 * while requests pile up unanswered. Since there is no real owner to wait
 * for on our image, reclaim it instead: remove whatever is there and create
 * fresh, exactly like mq_open_fresh() does for the omcli queue. */
void vq_ensure(void)
{
	static unsigned tick;
	long rc;

	if (vq >= 0 || (tick++ % (1000000 / NL_POLL_US)))
		return;
	rc = __syscall6(__NR_ipc, IPC_MSGGET, OMCI_MQ_KEY,
			IPC_CREAT | IPC_EXCL | 0600, 0, 0, 0);
	if (rc == -MQ_EEXIST) {
		long old = __syscall6(__NR_ipc, IPC_MSGGET, OMCI_MQ_KEY, 0,
				      0, 0, 0);

		if (old >= 0)
			mq_remove(old);
		rc = __syscall6(__NR_ipc, IPC_MSGGET, OMCI_MQ_KEY,
				IPC_CREAT | IPC_EXCL | 0600, 0, 0, 0);
	}
	vq = rc;
	if (vq >= 0) {
		vq_is_ours = 1;
		out("[cli] took the omcicli queue\n");
		out_flush();
	}
}

int vq_poll(void)
{
	long rc;

	vq_ensure();
	if (vq < 0)
		return 0;
	/* MSG_NOERROR: any root process can write this key, and without it an
	 * oversize message is left in the queue rather than consumed -- every
	 * later poll would retrieve the same unreadable message and the vendor
	 * CLI would be dead until this daemon restarts. */
	rc = mq_rcv_flags(vq, &vreq, OMCI_MQ_LEN - 4, -2, IPC_NOWAIT | MSG_NOERROR);
	if (rc < 0) {
		/* EIDRM/EINVAL: somebody removed it under us. Go back to
		 * looking for a chance to create it again. This used to test
		 * -43, which is EIDRM on x86 and ENOCSI on MIPS, so killing
		 * omci_app out from under us left a dead queue id here for
		 * good and the vendor CLI silently stopped working. */
		if (rc == -MQ_EIDRM || rc == -MQ_EINVAL) {
			vq = -1;
			vq_is_ours = 0;
		}
		return 0;
	}
	if (vreq.msgType == OMCI_MQ_TYPE_FRAME) {
		/* A raw OMCI frame, injected rather than received off the line.
		 * This is how `debug loadpkt` feeds the vendor a frame, and in
		 * the vendor it is not a debug path at all: the netlink reader
		 * posts every frame it receives onto this queue with this type,
		 * and OMCI_HandleMsg's case 0 hands it to the ordinary receive
		 * path. So an injected frame and a received one
		 * are the same frame arriving by two roads.
		 *
		 * We take the other road -- straight off netlink -- so this is
		 * purely an injection point, and it is what makes the responder
		 * testable with no OLT: feed it a request, watch the reply.
		 *
		 * The reply goes out netlink exactly as it would for a line
		 * frame. With no OLT attached that send reaches nobody, which
		 * is the point: what is being checked is the answer this
		 * daemon composes, and that is logged either way. */
		if (vreq.len >= OMCI_FRAME_LEN) {
			out_fmt("[mq] injected frame, %d bytes\n",
				(long)vreq.len);
			handle((int)nl_fd, nl_tid, vreq.p);
			out_flush();
		} else {
			out_fmt("[mq] injected frame ignored: %d bytes, "
				"need %d\n", (long)vreq.len,
				(long)OMCI_FRAME_LEN);
			out_flush();
		}
		return 1;                        /* a frame arrived: not idle */
	}
	if (vreq.msgType != OMCI_MQ_TYPE_CLI)
		return 0;
	vq_replied = 0;
	out_fmt("[cli] omcicli command %d\n", (long)mq_get32(vreq.p + OMCI_P_ID));
	out_flush();
	vq_dispatch();
	/* omci_SendCmdAndGet blocks on its reply queue with no timeout, so a
	 * request that asked for an answer and did not get one hangs the
	 * client for good. Answer everything that asked. */
	if (vreq.replyKey && !vq_replied)
		vq_reply(mq_get32(vreq.p + OMCI_P_ID), 0, 0, 0, 0);
	return 1;
}
