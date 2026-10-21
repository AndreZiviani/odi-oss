/* igmpd: IGMP snooping for the one UNI.
 *
 * What it does, when the OLT asks for snooping (omcid writes the profile to
 * /var/run/odi-mcast.conf, policy.h): take the switch's snooping lease
 * (a copy of the host port's IGMP to the CPU, switch_opt.h), keep the group
 * state those reports imply, and program one static L2 multicast row a
 * group, the host and the PON as members. Once every group the host already
 * watches has had a chance to report again (one membership interval), it
 * also turns on the PON's drop of IPv4 multicast no row names, so a group
 * nobody on the UNI joined stays off the UNI link.
 *
 * What it never does: receive a frame from the fibre. The copy is made only
 * for frames entering the UNI port, the OLT's queries are not copied and not
 * needed (the host answers them by itself, the PON is a member of every
 * row, and the answers are the reports this daemon sees), and nothing here
 * transmits.
 *
 * The lease is the descriptor of /dev/odi_sw this process holds: if the
 * daemon dies, the switch goes back to flooding and the rows it wrote are
 * deleted, which is what an image without snooping does.
 *
 * The programming is OFF unless -w is given: with it off the daemon prints
 * every row it would write and holds no lease, so a policy can be watched
 * before it is trusted. -j and -l drive the row path by hand.
 */
#include "sys.h"
#include "io.h"
#include "igmpd.h"
#include "group.h"
#include "program.h"
#include "entry.h"
#include "policy.h"
#include "state.h"
#include "../omci/nl.h"
#include "../omci/redirect_guard.h"

/* The receive timeout is also the tick: nothing else moves time forward, and
 * every expiry in group.c is in whole seconds. */
#define RCV_TIMEOUT_US  1000000u
/* Changes one frame, one tick or one policy can make. A v3 report of the
 * largest size the redirect carries holds about 190 records and each can
 * change a group; a tick can lapse every group and move the router port; a
 * policy can set a fixed group and remove a joined one. */
#define MAX_CHANGES     (2 * IGMP_MAX_GROUPS + 4)

static void out_mac(const uint8_t *m)
{
	for (int i = 0; i < 6; i++) {
		if (i)
			out_char(':');
		out_hex(m[i], 2);
	}
}

static void out_ip(uint32_t a)
{
	out_udec((a >> 24) & 0xff);
	out_char('.');
	out_udec((a >> 16) & 0xff);
	out_char('.');
	out_udec((a >> 8) & 0xff);
	out_char('.');
	out_udec(a & 0xff);
}

static void describe(const struct igmp_rx *rx, enum igmp_parse_rc rc)
{
	out_fmt("port %d  ", rx->port);
	if (rx->tagged)
		out_fmt("vlan %d  ", rx->vid);
	else
		out("untagged  ");

	out_mac(rx->src);
	out(" -> ");
	out_mac(rx->dst);

	if (rc != IGMP_OK) {
		out_fmt("  DROP: %s\n", igmp_parse_str(rc));
		return;
	}

	out_fmt("  %s", igmp_type_str(rx->type));
	if (rx->type == IGMP_V3_REPORT) {
		out_fmt(" (%d records)", rx->nrec);
	} else {
		out("  group ");
		out_ip(rx->group);
	}
	out("  src ");
	out_ip(rx->sip);
	/* Both belong in the line rather than in a check: a report with ttl 255
	 * or without the router alert is still a report, and a snooper that
	 * refuses it stops forwarding traffic somebody asked for. Worth seeing,
	 * not worth dropping. */
	if (rx->ttl != 1)
		out_fmt("  ttl %d", rx->ttl);
	if (!rx->router_alert)
		out("  no-alert");
	out_char('\n');
}

/* The PON is a member of every row: the OLT's side is where the querier and
 * the multicast source are, so a report the host sends to a group has to
 * reach it, and without the PON in the row it goes nowhere but back.
 * Hardware port 2. */
#define PON_PORTBIT     0x4u

/* The switch holds the lease only while it is taken: rows written without
 * it would not be taken back when this process dies. */
static uint32_t lease;

/* Set when a row write or delete failed (the table engine busy, a full
 * bucket): the table and the switch no longer agree, and with the PON's drop
 * on a group whose row never went in is a stream that never arrives. The
 * rows are written again from the table, a few seconds on. */
static int rows_dirty;
static uint32_t resync_at;

static int row_set(const struct igmp_db *db, uint16_t vid, uint32_t group)
{
	/* One hardware row per MAC, not per group: groups that alias, or the
	 * same group on two VLANs, share it. The row carries the union of
	 * their members, and goes only when nothing is left on it. The db is
	 * already in its final state for this batch, so the union is the same
	 * whichever change of the batch is applied first. */
	uint32_t u = igmp_db_mac_ports(db, group);

	return u ? igmp_hw_group_set(vid, group, u | PON_PORTBIT)
		 : igmp_hw_group_del(vid, group);
}

