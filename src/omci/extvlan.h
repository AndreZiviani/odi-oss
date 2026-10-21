/* Class 171, the received-frame VLAN tagging operation table.
 *
 * One table entry is four big-endian words, and the two halves are NOT laid
 * out the same way -- a filter word carries the priority in the top nibble, a
 * treatment word carries it at bit 16. Copying one layout to the other is the
 * obvious mistake and produces plausible-looking garbage, which is why the
 * decode lives here once rather than in each caller.
 *
 * The layout is the G.988 one for this table, and it matches the shifts the
 * stock dump command applies, field for field.
 *
 *   word0  filter outer      PRI 31..28  VID 27..15  TPID 14..12
 *   word1  filter inner      as word0, plus EthType 3..0
 *   word2  treatment outer   RemoveTags 31..30  PRI 19..16  VID 15..3  TPID 2..0
 *   word3  treatment inner   PRI 19..16  VID 15..3  TPID 2..0
 *
 * The sentinel values below are G.988 values.
 * They are here so downstream code can say what it means: a bare 4096 in a VID
 * comparison is unreadable, and 4096 means two different things depending on
 * which half of the entry it is in.
 */
#ifndef ODI_OMCI_EXTVLAN_H
#define ODI_OMCI_EXTVLAN_H

#include <stdint.h>

struct omci_vlan_oper;

/* Filter side. */
#define EVTOCD_F_PRI_IGNORE_OTHER   15   /* this whole tag is not examined */
#define EVTOCD_F_PRI_DEFAULT_RULE   14   /* the default rule for this tag count */
#define EVTOCD_F_PRI_DO_NOT_FILTER   8
#define EVTOCD_F_VID_DO_NOT_FILTER   4096
#define EVTOCD_F_TPID_DO_NOT_FILTER  0
#define EVTOCD_F_TPID_8100           4
#define EVTOCD_F_TPID_INPUT          5
#define EVTOCD_F_TPID_INPUT_DEI_0    6
#define EVTOCD_F_TPID_INPUT_DEI_1    7

#define EVTOCD_F_ET_DO_NOT_FILTER    0
#define EVTOCD_F_ET_IPOE_0800        1
#define EVTOCD_F_ET_PPPOE            2
#define EVTOCD_F_ET_ARP_0806         3
#define EVTOCD_F_ET_IPV6_86DD        4
#define EVTOCD_F_ET_EAPOL_888E       5

/* Treatment side. RemoveTags is two bits: 0, 1 or 2 tags removed, and 3 is not
 * a count at all -- it discards the frame. */
#define EVTOCD_T_DISCARD_FRAME       3
#define EVTOCD_T_PRI_COPY_INNER      8
#define EVTOCD_T_PRI_COPY_OUTER      9
#define EVTOCD_T_PRI_FROM_DSCP       10
#define EVTOCD_T_PRI_DO_NOT_ADD      15
#define EVTOCD_T_VID_COPY_INNER      4096
#define EVTOCD_T_VID_COPY_OUTER      4097
#define EVTOCD_T_TPID_COPY_INNER     0
#define EVTOCD_T_TPID_COPY_OUTER     1
#define EVTOCD_T_TPID_DEI_CP_INNER   2
#define EVTOCD_T_TPID_DEI_CP_OUTER   3
#define EVTOCD_T_TPID_8100           4
#define EVTOCD_T_TPID_OUTPUT_DEI_0   6
#define EVTOCD_T_TPID_OUTPUT_DEI_1   7

/* Legal ranges, from the same header. A VID of 4095 is reserved, hence 4094. */
#define EVTOCD_PRI_MAX               7
#define EVTOCD_VID_MAX               4094

#define EVTOCD_ENTRY_LEN             16

struct evtocd_entry {
	uint8_t  f_out_pri;
	uint16_t f_out_vid;
	uint8_t  f_out_tpid;
	uint8_t  f_in_pri;
	uint16_t f_in_vid;
	uint8_t  f_in_tpid;
	uint8_t  f_ethertype;

