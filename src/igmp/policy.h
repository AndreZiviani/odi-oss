/* What the OLT provisioned for multicast, as omcid writes it (omcid's
 * respond/mcast.c has the file format and the units). Parsing and the
 * questions asked of it are pure: no syscalls, so the tests drive them with
 * text.
 *
 * Unknown keys are ignored and a line that does not parse leaves its
 * default, so a newer omcid writing a key this igmpd does not know cannot
 * stop the snooper.
 */
#ifndef ODI_IGMP_POLICY_H
#define ODI_IGMP_POLICY_H

#include <stdint.h>

#define IGMP_POLICY_PATH      "/var/run/odi-mcast.conf"
#define IGMP_POLICY_MAX_RANGES 32
#define IGMP_POLICY_MAX_GRANTS 32
/* A range line can carry five more fields (about 115 bytes), and the dynamic
 * and static lists hold 32 ranges each, so the file outgrew 4096 bytes. */
#define IGMP_POLICY_MAX_BYTES  8192

/* One row of an access control list (ME 309 attributes 7 and 8), as far as
 * igmpd reads it. The fields past the range are what the row says about the
 * join, and 0 is the wildcard of each: a row that leaves them out behaves as
 * a bare range always did. */
struct igmp_range {
	uint32_t lo, hi;                /* inclusive, host order */
	uint16_t vid;                   /* VLAN (ANI), 1 to 4095; 0: any */
	uint16_t gem;                   /* GEM port ID; 0: unspecified. Carried only */
	uint32_t src;                   /* source address (SSM); 0: any */
	/* Imputed group bandwidth, bytes a second, 0: none. Carried for the
	 * bandwidth limit of ME 310 (attributes 4 and 5); nothing here
	 * enforces it. */
	uint32_t imputed_bw;
	/* Preview (row part 1): length in seconds, 0 for a group fully
	 * authorized; minimum seconds between two previews; how many
	 * previews, 0 unlimited; the reset time of the count (0 and 255: not
	 * reset by the clock). */
	uint16_t pv_len, pv_repeat, pv_count, pv_reset;
};

/* One row of the allowed preview groups table (ME 310 attribute 7), IPv4
 * rows only: a group the subscriber has paid for, fully authorized for
 * `minutes` (0: until the OLT removes it). */
struct igmp_grant {
	uint32_t group;                 /* destination address */
	uint32_t src;                   /* source address; 0: any. Carried only */
	uint16_t ani, uni;              /* VLAN of the stream, of the IGMP message */
	uint16_t minutes;
};

/* The imputed bandwidth of the groups of one dynamic list range (G.988
 * 9.3.27 attribute 7, bytes 18 to 21 of the row), in bytes a second. */
struct igmp_bw_range {
	uint32_t lo, hi;
	uint32_t bytes;
};

struct igmp_policy {
	int      snooping;              /* the OLT asks for it: 0 otherwise */
	uint32_t function;              /* ME 309 attribute 2 as sent */
	uint32_t igmp_version;          /* attribute 1: 1 to 3, 16 and 17 (MLD); 0 unset */
	uint32_t querier;               /* attribute 11, the proxy querier address; 0 unset.
					 * Carried only: nothing is ever sent */
	int      immediate_leave;
	uint32_t robustness;            /* count */
	uint32_t query_interval;        /* seconds */
	uint32_t query_max_response;    /* seconds */
	uint32_t last_member_query;     /* seconds */
	uint32_t max_groups;            /* 0: no limit */
	int      unauthorized_join;     /* 1: the OLT lets them through */
	/* ME 309 attributes 4 to 6: what the host's IGMP goes upstream with.
	 * Tag control 0 leaves it as the switch treats any frame; 1 adds the
	 * TCI as a tag, 2 replaces the whole TCI of the tag there (adds it to
	 * an untagged frame), 3 replaces only the VID (an untagged frame gets
	 * the whole TCI). Rate: messages a second, 0 no limit. */
	uint32_t us_tag_control;
	uint32_t us_tci;
	uint32_t us_rate;
	/* ME 310 attributes 4 and 5: the most imputed bandwidth the subscriber
	 * may join, bytes a second (0: no limit), and whether a join past it
	 * is refused (1) or only counted (0). */
	uint32_t max_bandwidth;
	int      bw_enforce;
	uint32_t nbw;
	struct igmp_bw_range bw[IGMP_POLICY_MAX_RANGES];
	int      overflow;              /* more ranges than fit: not enforced */
	uint32_t nallow, nstatic, ngrant;
	struct igmp_range allow[IGMP_POLICY_MAX_RANGES];
	struct igmp_range stat[IGMP_POLICY_MAX_RANGES];
	struct igmp_grant grant[IGMP_POLICY_MAX_GRANTS];
};

/* No policy: not snooping, the RFC 2236 timers. */
void igmp_policy_defaults(struct igmp_policy *p);

/* Parse the file's text into p, which is reset to the defaults first.
 * Returns how many lines it understood. */
int igmp_policy_parse(const char *text, uint32_t len, struct igmp_policy *p);

