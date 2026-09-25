/* igmpd, milestone 1: receive and describe.
 *
 * Register for packet-redirect uid 4, take every trapped frame, keep the group
 * and router-port state it implies, and program the switch to match.
 *
 * The programming is OFF unless -w is given. Nothing here has ever run on a
 * device, and a wrong L2 multicast entry does not fail visibly -- it forwards
 * traffic to the wrong port or stops forwarding it at all -- so the default is
 * to print every entry it would write. Run it that way first, against a live
 * OLT, and the output answers the question the vendor daemon cannot be asked:
 * who is joining what, and when does it lapse.
 */
#include "sys.h"
#include "io.h"
#include "igmpd.h"
#include "group.h"
#include "program.h"
#include "../omci/nl.h"
#include "../omci/redirect_guard.h"

/* The receive timeout is also the tick: nothing else moves time forward, and
 * every expiry in group.c is in whole seconds. */
#define RCV_TIMEOUT_US  1000000u
#define MAX_CHANGES     8

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

/* Apply what changed. The count can exceed MAX_CHANGES -- igmp_db_rx returns
 * how many there were, not how many fitted -- and anything past the array was
 * never written, so it must not be read. Saying so beats applying garbage. */
static void apply(const struct igmp_change *ch, int n)
{
	if (n > MAX_CHANGES) {
		out_fmt("  %d changes, only %d recorded\n", n, MAX_CHANGES);
		n = MAX_CHANGES;
	}
	for (int i = 0; i < n; i++) {
		switch (ch[i].kind) {
		case IGMP_CHANGE_ROUTER:
			/* Recorded, not programmed. Multicast has to reach the
			 * router ports too, and how that is expressed here --
			 * a member of every group, or a lookup-miss mask -- is
			 * not something the vendor callers showed. Printing it
			 * is honest; guessing it would forward traffic to a
			 * port on a guess. */
			out_fmt("  router ports -> 0x%x\n", ch[i].ports);
			break;
		case IGMP_CHANGE_DEL:
			igmp_hw_group_del(ch[i].vid, ch[i].group);
			break;
		default:
			igmp_hw_group_set(ch[i].vid, ch[i].group, ch[i].ports);
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
	    "  Registers for packet-redirect uid 4, describes every IGMP control\n"
	    "  frame the switch traps to the CPU, and keeps the group state.\n"
	    "  -w         WRITE the entries to the switch. Off by default: this\n"
	    "             has never run on a device, and a wrong multicast entry\n"
	    "             misforwards rather than failing.\n"
	    "  -n COUNT   stop after COUNT frames (0, the default, is forever)\n"
	    "  -f         start even if a live process holds uid 4\n"
	    "  -h         this text; registers nothing\n");
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

int main(int argc, char **argv)
{
	static uint8_t buf[NLMSG_HDR + IGMP_MAX_PAYLOAD];
	static struct igmp_db db;
	struct igmp_change ch[MAX_CHANGES];
	struct igmp_rx rx;
	uint32_t last_tick = 0;
	int clock_warned = 0;
	/* nl_open fills tid only on success, and gcc cannot see the early
	 * return between the two. */
	uint32_t tid = 0, want = 0, seen = 0;
	int force = 0;
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
		} else {
			usage();
			return 2;
		}
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
	igmp_hw_probe();
	out_fmt("listening on redirect uid %d, mtu %d, %s\n",
		IGMP_REDIRECT_UID, IGMP_MAX_PAYLOAD,
		igmp_hw_enabled() ? "WRITING to the switch" : "dry run (-w writes)");
	out_flush();

	for (;;) {
		uint32_t len = 0;
		uint32_t now;
		long rc = nl_recv((int)fd, buf, sizeof buf, &len);

		now = now_seconds(&clock_warned);
		/* Expiries first, so a frame that arrives in the same second as
		 * a lapse is applied to the state after the lapse rather than
		 * before it. */
		if (now != last_tick) {
			last_tick = now;
			apply(ch, igmp_db_tick(&db, now, ch, MAX_CHANGES));
		}
		if (rc <= 0 || len == 0) {    /* the receive timeout, mostly */
			out_flush();
			continue;
		}
		{
			enum igmp_parse_rc prc =
				igmp_parse(buf + NLMSG_HDR, len, &rx);

			describe(&rx, prc);
			if (prc == IGMP_OK)
				apply(ch, igmp_db_rx(&db, &rx, now, ch,
						     MAX_CHANGES));
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