	uint8_t  remove_tags;
	uint8_t  t_out_pri;
	uint16_t t_out_vid;
	uint8_t  t_out_tpid;
	uint8_t  t_in_pri;
	uint16_t t_in_vid;
	uint8_t  t_in_tpid;
};

void evtocd_decode(const uint8_t *e, struct evtocd_entry *out);

/* Whether the downstream generator creates any rule at all for this entry.
 *
 * No downstream rule is created when the ethertype filter is past IPoE or the
 * treatment is discard: such a rule would swallow the downstream multicast
 * classifier rule, and the classifier has too few entries to spare. Upstream
 * has no such early return.
 */
int evtocd_ds_skipped(const struct evtocd_entry *e);

/* Whether this entry is one of the G.988 default rules -- the ones whose filter
 * priority is the DEFAULT_TAG_RULE sentinel. The vendor generates a drop for
 * each unless the bdp_00000080 plugin rescues it.
 *
 * Returns the number of tags the rule is the default FOR: 1 when the outer is
 * ignored and the inner is DEFAULT, 2 when both are DEFAULT, and -1 when it is
 * not a default rule. There is no untagged default -- see the comment on the
 * definition, and note this returned one too few until a test caught it.
 */
int evtocd_default_rule_tags(const struct evtocd_entry *e);

/* ------------------------------------------------ what an entry means -------
 *
 * The half of the rule generation that transfers. What the stock daemon
 * builds from this table -- the switch fabric rule of omci_bdgconn.h -- is
 * not reproduced here. The semantics below are, and they follow G.988 and the
 * observed behaviour of the stock firmware rather than guesswork.
 */

/* How many tags the frames this entry is about carry. Taken from the FILTER
 * side: outer IGNORE with inner IGNORE is the untagged rule, outer IGNORE with
 * an inner that is anything else is the single-tag rule, otherwise double. */
enum evtocd_tags {
	EVTOCD_UNTAGGED = 0,
	EVTOCD_SINGLE_TAG,
	EVTOCD_DOUBLE_TAG,
};

/* Where the priority of an added tag comes from. */
enum evtocd_pri_src {
	EVTOCD_PRI_ASSIGN = 0,  /* the literal value in `pri` */
	EVTOCD_PRI_COPY_INNER,
	EVTOCD_PRI_COPY_OUTER,
	EVTOCD_PRI_DSCP,
	EVTOCD_PRI_IMPOSSIBLE,  /* copy from a tag the matched frame has not got */
};

enum evtocd_vid_src {
	EVTOCD_VID_ASSIGN = 0,
	EVTOCD_VID_COPY_INNER,
	EVTOCD_VID_COPY_OUTER,
};

struct evtocd_tagout {
	uint8_t  pri_src;       /* enum evtocd_pri_src */
	uint8_t  pri;           /* meaningful when pri_src is ASSIGN */
	uint8_t  vid_src;       /* enum evtocd_vid_src */
	uint16_t vid;           /* meaningful when vid_src is ASSIGN */
	uint16_t tpid;          /* resolved; 0 when the source says do not filter */
};

struct evtocd_action {
	uint8_t tags_in;        /* enum evtocd_tags */
	uint8_t discard;        /* the treatment is DISCARD_FRAME */
	uint8_t tags_removed;   /* 0, 1 or 2 -- not meaningful when discard */
	uint8_t tags_added;     /* 0, 1 or 2 */
	uint8_t transparent;    /* nothing removed and nothing added */
	struct evtocd_tagout add[2];   /* [0] outer, [1] inner; tags_added valid */
};

/* Decide what this entry does. `input_tpid` and `output_tpid` are the class 171
 * attributes of the same name, needed because a treatment can name either. */
void evtocd_action(const struct evtocd_entry *e, uint16_t input_tpid,
		   uint16_t output_tpid, struct evtocd_action *out);

/* One line an operator can read, into `buf`. Returns the length written.
 *
 * "untagged frames: add VLAN 1, priority 0, TPID 0x8100" says what
 * "PRI 15, VID 4096" does not, and that is the whole point of it.
 */
