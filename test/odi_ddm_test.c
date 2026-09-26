/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_ddm_test.c -- host-side test that odi_ddm_get() (odi_ddm.c, over
 * odi_i2c.c) reproduces every one of the seven `pon get transceiver
 * <keyword>` outputs a capture on isp1 captured while the stock kernel
 * modules still owned I2C init, run through src/diag/src/ddm.c's own
 * ddm_format() unmodified --
 * the same formatter `diag` itself calls, so this is "does the new data
 * path make diag print what it printed before", not a reimplementation
 * of the SFF-8472 math (src/diag/test/ddm_test.c already covers that).
 *
 * The mock table below is every (device select, byte address) -> byte
 * value the capture shows, for all seven brackets: vendor-name (A0h,
 * bytes 20-35), part-number (A0h, bytes 40-55), and the five two-byte
 * A2h numeric fields (temperature 96-97, voltage 98-99, bias-current
 * 100-101, tx-power 102-103, rx-power 104-105). odi_i2c_mock_byte() below
 * is odi_i2c.c's host-build hook (see odi_i2c.c's own header comment);
 * every entry in this table was read back off the capture's own
 * I2C_BYTE_ADDR (write) / I2C_READ_DATA (read) pairs, not invented.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_ddm.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_ddm.c"

#include "../src/diag/src/ddm.h"
#include "../src/diag/src/ddm.c"

static int failures;

struct mock_entry { uint32_t sel; uint32_t addr; uint8_t val; };

/* == ddm vendor-name -- "Vendor Name: ODI" */
static const struct mock_entry m_vendor_name[] = {
	{ ODI_I2C_SEL_A0, 20, 79 }, { ODI_I2C_SEL_A0, 21, 68 }, { ODI_I2C_SEL_A0, 22, 73 },
	{ ODI_I2C_SEL_A0, 23, 32 }, { ODI_I2C_SEL_A0, 24, 32 }, { ODI_I2C_SEL_A0, 25, 32 },
	{ ODI_I2C_SEL_A0, 26, 32 }, { ODI_I2C_SEL_A0, 27, 32 }, { ODI_I2C_SEL_A0, 28, 32 },
	{ ODI_I2C_SEL_A0, 29, 32 }, { ODI_I2C_SEL_A0, 30, 32 }, { ODI_I2C_SEL_A0, 31, 32 },
	{ ODI_I2C_SEL_A0, 32, 32 }, { ODI_I2C_SEL_A0, 33, 32 }, { ODI_I2C_SEL_A0, 34, 32 },
	{ ODI_I2C_SEL_A0, 35, 32 },
};

/* == ddm part-number -- "Part Number: DFP-34X-2C2" */
static const struct mock_entry m_part_number[] = {
	{ ODI_I2C_SEL_A0, 40, 68 }, { ODI_I2C_SEL_A0, 41, 70 }, { ODI_I2C_SEL_A0, 42, 80 },
	{ ODI_I2C_SEL_A0, 43, 45 }, { ODI_I2C_SEL_A0, 44, 51 }, { ODI_I2C_SEL_A0, 45, 52 },
	{ ODI_I2C_SEL_A0, 46, 88 }, { ODI_I2C_SEL_A0, 47, 45 }, { ODI_I2C_SEL_A0, 48, 50 },
	{ ODI_I2C_SEL_A0, 49, 67 }, { ODI_I2C_SEL_A0, 50, 50 }, { ODI_I2C_SEL_A0, 51, 0 },
	{ ODI_I2C_SEL_A0, 52, 0 },  { ODI_I2C_SEL_A0, 53, 0 },  { ODI_I2C_SEL_A0, 54, 0 },
	{ ODI_I2C_SEL_A0, 55, 32 },
};

/* == ddm temperature -- "Temperature: 38.292969 C" */
static const struct mock_entry m_temperature[] = {
	{ ODI_I2C_SEL_A2, 96, 38 }, { ODI_I2C_SEL_A2, 97, 75 },
};

/* == ddm voltage -- "Voltage: 3.178100 V" */
static const struct mock_entry m_voltage[] = {
	{ ODI_I2C_SEL_A2, 98, 124 }, { ODI_I2C_SEL_A2, 99, 37 },
};

/* == ddm bias-current -- "Bias Current: 13.650000 mA" */
static const struct mock_entry m_bias_current[] = {
	{ ODI_I2C_SEL_A2, 100, 26 }, { ODI_I2C_SEL_A2, 101, 169 },
};

