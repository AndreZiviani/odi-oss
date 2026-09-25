/* The bridge connection descriptor -- driver command 51, 160 bytes.
 *
 * Nine ints followed by a 124-byte VLAN operation block. On this firmware's
 * driver ABI the VLAN filter carries no per-DSCP array, which makes the whole
 * descriptor exactly the 160 we measured.
 *
 * Two independent checks agree with the layout. `omci_ApplyTrafficRule` patches
 * three fields to 8 before the send, and they land on `outer_mode` (+48),
 * `inner_mode` (+52) and `out.out_tag.pri` (+132) with no fitting --
 * the VID filter bit is 1<<3 and the ignore-priority sentinel is 8.
 *
 * WHAT THIS HEADER USED TO SAY, AND WHY IT WAS WRONG. It carried twelve
 * offsets taken from the labels `omcicli dump conn` prints. Those labels do not
 * describe this structure at all: that dump prints the MIB tree's own
 * connection record, a list of pointers into the MIB, and then a list of the
 * generated VLAN rules. Every offset in the old table described the tree
 * record. They are gone.
 *
 * Later driver versions in the same family add fields; this firmware's
 * driver ABI does not carry them.
 */
#ifndef OMCI_BDGCONN_H
#define OMCI_BDGCONN_H

/* Kernel-portable guard, see omci_gemflow.h. */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define OMCI_BDGCONN_CMD        51      /* activate */
#define OMCI_BDGCONN_OFF_CMD    50      /* deactivate, 4 bytes */
#define OMCI_BDGCONN_LEN        160

/* GEM port direction. Note these are a bitmask in spirit -- BI is US|DS --
 * and the constructor tests them that way when deciding which flow ids to
 * fill in. */
#define OMCI_DIR_US             0x1
#define OMCI_DIR_DS             0x2
#define OMCI_DIR_BI             0x3

/* Which of the rule generators produced this
 * rule, and therefore how the driver reads the filter and action halves. */
#define OMCI_VLAN_OPER_FORWARD_ALL        0
#define OMCI_VLAN_OPER_FORWARD_UNTAG      1
#define OMCI_VLAN_OPER_FORWARD_SINGLETAG  2
#define OMCI_VLAN_OPER_FILTER_INNER_PRI   3
#define OMCI_VLAN_OPER_FILTER_SINGLETAG   4
#define OMCI_VLAN_OPER_EXTVLAN            5
#define OMCI_VLAN_OPER_VLANTAG_OPER       6

/* Filter mode bits, for outer_mode / inner_mode. */
#define OMCI_TAGF_ANY  (1 << 0)
#define OMCI_TAGF_TAGGED     (1 << 1)
#define OMCI_TAGF_UNTAGGED       (1 << 2)
#define OMCI_TAGF_VID          (1 << 3)
#define OMCI_TAGF_PRI          (1 << 4)
#define OMCI_TAGF_TCI          (1 << 5)
#define OMCI_TAGF_ETHTYPE      (1 << 6)
#define OMCI_TAGF_DSCP     (1 << 7)

/* The treatment TPID/DE field of an OMCI_VLAN, decoded from the jump table
 * behind `omcicli dump conn`'s own printer in libomci_mib.so (0xd8f8). The
 * order is NOT the order the strings sit in .rodata -- 0 is COPY_FROM_INNER and
 * 1 is COPY_FROM_OUTER, the reverse -- which is why the table was read rather
 * than the strings counted. */
#define OMCI_TREAT_TPID_COPY_INNER    0
#define OMCI_TREAT_TPID_COPY_OUTER    1
#define OMCI_TREAT_TPID_OUT_DEI_INNER 2
#define OMCI_TREAT_TPID_OUT_DEI_OUTER 3
#define OMCI_TREAT_TPID_8100          4
#define OMCI_TREAT_TPID_OUT_DEI_0     6
#define OMCI_TREAT_TPID_OUT_DEI_1     7

/* The filter TPID field, same printer, the other branch. */
#define OMCI_FILTER_TPID_DO_NOT       0
#define OMCI_FILTER_TPID_8100         4

/* out.tpid, a third and much smaller enum: the printer tests it against
 * 1, 0 and 2 in that order and prints 88A8, 8100 and 0800. */
#define OMCI_OUT_TPID_8100            0
#define OMCI_OUT_TPID_88A8            1
#define OMCI_OUT_TPID_0800            2

