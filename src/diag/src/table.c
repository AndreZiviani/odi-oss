#include "table.h"
#include "hw.h"

/* The order here is the order `?` lists them in. The first five transceiver
 * rows, onu-state, alarm-status and the MIB dump are what the Prometheus
 * exporter sends; their syntax and output are a contract (README.md). */
const struct cmd_def cmd_table[] = {
	{ "pon get transceiver vendor-name",
	  "module vendor name (SFF-8472 A0h)", CMD_TRANSCEIVER, DDM_VENDOR_NAME },
	{ "pon get transceiver part-number",
	  "module part number (SFF-8472 A0h)", CMD_TRANSCEIVER, DDM_PART_NUMBER },
	{ "pon get transceiver temperature",
	  "module temperature, C", CMD_TRANSCEIVER, DDM_TEMPERATURE },
	{ "pon get transceiver voltage",
	  "module supply voltage, V", CMD_TRANSCEIVER, DDM_VOLTAGE },
	{ "pon get transceiver bias-current",
	  "laser bias current, mA", CMD_TRANSCEIVER, DDM_BIAS_CURRENT },
	{ "pon get transceiver tx-power",
	  "optical transmit power, dBm", CMD_TRANSCEIVER, DDM_TX_POWER },
	{ "pon get transceiver rx-power",
	  "optical receive power, dBm", CMD_TRANSCEIVER, DDM_RX_POWER },
	{ "pon get transceiver all",
	  "all seven transceiver readings above", CMD_TRANSCEIVER_ALL, 0 },
	{ "gpon get onu-state",
	  "GPON state machine state, O1-O7", CMD_ONU_STATE, 0 },
	{ "gpon get alarm-status",
	  "LOS, LOF and LOM, live from the GPON block", CMD_ALARM_STATUS, 0 },
	{ "gpon get flows",
	  "GEM flows omcid programmed, as odi_switch recorded them",
	  CMD_GPON_FLOWS, 0 },
	{ "mib dump counter port <ports>",
	  "switch port MIB counters; all = every port that answers",
	  CMD_MIB_DUMP, 0 },
	{ "register get <address> <words>",
	  "read switch-core registers, four per line", CMD_REG_GET, 0 },
	{ "register set <address> <value>",
	  "write one switch-core register", CMD_REG_SET, 0 },
	{ "help", "list the commands", CMD_HELP, 0 },
	{ "exit", "leave the shell", CMD_EXIT, 0 },
};

const int cmd_table_len = (int)(sizeof cmd_table / sizeof cmd_table[0]);