/* Every live group, written again: after the lease is taken, since rows
 * written before it are not the lease's to delete, and after a failed
 * write. */
static void resync(const struct igmp_db *db)
{
	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use && row_set(db, db->g[i].vid, db->g[i].group))
			rows_dirty = 1;
}

/* Apply what changed. The count can exceed `cap` -- igmp_db_rx returns how
 * many there were, not how many fitted -- and anything past the array was
 * never written, so it must not be read: the table is written out whole
 * instead (a group that lapsed in the lost part keeps a stale row until its
 * MAC is written again, which only over-forwards). */
static void apply(const struct igmp_db *db, const struct igmp_change *ch,
		  int n, int cap)
{
	if (n > cap) {
		out_fmt("  %d changes, only %d recorded: writing every group again\n", n, cap);
		if (lease || !igmp_hw_enabled())
			resync(db);
		return;
	}
	for (int i = 0; i < n; i++) {
		switch (ch[i].kind) {
		case IGMP_CHANGE_ROUTER:
			/* Recorded, not programmed: the PON is already a member
			 * of every row, which is where the router is. A query
			 * that came in on the UNI port (a router behind the
			 * stick) would show up here, and its port is a member
			 * of the groups it joined like any other. */
			out_fmt("  router ports -> 0x%x\n", ch[i].ports);
			break;
		default:
			if ((lease || !igmp_hw_enabled()) &&
			    row_set(db, ch[i].vid, ch[i].group))
				rows_dirty = 1;
			break;
		}
	}
}

/* Monotonic seconds. A failure here is not fatal: time stops advancing, which
 * freezes every expiry, and a snooper that forgets nothing over-forwards. That
 * is the safe direction, and the message says it happened. */
static uint32_t now_seconds(int *warned)
{
	long sec = 0, nsec = 0;

	if (sys_clock_gettime(CLOCK_MONOTONIC, &sec, &nsec) != 0) {
		if (!*warned) {
			out("clock_gettime failed: nothing will expire\n");
			*warned = 1;
		}
		return 0;
	}
	return (uint32_t)sec;
}

static void usage(void)
{
	out("usage: igmpd [-w] [-f] [-n COUNT]\n"
	    "       igmpd [-w] -j GROUP -p PORTS [-v VID]\n"
	    "       igmpd [-w] -l GROUP [-v VID]\n"
	    "  Registers for packet-redirect uid 4, describes every IGMP control\n"
	    "  frame the switch traps to the CPU, and keeps the group state.\n"
	    "  -w         WRITE the entries to the switch. Off by default: this\n"
	    "             has never run on a device, and a wrong multicast entry\n"
	    "             misforwards rather than failing.\n"
	    "  -n COUNT   stop after COUNT frames (0, the default, is forever)\n"
	    "  -f         start even if a live process holds uid 4\n"
	    "  -j GROUP   one-shot: program GROUP (a.b.c.d) with member PORTS\n"
	    "             (-p, hardware port bits: 0x1 the UNI) and exit; the\n"
	    "             same switch path the daemon uses, no uid 4, no state\n"
	    "  -l GROUP   one-shot: remove GROUP from the switch and exit\n"
	    "  -v VID     the VID the one-shot names (default 0); printed only:\n"
	    "             the entry is keyed on filtering id 0 (SVL)\n"
	    "  -h         this text; registers nothing\n");
}

/* The one-shot join or leave: the daemon own switch path, driven by hand, so
 * a trial can check an entry lands in the table (diag l2-table get all)
 * without an IGMP frame ever reaching the CPU. */
static int one_shot(int join, uint32_t group, uint32_t ports, uint32_t vid)
{
	int rc;

	if ((group >> 28) != 0xe) {
		out("not an IPv4 multicast group (224.0.0.0/4)\n");
		return 2;
	}
	igmp_hw_probe();
	rc = join ? igmp_hw_group_set((uint16_t)vid, group, ports)
		  : igmp_hw_group_del((uint16_t)vid, group);
	out_flush();
	return rc ? 1 : 0;
}

static int parse_uint(const char *s, uint32_t *out_v)
{
	uint32_t v = 0;
	int n = 0;

	for (; *s; s++, n++) {
		if (*s < '0' || *s > '9')
			return 0;
		v = v * 10 + (uint32_t)(*s - '0');
	}
	*out_v = v;
	return n > 0;
}

/* ---- the policy and the lease ------------------------------------- */

static struct igmp_policy pol;
static char pol_text[IGMP_POLICY_MAX_BYTES];
static uint32_t pol_len;
/* The PON's drop waits until this second: every group the host already
 * watches has to be able to report once before an unregistered one is a
 * group nobody wants. */
static uint32_t learn_until;
static uint32_t lease_retry;

