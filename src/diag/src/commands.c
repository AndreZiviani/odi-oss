#include "commands.h"
#include "io.h"
#include "hw.h"
#include "ddm.h"
#include "gpon_status.h"
#include "mib.h"
#include "omci_flows.h"

/* Width of the syntax column in the help listing: the longest syntax line
 * plus two spaces. */
#define HELP_SYNTAX_WIDTH 34

static int quit_requested;

/* ------------------------------------------------------------ transceiver */

/* Labels, by enum ddm_sel. Part of the exporter contract: it matches the
 * text after the label as a number. */
static const char *const ddm_label[DDM_SEL_COUNT] = {
	[DDM_VENDOR_NAME]  = "Vendor Name: ",
	[DDM_PART_NUMBER]  = "Part Number: ",
	[DDM_TEMPERATURE]  = "Temperature: ",
	[DDM_VOLTAGE]      = "Voltage: ",
	[DDM_BIAS_CURRENT] = "Bias Current: ",
	[DDM_TX_POWER]     = "Tx Power: ",
	[DDM_RX_POWER]     = "Rx Power: ",
};

static int transceiver_one(int sel)
{
	uint8_t raw[DDM_RAW_LEN];
	char value[64];

	if (hw_transceiver_get(sel, raw) != 0) {
		out("% transceiver read failed\n");
		return 1;
	}
	ddm_format(sel, raw, value, sizeof value);
	out_fmt("%s%s\n", ddm_label[sel], value);
	return 0;
}

static int cmd_transceiver_all(void)
{
	int rc = 0;

	for (int sel = 0; sel < DDM_SEL_COUNT; sel++)
		rc |= transceiver_one(sel);
	return rc;
}

/* --------------------------------------------------------------- GPON */

/* The "(On)" names and the trailing " \n\r" -- in that order, not "\r\n" --
 * are the exporter contract: it reads the N out of "(ON)". */
static const char *const gpon_states[] = {
	"Unknown",
	"Initial State(O1)",
	"Standby State(O2)",
	"Serial Number State(O3)",
	"Ranging State(O4)",
	"Operation State(O5)",
	"POPUP State(O6)",
	"Emergency Stop State(O7)",
};

static int cmd_onu_state(void)
{
	uint32_t state = 0;

	if (hw_gpon_status_get(&state) != 0) {
		out("% onu-state read failed\n");
		return 1;
	}
	if (state >= sizeof gpon_states / sizeof gpon_states[0])
		state = 0;
	out_fmt("ONU state: %s \n\r", gpon_states[state]);
	return 0;
}

/* One "Alarm <name>, status: clear|occur" line per alarm odi_gpon tracks.
 * The exporter reads anything other than "clear" as asserted and a line it
 * never sees as no data, so the alarms odi_gpon cannot answer are named on
 * a note line instead of printed as clear -- and the note avoids the word
 * the exporter scans for. */
static int cmd_alarm_status(void)
{
	static const struct {
		const char *name;
		uint32_t bit;
	} alarm[] = {
		{ "LOS", GPON_ALARM_LOS },
		{ "LOF", GPON_ALARM_LOF },
		{ "LOM", GPON_ALARM_LOM },
	};
	uint32_t alarms = 0, known = 0;

	if (hw_gpon_alarms_get(&alarms, &known) != 0) {
		out("% alarm status read failed (/proc/odi_gpon and /dev/odi_sw)\n");
		return 1;
	}
	for (unsigned i = 0; i < sizeof alarm / sizeof alarm[0]; i++)
		if (known & alarm[i].bit)
			out_fmt("Alarm %s, status: %s\n", alarm[i].name,
				(alarms & alarm[i].bit) ? "occur" : "clear");
	out("% SF, SD, TX Too Long, TX Mismatch: not tracked by odi_gpon\n");
	return 0;
}

/* What odi_switch recorded when omcid programmed each flow (cmd 25): the
 * software record, not a read of the hardware tables. The traffic type is
 * the byte written at creation, so a later AES enable does not show, and a
 * row programmed by anything else is invisible. */
static int cmd_gpon_flows(void)
{
	struct omci_flows f;
	uint32_t nds, nus;
	int rc = hw_gpon_flows_get(&f);

	if (rc == 2) {
		out("% odi_switch on this kernel has no flow readback (EOPNOTSUPP)\n");
		return 1;
	}
	if (rc != 0) {
		out("% no answer on the odi_omci netlink command path\n");
		return 1;
	}
	nds = f.ds_count < OMCI_FLOWS_MAX ? f.ds_count : OMCI_FLOWS_MAX;
	nus = f.us_count < OMCI_FLOWS_MAX ? f.us_count : OMCI_FLOWS_MAX;
	out_fmt("odi_switch record (what omcid programmed; not a hardware read): "
		"%d ds, %d us\n", (long)f.ds_count, (long)f.us_count);
	for (uint32_t i = 0; i < nds; i++)
		out_fmt("ds %d gem %d traffic 0x%x%s\n", (long)i, (long)f.ds_gem[i],
			(unsigned long)f.ds_cfg[i],
			f.ds_gem[i] == 0xfff ? " (omci)" : "");
	for (uint32_t i = 0; i < nus; i++)
		out_fmt("us %d gem %d\n", (long)i, (long)f.us_gem[i]);
	if (f.ds_count > nds || f.us_count > nus)
		out_fmt("(only the first %d of each are kept)\n", (long)OMCI_FLOWS_MAX);
	return 0;
}