int evtocd_describe(const struct evtocd_entry *e, uint16_t input_tpid,
		    uint16_t output_tpid, char *buf, int max);

/* The one shape of class 171 the datapath builds a rule from: `n` entries of
 * one instance, an untagged rule that adds a single tag with a concrete VID
 * (1..4094) and priority (0..7) at TPID 0x8100, and nothing else but default
 * rules that drop or pass. Returns NULL and the tag in *vid / *pri, or a
 * static reason naming what was not supported -- what an operator reports. */
const char *evtocd_olt_tag(const uint8_t *tbl, int n, uint16_t output_tpid,
			   int *vid, int *pri);


/* ------------------------------------------ what the classifier can do -----
 *
 * One entry of the table, reduced to what the switch's classifier can carry
 * out. The classifier compares one tag's VID and priority per row, tells an
 * S-tag (TPID 0x88a8, the primary S-VLAN TPID) from a C-tag (0x8100), and
 * acts on the two tags as two independent planes: the C-tag can be added,
 * replaced (its priority kept, or assigned) or removed, the S-tag added,
 * replaced or removed. An entry is a "plan" when it is expressible that way;
 * otherwise evtocd_plan() says in words what it is not.
 *
 * The same structure describes either direction. evtocd_plan() builds the
 * upstream one from an entry; evtocd_plan_ds() turns it into the downstream
 * one, G.988 "inverse" operation included, so a downstream row is just
 * another plan and nothing downstream-specific has to be taught to the code
 * that turns plans into rules.
 */
#define EVTOCD_TPID_C                0x8100
#define EVTOCD_TPID_S                0x88a8

enum evtocd_op {
	EVTOCD_OP_PASS = 0,     /* the tag, if the frame has one, is left alone */
	EVTOCD_OP_PUSH,         /* the frame had no such tag; it gets one */
	EVTOCD_OP_REWRITE,      /* the frame had one; it gets other values */
	EVTOCD_OP_POP,          /* the frame had one; it goes */
};

/* One tag position: the C-tag, or the S-tag. */
struct evtocd_plane {
	uint8_t in;             /* the matched frame carries this tag */
	uint8_t out;            /* and leaves carrying it */
	uint8_t op;             /* enum evtocd_op */
	int16_t f_vid;          /* match on the tag's VID, -1 not examined */
	int8_t  f_pri;          /* match on its priority, -1 not examined */
	int16_t vid;            /* PUSH, REWRITE: the VID written */
	int8_t  pri;            /* PUSH, REWRITE: the priority written, -1 keep
				 * the one the tag has (a REWRITE of a C-tag) */
};

/* Which tag the classifier compares. Only one, so a plan whose frame has both
 * tags and examines both is refused, and a downstream one narrows to one. */
enum evtocd_cmp { EVTOCD_CMP_C = 0, EVTOCD_CMP_S };

struct evtocd_plan {
	struct evtocd_plane s, c;
	uint8_t cmp;            /* enum evtocd_cmp: the tag f_vid / f_pri belong to */
	uint8_t out_tags;       /* tags the frame leaves with: 0, 1 or 2 */
	int16_t out_vid;        /* the outermost of them, -1 when it follows the frame */
	int8_t  out_pri;        /* likewise; what picks the GEM port on a mapper */
	uint8_t ds_wider;       /* downstream: matches more than the OLT said (the
				 * discard half of modes 5 to 7 is not done, or only
				 * one of two tags is compared) */
	uint8_t et_n;           /* upstream, untagged frames only: the EtherTypes the
				 * entry names, 0 (any), 1, or 2 (PPPoE) */
	uint16_t et[2];
};

