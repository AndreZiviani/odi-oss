/* The way back up: what igmpd sends to the OLT in place of a report the
 * switch took (the lease's TRAP mode, switch_opt.h).
 *
 * While the profile asks for something the switch cannot do on its own (an
 * upstream IGMP tag, a rate, the discard of joins outside the lists: G.988
 * 9.3.27 attributes 4 to 6 and 15, policy.h igmp_policy_takes()), the switch
 * hands the host's IGMP to the CPU and forwards none of it. Each message then
 * goes up only if igmpd sends it, and goes up as the profile says:
 *
 *   - a message whose joins the lists all refuse is withheld, unless the
 *     profile forwards unauthorized joins; a leave, a join the lists allow,
 *     and anything that is not a join go;
 *   - a query from the host side is not sent: the OLT is the querier;
 *   - no more than the profile's rate a second (messages, G.988's unit);
 *   - tag control 0: tagged as the switch would have tagged it, by the
 *     classifier row the frame matches (the kernel says which, and its flow);
 *     1 to 3: the profile's TCI added or put in place of the one there, sent
 *     on the flow of the row that carries that VLAN up, else the flow the
 *     frame itself would have taken.
 *
 * Everything here is pure (no syscalls), so the decisions and the frames are
 * tested on the host against synthetic input.
 */
#ifndef ODI_IGMP_UPLINK_H
#define ODI_IGMP_UPLINK_H

#include <stdint.h>
#include "igmpd.h"
#include "policy.h"
#include "odi_sw_ioctl.h"

#define IGMP_TPID_C         0x8100
#define IGMP_TPID_S         0x88a8
/* A minimum Ethernet frame without its FCS, which the NIC adds. */
#define IGMP_UP_MIN_FRAME   60
/* What igmpd sends: the largest redirect payload plus a tag. */
#define IGMP_UP_MAX_FRAME   (IGMP_MAX_PAYLOAD + 4)

/* The tags in front of a frame's EtherType, as the classifier reads them: an
 * S-tag (TPID 0x88a8) outermost if there is one, then a C-tag (0x8100). A
 * second C-tag behind a first is payload to it, as it is here. */
struct igmp_up_tags {
	int      stag, ctag;
	uint16_t stci, ctci;            /* priority 15:13, DEI 12, VID 11:0 */
	uint16_t ethertype;             /* what follows the tags */
	uint32_t hdr_len;               /* 12 + 4 a tag + 2: where the payload starts */
};

/* Reads the tags. 0 when the frame is too short to have what it says. */
int igmp_up_read_tags(const uint8_t *f, uint32_t len, struct igmp_up_tags *t);

/* What to do with one taken message. */
enum igmp_up_verdict {
	IGMP_UP_SEND = 0,
	IGMP_UP_WITHHELD,       /* every join refused by the lists (G.988 attr 15 = 0) */
	IGMP_UP_QUERY,          /* a query from the host side */
	IGMP_UP_RATE,           /* over the profile's rate */
};

/* The rate window: messages sent in the current second. */
struct igmp_up_rate {
	uint32_t sec;
	uint32_t sent;
	int      started;
};

/* Decide, given the message's type (0 for one that did not parse), whether
 * the group table refused every join it carried by the lists (`all_refused`:
 * igmp_db_rx() leaves the counts in the db, group.h), the profile and the
 * clock. A message
 * that is sent counts against the rate; one that is not, does not. */
enum igmp_up_verdict igmp_up_decide(const struct igmp_policy *p, uint8_t type,
				    int all_refused, struct igmp_up_rate *rate,
				    uint32_t now);

/* The frame with the profile's tag, tag control 1 to 3, padded to the
 * minimum frame: its length, or 0 when it does not fit `max` or the control
 * is not one of those. `vid` and `pri` say what the outer tag became, for
 * finding the flow that carries it. */
uint32_t igmp_up_retag(const uint8_t *in, uint32_t len, uint32_t control,
		       uint16_t tci, uint8_t *out, uint32_t max,
		       uint16_t *vid, uint8_t *pri);

/* The frame as the classifier row treats it (the kernel's answer to a FRAME
 * route, odi_sw_ioctl.h), padded to the minimum frame: its length, or 0 when
 * it does not fit or the treatment is one the switch is not known to do (an
 * S-tag rewritten into a C-tag). */
uint32_t igmp_up_treat(const uint8_t *in, uint32_t len,
		       const struct odi_sw_mc_route *r, uint8_t *out, uint32_t max);

/* What a verdict that is not IGMP_UP_SEND means, for the log. */
const char *igmp_up_verdict_str(enum igmp_up_verdict v);

#endif
