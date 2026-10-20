/* The two driver payloads a MAC bridge port config data row builds directly.
 *
 * Both were recovered from MacBriPortCfgDataDrvCfg in mib_MacBriPortCfgData.so
 * plus the libomci_mib wrappers it calls, which are pure pass-throughs: each
 * one hands the caller's own stack buffer to the driver and does nothing else
 * but log a failure. So the field order below is the wire order, not a guess.
 *
 * The third payload class 47 reaches, driver command 53 (setMacLearnLimit), is
 * two words and already has a generated accessor that takes them as arguments.
 */
#ifndef OMCI_BRIDGEPORT_H
#define OMCI_BRIDGEPORT_H

/* Kernel-portable guard, see omci_gemflow.h. */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define OMCI_FLOOD_CMD     64
#define OMCI_FLOOD_LEN     12

/* Command 64, 12 bytes. The vendor sends it from two places, a bridge port
 * create and the matching delete, and they differ in one word. */
struct omci_flood {
	/* Which lookup-miss mask: 0 unknown unicast, 1 broadcast, 2 unknown
	 * multicast (the stock omci_flood_type_t). Zero at both call sites. */
	uint32_t sel;
	/* Create sends (class 45's DiscardUnknow != 1), which is NOT the same
	 * as !DiscardUnknow: the vendor writes `xori v0,v0,1; sltu v0,zero,v0`
	 * on that byte, so a value of 2 enables flooding just as 0 does.
	 * Delete sends 0. The kernel takes 1 as add portMask's ports, 0 as
	 * remove them (the stock omci_flood_act_t; its 2, trap, is refused). */
	uint32_t enable;
	/* 1 << switchPort -- the port being added or removed, alone, not the
	 * bridge's whole membership. */
	uint32_t portMask;
};

#define OMCI_UNIRATE_CMD   67
#define OMCI_UNIRATE_LEN   12

/* Command 67, 12 bytes, built by omci_apply_traffic_descriptor_to_uni_port
 * out of one class 280 row. */
struct omci_unirate {
	uint32_t port;          /* the UNI's switch port */
	/* 1 is the INBOUND traffic descriptor and 2 the outbound, on a UNI
	 * port. The GEM-port helper uses the opposite pairing, which looks like
	 * a bug and is not: traffic entering the bridge from a UNI and traffic
	 * leaving it toward a GEM port are the same direction. */
	uint32_t dir;
	/* PIR converted from bytes per second: (PIR << 3) >> 10. A zero result
	 * and a missing descriptor both become OMCI_UNIRATE_NOLIMIT. */
	uint32_t kbps;
};

#define OMCI_UNIRATE_DIR_IN   1
#define OMCI_UNIRATE_DIR_OUT  2
#define OMCI_UNIRATE_NOLIMIT  0xffff8u   /* 1048568 kbit/s, the vendor's "off" */

/* What the vendor hands setDsBcGemFlow when the last downstream-broadcast
 * bridge port is deleted. */
#define OMCI_BC_FLOW_NONE  0xffffffffu

/* Commands 59 and 60, 20 bytes each, from class 298's handler.
 *
 * Neither wrapper is a pass-through: each takes a 16-byte descriptor, finds the
 * slot already holding its (portMask, kind) -- or, setting, the first free one
 * -- and sends the slot in front of it. So the twenty bytes below are the wire
 * payload and the slot is the caller's business.
 *
 * The vendor's table size is gInfo[0xdc] and one index in it is reserved by
 * gInfo[0xe0]; neither is recovered. Ours is sixteen slots, which is a choice,
 * not a measurement. What has to hold is only that a delete names the slot its
 * set used -- the driver's table is indexed by it and the first two words are
 * what identify it.
 */
#define OMCI_DOT1RL_SET_CMD  59
#define OMCI_DOT1RL_DEL_CMD  60
#define OMCI_DOT1RL_LEN      20
#define OMCI_DOT1RL_SLOTS    16

struct omci_dot1rl {
	uint32_t slot;
	/* The identity, both words of it: every PPTP Ethernet UNI switch port
	 * in the parent bridge, and which of the three rates this is. */
	uint32_t portMask;
	uint32_t kind;
	/* Payload, straight off the traffic descriptor. Not even initialised on
	 * the vendor's delete path. */
	uint32_t cir;
	uint32_t cbs;
};

#define OMCI_DOT1RL_UC_FLOOD 0
#define OMCI_DOT1RL_BC       1
#define OMCI_DOT1RL_MC       2

