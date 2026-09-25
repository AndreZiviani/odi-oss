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
	CMD_GPON_FLOWS,
	CMD_MIB_DUMP,
	CMD_REG_GET,
	CMD_REG_SET,
};

struct cmd_def {
	const char *syntax;
	const char *help;
	uint8_t id;
	uint8_t sel;
};

extern const struct cmd_def cmd_table[];
extern const int cmd_table_len;

#endif
