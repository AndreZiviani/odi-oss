/* omcid's event lines: what the OLT did to this ONU's management channel, and
 * what omcid itself did, one line per event.
 *
 * The OMCI log (omcid.log) has every frame; that is too much to forward and
 * too little to read after an outage. These are the lines that answer "who
 * started it": a MIB reset and who sent it, a MIB upload, the first and last
 * config write of a provisioning burst and what it added up to, a reboot or a
 * software download the OLT asked for, and omcid starting -- resumed from its
 * snapshot or back to an empty MIB. docs/TOOLS.md, "Link and provisioning
 * events", has the table, next to the kernel's own event=onu_state lines.
 *
 * Every line is "event=<name>" and then key=value pairs, written twice: to
 * stdout, so omcid.log has it in order with the frames around it, and to
 * syslog as "omcid[pid]: event=...", facility daemon.
 *
 * syslog(3) is what this would call, and omcid has no libc. What syslog(3)
 * does is send one datagram to /dev/log, and that is all this does: the same
 * socket, the same "<PRI>tag[pid]: message" framing busybox syslogd parses,
 * which stamps the time and, with SYSLOG_SERVER, forwards it. Nothing of ours
 * sits between this and the collector. MSG_DONTWAIT, because a wedged syslogd
 * with a full socket queue must cost a dropped line, never a stalled OMCI
 * loop; a missing /dev/log (qemu, a shell with syslogd stopped) costs the
 * syslog copy and nothing else.
 *
 * Rate limited, since a misbehaving OLT can repeat any of these: at most
 * EV_BURST lines per EV_WINDOW_S, and the first line of the next window says
 * how many were dropped (event=suppressed), as the kernel's ratelimit does.
 */
#include "omcid.h"

#define EV_LINE_MAX      240
#define EV_WINDOW_S      60
#define EV_BURST         20
/* A provisioning burst ends after this long with no Create, Set or Delete.
 * Gets and Tests do not count: ISP2's OLT interleaves many of both. ISP1
 * sends its 83 creates and 102 sets back to back, well inside this. */
#define EV_PROV_QUIET_S  10

#define EV_AF_UNIX       1
#define EV_SOCK_DGRAM    1       /* MIPS: DGRAM is 1 and STREAM 2, the
				  * other way round from x86 */
#define EV_MSG_DONTWAIT  0x40
#define EV_LOG_PATH      "/dev/log"
#define EV_FAC_DAEMON    (3 << 3)
#define EV_SEV_NOTICE    5
#define EV_SEV_INFO      6

struct evline {
	char b[EV_LINE_MAX];
	int n;
};

struct ev_sockaddr_un {
	uint16_t family;
	char path[108];
};

static long ev_fd = -2;          /* -2: not tried yet; -1: no socket */
static long ev_window_start = -1;
static int ev_in_window;
static unsigned long ev_dropped;

/* A monotonic time as seconds and milliseconds, kept apart: long is 32 bits
 * here, so seconds times 1000 would wrap after 24 days up, and a 64-bit
 * divide is a libgcc call this -nostdlib link does not have. */
struct ev_time {
	long s, ms;
};

static struct ev_time ev_now(void)
{
	struct ev_time t = { 0, 0 };
	long ns = 0;

	if (sys_clock_gettime(CLOCK_MONOTONIC, &t.s, &ns) == 0)
		t.ms = ns / 1000000;
	return t;
}

/* b - a in milliseconds, for the short spans these lines report; capped at
 * a day, far past any upload or provisioning burst. */
static long ev_ms_since(struct ev_time a, struct ev_time b)
{
	long ds = b.s - a.s;

	if (ds > 86400)
		return 86400L * 1000;
	return ds * 1000 + (b.ms - a.ms);
}

static void ev_put(struct evline *e, const char *s)
{
	while (*s && e->n < EV_LINE_MAX - 1)
		e->b[e->n++] = *s++;
}