/* Downstream behaviour, class 171 attribute 5 ("DsMode"), G.988 table 9.3.13-1. */
#define EVTOCD_DSMODE_INVERSE        0   /* undo the upstream operation */
#define EVTOCD_DSMODE_FORWARD        1   /* forward everything unchanged */
#define EVTOCD_DSMODE_VID_PRI        2   /* undo on VID and priority, else forward */
#define EVTOCD_DSMODE_VID            3   /* undo on VID, else forward */
#define EVTOCD_DSMODE_PRI            4   /* undo on priority, else forward */
#define EVTOCD_DSMODE_VID_PRI_DROP   5   /* as 2, but discard the rest */
#define EVTOCD_DSMODE_VID_DROP       6
#define EVTOCD_DSMODE_PRI_DROP       7
#define EVTOCD_DSMODE_DROP_ALL       8

/* Turn one entry into its upstream plan. Returns NULL, or a static reason
 * naming what the classifier cannot do (what an operator reports). Default
 * rules and discard entries are not plans: the caller reads them with
 * evtocd_default_rule_tags() and evtocd_action(). */
const char *evtocd_plan(const struct evtocd_entry *e, uint16_t input_tpid,
			uint16_t output_tpid, struct evtocd_plan *p);

/* What an entry that discards the frames it matches comes to: the classifier
 * has no drop action, the ACL has one, and it compares the C-tag's VID and
 * priority of a frame on its way in. So the entries that can be done are the
 * ones that name a C-tagged frame by VID, with a priority or without. */
struct evtocd_drop {
	int16_t vid;            /* 1..4094 */
	int8_t  pri;            /* 0..7, -1 any */
};

/* Reads an entry whose treatment discards (and that is not a default rule)
 * as a drop. Returns NULL with *d filled, or a static reason. */
const char *evtocd_drop(const struct evtocd_entry *e, uint16_t input_tpid,
			struct evtocd_drop *d);

/* Which priorities of the drop's VID the ACL may discard, given the plans of
 * the other entries of the table. The ACL decides before the classifier, so a
 * priority another entry of the same VID names must not be discarded: that
 * entry is the more specific one and would never see its frames. Start *pmask
 * at evtocd_drop_all(d) and call this with the upstream plan of every other
 * entry; bit n is left set for priority n still to discard (0xff: every
 * priority, one rule, no split). Returns NULL or a static reason: an overlap
 * the table does not order, or an entry on an S-tag, whose C-tag the ACL
 * field is not known to hide. */
const char *evtocd_drop_shadow(const struct evtocd_drop *d, const struct evtocd_plan *r,
			       unsigned *pmask);
static inline unsigned evtocd_drop_all(const struct evtocd_drop *d)
{
	return d->pri >= 0 ? 1u << d->pri : 0xffu;
}

/* The bridge rule of cmd 51, rule_gen OMCI_VLAN_OPER_EXTVLAN_DROP, for one
 * priority of the drop (pri -1 for any). */
void evtocd_drop_oper(const struct evtocd_drop *d, int pri, struct omci_vlan_oper *vr);

/* An upstream rule of a plan with an EtherType: the same descriptor with the
 * EtherType `et` named outright, a literal 16-bit value. */
void evtocd_oper_ethertype(struct omci_vlan_oper *vr, uint16_t et);

/* The rule of an untagged frame whose EtherType no entry names: in a table
 * that has EtherType entries it must sit after them. */
void evtocd_oper_rest(struct omci_vlan_oper *vr);

/* The downstream plan of an upstream one under `ds_mode`. Returns 1 with *d
 * filled, or 0 with no downstream rule and *why saying so: mode 1 and 8 want
 * none, and a rule that merged several frames into one (a filter that did not
 * name the VID it replaced) cannot be undone. */
int evtocd_plan_ds(const struct evtocd_plan *u, uint8_t ds_mode,
		   struct evtocd_plan *d, const char **why);

/* The plan as the bridge rule of cmd 51, rule_gen OMCI_VLAN_OPER_EXTVLAN_GEN
 * (omci_bdgconn.h). Either direction's plan; the caller sets the descriptor's
 * direction. */
struct omci_vlan_oper;
void evtocd_plan_oper(const struct evtocd_plan *p, struct omci_vlan_oper *vr);

/* One line for the log: "C-VID 410 pri 5 -> C-VID 1410 pri kept", say. */
int evtocd_plan_describe(const struct evtocd_plan *p, char *buf, int max);

#endif
