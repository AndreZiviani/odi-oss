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

#endif
