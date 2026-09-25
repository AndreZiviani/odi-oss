/* omcli -- omcicli, without the race.
 *
 * Phase 5 of replacing omci_app (see ../PLAN.md). The wire format is the
 * vendor's, so this drives the stock daemon today and our own later: one
 * System V message queue, a 20-byte header and a 240-byte payload with the
 * command id in word 0. What changes is everything around it.
 *
 *   * **Dumps are waited for, not slept through.** The stock client sleeps
 *     12 ms, then prints whatever is in the temp file -- which is why long
 *     dumps come back cut off mid-entry. omci_app's handler loop is a single
 *     thread and every CLI message carries mtype 2, so CLI messages are FIFO
 *     with respect to each other: a cheap queue-answering command sent after a
 *     dump cannot be answered until the dump handler has returned, which is
 *     after its fflush. That reply is an exact completion signal.
 *   * **The reply queue is per process.** The vendor hardcodes key 0x6868, so
 *     two concurrent runs read each other's answers.
 *   * **The temp file is read, not `cat`ed**, and removed afterwards rather
 *     than left for the next command's `rm -rf` to find.
 *   * **`get tables` is refused.** Its handler, MIB_ShowAll, takes the table
 *     mutex twice and deadlocks the daemon's only message-loop thread. The
 *     stock client will happily send it.
 *
 * Payload offsets are recovered from omci_app's own dispatch table by
 * ../tools/omci-cli-table.py, not read off the stock client.
 */
#include "sys.h"
#include "io.h"
#include "../omci.h"
#include "../omci_msgq.h"
#include "../omci_cli_proto.h"
#include "../omci_tmpfile.h"

#define SIGPIPE 13
#define SIGINT   2
#define SIGTERM 15
#define SIGHUP   1

/* The reply queue native_run is holding, for the signal handler.
 *
 * It is created with IPC_PRIVATE -- no key -- so a client killed before its
 * cleanup leaks a queue that NOTHING can remove afterwards: `--rmq` takes a
 * key, and this busybox has no ipcrm. The kernel has 53 queue slots in total,
 * so a handful of interrupted runs is a real budget. */
static volatile long cleanup_q = -1;

#define IPC_NOWAIT      04000
#define BARRIER_ID      13      /* OMCI_LoidAuthStatusGet_Cmd: two loads and a
				 * send, the cheapest thing that answers */
#define WAIT_MS         20000   /* a full MIB dump on a provisioned stick is
				 * seconds, not milliseconds */

enum { ANS_NONE, ANS_QUEUE, ANS_FILE };
enum { A_NONE, A_MIBSEL, A_CLSENT, A_ATTR, A_NUM,
       A_LOG, A_LOGFILE, A_STR35, A_KEYNUM, A_LOID, A_SN,
       A_MIBDEL, A_MIBCREATE, A_MIBSET, A_SIMAVC };

/* Commands that change something need -f, on the same reasoning as omciprobe:
 * this drives a daemon that is running a live line, and a typo in a class id
 * is a deleted managed entity. Reading needs no ceremony; `set log` and the
 * debug dumps are reversible and need none either. */
#define F_WRITE 1

/* Keyword tables the vendor resolves by strcmp inside the handler or the
 * client. Values read from the handlers, not from the usage text. */
static const char *const log_levels[] = { "off", "err", "warn", "info", "dbg", 0 };
static const char *const dm_modes[] = { "off", "on_wq", "on_bc_mc",
					"on_wq_bc_mc", 0 };

struct cmd {
	const char *group;
	const char *name;
	unsigned char id;
	unsigned char answer;
	unsigned char args;
	unsigned char flags;
	const char *help;
};

/* Only the commands that read. The setters are the same transport and are the
 * next thing to add; until each one's payload has been read out of its handler
 * rather than guessed, sending it would be writing bytes into a live daemon on
 * a hunch. */
