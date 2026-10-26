/* The command table: every command diag accepts, written out by hand.
 *
 * A row is a syntax line and a one-line description. A syntax word is either
 * a keyword, matched on any unambiguous prefix, or a parameter in angle
 * brackets:
 *
 *   <ports>    a port list: "all", "2", "0-3", "0,2-3"
 *   <...>      anything else is an unsigned 32-bit number, decimal or 0x hex
 *
 * `id` says which handler runs (commands.c), `sel` is handed to it, so one
 * handler can serve several rows -- the seven transceiver readings are seven
 * rows and one handler.
 *
 * Kept free of handler code so the parser and its tests build on the host.
 */
#ifndef ODI_TABLE_H
#define ODI_TABLE_H

#include <stdint.h>

enum cmd_id {
	CMD_HELP,
	CMD_EXIT,
	CMD_TRANSCEIVER,        /* sel: enum ddm_sel */
	CMD_TRANSCEIVER_ALL,
	CMD_ONU_STATE,
	CMD_ALARM_STATUS,
	CMD_TRANSCEIVER_ALARMS,
	CMD_TRANSCEIVER_MONITOR,
	CMD_TRANSCEIVER_LIMITS,
	CMD_GPON_FLOWS,
	CMD_GPON_FLOW_COUNTERS,
	CMD_MIB_DUMP,
	CMD_REG_GET,
	CMD_REG_SET,
	CMD_L2_ALL,
	CMD_L2_INDEX,
	CMD_PROBE_SET,          /* sel: PROBE_SEL_* below */
	CMD_PROBE_SHOW,
	CMD_PROBE_DELETE,
	CMD_PROBE_DELETE_ALL,
	CMD_MCC_SET,            /* sel: MCC_SEL_* below */
	CMD_MCC_SHOW,
	CMD_MCC_DELETE,
	CMD_MCC_DELETE_ALL,
};

/* The variants of `acl-probe`: the action, the counter unit and the match. */
#define PROBE_SEL_COUNT    0x01u
#define PROBE_SEL_MIRROR   0x02u
#define PROBE_SEL_TRAP     0x03u
#define PROBE_SEL_ACTION   0x03u
#define PROBE_SEL_BYTES    0x04u
#define PROBE_SEL_GROUP    0x08u   /* <group> <length>, else proto <proto> */

/* The variants of `mc-count`: the port, the unit and the match. */
#define MCC_SEL_PON        0x01u   /* else the UNI */
#define MCC_SEL_BYTES      0x02u   /* else frames */
#define MCC_SEL_IPV4       0x04u   /* <address> <length>, else mac <high> <low> <length> */

struct cmd_def {
	const char *syntax;
	const char *help;
	uint8_t id;
	uint8_t sel;
};

extern const struct cmd_def cmd_table[];
extern const int cmd_table_len;

#endif
