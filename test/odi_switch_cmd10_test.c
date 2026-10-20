/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_cmd10_test.c -- cmd 10 (getTransceiverStatus), the DDM word
 * behind omcid's ANI-G Test result (src/omci/respond/omcimsg.c,
 * ddm_word()): for each numeric type, the A2h word comes back big-endian
 * at data[0..1] with the rest of data zeroed, through the device select,
 * byte address and start of each byte at that type's SFF-8472 offset;
 * an unknown type and a short buffer are refused without touching the
 * controller; a read that times out or is not acknowledged is an error;
 * and odi_i2c_lock is free again after every call. Host-side, against the
 * switch mock and the module pages of odi_switch_unity.h. Part of `make
 * test-host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

#define CMD10_LEN	36U	/* what omcid sends: {type, data[32]} */

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-60s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

/* One cmd 10 call, the way omcid builds it: the type a big-endian word at
 * bytes 0-3; data filled with 0xa5, so a byte the command did not write
 * shows.
 */
static int cmd10(uint32_t type, uint8_t *b, uint32_t len)
{
	memset(b, 0xa5, len);
	b[0] = (uint8_t)(type >> 24);
	b[1] = (uint8_t)(type >> 16);
	b[2] = (uint8_t)(type >> 8);
	b[3] = (uint8_t)type;
	return odi_switch_cmd(OMCI_TRANSCEIVER_STATUS_CMD, b, len);
}

/* The writes of a one-byte read of A2h byte `addr`, starting at log
 * entry i: device select, byte address, start.
 */
static int byte_read_at(unsigned int i, uint32_t addr)
{
	const struct odi_mock_write *l = odi_mock.log;

	return odi_mock.log_n >= i + 3 &&
	       l[i].kind == 'W' && l[i].addr == ODI_I2C_MASTER_SETUP && l[i].val == ODI_I2C_SEL_A2 &&
	       l[i + 1].kind == 'W' && l[i + 1].addr == ODI_I2C_BYTE_ADDR && l[i + 1].val == addr &&
	       l[i + 2].kind == 'W' && l[i + 2].addr == ODI_I2C_CMD && l[i + 2].val == ODI_I2C_CMD_START;
}

/* A start leaves the transaction running (with poll_stuck, forever) or
 * finished but not acknowledged.
 */
static void busy_hook(uint32_t off, uint32_t val)
{
	if (off == ODI_I2C_CMD && (val & ODI_I2C_CMD_START))
		odi_mock.regs[odi_mock_slot(ODI_I2C_CMD)] = val | ODI_I2C_CMD_IN_PROGRESS;
}

static void nack_hook(uint32_t off, uint32_t val)
{
	if (off == ODI_I2C_CMD && (val & ODI_I2C_CMD_START))
		odi_mock.regs[odi_mock_slot(ODI_I2C_CMD)] = val | ODI_I2C_CMD_NOT_ACKED;
}

int main(void)
{
	static const struct { uint32_t type; uint32_t off; uint16_t word; const char *name; } f[] = {
		{ ODI_DDM_TEMPERATURE,  96, 0x264b, "temperature" },
		{ ODI_DDM_VOLTAGE,      98, 0x7c25, "supply voltage" },
		{ ODI_DDM_BIAS_CURRENT, 100, 0x1aa9, "bias current" },
		{ ODI_DDM_TX_POWER,     102, 0x43ce, "tx power" },
		{ ODI_DDM_RX_POWER,     104, 0x0032, "rx power" },
	};
	static const uint32_t refused[] = {
		ODI_DDM_VENDOR_NAME, ODI_DDM_PART_NUMBER, ODI_DDM_MONITOR_TYPE, 99,
		0x0100U | ODI_DDM_VOLTAGE,	/* the type is all four bytes, not byte 3 alone */
	};
	uint8_t b[CMD10_LEN];
	char label[96];
	unsigned int i, j;
	int rc, rest_zero;

	odi_switch_cmd_reset_state();
	for (i = 0; i < sizeof f / sizeof f[0]; i++) {
		odi_unity_sfp_a2[f[i].off] = (uint8_t)(f[i].word >> 8);
		odi_unity_sfp_a2[f[i].off + 1] = (uint8_t)f[i].word;
	}

	printf("cmd 10, each numeric type:\n");
	for (i = 0; i < sizeof f / sizeof f[0]; i++) {
		odi_mock_reset();
		rc = cmd10(f[i].type, b, sizeof b);
		snprintf(label, sizeof label, "%s: rc 0, word 0x%04x at data[0..1]", f[i].name, f[i].word);
		ok(rc == 0 && b[4] == (uint8_t)(f[i].word >> 8) && b[5] == (uint8_t)f[i].word, label);
		for (rest_zero = 1, j = 6; j < sizeof b; j++)
			rest_zero &= b[j] == 0;
		snprintf(label, sizeof label, "%s: the rest of data zeroed", f[i].name);
		ok(rest_zero, label);
		snprintf(label, sizeof label, "%s: A2h bytes %u and %u read, six writes", f[i].name,
			 f[i].off, f[i].off + 1);
		ok(odi_mock.log_n == 6 && byte_read_at(0, f[i].off) && byte_read_at(3, f[i].off + 1), label);
		snprintf(label, sizeof label, "%s: odi_i2c_lock released", f[i].name);
		ok(odi_mock_locks_idle(), label);
	}

	/* The alarm and warning flags: A2h 110-117 whole, at data[0..7]. */
	printf("cmd 10, the module alarm flags:\n");
	for (i = 0; i < 8; i++)
		odi_unity_sfp_a2[110 + i] = (uint8_t)(0x10 + i);
	odi_mock_reset();
	rc = cmd10(ODI_DDM_ALARM_STATUS, b, sizeof b);
	for (rest_zero = 1, j = 0; j < 8; j++)
		rest_zero &= b[4 + j] == 0x10 + j;
	ok(rc == 0 && rest_zero, "alarm status: A2h bytes 110-117 at data[0..7]");
	for (rest_zero = 1, j = 12; j < sizeof b; j++)
		rest_zero &= b[j] == 0;
	ok(rest_zero && odi_mock_locks_idle(), "alarm status: the rest of data zeroed, lock released");
	odi_mock_reset();
	rc = cmd10(ODI_DDM_ALARM_STATUS, b, 11);
	ok(rc != 0 && odi_mock.log_n == 0, "alarm status: a buffer short of eight bytes: error, no write");

	printf("cmd 10, refused without touching the controller:\n");
	for (i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		odi_mock_reset();
		rc = cmd10(refused[i], b, sizeof b);
		snprintf(label, sizeof label, "type 0x%x: error, no register write, data untouched",
			 (unsigned int)refused[i]);
		ok(rc != 0 && odi_mock.log_n == 0 && b[4] == 0xa5 && b[5] == 0xa5, label);
	}
	odi_mock_reset();
	rc = cmd10(ODI_DDM_VOLTAGE, b, 5);
	ok(rc != 0 && odi_mock.log_n == 0, "a buffer with no room for the word: error, no write");
	odi_mock_reset();
	ok(odi_switch_cmd(OMCI_TRANSCEIVER_STATUS_CMD, NULL, CMD10_LEN) != 0 && odi_mock.log_n == 0,
	   "no buffer: error, no write");

	printf("cmd 10, a read that does not complete:\n");
	odi_mock_reset();
	odi_mock.poll_stuck = 1;
	odi_mock_write_hook = busy_hook;
	rc = cmd10(ODI_DDM_RX_POWER, b, sizeof b);
	odi_mock_write_hook = NULL;
	ok(rc != 0, "IN_PROGRESS never clears: error");
	ok(odi_mock_locks_idle(), "odi_i2c_lock released after the timeout");
	odi_mock_reset();
	odi_mock_write_hook = nack_hook;
	rc = cmd10(ODI_DDM_RX_POWER, b, sizeof b);
	odi_mock_write_hook = NULL;
	ok(rc != 0, "the module does not acknowledge: error");
	ok(odi_mock_locks_idle(), "odi_i2c_lock released after the nack");

	if (failures) {
		printf("odi_switch_cmd10_test: %d FAILED\n", failures);
		return 1;
	}
	printf("odi_switch_cmd10_test: ok\n");
	return 0;
}