/* == ddm tx-power -- "Tx Power: 2.394997  dBm" */
static const struct mock_entry m_tx_power[] = {
	{ ODI_I2C_SEL_A2, 102, 67 }, { ODI_I2C_SEL_A2, 103, 206 },
};

/* == ddm rx-power -- "Rx Power: -23.010300  dBm" */
static const struct mock_entry m_rx_power[] = {
	{ ODI_I2C_SEL_A2, 104, 0 }, { ODI_I2C_SEL_A2, 105, 50 },
};

static const struct mock_entry *mock_table;
static unsigned int mock_table_n;

uint8_t odi_i2c_mock_byte(uint32_t sel, uint32_t addr)
{
	unsigned int i;

	for (i = 0; i < mock_table_n; i++) {
		if (mock_table[i].sel == sel && mock_table[i].addr == addr)
			return mock_table[i].val;
	}
	fprintf(stderr, "odi_ddm_test: no mock byte for sel=0x%08x addr=%u\n",
		sel, addr);
	abort();
}

#define USE_TABLE(t) do { mock_table = (t); mock_table_n = sizeof(t) / sizeof((t)[0]); } while (0)

static void check_str(int type, const struct mock_entry *table, unsigned int n,
		      const char *expect, const char *label)
{
	uint8_t raw[24];
	char got[64];

	mock_table = table;
	mock_table_n = n;
	odi_mock_reset();

	if (odi_ddm_get(type, raw) != 0) {
		printf("  %-14s odi_ddm_get FAILED\n", label);
		failures++;
		return;
	}
	ddm_format(type, raw, got, sizeof got);
	printf("  %-14s -> %-16s (expect %s)  %s\n", label, got, expect,
	       strcmp(got, expect) == 0 ? "ok" : "FAIL");
	if (strcmp(got, expect) != 0)
		failures++;
}

static void check_numeric(int type, const struct mock_entry *table, unsigned int n,
			  double expect, const char *unit, const char *label)
{
	uint8_t raw[24];
	char got[64];
	double parsed, diff;
	const char *p;
	int ok;

	mock_table = table;
	mock_table_n = n;
	odi_mock_reset();

	if (odi_ddm_get(type, raw) != 0) {
		printf("  %-14s odi_ddm_get FAILED\n", label);
		failures++;
		return;
	}
	ddm_format(type, raw, got, sizeof got);
	parsed = atof(got);
	p = strstr(got, unit);
	diff = fabs(parsed - expect);
	ok = p && diff < 2e-5;
	printf("  %-14s -> %-16s (expect %.6f%s)  %s\n", label, got, expect, unit,
	       ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

int main(void)
{
	puts("odi_ddm_get() -> ddm_format(), against the isp1 g4 DDM capture:");

	check_str(ODI_DDM_VENDOR_NAME, m_vendor_name,
		  sizeof m_vendor_name / sizeof m_vendor_name[0], "ODI", "vendor-name");
	check_str(ODI_DDM_PART_NUMBER, m_part_number,
		  sizeof m_part_number / sizeof m_part_number[0], "DFP-34X-2C2", "part-number");

	check_numeric(ODI_DDM_TEMPERATURE, m_temperature,
		     sizeof m_temperature / sizeof m_temperature[0],
		     (double)(short)0x264b / 256.0, " C", "temperature");
	check_numeric(ODI_DDM_VOLTAGE, m_voltage,
		     sizeof m_voltage / sizeof m_voltage[0],
		     0x7c25 / 10000.0, " V", "voltage");
	check_numeric(ODI_DDM_BIAS_CURRENT, m_bias_current,
		     sizeof m_bias_current / sizeof m_bias_current[0],
		     0x1aa9 * 2 / 1000.0, " mA", "bias-current");
	check_numeric(ODI_DDM_TX_POWER, m_tx_power,
		     sizeof m_tx_power / sizeof m_tx_power[0],
		     10 * log10(0x43ce / 10000.0), " dBm", "tx-power");
	check_numeric(ODI_DDM_RX_POWER, m_rx_power,
		     sizeof m_rx_power / sizeof m_rx_power[0],
		     10 * log10(0x0032 / 10000.0), " dBm", "rx-power");

	/* An unsupported selector (the module serial number, ODI_DDM_* has
	 * no entry for it -- selector 7) is refused
	 * rather than answered with garbage.
	 */
	{
		uint8_t raw[24];

		mock_table = NULL;
		mock_table_n = 0;
		if (odi_ddm_get(7, raw) == 0) {
			printf("  %-14s expected refusal, got success  FAIL\n", "sn (7)");
			failures++;
		} else {
			printf("  %-14s refused, as expected  ok\n", "sn (7)");
		}
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