static const struct cmd cmds[] = {
{ "get",  "sn",        1,  ANS_FILE,  A_NONE,   0, "serial number" },
{ "get",  "log",       3,  ANS_FILE,  A_NONE,   0, "runtime log level" },
{ "get",  "logfile",   5,  ANS_FILE,  A_NONE,   0, "omci msg log mode and action mask" },
{ "get",  "devmode",   8,  ANS_FILE,  A_NONE,   0, "omci device mode" },
{ "get",  "dmmode",   10,  ANS_FILE,  A_NONE,   0, "dual management mode" },
{ "get",  "loid",     12,  ANS_FILE,  A_NONE,   0, "loid and password" },
{ "get",  "loidauth", 13,  ANS_QUEUE, A_NONE,   0, "loid auth status" },
{ "get",  "loidnum",  14,  ANS_QUEUE, A_NONE,   0, "loid auth attempt count" },
{ "get",  "cflag",    22,  ANS_FILE,  A_NONE,   0, "customized flag" },
{ "get",  "authuptime", 23, ANS_QUEUE, A_NONE,  0, "auth uptime" },
{ "get",  "oltloc",   24,  ANS_QUEUE, A_NONE,   0, "OLT location, ME 131 -- no stock keyword reaches this" },
{ "get",  "version",  19,  ANS_FILE,  A_NONE,   0, "driver version" },
{ "get",  "tables",    6,  ANS_FILE,  A_NONE,   0, "registered MIB tables -- DEADLOCKS omci_app, needs -f" },

{ "set",  "log",       2,  ANS_NONE,  A_LOG,     0, "<usrLevel> <drvLevel>, each off|err|warn|info|dbg" },
{ "set",  "logfile",   4,  ANS_NONE,  A_LOGFILE, 0, "<mode> [actionMask] [file]" },
{ "set",  "sn",        0,  ANS_NONE,  A_SN,      F_WRITE, "<vendorId> <8 hex digits>" },
{ "set",  "devmode",   7,  ANS_NONE,  A_STR35,   F_WRITE, "router|bridge|hybrid" },
{ "set",  "dmmode",    9,  ANS_NONE,  A_KEYNUM,  F_WRITE, "off|on_wq|on_bc_mc|on_wq_bc_mc" },
{ "set",  "loid",     11,  ANS_NONE,  A_LOID,    F_WRITE, "<loid> [password]" },
{ "set",  "resetlauth", 15, ANS_QUEUE, A_NONE,   F_WRITE, "reset the loid auth attempt count" },

{ "mib",  "get",      29,  ANS_FILE,  A_MIBSEL, 0, "[all | classId | tableName] [entityId]" },
{ "mib",  "getcurr",  30,  ANS_FILE,  A_MIBSEL, 0, "PM current accumulations: classId|tableName [entityId]" },
{ "mib",  "getalm",   31,  ANS_FILE,  A_CLSENT, 0, "[classId [entityId]]" },
{ "mib",  "getattr",  33,  ANS_QUEUE, A_ATTR,   0, "classId entityId attrIndex" },
{ "mib",  "create",   26,  ANS_NONE,  A_MIBCREATE, F_WRITE, "classId entityId \"set-by-create values\"" },
{ "mib",  "delete",   27,  ANS_NONE,  A_MIBDEL,    F_WRITE, "classId entityId" },
{ "mib",  "set",      28,  ANS_NONE,  A_MIBSET,    F_WRITE, "classId entityId attrName value" },
{ "mib",  "reset",    32,  ANS_NONE,  A_NONE,      F_WRITE, "trigger a MIB reset -- the OLT reprovisions, or does not" },

{ "dump", "avltree",  34,  ANS_FILE,  A_NUM,    0, "[avlKeyId]" },
{ "dump", "qmap",     35,  ANS_FILE,  A_NONE,   0, "tcont queue mapping" },
{ "dump", "conn",     36,  ANS_FILE,  A_NONE,   0, "data path connections" },
{ "dump", "srvflow",  37,  ANS_FILE,  A_NONE,   0, "data path service flows" },
{ "dump", "tasks",    38,  ANS_FILE,  A_NONE,   0, "tasks" },

{ "debug", "simavc",  40,  ANS_NONE,  A_SIMAVC, F_WRITE, "classId entityId attrIndex (1..16)" },
{ "debug", "detectiotvlan", 41, ANS_FILE, A_NONE, 0, "detect IOT vlan info" },
{ "debug", "gendot",  42,  ANS_FILE,  A_NONE,   0, "write /tmp/omci_dot_file" },
{ "debug", "showregmod", 43, ANS_FILE, A_NONE,  0, "registered feature modules" },
{ "debug", "showregapi", 44, ANS_FILE, A_NONE,  0, "registered feature api" },
};

#define NCMDS ((int)(sizeof cmds / sizeof cmds[0]))

static struct omci_msg req, rep;
static char dirbuf[4096];
static char filebuf[8192];

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	int n = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (s += 2; *s; s++, n++) {
			char c = *s;

			if (is_digit(c)) v = v * 16 + (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
			else return 0;
		}
	} else {
		for (; *s; s++, n++) {
			if (!is_digit(*s))
				return 0;
			v = v * 10 + (uint32_t)(*s - '0');
		}
	}
	*out = v;
	return n > 0;
}

