/* The GEM flow descriptor -- driver command 25, 68 bytes.
 *
 * Recovered from GemPortCtpDrvCfg in mib_GemPortCtp.so, which is where the
 * structure is built from the managed entity's own row. It is passed BY VALUE:
 * o32 puts the first four words in a0..a3 and memcpys the remaining 52 bytes
 * into the outgoing argument area, which is why the wrappers appear at first to
 * pack a local buffer.
 *
 * `omci_wrapper_updateGemFlow` and `updateUsGemFlow` are the same command with
 * `mode` forced to 2 and 1; `cfgGemFlow` sends whatever the caller set.
 *
 * Seven of the seventeen words are pinned to a source in the MIB row, and the
 * vendor's row layout is the model's attributes at natural alignment:
 *
 *     +0 EntityId u16   +2 PortID u16   +4 TcAdapterPtr u16   +6 Direction u8
 *     +8 UsTraffMgmtPtr u16   +10 UsTraffDescPtr u16   +12 UniCounter u8
 *     +14 DsPriQPtr u16   +16 DsTraffDescPtr u16   ...
 *
 * The rest are computed in the plugin from flow ids and connection state, and
 * are named here only as far as the code says. Anything marked unknown is
 * unknown: do not invent a meaning for it.
 */
#ifndef OMCI_GEMFLOW_H
#define OMCI_GEMFLOW_H

/* Kernel-portable so odi_switch_cmd.c can include
 * this header unmodified in both the host test build and __KERNEL__ --
 * odi_switch_hw.h/odi_switch_dal.h already use this exact guard. No struct
 * or field changed.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define OMCI_GEMFLOW_CMD   25
#define OMCI_TCONT_CMD     21
#define OMCI_GEMFLOW_LEN   68

#define OMCI_GEMFLOW_US    1        /* what updateUsGemFlow forces */
#define OMCI_GEMFLOW_DS    2        /* what updateGemFlow forces */

struct omci_gemflow {
	/* The flow id, and the whole point of the structure: the driver's flow
	 * table is indexed by it. omci_wrapper_cfgGemFlow picks it before the
	 * call -- it reuses the id this port already holds in this direction,
	 * or takes the lowest free slot -- and records the port against it on
	 * success. Upstream and downstream are separate tables of
	 * getDevCapabilities[92] entries each, so one GEM port in both
	 * directions holds one id in each.
	 *
	 * Sending zero for every flow, which is what an uninitialised field
	 * does, means the second flow overwrites the first: that is why a
	 * second downstream flow was refused. */
	uint32_t flow_id;
	uint32_t gem_port;        /* row+2,  GEM Port-ID */
	uint32_t tcont;       /* row+4,  T-CONT. Not the ME id: sending
				 * the ME id is what
				 * made the upstream leg fail -- it wants the
				 * index createTcont hands back. */
	uint32_t alloc_id;       /* Alloc-ID of the T-CONT */
	uint32_t queue;       /* row+8,  normalized queue ordinal in T-CONT */
	/* The downstream queue block, six words -- and counting it as four is what created
	 * a phantom conflict about where `dir` lives. */
	uint32_t ds_pq_pri;   /* row+14, downstream priority queue */
	uint32_t ds_dp_mark;
	uint32_t ds_port;
	/* Queue policy: 0 is strict priority, 1 is weighted round robin, and
	 * the vendor computes it as (Weight >= 2) from the priority queue's own
	 * row. This field read 48 until the instructions were looked at:
	 *
	 *     lbu a0, 0xaa(sp)   ; Weight, class 277 row+18
	 *     sltiu a2, a0, 0x2
	 *     xori  a2, a2, 0x1  ; (Weight >= 2)
	 *     sw    a2, 0xe8(sp) ; descriptor word 8
	 *
	 * 48 is 0x30, the MIB_Get length of that row, two instructions earlier
	 * -- a size argument mistaken for a struct store. */
	uint32_t ds_wrr;
	uint32_t ds_prio;
	uint32_t ds_weight;
	uint32_t cir;
	uint32_t pir;
	uint32_t omcc;
	uint32_t mcast_filter;
	/* 1 asks for a create, and is what every path here uses; the wrapper
	 * treats any other value as an update of the flow the port already
	 * holds, and fails if it holds none. */
	uint32_t ena;
	/* Direction: US=1, DS=2. updateUsGemFlow forces 1 and
	 * updateGemFlow forces 2, which is how this was found. */
	uint32_t dir;
};

