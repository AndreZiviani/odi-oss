/* Metering the joined groups: which client joined each, when, how much of
 * its stream arrives, and whether it stopped arriving.
 *
 * G.988 asks the ONU for three things a snooper alone cannot see: the lost
 * groups of the multicast operations profile (9.3.27 attribute 9, a joined
 * group of the dynamic list with no downstream flow, and alarm 0 while
 * there is one), and in the subscriber monitor (9.3.29) the bandwidth of
 * each active group and of them all, with the client of each and the time
 * since its last join. The client and the time come from the reports this
 * daemon already reads; the flow comes from the switch: one counting rule
 * per group MAC on the PON ingress (odi_switch_mc_count.h in the kernel
 * tree, 16 of them), counting bytes, read once a second.
 *
 * The rule: a group is LOST when its counter has grown since the rule went
 * in, and then has not grown for IGMP_METER_LOST_S seconds; it is flowing
 * again the second the counter grows. A counter that has never grown says
 * nothing: on a stick where a rule on the PON ingress counts nothing at all
 * (not yet ruled out, docs/SWITCH.md), every group would otherwise be lost,
 * and the OLT would be told so. Such a group, and one without a counter (a
 * table of more MACs than rules, a kernel that refused), is "unknown", never
 * lost, and its bandwidth reads zero. A counter that fails a read is put in
 * again, and starts over as unknown.
 *
 * What it shares with omcid is a file, written whole and renamed like the
 * state file (state.h), only when its text changed:
 *
 *   bps=N         bytes a second of every metered group, each MAC once
 *   lost=N        how many groups are lost
 *   group=A.B.C.D vid=V client=A.B.C.D joined=T [bps=N] flow=up|lost|unknown
 *                 one line per joined (VLAN, group) of the dynamic table,
 *                 the static ones excluded: the client of its last join and
 *                 the daemon's monotonic second of it (omcid reads the same
 *                 clock), bps only for a metered group
 *
 * The bandwidth is the bytes the counter grew by over the last
 * IGMP_METER_WINDOW_S seconds, divided by them: the FCS is in it, the GEM
 * framing is not. Groups that share a MAC share a counter, and each reads
 * its whole count.
 *
 * Pure but for the three accessors the caller gives it, so a transcript
 * (replay.c) can drive the counters.
 */
#ifndef ODI_IGMP_METER_H
#define ODI_IGMP_METER_H

#include <stdint.h>
#include "igmpd.h"
#include "group.h"

#define IGMP_METER_PATH       "/var/run/odi-mcast.meter"
#define IGMP_METER_TMP_PATH   "/var/run/odi-mcast.meter.tmp"

#define IGMP_METER_SLOTS      16      /* the kernel's ODI_SW_MCC_SLOTS */
#define IGMP_METER_LOST_S     10
#define IGMP_METER_WINDOW_S   5
/* A kernel that refused a rule (another holder, a probe in) is asked again
 * this many seconds on, not every second. */
#define IGMP_METER_RETRY_S    30

/* Two header lines and one of at most 104 bytes per group:
 * "group=239.255.255.250 vid=4095 client=255.255.255.255 joined=4294967295
 * bps=4294967295 flow=unknown\n". omcid reads it in a buffer this size. */
#define IGMP_METER_MAX        (64 + IGMP_MAX_GROUPS * 104)

/* .flow */
#define IGMP_FLOW_UNKNOWN     0
#define IGMP_FLOW_UP          1
#define IGMP_FLOW_LOST        2

/* The switch side, set by the caller. install and remove return 0 or a
 * negative errno; read gives what the rule of `slot` (counting `group`'s
 * MAC) has counted since it went in, the low 32 bits, and returns 0 or a
 * negative errno. NULL install: no counters at all (a dry run), only the
 * clients and times. */
struct igmp_meter_ops {
	int (*install)(uint32_t slot, uint32_t group);
	int (*remove)(uint32_t slot);
	int (*read)(uint32_t slot, uint32_t group, uint32_t *total);
};

/* One (VLAN, group) of the dynamic table: the client of its last join. */
struct igmp_meter_grp {
	uint8_t  used;
	uint16_t vid;
	uint32_t group;
	uint32_t client;
	uint32_t joined;
};

/* One counting rule, for one group MAC. */
struct igmp_meter_ctr {
	uint8_t  used;
	uint8_t  moved;         /* grown since the rule went in */
	uint8_t  flow;          /* IGMP_FLOW_* */
	uint32_t group;         /* the group whose MAC it counts */
	uint32_t last;          /* the count at the last read */
	uint32_t grew_at;       /* the second it last grew */
	uint32_t win_at, win_bytes;
	uint32_t bps;
};

struct igmp_meter {
	struct igmp_meter_grp g[IGMP_MAX_GROUPS];
	struct igmp_meter_ctr c[IGMP_METER_SLOTS];
	uint32_t retry_at;      /* no install before this second */
	int      refused;       /* the last install was refused */
};

void igmp_meter_init(struct igmp_meter *m);

/* A frame igmp_db_rx() has just applied: every group it joined, and the
 * table now holds, gets this client and second. Leaves change nothing. */
void igmp_meter_rx(struct igmp_meter *m, const struct igmp_db *db,
		   const struct igmp_rx *rx, uint32_t now);

/* Once a second: follow the table (a counter for each dynamic group MAC
 * while `on`, none otherwise), read the counters, and decide what is lost.
 * Prints a line for each group that goes lost or flows again. */
void igmp_meter_tick(struct igmp_meter *m, const struct igmp_db *db,
		     const struct igmp_meter_ops *ops, int on, uint32_t now);

/* Take every counter out (the lease given back, the daemon leaving). */
void igmp_meter_release(struct igmp_meter *m, const struct igmp_meter_ops *ops);

/* The flow of one (VLAN, group): IGMP_FLOW_*. */
int igmp_meter_flow(const struct igmp_meter *m, uint32_t group);

/* The file above for the table as it is. Returns the length, or 0 when max
 * is too small. */
uint32_t igmp_meter_format(const struct igmp_meter *m, const struct igmp_db *db,
			   char *buf, uint32_t max);

#endif