/* The learning limit of ME 45 / ME 47 (command 53, setMacLearnLimit): two
 * words, {port, limit}. Its accessor is generated (omci_drv.c), so there is
 * no struct for omcid, only this one for the kernel.
 *
 * The limit is a count of learned addresses; a count at or past the table
 * size (OMCI_LEARN_LIMIT_TABLE) is the same as none. Zero is refused, not
 * taken as "learn nothing": omcid never sends it, and a limit of zero turns
 * address learning off on the port, which is a forwarding change nobody has
 * asked for.
 */
#define OMCI_LEARN_LIMIT_CMD    53
#define OMCI_LEARN_LIMIT_LEN    8
#define OMCI_LEARN_LIMIT_TABLE  1088u

struct omci_learn_limit {
	uint32_t port;
	uint32_t limit;
};

/* The bridge table of one switch port, ME 50 (MAC bridge port bridge table
 * data).
 *
 * Command 61, getBridgeTableByPort, is the stock wrapper's: four bytes, the
 * port in and the number of unicast addresses the switch holds for that port
 * out. The stock driver hands the addresses themselves back through a
 * memory-mapped buffer of its character device, which does not exist behind
 * a netlink socket, so the addresses come through command OMCI_BRIDGE_TBL_CMD
 * instead, ours and outside the stock numbering like OMCI_FLOWS_CMD.
 *
 * OMCI_BRIDGE_TBL_CMD reads one page: `first` is the index, among the port's
 * addresses in table order, of the first one wanted. It answers `total`, the
 * addresses the port has, and `count`, how many of them are in entry[].
 * Each entry is the 8 bytes ME 50 carries on the wire, as the stock driver
 * wrote them: a big-endian 16-bit information word
 *     bits 15..4  age, in seconds (the row's 3-bit age counter scaled by the
 *                 configured ageing time)
 *     bit 2       1 for a learned row, 0 for a static one
 *     bit 0       1 when the row blocks frames to the address
 * then the six MAC octets. The table is read row by row while learning goes
 * on, so two pages are not one atomic snapshot; a reader that needs one
 * stops asking for more once it holds `total` entries.
 */
#define OMCI_BRIDGE_TBL_PORT_CMD   61
#define OMCI_BRIDGE_TBL_PORT_LEN   4

struct omci_bridge_tbl_per_port {
	uint16_t portId;
	uint16_t cnt;
};

#define OMCI_BRIDGE_TBL_CMD        0x101u
#define OMCI_BRIDGE_TBL_ENTRY_LEN  8u
#define OMCI_BRIDGE_TBL_PAGE       30u
#define OMCI_BRIDGE_TBL_LEN        256u

struct omci_bridge_tbl {
	uint32_t port;                  /* in: the switch port */
	uint32_t first;                 /* in: first entry wanted */
	uint32_t total;                 /* out: entries the port has */
	uint32_t count;                 /* out: entries in entry[] */
	uint8_t  entry[OMCI_BRIDGE_TBL_PAGE * OMCI_BRIDGE_TBL_ENTRY_LEN];
};

typedef char omci_bridge_tbl_size_check[
	(sizeof(struct omci_bridge_tbl) == OMCI_BRIDGE_TBL_LEN) ? 1 : -1];
typedef char omci_bridge_tbl_port_size_check[
	(sizeof(struct omci_bridge_tbl_per_port) == OMCI_BRIDGE_TBL_PORT_LEN) ? 1 : -1];
typedef char omci_learn_limit_size_check[
	(sizeof(struct omci_learn_limit) == OMCI_LEARN_LIMIT_LEN) ? 1 : -1];

/* Command 63 (setPortBridging) takes one word, 1 for "the UNIs bridge to one
 * another and to the PON". Command 40 (setPauseControl), {port, pause time}. */
#define OMCI_PORT_BRIDGING_CMD  63
#define OMCI_PAUSE_CONTROL_CMD  40

typedef char omci_dot1rl_size_check[
	(sizeof(struct omci_dot1rl) == OMCI_DOT1RL_LEN) ? 1 : -1];
typedef char omci_flood_size_check[
	(sizeof(struct omci_flood) == OMCI_FLOOD_LEN) ? 1 : -1];
typedef char omci_unirate_size_check[
	(sizeof(struct omci_unirate) == OMCI_UNIRATE_LEN) ? 1 : -1];

#endif