static void ev_putu(struct evline *e, unsigned long v)
{
	char t[12];
	int i = 0;

	do {
		t[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < (int)sizeof t);
	while (i && e->n < EV_LINE_MAX - 1)
		e->b[e->n++] = t[--i];
}

static void ev_begin(struct evline *e, const char *name)
{
	e->n = 0;
	ev_put(e, "event=");
	ev_put(e, name);
}

static void ev_str(struct evline *e, const char *key, const char *val)
{
	ev_put(e, " ");
	ev_put(e, key);
	ev_put(e, "=");
	ev_put(e, val);
}

static void ev_num(struct evline *e, const char *key, unsigned long v)
{
	ev_put(e, " ");
	ev_put(e, key);
	ev_put(e, "=");
	ev_putu(e, v);
}

/* Milliseconds as seconds with three decimals, the kernel lines' shape. */
static void ev_secs(struct evline *e, const char *key, long ms)
{
	static const char d[] = "0123456789";

	if (ms < 0)
		ms = 0;
	ev_num(e, key, (unsigned long)(ms / 1000));
	ev_put(e, ".");
	ms %= 1000;
	if (e->n < EV_LINE_MAX - 4) {
		e->b[e->n++] = d[ms / 100];
		e->b[e->n++] = d[ms / 10 % 10];
		e->b[e->n++] = d[ms % 10];
	}
}

static void ev_state(struct evline *e, uint32_t st)
{
	char s[3] = { 'O', (char)('0' + (st <= 9 ? st : 0)), 0 };

	ev_str(e, "onu_state", st >= 1 && st <= 7 ? s : "unknown");
}

static void ev_syslog(const struct evline *e, int sev)
{
	static struct ev_sockaddr_un to;
	struct evline f;
	struct iovec iov;
	struct msghdr msg;

	if (ev_fd == -2) {
		ev_fd = sys_socket(EV_AF_UNIX, EV_SOCK_DGRAM, 0);
		if (ev_fd < 0)
			ev_fd = -1;
		to.family = EV_AF_UNIX;
		str_copy(to.path, EV_LOG_PATH, (int)sizeof to.path);
	}
	if (ev_fd < 0)
		return;
	f.n = 0;
	ev_put(&f, "<");
	ev_putu(&f, (unsigned long)(EV_FAC_DAEMON | sev));
	ev_put(&f, ">omcid[");
	ev_putu(&f, (unsigned long)sys_getpid());
	ev_put(&f, "]: ");
	for (int i = 0; i < e->n && f.n < EV_LINE_MAX - 1; i++)
		f.b[f.n++] = e->b[i];
	iov.base = f.b;
	iov.len = (uint32_t)f.n;
	msg.name = &to;
	msg.namelen = (uint32_t)(2 + str_len(EV_LOG_PATH) + 1);
	msg.iov = &iov;
	msg.iovlen = 1;
	msg.control = 0;
	msg.controllen = 0;
	msg.flags = 0;
	/* Unconnected: every send names /dev/log, so a syslogd that restarted
	 * (apply.sh syslog) is found again on the next line without reopening
	 * anything. A failure is a lost copy of a line omcid.log still has. */
	(void)sys_sendmsg((int)ev_fd, &msg, EV_MSG_DONTWAIT);
}

static void ev_write(const struct evline *e, int sev)
{
	/* Anything half-built in the output buffer goes first, so the line
	 * lands between whole lines of omcid.log. Straight to fd 1 rather
	 * than through out(): an event can fire inside a CLI command (a MIB
	 * reset from omcicli), whose output belongs to the client. */
	out_flush();
	sys_write(1, e->b, (unsigned long)e->n);
	sys_write(1, "\n", 1);
	ev_syslog(e, sev);
}

static void ev_emit(struct evline *e, int sev)
{
	long now = ev_now().s;

	if (ev_window_start < 0 || now - ev_window_start >= EV_WINDOW_S) {
		ev_window_start = now;
		ev_in_window = 0;
		if (ev_dropped) {
			struct evline s;

			ev_begin(&s, "suppressed");
			ev_num(&s, "count", ev_dropped);
			ev_num(&s, "window_s", EV_WINDOW_S);
			ev_write(&s, EV_SEV_NOTICE);
			ev_in_window++;
			ev_dropped = 0;
		}
	}
	if (ev_in_window >= EV_BURST) {
		ev_dropped++;
		return;
	}
	ev_in_window++;
	ev_write(e, sev);
}

/* ------------------------------------------------------------ the events */

static unsigned ev_services(void)
{
	unsigned n = 0;

	for (int i = 0; i < SERV_MAX; i++)
		n += servtab[i].in_use ? 1u : 0u;
	return n;
}

void ev_start(int restart, int resumed, const char *why, uint32_t onu_state)
{
	struct evline e;

	ev_begin(&e, "start");
	ev_str(&e, "run", restart ? "restart" : "boot");
	ev_str(&e, "mode", resumed ? "resume" : "reregister");
	if (!resumed)
		ev_str(&e, "reason", why);
	ev_state(&e, onu_state);
	ev_num(&e, "rows", (unsigned long)mib_count());
	ev_emit(&e, restart ? EV_SEV_NOTICE : EV_SEV_INFO);
}

/* The provisioning burst: the first Create/Set/Delete opens it, EV_PROV_QUIET_S
 * without one closes it (ev_tick()). */
static struct {
	int open;
	int after_reset;         /* a MIB reset came first: a full reprovisioning */
	struct ev_time first, last;
	unsigned long creates, sets, deletes;
} prov;

static int reset_pending;        /* a MIB reset, and no burst since */

static void prov_close(void)
{
	struct evline e;

	if (!prov.open)
		return;
	ev_begin(&e, "provision_end");
	ev_num(&e, "creates", prov.creates);
	ev_num(&e, "sets", prov.sets);
	ev_num(&e, "deletes", prov.deletes);
	ev_secs(&e, "duration_s", ev_ms_since(prov.first, prov.last));
	ev_num(&e, "rows", (unsigned long)mib_count());
	ev_num(&e, "services", ev_services());
	ev_num(&e, "after_mib_reset", (unsigned long)prov.after_reset);
	ev_emit(&e, EV_SEV_INFO);
	prov.open = 0;
}

void ev_mib_reset(const char *side)
{
	struct evline e;

	/* A reset in the middle of a burst ends it: what came before is
	 * about to be thrown away, and the next write starts a new one. */
	prov_close();
	ev_begin(&e, "mib_reset");
	ev_str(&e, "side", side);
	ev_num(&e, "rows", (unsigned long)mib_count());
	ev_num(&e, "mib_data_sync", mib_data_sync);
	ev_num(&e, "services", ev_services());
	ev_emit(&e, EV_SEV_NOTICE);
	reset_pending = 1;
}

void ev_config_write(uint8_t mt, uint16_t cls, uint16_t inst)
{
	struct ev_time now = ev_now();

	if (!prov.open) {
		struct evline e;

		prov.open = 1;
		prov.after_reset = reset_pending;
		reset_pending = 0;
		prov.first = now;
		prov.creates = prov.sets = prov.deletes = 0;
		ev_begin(&e, "provision_begin");
		ev_str(&e, "op", mt == OMCI_MT_CREATE ? "create" :
				 mt == OMCI_MT_SET ? "set" : "delete");
		ev_num(&e, "class", cls);
		ev_num(&e, "inst", inst);
		ev_num(&e, "after_mib_reset", (unsigned long)prov.after_reset);
		ev_emit(&e, EV_SEV_INFO);
	}
	prov.last = now;
	if (mt == OMCI_MT_CREATE)
		prov.creates++;
	else if (mt == OMCI_MT_SET)
		prov.sets++;
	else
		prov.deletes++;
}

/* The MIB upload: its count, and the Next that answers its last entity. An
 * upload the OLT abandons half way has a begin and no end. */
static struct {
	int open;
	uint16_t total;
	struct ev_time first;
} upl;

void ev_mib_upload(uint16_t total)
{
	struct evline e;

	upl.open = 1;
	upl.total = total;
	upl.first = ev_now();
	ev_begin(&e, "mib_upload_begin");
	ev_num(&e, "entities", total);
	ev_emit(&e, EV_SEV_INFO);
}

void ev_mib_upload_next(uint16_t seq)
{
	struct evline e;

	if (!upl.open || (unsigned)seq + 1 != upl.total)
		return;
	upl.open = 0;
	ev_begin(&e, "mib_upload_end");
	ev_num(&e, "entities", upl.total);
	ev_secs(&e, "duration_s", ev_ms_since(upl.first, ev_now()));
	ev_emit(&e, EV_SEV_INFO);
}

/* A reboot or a software image download the OLT asked for. omcid answers
 * neither (the OLT gets "not supported"), so each attempt is worth a line:
 * an OLT that follows a refused reboot with a deactivation is an outage the
 * ISP started. Download sections are counted, not logged: an image is
 * thousands of them. */
static unsigned long sw_sections;

void ev_olt_command(uint8_t mt, uint16_t cls, uint16_t inst)
{
	struct evline e;
	const char *op = 0;

	if (mt == OMCI_MT_DOWNLOAD_SECTION) {
		sw_sections++;
		return;
	}
	if (mt == OMCI_MT_REBOOT) {
		ev_begin(&e, "olt_reboot");
		ev_num(&e, "class", cls);
		ev_num(&e, "inst", inst);
		ev_str(&e, "result", "not_supported");
		ev_emit(&e, EV_SEV_NOTICE);
		return;
	}
	if (mt == OMCI_MT_START_SW_DOWNLOAD) {
		op = "download_start";
		sw_sections = 0;
	} else if (mt == OMCI_MT_END_SW_DOWNLOAD) {
		op = "download_end";
	} else if (mt == OMCI_MT_ACTIVATE_SW) {
		op = "activate";
	} else if (mt == OMCI_MT_COMMIT_SW) {
		op = "commit";
	}
	if (!op)
		return;
	ev_begin(&e, "sw_image");
	ev_str(&e, "op", op);
	ev_num(&e, "inst", inst);
	if (mt == OMCI_MT_END_SW_DOWNLOAD)
		ev_num(&e, "sections", sw_sections);
	ev_str(&e, "result", "not_supported");
	ev_emit(&e, EV_SEV_NOTICE);
}

/* The Alloc-IDs the OLT assigned by PLOAM, whenever the kernel list
 * changes (apply_qos.c binds the T-CONTs the OLT does not set to them):
 * during ranging, on a deallocation, and when a re-ranging clears them. */
void ev_alloc_ids(const uint16_t *ids, unsigned n)
{
	struct evline e;

	ev_begin(&e, "alloc_ids");
	ev_num(&e, "count", n);
	ev_put(&e, " ids=");
	for (unsigned i = 0; i < n; i++) {
		if (i)
			ev_put(&e, ",");
		ev_putu(&e, ids[i]);
	}
	if (!n)
		ev_put(&e, "none");
	ev_emit(&e, EV_SEV_INFO);
}

/* ------------------------------------------- what omcid does not model
 *
 * An OLT of another vendor can send a managed entity class omcid has no
 * model for, or a message type it does not handle. Each is worth one line,
 * not one per frame: an OLT that stalls on an answer retries the same
 * request for as long as it waits. So each unknown (class, operation) pair
 * and each unknown message type is logged once per boot, as
 * event=unknown_me or event=unknown_msg, and kept with a count in
 * UNKNOWN_PATH, which diag-bundle.sh collects with the rest of /var/log.
 *
 * "Per boot", not per process: /var/log is tmpfs, so the file itself is the
 * record of this boot, and a respawned omcid reads it back before it logs
 * anything, rather than logging every unknown again. Counts are rewritten
 * at most once a second (ev_tick()); a new entry is written at once. */
#define UNK_MAX      32
#define UNK_BUF      4096

struct unk_entry {
	uint16_t cls;            /* unknown_me: the class; unknown_msg: the
				  * class of the first frame of that type */
	uint8_t mt;
	uint8_t msg;             /* 1: an unknown message type */
	unsigned long count;
	unsigned long first_s;   /* monotonic seconds, i.e. uptime */
};

static struct unk_entry unk[UNK_MAX];
static int unk_n, unk_loaded, unk_dirty;
static unsigned long unk_overflow;
static char unkbuf[UNK_BUF];

int ev_msg_known(uint8_t mt)
{
	return mt == OMCI_MT_CREATE || mt == OMCI_MT_DELETE ||
	       mt == OMCI_MT_SET || mt == OMCI_MT_GET ||
	       mt == OMCI_MT_GET_ALL_ALARMS || mt == OMCI_MT_GET_ALL_ALARMS_NEXT ||
	       mt == OMCI_MT_MIB_UPLOAD || mt == OMCI_MT_MIB_UPLOAD_NEXT ||
	       mt == OMCI_MT_MIB_RESET || mt == OMCI_MT_TEST ||
	       (mt >= OMCI_MT_START_SW_DOWNLOAD && mt <= OMCI_MT_REBOOT) ||
	       mt == OMCI_MT_GET_NEXT;
}

static const char *unk_op(uint8_t mt)
{
	switch (mt) {
	case OMCI_MT_CREATE:   return "create";
	case OMCI_MT_DELETE:   return "delete";
	case OMCI_MT_SET:      return "set";
	case OMCI_MT_GET:      return "get";
	case OMCI_MT_GET_NEXT: return "get_next";
	case OMCI_MT_TEST:     return "test";
	default:               return 0;
	}
}

/* The number after `key` on one line, or -1. */
static long unk_field(const char *l, int n, const char *key)
{
	int k = str_len(key);

	for (int i = 0; i + k <= n; i++) {
		int j = 0;
		long v = 0;

		if (i && l[i - 1] != ' ')
			continue;
		while (j < k && l[i + j] == key[j])
			j++;
		if (j < k)
			continue;
		if (i + k >= n || l[i + k] < '0' || l[i + k] > '9')
			return -1;
		for (j = i + k; j < n && l[j] >= '0' && l[j] <= '9'; j++)
			v = v * 10 + (l[j] - '0');
		return v;
	}
	return -1;
}

static int unk_prefix(const char *l, int n, const char *p)
{
	int k = str_len(p);

	if (n < k)
		return 0;
	for (int i = 0; i < k; i++)
		if (l[i] != p[i])
			return 0;
	return 1;
}

/* The entries an earlier omcid of this boot wrote. A line this does not
 * recognise is dropped at the next rewrite. */
static void unk_load(void)
{
	long fd, r;
	int n = 0;

	unk_loaded = 1;
	fd = sys_open(UNKNOWN_PATH, O_RDONLY);
	if (fd < 0)
		return;
	while (n < UNK_BUF - 1 && (r = sys_read((int)fd, unkbuf + n,
						 UNK_BUF - 1 - n)) > 0)
		n += (int)r;
	sys_close((int)fd);
	for (int i = 0; i < n && unk_n < UNK_MAX; ) {
		int e = i;
		const char *l = unkbuf + i;
		long cls, mt, cnt, first;
		int msg = -1;

		while (e < n && unkbuf[e] != '\n')
			e++;
		if (unk_prefix(l, e - i, "unknown_me "))
			msg = 0;
		else if (unk_prefix(l, e - i, "unknown_msg "))
			msg = 1;
		cls = unk_field(l, e - i, "class=");
		mt = unk_field(l, e - i, msg ? "type=" : "mt=");
		cnt = unk_field(l, e - i, "count=");
		first = unk_field(l, e - i, "first_uptime_s=");
		if (msg >= 0 && cls >= 0 && cls <= 0xffff && mt >= 0 && mt <= 0xff &&
		    cnt >= 0 && first >= 0) {
			struct unk_entry *u = &unk[unk_n++];

			u->cls = (uint16_t)cls;
			u->mt = (uint8_t)mt;
			u->msg = (uint8_t)msg;
			u->count = (unsigned long)cnt;
			u->first_s = (unsigned long)first;
		} else if (unk_prefix(l, e - i, "overflow ") &&
			   (cnt = unk_field(l, e - i, "count=")) >= 0) {
			unk_overflow = (unsigned long)cnt;
		}
		i = e + 1;
	}
}

static void unk_line(struct evline *e, const struct unk_entry *u)
{
	const char *op = unk_op(u->mt);

	e->n = 0;
	if (u->msg) {
		ev_put(e, "unknown_msg");
		ev_num(e, "type", u->mt);
		ev_num(e, "class", u->cls);
	} else {
		ev_put(e, "unknown_me");
		ev_num(e, "class", u->cls);
		if (op)
			ev_str(e, "op", op);
		else
			ev_num(e, "op", u->mt);
		ev_num(e, "mt", u->mt);
	}
}

static void unk_save(void)
{
	static const char head[] =
		"# omcid: what the OLT sent that omcid does not model, this boot.\n"
		"# One line per class and operation (unknown_me) or message type\n"
		"# (unknown_msg); docs/TOOLS.md, \"Link and provisioning events\".\n";
	int n = 0;

	for (int i = 0; head[i] && n < UNK_BUF - 1; i++)
		unkbuf[n++] = head[i];
	for (int i = 0; i < unk_n; i++) {
		struct evline e;

		unk_line(&e, &unk[i]);
		ev_num(&e, "count", unk[i].count);
		ev_num(&e, "first_uptime_s", unk[i].first_s);
		for (int k = 0; k < e.n && n < UNK_BUF - 1; k++)
			unkbuf[n++] = e.b[k];
		if (n < UNK_BUF - 1)
			unkbuf[n++] = '\n';
	}
	if (unk_overflow) {
		struct evline e;

		e.n = 0;
		ev_put(&e, "overflow");
		ev_num(&e, "count", unk_overflow);
		ev_put(&e, " (more distinct unknowns than the table holds)\n");
		for (int k = 0; k < e.n && n < UNK_BUF - 1; k++)
			unkbuf[n++] = e.b[k];
	}
	/* A failed write (a full /var) costs this copy; the next change
	 * tries again. */
	(void)atomic_write(UNKNOWN_PATH, UNKNOWN_TMP_PATH,
			   (const uint8_t *)unkbuf, (uint32_t)n);
	unk_dirty = 0;
}

static void unk_note(uint8_t msg, uint16_t cls, uint8_t mt)
{
	struct evline e;
	struct unk_entry *u;

	if (!unk_loaded)
		unk_load();
	for (int i = 0; i < unk_n; i++) {
		u = &unk[i];
		if (u->msg == msg && u->mt == mt && (msg || u->cls == cls)) {
			u->count++;
			unk_dirty = 1;
			return;
		}
	}
	if (unk_n >= UNK_MAX) {
		unk_overflow++;
		unk_dirty = 1;
		return;
	}
	u = &unk[unk_n++];
	u->msg = msg;
	u->cls = cls;
	u->mt = mt;
	u->count = 1;
	u->first_s = (unsigned long)ev_now().s;
	unk_save();
	/* The summary line is the event line minus the counters; the rate
	 * limit may drop the event, never the file entry. */
	unk_line(&e, u);
	{
		struct evline ev;

		ev_begin(&ev, msg ? "unknown_msg" : "unknown_me");
		for (int k = msg ? 11 : 10; k < e.n && ev.n < EV_LINE_MAX - 1; k++)
			ev.b[ev.n++] = e.b[k];
		ev_emit(&ev, EV_SEV_NOTICE);
	}
}

void ev_unknown_me(uint16_t cls, uint8_t mt)
{
	unk_note(0, cls, mt);
}

void ev_unknown_msg(uint8_t mt, uint16_t cls)
{
	unk_note(1, cls, mt);
}

/* Once a second, from the main loop. */
void ev_tick(void)
{
	if (prov.open && ev_ms_since(prov.last, ev_now()) >= EV_PROV_QUIET_S * 1000L)
		prov_close();
	if (unk_dirty)
		unk_save();
}