static void usage(void)
{
	const char *g = "";

	out("usage: omcli <group> <command> [args]\n\n");
	for (int i = 0; i < NCMDS; i++) {
		if (!str_eq(g, cmds[i].group)) {
			g = cmds[i].group;
			out_fmt("  %s\n", g);
		}
		out_fmt("    %-11s id %-3d %s %s\n", cmds[i].name, (long)cmds[i].id,
			(cmds[i].flags & F_WRITE) ? "-f" : "  ", cmds[i].help);
	}
	out("\n  --id N [-f]     send a command id directly; -f is needed for\n"
	    "                  any id not listed above, which is assumed to write\n");
	out("  --vendor        skip omcid and talk to the stock daemon\n");
	out("  --rmq KEY       remove a message queue left by a killed client\n");
	out("  --inject HEX    feed a 48-byte OMCI frame in as if it arrived\n"
	    "                  off the line; the answer goes out the line\n");
	out("\nThe list above is the stock daemon's. When omcid is listening its\n"
	    "own commands are used instead -- `omcli help` for those.\n");
	out("  Read commands only for now; the setters come with their payloads.\n");
	out_flush();
}

/* ------------------------------------------------------------------ the wire */

static long cmdq = -1, replyq = -1;
static uint32_t replykey;

static void msg_init(struct omci_msg *m, uint32_t id, uint32_t reply_to)
{
	uint8_t *b = (uint8_t *)m;

	for (unsigned i = 0; i < sizeof *m; i++)
		b[i] = 0;
	m->mtype    = OMCI_MQ_MTYPE;
	m->msgType  = OMCI_MQ_TYPE_CLI;
	m->replyKey = reply_to;
	m->len      = OMCI_MQ_LEN;
	mq_put32(m->p + OMCI_P_ID, id);
}

/* Wait for a reply, without blocking forever on a daemon that has gone away. */
static int wait_reply(void)
{
	for (int ms = 0; ms < WAIT_MS; ms += 10) {
		long rc = mq_recv_flags(replyq, &rep, -2, IPC_NOWAIT);

		if (rc >= 0)
			return 0;
		sys_nanosleep(0, 10 * 1000 * 1000);
	}
	return -1;
}

