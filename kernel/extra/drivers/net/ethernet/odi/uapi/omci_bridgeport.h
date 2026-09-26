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
	/* Zero at both call sites. Its meaning is unread -- do not invent one;
	 * sending anything else here has never been observed. */
	uint32_t sel;
	/* Create sends (class 45's DiscardUnknow != 1), which is NOT the same
	 * as !DiscardUnknow: the vendor writes `xori v0,v0,1; sltu v0,zero,v0`
	 * on that byte, so a value of 2 enables flooding just as 0 does.
	 * Delete sends 0. */
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

typedef char omci_dot1rl_size_check[
	(sizeof(struct omci_dot1rl) == OMCI_DOT1RL_LEN) ? 1 : -1];
typedef char omci_flood_size_check[
	(sizeof(struct omci_flood) == OMCI_FLOOD_LEN) ? 1 : -1];
typedef char omci_unirate_size_check[
	(sizeof(struct omci_unirate) == OMCI_UNIRATE_LEN) ? 1 : -1];

#endif