/* Reads the policy file into buf. Returns its length, 0 when there is none
 * (omcid has not written it yet, or the MIB was reset and it says no
 * snooping anyway). */
static uint32_t read_policy(char *buf, uint32_t max)
{
	long fd = sys_open(IGMP_POLICY_PATH, O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = sys_read((int)fd, buf, max);
	sys_close((int)fd);
	return n > 0 ? (uint32_t)n : 0;
}

static int same_text(const char *a, uint32_t an, const char *b, uint32_t bn)
{
	if (an != bn)
		return 0;
	for (uint32_t i = 0; i < an; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* What to hold at this second. */
static uint32_t wanted_lease(const struct igmp_db *db, uint32_t now)
{
	return igmp_policy_lease(&pol, db->static_overflow, now, learn_until);
}

/* Bring the lease to `want`. A failure is retried every ten seconds, and
 * says so once per change of mind. */
static void settle_lease(struct igmp_db *db, uint32_t want, uint32_t now)
{
	int rc;

	if (want == lease || (int32_t)(now - lease_retry) < 0)
		return;
	rc = igmp_hw_snoop(want);
	if (rc) {
		lease_retry = now + 10;
		return;
	}
	if (!lease && want)
		resync(db);            /* rows kept while no lease was held */
	lease = want;
}

/* The join count the state file last carried: reports that change nothing
 * still count, and the file follows them once a second at most. */
static uint32_t state_joins;

static void write_state(const struct igmp_db *db)
{
	static char buf[IGMP_STATE_MAX];
	uint32_t n = igmp_state_format(db, buf, sizeof buf);
	long fd;

	state_joins = db->joins;
	if (!n || !igmp_hw_enabled())
		return;
	fd = sys_create(IGMP_STATE_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	if (sys_write((int)fd, buf, n) != (long)n) {
		sys_close((int)fd);
		sys_unlink(IGMP_STATE_TMP_PATH);
		return;
	}
	sys_close((int)fd);
	if (sys_rename(IGMP_STATE_TMP_PATH, IGMP_STATE_PATH) < 0)
		sys_unlink(IGMP_STATE_TMP_PATH);
}

/* Once a second: has the OLT changed what it asks for, and does the lease
 * match it. */
static void policy_tick(struct igmp_db *db, uint32_t now, int first)
{
	static char tmp[IGMP_POLICY_MAX_BYTES];
	static struct igmp_change ch[MAX_CHANGES];
	uint32_t n = read_policy(tmp, sizeof tmp);

	if (first || !same_text(tmp, n, pol_text, pol_len)) {
		struct igmp_policy np;
		int was = pol.snooping, changes;

		for (uint32_t i = 0; i < n; i++)
			pol_text[i] = tmp[i];
		pol_len = n;
		igmp_policy_parse(tmp, n, &np);
		pol = np;
		out_fmt("policy: %s, function %d, %s leave, %d/%d/%d s, limit %d, "
			"%d allow + %d static ranges%s\n",
			pol.snooping ? "snooping" : "not snooping",
			(long)pol.function, pol.immediate_leave ? "immediate" : "timed",
			(long)pol.robustness, (long)pol.query_interval,
			(long)pol.last_member_query, (long)pol.max_groups,
			(long)pol.nallow, (long)pol.nstatic,
			pol.overflow ? " (too many: not enforced)" : "");
		if (!pol.snooping) {
			/* Back to flooding. Releasing the lease deletes the rows,
			 * so the table starts again from nothing. */
			settle_lease(db, 0, now);
			igmp_db_init(db);
			igmp_db_set_policy(db, &pol, ch, MAX_CHANGES);
			write_state(db);
			return;
		}
		if (!was)
			learn_until = now + igmp_policy_membership_ttl(&pol);
		/* The lease first: the rows the policy gives rise to (static
		 * groups) are written under it. */
		settle_lease(db, wanted_lease(db, now), now);
		changes = igmp_db_set_policy(db, &pol, ch, MAX_CHANGES);
		apply(db, ch, changes, MAX_CHANGES);
		write_state(db);
		return;
	}
	if (pol.snooping)
		settle_lease(db, wanted_lease(db, now), now);
}

int main(int argc, char **argv)
{
	static uint8_t buf[NLMSG_HDR + IGMP_MAX_PAYLOAD];
	static struct igmp_db db;
	static struct igmp_change ch[MAX_CHANGES];
	struct igmp_rx rx;
	uint32_t last_tick = 0;
	int clock_warned = 0;
	/* nl_open fills tid only on success, and gcc cannot see the early
	 * return between the two. */
	uint32_t tid = 0, want = 0, seen = 0;
	int force = 0, shot = 0;
	uint32_t shot_group = 0, shot_ports = 0, shot_vid = 0;
	int have_ports = 0;
	long fd;

	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-h") || str_eq(argv[i], "--help")) {
			usage();
			return 0;
		} else if (str_eq(argv[i], "-f") || str_eq(argv[i], "--force")) {
			force = 1;
		} else if (str_eq(argv[i], "-w")) {
			igmp_hw_set_enabled(1);
		} else if (str_eq(argv[i], "-n") && i + 1 < argc) {
			if (!parse_uint(argv[++i], &want)) {
				usage();
				return 2;
			}
		} else if ((str_eq(argv[i], "-j") || str_eq(argv[i], "-l")) &&
			   i + 1 < argc && !shot) {
			shot = argv[i][1] == 'j' ? 1 : 2;
			if (!igmp_parse_ipv4(argv[++i], &shot_group)) {
				usage();
				return 2;
			}
		} else if (str_eq(argv[i], "-p") && i + 1 < argc) {
			if (!igmp_parse_num(argv[++i], &shot_ports)) {
				usage();
				return 2;
			}
			have_ports = 1;
		} else if (str_eq(argv[i], "-v") && i + 1 < argc) {
			if (!igmp_parse_num(argv[++i], &shot_vid) || shot_vid > 4095) {
				usage();
				return 2;
			}
		} else {
			usage();
			return 2;
		}
	}

	if (shot) {
		if (shot == 1 && !have_ports) {
			usage();
			return 2;
		}
		return one_shot(shot == 1, shot_group, shot_ports, shot_vid);
	}

	fd = nl_open(&tid, RCV_TIMEOUT_US);
	if (fd < 0) {
		out_fmt("netlink open failed: %d\n", (int)fd);
		return 1;
	}
	/* The same one-receiver-per-uid rule omcid lives by: a second igmpd
	 * would take uid 4 from the first and, on exit, leave it with none. */
	if (redirect_guard("igmpd", IGMP_REDIRECT_UID, force, tid)) {
		sys_close((int)fd);
		return 1;
	}
	/* There is one receiver per uid: registering takes the channel from
	 * the vendor igmpd if it is running, and it takes it back when it
	 * registers again. Say so rather than let it look like a coincidence. */
	if (nl_redirect((int)fd, tid, IGMP_REDIRECT_UID, NL_ACT_REG,
			IGMP_MAX_PAYLOAD) < 0) {
		out("redirect register failed\n");
		sys_close((int)fd);
		return 1;
	}
	igmp_db_init(&db);
	igmp_policy_defaults(&pol);
	igmp_hw_probe();
	out_fmt("listening on redirect uid %d, mtu %d, %s\n",
		IGMP_REDIRECT_UID, IGMP_MAX_PAYLOAD,
		igmp_hw_enabled() ? "WRITING to the switch" : "dry run (-w writes)");
	out_flush();

	for (int first = 1;; first = 0) {
		uint32_t len = 0;
		uint32_t now;
		long rc = nl_recv((int)fd, buf, sizeof buf, &len);

		now = now_seconds(&clock_warned);
		/* Expiries first, so a frame that arrives in the same second as
		 * a lapse is applied to the state after the lapse rather than
		 * before it. */
		if (now != last_tick || first) {
			int changes;

			last_tick = now;
			policy_tick(&db, now, first);
			changes = igmp_db_tick(&db, now, ch, MAX_CHANGES);
			apply(&db, ch, changes, MAX_CHANGES);
			if (rows_dirty && lease && (int32_t)(now - resync_at) >= 0) {
				rows_dirty = 0;
				resync_at = now + 5;
				resync(&db);
			}
			if (changes || db.joins != state_joins)
				write_state(&db);
		}
		if (rc <= 0 || len == 0) {    /* the receive timeout, mostly */
			out_flush();
			continue;
		}
		{
			enum igmp_parse_rc prc =
				igmp_parse(buf + NLMSG_HDR, len, &rx);
			int changes = 0;

			/* Only while snooping: a copy that arrives after the
			 * policy said stop is not state to keep. */
			if (prc == IGMP_OK && pol.snooping)
				changes = igmp_db_rx(&db, &rx, now, ch, MAX_CHANGES);
			/* A refresh changes nothing and says nothing: reports
			 * come every few seconds per group, and this goes to a
			 * log on a small tmpfs. A dry run, which exists to be
			 * watched, and anything refused or malformed, is said. */
			if (!igmp_hw_enabled() || changes || prc != IGMP_OK ||
			    !pol.snooping)
				describe(&rx, prc);
			if (changes) {
				apply(&db, ch, changes, MAX_CHANGES);
				write_state(&db);
			}
		}
		out_flush();
		if (want && ++seen >= want)
			break;
	}

	nl_redirect((int)fd, tid, IGMP_REDIRECT_UID, NL_ACT_DEREG,
		    IGMP_MAX_PAYLOAD);
	sys_close((int)fd);
	return 0;
}