/* Whether the access control lists restrict anything: at least one range
 * was provisioned and they all fitted. An empty list is no restriction --
 * the OLT is the authority, and an ONU that blocks a stream because a
 * table was never provisioned breaks service that a transparent one
 * would have carried. */
int igmp_policy_acl_active(const struct igmp_policy *p);

/* A group the subscriber may join: no restriction, or inside a dynamic or
 * a static range. */
int igmp_policy_allows(const struct igmp_policy *p, uint32_t group);

/* The imputed bandwidth of a group, bytes a second: that of the first
 * dynamic list range holding it, 0 when none says (G.988: 0 is a group no
 * bandwidth limit applies to). */
uint32_t igmp_policy_imputed(const struct igmp_policy *p, uint32_t group);
/* What one join asks for, as the group table knows it. */
struct igmp_join {
	uint32_t group;
	uint16_t uni_vid;               /* VID of the message as the host sent it; 0 untagged */
	int      ani_known;             /* ani_vid is the VLAN the message goes up on */
	uint16_t ani_vid;
	/* The sources of an IGMPv3 record that names some: the addresses (in
	 * network order, 4 bytes each) and whether the host excludes them.
	 * None, for a v1 or v2 report and a record with no sources: any source. */
	int      has_src;
	int      exclude;
	const uint8_t *srcs;
	uint32_t nsrc;
};

/* Fill in the VLAN the join goes up on, when it is known: an upstream tag
 * control of 1 to 3 puts the profile's own VID on every IGMP message and
 * ignores any extended VLAN operation; with control 0 the message goes as it
 * came, so a tagged one keeps its VID (an extended VLAN rule may translate it
 * in between, which nothing here sees), and an untagged one is unknown. */
void igmp_policy_ani(const struct igmp_policy *p, struct igmp_join *j);

enum igmp_join_verdict {
	IGMP_JOIN_DENY = 0,             /* no row lets it in */
	IGMP_JOIN_FULL,                 /* fully authorized */
	IGMP_JOIN_PREVIEW,              /* a preview-only group: *row says how long */
};

/* The lists' answer for one join (G.988 9.3.27 attributes 7 and 8): a row
 * matches when the group is in its range, its VLAN (when it names one) is the
 * join's, if the join's VLAN is one that some row names (policy.c says why
 * only then), and its source (when it names one) is among the sources the
 * join asks for, or the join names none. A group in a
 * static range is let in whatever its VLAN and source. A matching row with no
 * preview length is full access; with one, a preview. With no restriction
 * provisioned, everything is full. The allowed preview table is the group
 * table's to check (it needs the clock). */
int igmp_policy_join(const struct igmp_policy *p, const struct igmp_join *j,
		     const struct igmp_range **row);

/* Whether the profile's IGMP version admits this message type from the host
 * (G.988 9.3.27 attribute 1: support of a version implies the earlier ones).
 * A message above it is not one the profile supports; it is handled as any
 * other and only counted (above_version), as a set-top box that is never
 * queried does not fall back to the profile's version. A
 * profile that is not snooping, names no version, names 3 or an MLD version,
 * admits every one. */
int igmp_policy_version_ok(const struct igmp_policy *p, uint8_t type);

/* The membership interval: robustness * query interval + response time.
 * Setting it shorter than the querier's own interval prunes members that
 * are still watching. */
uint32_t igmp_policy_membership_ttl(const struct igmp_policy *p);

/* How long a member that left stays, with no immediate leave: the
 * last-member time, robustness * last member query interval. Within it the
 * OLT's group-specific query reaches the hosts and a listener that is still
 * there answers, which refreshes the membership. */
uint32_t igmp_policy_last_member_ttl(const struct igmp_policy *p);

/* The upstream tag controls G.988 defines (9.3.27 attribute 5). */
#define IGMP_US_TAG_PASS      0
#define IGMP_US_TAG_ADD       1
#define IGMP_US_TAG_REPLACE   2
#define IGMP_US_TAG_VID       3

/* Whether the host's IGMP has to be taken and sent upstream by igmpd rather
 * than forwarded by the switch: the profile asks for a tag (control 1 to 3),
 * a rate, or the discard of joins outside its lists (unauthorized join
 * behaviour 0, the G.988 default, with lists that restrict something). With
 * none of these the switch's own forwarding is already what the profile
 * says, and the copy is left a copy. */
int igmp_policy_takes(const struct igmp_policy *p);

/* The ODI_SW_MC_SNOOP_* flags to hold at second `now`: nothing when the OLT
 * does not ask for snooping; the copy alone until `learn_until`, so that
 * every group the host already watches can report once before an
 * unregistered one is a group nobody wants; then the copy and the PON's drop,
 * unless a static range was too wide to name, or the lists were longer than
 * fit (their groups would be dropped as unregistered). `pon_drop` is the
 * opt-in for the drop (igmpd -d): 0 holds the copy alone, always. The copy
 * is a take (ODI_SW_MC_SNOOP_TRAP) whenever igmp_policy_takes() says so,
 * from the start: it does not wait for the learning interval, since sending
 * the report as the profile says is not a pruning. */
uint32_t igmp_policy_lease(const struct igmp_policy *p, int static_overflow,
			   uint32_t now, uint32_t learn_until, int pon_drop);

#endif