/* Command 23, 28 bytes -- omci_wrapper_setPriQueue, which the vendor sends
 * before creating a flow that uses the queue. Only for DOWNSTREAM queues: the
 * wrapper branches on bit 15 of the priority queue's ME id (set means
 * upstream, per G.988) and the upstream branch never reaches the driver, it
 * only updates omci_app's own queue database.
 *
 * Downstream, every field comes from three attributes of the class 277 row:
 * RelatedPort, Weight and DropPrecedenceColourMarking.
 *
 * It is ALSO sent for upstream queues, which the wrapper's early return hides:
 * a helper at the tail of omci_CreatePriQByTcontId sends the same command once
 * per queue of a T-CONT, out of its own queue table rather than out of the MIB row.
 */
#define OMCI_PRIQ_CMD      23
#define OMCI_PRIQ_DEL_CMD  24
#define OMCI_PRIQ_LEN      28

struct omci_priq {
	/* The first two halves mean different things by direction, which is
	 * why they are not named for either: downstream they are the priority
	 * and the switch port, upstream the queue id and the T-CONT index. */
	uint16_t index;         /* DS: RelatedPort & 0xffff.  US: the queue id */
	uint16_t owner;         /* DS: switch port.           US: T-CONT index */
	/* Zero on the downstream path and NOT zero upstream, where they come
	 * from that queue table entry at +16 and +20. Those two words are the committed
	 * and peak rates (cir, pir), in units of 8 Kbit/s -- the wrapper divides the traffic descriptor's byte/s by
	 * 1024 on the way in. This is how a GEM port's traffic descriptor
	 * reaches the hardware upstream: there is no separate rate command in
	 * this version, the queue is simply re-sent with new rates. */
	uint32_t cir;
	uint32_t pir;
	uint16_t weight;        /* Weight, floored at 1 */
	uint16_t rsv3;
	/* 0 strict priority, 1 weighted round robin. Set from weight >= 2. */
	uint32_t wrr;
	uint8_t  dp_mark;     /* DropPrecedenceColourMarking */
	uint8_t  pad[3];
	uint32_t dir;           /* OMCI_GEMFLOW_US or _DS */
};

typedef char omci_priq_size_check[
	(sizeof(struct omci_priq) == OMCI_PRIQ_LEN) ? 1 : -1];

/* Command 21, 8 bytes: the allocation id goes in, the T-CONT index comes back.
 * Note that word 0 is never initialised on the way in -- the vendor sends
 * whatever was on its stack -- so treat it as output only. */
struct omci_tcont {
	uint32_t index;         /* out: the driver's T-CONT index */
	uint32_t alloc_id;       /* in:  from the T-CONT ME's Alloc-ID */
};

/* Command 65, 72 bytes -- the VEIP GEM flow set, and 66 its delete.
 * It re-provisions the upstream queue set of one VEIP GEM port: the flow and
 * the tc-queue behind each of up to eight WAN queues.
 *
 * WE DO NOT SEND IT, and the vendor firmware beside us should not either.
 * Its only caller is setUsVeipPriQ / setUsSingleVeipPriQ, which ought to
 * refuse both when dual management mode is off and when there is at most one
 * WAN queue. Our libomci_mib.so has only the first check: its refusal string
 * for dual management mode off is present, and nothing checks the WAN queue
 * count. DUAL_MGMT_MODE is 1 here, so nothing stops the routine, and
 * with OMCI_WAN_QOS_QUEUE_NUM at 1 it expands into queues that do not exist.
 * isp2 logs the result at every start:
 *
 *     del veip gem flow [0] failed!
 *     set veip gem flow [0] failed!
 *
 * So this is a vendor bug, and NOT sending it
 * is the correct behaviour rather than a missing feature. The struct is here
 * because the gate is ours to open: our own config-store writer can raise
 * OMCI_WAN_QOS_QUEUE_NUM, and then this becomes real work. */
#define OMCI_VEIP_GEMFLOW_CMD      65
#define OMCI_VEIP_GEMFLOW_DEL_CMD  66
#define OMCI_VEIP_GEMFLOW_LEN      72
#define OMCI_WAN_QUEUE_MAX          8

struct omci_veip_gemflow {
	uint32_t gem_port;
	uint32_t tcont;
	uint32_t flow_id[OMCI_WAN_QUEUE_MAX];
	uint32_t tc_queue[OMCI_WAN_QUEUE_MAX];
};

typedef char omci_veip_gemflow_size_check[
	(sizeof(struct omci_veip_gemflow) == OMCI_VEIP_GEMFLOW_LEN) ? 1 : -1];

#endif