/* Ethertype filter, for ethertype. */
#define OMCI_ETHTYPE_NO_CARE     0
#define OMCI_ETHTYPE_IP          1
#define OMCI_ETHTYPE_PPPOE       2
#define OMCI_ETHTYPE_ARP         3
#define OMCI_ETHTYPE_IPV6        4
#define OMCI_ETHTYPE_PPPOE_S     5

/* Tag action -- tag_op, and also out.ds_tag_op. */
#define OMCI_TAGOP_NONE             0
#define OMCI_TAGOP_PUSH             1
#define OMCI_TAGOP_POP          2
#define OMCI_TAGOP_REWRITE          3
#define OMCI_TAGOP_PASS     4

/* VID action */
#define OMCI_VIDOP_SET           0
#define OMCI_VIDOP_FROM_INNER       1
#define OMCI_VIDOP_FROM_OUTER       2
#define OMCI_VIDOP_PASS      3

/* Priority action */
#define OMCI_PRIOP_SET           0
#define OMCI_PRIOP_FROM_INNER       1
#define OMCI_PRIOP_FROM_OUTER       2
#define OMCI_PRIOP_FROM_DSCP        3
#define OMCI_PRIOP_PASS      4

/* The "don't care" sentinels. These are the values a zeroed descriptor does
 * NOT have, which is the trap: a field left at 0 means "filter on priority 0",
 * not "ignore priority". */
#define OMCI_VID_ANY            4096
#define OMCI_PRI_ANY            8
#define OMCI_EXTVLAN_DROP_FRAME   3

/* One tag, 12 bytes. */
struct omci_vlan {
	uint32_t pri;
	uint32_t vid;
	uint32_t tpid;
};

/* The filter half, 36 bytes. */
struct omci_vlan_filter {
	struct omci_vlan outer;    /* rule +4  (descriptor +40) */
	struct omci_vlan inner;    /* rule +16 (descriptor +52) */
	uint32_t outer_mode;        /* rule +28 (descriptor +64) */
	uint32_t inner_mode;        /* rule +32 (descriptor +68) */
	uint32_t ethertype;             /* rule +36 (descriptor +72) */
};

/* One tag action, 24 bytes. */
struct omci_vlan_act {
	uint32_t tag_op;
	uint32_t vid_op;
	uint32_t pri_op;
	struct omci_vlan set_tag;
};

/* The output style, 36 bytes. */
struct omci_vlan_out {
	uint32_t is_default;
	uint32_t is_mcast;
	uint32_t tag_count;
	uint32_t tpid;
	uint32_t ds_mode;
	uint32_t ds_tag_op;         /* an OMCI_TAGOP_* value */
	struct omci_vlan out_tag;       /* .pri is the third field patched to 8 */
};

/* The VLAN operation block, 124 bytes, copied wholesale out of the rule the
 * generator produced -- it is not rebuilt. */
struct omci_vlan_oper {
	uint32_t rule_gen;            /* OMCI_VLAN_OPER_* */
	struct omci_vlan_filter filter;
	struct omci_vlan_act outer_act;
	struct omci_vlan_act inner_act;
	struct omci_vlan_out out;
};

/* The whole descriptor, 160 bytes. Offsets in the comments are from the start
 * of the descriptor, which is what a hex dump of a captured command shows. */
struct omci_bdgconn {
	uint32_t in_use;        /* +0   TRUE before the send */
	uint32_t latch;       /* +4   ACL latch flag, from the rule */
	int32_t  service_id;        /* +8   service id, one per classifier rule */
	uint32_t uni_mask;       /* +12  upstream: the UNI ports it applies to */
	uint32_t us_flow;      /* +16  upstream GEM flow id */
	uint32_t us_dp_flow;    /* +20 */
	uint32_t us_dp_mark;   /* +24 */
	uint32_t ds_flow;      /* +28  downstream GEM flow id */
	uint32_t dir;           /* +32  OMCI_DIR_* */
	struct omci_vlan_oper vlan_op;  /* +36 .. +159 */
};

/* Build-time proof the layout is the 160 bytes the driver wants. A negative
 * array size is the freestanding way to say static_assert. */
typedef char omci_bdgconn_size_check[
	(sizeof(struct omci_bdgconn) == OMCI_BDGCONN_LEN) ? 1 : -1];

int omci_activeBdgConn(void *desc160);
int omci_deactiveBdgConn(uint32_t which);

#endif