/* The completion signal the stock client replaces with usleep(12000). */
static int barrier(void)
{
	struct omci_msg b;

	msg_init(&b, BARRIER_ID, replykey);
	if (mq_send(cmdq, &b) < 0) {
		out("% could not send the completion barrier\n");
		return -1;
	}
	if (wait_reply() < 0) {
		out("% the daemon did not answer; the dump may be incomplete\n");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------- the temp file */

/* omci_open_cli_fd() removes /tmp/temp_omcicli* before it makes its own, so
 * after our dump there is exactly one and it is ours. */
static int find_dump(char *path, int max)
{
	long fd = sys_open(OMCI_TMP_DIR, 0 /* O_RDONLY */), n;
	int found = 0;

	if (fd < 0)
		return 0;
	while (!found && (n = sys_getdents64((int)fd, dirbuf, sizeof dirbuf)) > 0) {
		for (long off = 0; off < n;) {
			struct dirent64 *d = (struct dirent64 *)(dirbuf + off);

			/* str_has_prefix comes from diag's io.c, which this
			 * already links -- there was a second copy of it here
			 * under another name. */
			if (str_has_prefix(d->d_name, OMCI_TMP_PREFIX)) {
				omci_tmp_path(path, max, d->d_name);
				found = 1;
				break;
			}
			off += d->d_reclen;
		}
	}
	sys_close((int)fd);
	return found;
}

static void print_dump(void)
{
	char path[128];
	long fd, n;

	if (!find_dump(path, sizeof path)) {
		out("% the daemon produced no output file\n");
		return;
	}
	fd = sys_open(path, 0);
	if (fd < 0) {
		out_fmt("%% cannot open %s\n", path);
		return;
	}
	while ((n = sys_read((int)fd, filebuf, sizeof filebuf)) > 0)
		out_n(filebuf, (int)n);
	sys_close((int)fd);
	sys_unlink(path);
}

/* --------------------------------------------------------------- the answers */

static void print_reply(uint32_t id)
{
	const uint8_t *p = rep.p;
	uint32_t echoed = mq_get32(p + OMCI_P_ID);

	/* Most handlers echo the id, but not all: OMCI_MibAttrGet_Cmd stores 33
	 * into its reply and *then* memsets the buffer, so id 33 always answers
	 * with a zero there. The reply queue is ours alone, so this is worth
	 * noting and not worth refusing. */
	if (echoed != id && echoed != 0)
		out_fmt("%% note: reply echoes command %d, not %d\n",
			(long)echoed, (long)id);
	switch (id) {
	/* The vendor omcicli shapes, captured on isp2 2026-09-21. */
	case 13:
		out_fmt("Auth Status : %d\nAuth Num : %d\nAuth Success Num : %d\n",
			(long)mq_get32(p + 4), (long)mq_get32(p + 8),
			(long)mq_get32(p + 12));
		break;
	case 23:
		out_fmt("PON duration time : %d.000000 seconds\n",
			(long)mq_get32(p + 4));
		break;
	case 24:
		out("olt location: ");
		out_n((const char *)p + OMCI_P_VALUE, 38);
		out_char('\n');
		break;
	case 33:
		/* The value comes back as text, which is why this is the read
		 * worth preferring: no file, no race. */
		out_fmt("class %d entity %d attr %d = %s\n",
			(long)mq_get32(p + OMCI_P_CLASS),
			(long)((p[OMCI_P_ENTITY] << 8) | p[OMCI_P_ENTITY + 1]),
			(long)mq_get32(p + OMCI_P_ARG),
			(const char *)p + OMCI_P_VALUE);
		break;
	default:
		out_fmt("reply for %d, word1 %d\n", (long)id, (long)mq_get32(p + 4));
		break;
	}
}

/* ------------------------------------------------------ omcid's own protocol
 *
 * When omcid is the daemon there is no temp file, no sleep and no 240-byte
 * ceiling: the request is argv and the answer is a framed stream that says how
 * long it is and when it ends. Which daemon is listening is settled by asking
 * whether omcid's queue exists -- a question with no side effects, where
 * sending an unknown message type to the vendor's daemon would be a guess.
 */
static struct omcli_req nreq;
static struct omcli_rep nrep;

static int native_run(int argc, char **argv, int first, long q)
{
	uint32_t arglen = 0, nargs = 0;
	long myq;

	/* IPC_PRIVATE: a queue with no key, so no two clients can collide and
	 * there is no key space to keep tidy. */
	myq = mq_open(0, 1);
	cleanup_q = myq;
	if (myq < 0) {
		out_fmt("%% cannot make a reply queue: %d\n", (long)myq);
		return 1;
	}
	for (int i = first; i < argc && nargs < OMCLI_MAXARGS; i++) {
		const char *a = argv[i];

		if (str_eq(a, "-f") || str_eq(a, "--native"))
			continue;
		for (; *a; a++) {
			if (arglen >= OMCLI_ARGS - 1)
				break;
			nreq.args[arglen++] = (uint8_t)*a;
		}
		/* The terminator has to fit too. It used to be written
		 * unconditionally, so once arglen reached OMCLI_ARGS - 1 every
		 * further argument broke out of the loop above and still wrote
		 * its NUL -- up to fifteen bytes past args[], into the reply
		 * buffer behind it -- and made a request one byte too long for
		 * the daemon's receive, which without MSG_NOERROR wedged its
		 * queue for good. Refuse instead. */
		if (arglen >= OMCLI_ARGS - 1) {
			out("% command line too long\n");
			mq_remove(myq);
		cleanup_q = -1;
			return 1;
		}
		nreq.args[arglen++] = 0;
		nargs++;
	}
	nreq.mtype    = OMCLI_MTYPE;
	nreq.magic    = OMCLI_MAGIC;
	nreq.version  = OMCLI_VERSION;
	nreq.replyQid = (uint32_t)myq;
	nreq.nargs    = nargs;
	if (mq_snd_flags(q, &nreq, OMCLI_REQ_LEN(arglen), 0) < 0) {
		out("% send failed\n");
		mq_remove(myq);
		cleanup_q = -1;
		return 1;
	}

	for (uint32_t want = 0;; want++) {
		int got = 0;

		for (int ms = 0; ms < WAIT_MS && !got; ms += 10) {
			if (mq_rcv_flags(myq, &nrep, sizeof nrep - 4, -2,
					 IPC_NOWAIT) >= 0)
				got = 1;
			else
				sys_nanosleep(0, 10 * 1000 * 1000);
		}
		if (!got) {
			out("% the daemon stopped answering mid-stream\n");
			mq_remove(myq);
		cleanup_q = -1;
			return 1;
		}
		/* A stream that skips a sequence number has lost a chunk; say so
		 * rather than print a plausible truncation. */
		if (nrep.magic != OMCLI_MAGIC || nrep.seq != want) {
			out_fmt("%% reply out of order (seq %d, wanted %d)\n",
				(long)nrep.seq, (long)want);
			mq_remove(myq);
		cleanup_q = -1;
			return 1;
		}
		if (nrep.len)
			out_n((const char *)nrep.data, (int)nrep.len);
		if (!(nrep.flags & OMCLI_MORE)) {
			if (nrep.status != OMCLI_OK)
				out_fmt("%% command failed, status %d\n",
					(long)nrep.status);
			mq_remove(myq);
		cleanup_q = -1;
			out_flush();
			return nrep.status ? 1 : 0;
		}
	}
}

/* ---------------------------------------------------------------------- main */

static int keyword(const char *const *tab, const char *s, uint32_t *out)
{
	for (int i = 0; tab[i]; i++)
		if (str_eq(tab[i], s)) {
			*out = (uint32_t)i;
			return 1;
		}
	return parse_u32(s, out);        /* a number is accepted too */
}

static void put_str(unsigned off, const char *s)
{
	str_copy((char *)req.p + off, s, 60);
}

static void put_entity(uint32_t v)
{
	req.p[OMCI_P_ENTITY] = (uint8_t)(v >> 8);
	req.p[OMCI_P_ENTITY + 1] = (uint8_t)v;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Two numbers, then optionally more: every mib command starts this way. */
static int class_entity(int argc, char **argv, int first, int need)
{
	uint32_t v;

	if (first + need > argc) {
		out("% not enough arguments\n");
		return -1;
	}
	if (!parse_u32(argv[first], &v)) {
		out("% class id must be a number\n");
		return -1;
	}
	mq_put32(req.p + OMCI_P_CLASS, v);
	if (!parse_u32(argv[first + 1], &v)) {
		out("% entity id must be a number\n");
		return -1;
	}
	put_entity(v);
	return 0;
}

static int fill_args(const struct cmd *c, int argc, char **argv, int first)
{
	uint32_t v;

	switch (c->args) {
	case A_NONE:
		return 0;
	case A_NUM:
		/* -1 is "all of them": `dump avltree` with no key id dumps
		 * every tree, and zero would silently mean tree 0 alone. */
		mq_put32(req.p + OMCI_P_ARG, 0xffffffffu);
		if (first < argc && parse_u32(argv[first], &v))
			mq_put32(req.p + OMCI_P_ARG, v);
		return 0;
	case A_MIBSEL:
		/* OMCI_MibDump_Cmd takes the selector as *text* and resolves
		 * it itself -- all digits means a class id, anything else a
		 * table name -- so the number at +20 is not a class id at all.
		 * It is only ever -1, which is what asks for every table; a
		 * non-negative value there means "use the string". Both the
		 * selector and the entity id default to -1, and that is the
		 * difference between dumping a table and dumping entity 0 of
		 * it. */
		mq_put32(req.p + OMCI_P_ARG, 0xffffffffu);
		if (first >= argc || str_eq(argv[first], "all")) {
			mq_put32(req.p + OMCI_P_CLASS, 0xffffffffu);
			return 0;
		}
		str_copy((char *)req.p + OMCI_P_NAME, argv[first], 60);
		if (first + 1 < argc && parse_u32(argv[first + 1], &v))
			mq_put32(req.p + OMCI_P_ARG, v);
		return 0;
	case A_CLSENT:
		/* "every class" is 0x7fffffff here, not -1: the handler
		 * compares against INT_MAX. The entity id is a halfword and
		 * its wildcard is 0xffff. Two different sentinels in two
		 * neighbouring commands, so neither is guessable -- both come
		 * from the handler. */
		mq_put32(req.p + OMCI_P_CLASS, 0x7fffffffu);
		req.p[OMCI_P_ENTITY] = 0xff;
		req.p[OMCI_P_ENTITY + 1] = 0xff;
		if (first < argc && parse_u32(argv[first], &v))
			mq_put32(req.p + OMCI_P_CLASS, v);
		if (first + 1 < argc && parse_u32(argv[first + 1], &v)) {
			req.p[OMCI_P_ENTITY] = (uint8_t)(v >> 8);
			req.p[OMCI_P_ENTITY + 1] = (uint8_t)v;
		}
		return 0;
	case A_LOG:
		/* off err warn info dbg, 0..4, as OMCI_LogSet_Cmd takes them:
		 * the user level into word 3 and the driver level into word 4. */
		if (first + 1 >= argc) {
			out("% set log needs a user level and a driver level\n");
			return -1;
		}
		if (!keyword(log_levels, argv[first], &v) ||
		    !keyword(log_levels, argv[first + 1], &v)) {
			out("% level must be off, err, warn, info or dbg\n");
			return -1;
		}
		keyword(log_levels, argv[first], &v);
		mq_put32(req.p + 12, v);
		keyword(log_levels, argv[first + 1], &v);
		mq_put32(req.p + 16, v);
		return 0;
	case A_LOGFILE:
		if (first >= argc || !parse_u32(argv[first], &v)) {
			out("% set logfile needs a mode (a mask, 0..8)\n");
			return -1;
		}
		mq_put32(req.p + 4, v);
		if (first + 1 < argc && parse_u32(argv[first + 1], &v))
			mq_put32(req.p + 8, v);
		if (first + 2 < argc)
			put_str(OMCI_P_NAME, argv[first + 2]);
		return 0;
	case A_STR35:
		if (first >= argc) {
			out("% needs a keyword\n");
			return -1;
		}
		put_str(OMCI_P_NAME, argv[first]);
		return 0;
	case A_KEYNUM:
		if (first >= argc || !keyword(dm_modes, argv[first], &v)) {
			out("% mode must be off, on_wq, on_bc_mc or on_wq_bc_mc\n");
			return -1;
		}
		mq_put32(req.p + 4, v);
		return 0;
	case A_LOID:
		if (first >= argc) {
			out("% set loid needs a loid\n");
			return -1;
		}
		put_str(OMCI_P_NAME, argv[first]);
		if (first + 1 < argc)
			put_str(OMCI_P_VALUE, argv[first + 1]);
		return 0;
	case A_SN: {
		/* Nine bytes at +26, which is what OMCI_SnSet_Cmd memcpys into
		 * the ONT-G row: four characters of vendor id then four bytes
		 * written as eight hex digits, the way getSerialNum reports
		 * them. */
		const char *vid, *hex;

		if (first + 1 >= argc) {
			out("% set sn needs a vendor id and eight hex digits\n");
			return -1;
		}
		vid = argv[first];
		hex = argv[first + 1];
		if (str_len(vid) != 4 || str_len(hex) != 8) {
			out("% vendor id is 4 characters and the serial 8 hex digits\n");
			return -1;
		}
		for (int i = 0; i < 4; i++)
			req.p[26 + i] = (uint8_t)vid[i];
		for (int i = 0; i < 4; i++) {
			int hi = hex_nibble(hex[i * 2]), lo = hex_nibble(hex[i * 2 + 1]);

			if (hi < 0 || lo < 0) {
				out("% the serial must be hex\n");
				return -1;
			}
			req.p[30 + i] = (uint8_t)((hi << 4) | lo);
		}
		return 0;
	}
	case A_MIBDEL:
		return class_entity(argc, argv, first, 2);
	case A_MIBCREATE:
		if (class_entity(argc, argv, first, 3) < 0)
			return -1;
		put_str(OMCI_P_VALUE, argv[first + 2]);
		return 0;
	case A_MIBSET:
		if (class_entity(argc, argv, first, 4) < 0)
			return -1;
		put_str(OMCI_P_NAME, argv[first + 2]);
		put_str(OMCI_P_VALUE, argv[first + 3]);
		return 0;
	case A_SIMAVC:
		if (class_entity(argc, argv, first, 3) < 0)
			return -1;
		if (!parse_u32(argv[first + 2], &v) || v < 1 || v > 16) {
			out("% attribute index is 1..16\n");
			return -1;
		}
		req.p[OMCI_P_BYTE0] = (uint8_t)v;
		return 0;
	case A_ATTR:
		if (first + 2 >= argc) {
			out("% getattr needs classId, entityId and attrIndex\n");
			return -1;
		}
		if (!parse_u32(argv[first], &v))
			return -1;
		mq_put32(req.p + OMCI_P_CLASS, v);
		if (!parse_u32(argv[first + 1], &v))
			return -1;
		req.p[OMCI_P_ENTITY] = (uint8_t)(v >> 8);
		req.p[OMCI_P_ENTITY + 1] = (uint8_t)v;
		if (!parse_u32(argv[first + 2], &v))
			return -1;
		mq_put32(req.p + OMCI_P_ARG, v);
		return 0;
	}
	return 0;
}

/* Without a libc there is no sigreturn trampoline, so this must not return. */
static void on_signal(int sig)
{
	(void)sig;
	if (cleanup_q >= 0)
		mq_remove(cleanup_q);
	sys_exit(1);
}

int main(int argc, char **argv)
{
	/* A client killed mid-stream -- `omcli mib | head`, or a pipe to a
	 * program this busybox does not have -- must still reach its own
	 * cleanup, or it leaves a message queue behind for good. With SIGPIPE
	 * ignored the write simply fails and the normal path runs. */
	sys_signal(SIGPIPE, (void (*)(int))1);
	/* Ctrl-C during the twenty-second wait, or a SIGTERM, otherwise leaves
	 * the keyless reply queue behind with no way to reach it. */
	sys_signal(SIGINT, on_signal);
	sys_signal(SIGTERM, on_signal);
	sys_signal(SIGHUP, on_signal);

	const struct cmd *c = 0;
	uint32_t id = 0;
	int first = 3, answer = ANS_FILE, force = 0, vendor = 0;

	/* Flags are position-independent, and everything after this reads the
	 * group and command at argv[1] and argv[2] with arguments from
	 * argv[3]. So a LEADING flag has to be lifted out of argv rather than
	 * merely noticed: `omcli --vendor mib get 84` used to parse
	 * "--vendor" as the group and report "no such command", while the same
	 * flag at the end worked. Compacting argv here means either spelling
	 * behaves the same. */
	for (int i = 1; i < argc;) {
		if (str_eq(argv[i], "-f") || str_eq(argv[i], "--vendor")) {
			if (argv[i][1] == 'f')
				force = 1;
			else
				vendor = 1;
			for (int k = i; k + 1 < argc; k++)
				argv[k] = argv[k + 1];
			argc--;
			continue;
		}
		i++;
	}

	if (argc < 2) {
		usage();
		return 1;
	}

	/* Clean up after a client that was killed before it could. There is no
	 * ipcrm on this busybox and the kernel has 53 queues in all, so a few
	 * corpses matter. */
	if (str_eq(argv[1], "--rmq") && argc > 2) {
		uint32_t key;
		long q;

		if (!parse_u32(argv[2], &key)) {
			out("% --rmq needs a key\n");
			out_flush();
			return 1;
		}
		q = mq_open(key, 0);
		if (q < 0) {
			out_fmt("%% no queue at key %d\n", (long)key);
			out_flush();
			return 1;
		}
		out_fmt("removed queue at key %d: %d\n", (long)key,
			(long)mq_remove(q));
		out_flush();
		return 0;
	}

	/* Inject a raw OMCI frame, the way `debug loadpkt` does.
	 *
	 * This goes to the VENDOR queue with msgType 0, not to omcid's own
	 * queue, because that is where a frame arrives in the vendor's design:
	 * its netlink reader posts every received frame here with this type,
	 * and the message handler's case 0 is the only thing that processes a
	 * received frame at all. An injected frame and one off the line are the
	 * same frame arriving by two roads.
	 *
	 * Which is what makes it useful: the responder can be exercised with no
	 * OLT attached. Feed it a request, read the answer in omcid's log.
	 *
	 * Whoever is listening on that queue gets it -- omcid if it has taken
	 * the queue over, the vendor's omci_app otherwise. There is no reply to
	 * wait for: the answer goes out the line, not back here. */
	if (str_eq(argv[1], "--inject") && argc > 2) {
		struct omci_msg m;
		const char *h = argv[2];
		unsigned n = 0;
		long q;

		for (unsigned i = 0; i < sizeof m; i++)
			((uint8_t *)&m)[i] = 0;
		while (*h && n < OMCI_MQ_PAYLOAD) {
			int hi, lo;

			while (*h == ' ' || *h == ':' || *h == '-')
				h++;
			if (!*h)
				break;
			hi = hexval(*h++);
			lo = *h ? hexval(*h++) : -1;
			if (hi < 0 || lo < 0) {
				out("% --inject takes hex bytes\n");
				out_flush();
				return 1;
			}
			m.p[n++] = (uint8_t)((hi << 4) | lo);
		}
		if (n != OMCI_FRAME_LEN) {
			out_fmt("%% a baseline OMCI frame is %d bytes; got %d\n",
				(long)OMCI_FRAME_LEN, (long)n);
			out_flush();
			return 1;
		}
		q = mq_open(OMCI_MQ_KEY, 0);
		if (q < 0) {
			out_fmt("%% no queue at key %d: nothing is listening\n",
				(long)OMCI_MQ_KEY);
			out_flush();
			return 1;
		}
		m.mtype   = OMCI_MQ_MTYPE;
		m.msgType = OMCI_MQ_TYPE_FRAME;
		m.len     = OMCI_FRAME_LEN;
		if (mq_send(q, &m) < 0) {
			out("% could not send the frame\n");
			out_flush();
			return 1;
		}
		out_fmt("injected %d bytes; the answer goes to the line, not "
			"here\n", (long)n);
		out_flush();
		return 0;
	}

	/* omcid first, if it is the one listening. Its queue existing is the
	 * whole handshake -- but a command in the vendor table below (`get sn`,
	 * `mib get 84`) goes down the vendor queue even then, because omcid
	 * serves that queue too and answers in the vendor shapes the UI and
	 * the exporter read. `get sn` used to land on the native verb table and
	 * report "no such command: get" (every boot until 2026-09-21). */
	if (!vendor) {
		long q = mq_open(OMCLI_KEY, 0);
		int in_table = 0;

		if (argc >= 3)
			for (int i = 0; i < NCMDS; i++)
				if (str_eq(cmds[i].group, argv[1]) &&
				    str_eq(cmds[i].name, argv[2]))
					in_table = 1;
		if (q >= 0 && !in_table)
			return native_run(argc, argv, 1, q);
	}

	if (str_eq(argv[1], "--id")) {
		int known = 0;

		if (argc < 3 || !parse_u32(argv[2], &id)) {
			out("% --id needs a number\n");
			out_flush();
			return 1;
		}
		for (int i = 0; i < NCMDS; i++)
			if (cmds[i].id == id) {
				known = 1;
				answer = cmds[i].answer;
			}
		/* Same rule as omciprobe: an id this does not know is assumed
		 * to write, because most of the 45 do. */
		if (!known && !force) {
			out_fmt("%% id %d is not one of the read commands; "
				"add -f to send it anyway\n", (long)id);
			out_flush();
			return 1;
		}
		first = 3;
	} else {
		if (argc < 3) {
			usage();
			return 1;
		}
		for (int i = 0; i < NCMDS; i++)
			if (str_eq(cmds[i].group, argv[1]) && str_eq(cmds[i].name, argv[2])) {
				c = &cmds[i];
				break;
			}
		if (!c) {
			out_fmt("%% no such command: %s %s\n", argv[1], argv[2]);
			out_flush();
			return 1;
		}
		id = c->id;
		answer = c->answer;
		if ((c->flags & F_WRITE) && !force) {
			out_fmt("%% `%s %s` changes the daemon's state; add -f\n",
				c->group, c->name);
			out_flush();
			return 1;
		}
	}

	/* The one command that must never be sent TO THE VENDOR. MIB_ShowAll
	 * locks the table mutex twice; omci_app has one message-loop thread and
	 * it never comes back, so every later command of any kind goes
	 * unanswered -- OMCI frames from the line included -- until the daemon
	 * is restarted.
	 *
	 * omcid implements the same command correctly and cannot deadlock: it
	 * has no mutex. But the client cannot tell which daemon owns queue
	 * 0x800, so the refusal stays and -f lifts it. Sending this to a stick
	 * running the stock userland is still a way to take it off the air. */
	if (id == 6) {
		/* Whether this is safe turns on WHICH daemon owns 0x800, and
		 * the client can tell: omcid's own queue existing is the
		 * handshake, and omcid takes the vendor queue only when
		 * omci_app is not there. --vendor does not settle it -- it
		 * selects the protocol, not the daemon -- so probe.
		 *
		 * Reaching here at all means either --vendor, or the native
		 * queue was already found absent; in that second case the
		 * daemon on 0x800 is omci_app and -f would deadlock the ONU,
		 * which is what this guard exists to prevent. */
		long probe = mq_open(OMCLI_KEY, 0);

		if (probe < 0 && !force) {
			out("% command 6 (get tables) deadlocks omci_app and\n"
			    "% needs a restart to recover, and omcid is not\n"
			    "% listening -- so omci_app is what would get it.\n"
			    "% omcid answers this command correctly; start it,\n"
			    "% or add -f if you are certain.\n");
			out_flush();
			return 1;
		}
	}

	cmdq = mq_open(OMCI_MQ_KEY, 0);
	if (cmdq < 0) {
		out_fmt("%% no command queue at key 0x%x -- is omci_app running? (%d)\n",
			(long)OMCI_MQ_KEY, (long)cmdq);
		out_flush();
		return 1;
	}
	replykey = 0x6900 + (uint32_t)(sys_getpid() & 0xff);
	replyq = mq_open_fresh(replykey);
	if (replyq < 0) {
		out_fmt("%% cannot make a reply queue: %d\n", (long)replyq);
		out_flush();
		return 1;
	}

	msg_init(&req, id, answer == ANS_QUEUE ? replykey : 0);
	if (c && fill_args(c, argc, argv, first) < 0) {
		mq_remove(replyq);
		out_flush();
		return 1;
	}
	if (mq_send(cmdq, &req) < 0) {
		out("% send failed\n");
		mq_remove(replyq);
		out_flush();
		return 1;
	}

	if (answer == ANS_QUEUE) {
		if (wait_reply() == 0)
			print_reply(id);
		else
			out("% no answer\n");
	} else if (answer == ANS_FILE) {
		if (barrier() == 0)
			print_dump();
	}

	mq_remove(replyq);
	out_flush();
	return 0;
}