/* ---------------------------------------------------------------- MIB */

/* A heading per port, then one "%-35s: %25llu" line per counter the driver
 * answers; a counter it has no register for is left out. Counter 0 decides
 * whether a port exists at all, so "all" prints only the ports that answer
 * and costs one read per absent port. */
static int cmd_mib_dump(const struct port_list *ports)
{
	for (uint32_t port = ports->lo; port <= ports->hi; port++) {
		uint64_t probe;

		if (!ports_has(ports, port))
			continue;
		if (hw_stat_port_get(port, 0, &probe) != 0)
			continue;

		out_fmt("Port: %d\n", (long)port);
		for (uint32_t i = 0; i < MIB_COUNT; i++) {
			uint64_t v;

			if (hw_stat_port_get(port, i, &v) != 0)
				continue;
			out_str_pad(mib_names[i], 35);
			out(": ");
			out_u64_pad(v, 25);
			out_char('\n');
		}
		out_char('\n');
	}
	return 0;
}

/* ----------------------------------------------------------- registers */

/* One field: "0x" and eight hex digits, then a space. */
static void put_word(uint32_t v)
{
	out("0x");
	out_hex(v, 8);
	out_char(' ');
}

static int addr_ok(uint32_t addr)
{
	if (addr & 3) {
		out("% address must be a multiple of 4\n");
		return 0;
	}
	if (addr >= SWCORE_SIZE) {
		out("% address must be below 0x2000000\n");
		return 0;
	}
	return 1;
}

/* The start address, then the words, four to a line with the line's address
 * first. rcS and network.sh read the first two fields of each line. */
static int cmd_reg_get(uint32_t addr, uint32_t words)
{
	if (!addr_ok(addr))
		return 1;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t v;

		/* Checked every word, not once: a long count must stop at the
		 * end of the window rather than print a failure per word. */
		if (addr >= SWCORE_SIZE) {
			out("\n% address ran past the end of the window\n");
			return 1;
		}
		if (hw_addr_get(addr, &v) != 0) {
			out("\n% register read failed\n");
			return 1;
		}
		if ((i & 3) == 0) {
			out_char('\n');
			put_word(addr);
		}
		put_word(v);
		addr += 4;
	}
	out_char('\n');
	return 0;
}

static int cmd_reg_set(uint32_t addr, uint32_t value)
{
	if (!addr_ok(addr))
		return 1;
	if (hw_addr_set(addr, value) != 0) {
		out("% register write failed\n");
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------ dispatch */

void cmd_list(const struct cmd_def *const list[], int n)
{
	for (int i = 0; i < n; i++) {
		out("  ");
		out_str_pad(list[i]->syntax, HELP_SYNTAX_WIDTH);
		out(list[i]->help);
		out_char('\n');
	}
}

static int cmd_help(void)
{
	const struct cmd_def *all[CMD_LIST_MAX];
	int n = 0;

	for (int i = 0; i < cmd_table_len && n < CMD_LIST_MAX; i++)
		all[n++] = &cmd_table[i];
	cmd_list(all, n);
	out("Words may be shortened while unambiguous. A trailing ? lists the\n"
	    "commands that start with what was typed.\n");
	return 0;
}

int cmd_run(const struct parsed *p)
{
	switch (p->cmd->id) {
	case CMD_HELP:
		return cmd_help();
	case CMD_EXIT:
		quit_requested = 1;
		return 0;
	case CMD_TRANSCEIVER:
		return transceiver_one(p->cmd->sel);
	case CMD_TRANSCEIVER_ALL:
		return cmd_transceiver_all();
	case CMD_ONU_STATE:
		return cmd_onu_state();
	case CMD_ALARM_STATUS:
		return cmd_alarm_status();
	case CMD_GPON_FLOWS:
		return cmd_gpon_flows();
	case CMD_MIB_DUMP:
		return cmd_mib_dump(&p->ports);
	case CMD_REG_GET:
		return cmd_reg_get(p->u[0], p->u[1]);
	case CMD_REG_SET:
		return cmd_reg_set(p->u[0], p->u[1]);
	}
	return 1;
}

int cmd_should_quit(void)
{
	return quit_requested;
}
