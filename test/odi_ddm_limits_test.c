/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_ddm_limits_test.c -- the DDM selectors added for the module's own
 * description of its readings (odi_ddm.h: ODI_DDM_MONITOR_TYPE,
 * ODI_DDM_LIMITS_A, ODI_DDM_LIMITS_B), against the scriptable module model
 * (odi_optics_model.h).
 *
 * The module pages below are the A0h bytes 92-94 and A2h bytes 0-39 of one
 * module's EEPROM as read over I2C (a reporter's diagnostics bundle, one
 * stick): a working module, internally calibrated, with its own alarm and
 * warning limits. They are one module, not "the" module: the point of
 * reading them is that they differ.
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

#include "odi_optics_model.h"

#include "../src/diag/src/ddm.h"
#include "../src/diag/src/ddm.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static const uint8_t limits_a2[40] = {
	0x64, 0x00, 0xd8, 0x00, 0x5a, 0x00, 0xec, 0x00,	/* temperature */
	0x8c, 0xa0, 0x75, 0x30, 0x88, 0xb8, 0x79, 0x18,	/* voltage */
	0xc3, 0x50, 0x01, 0xf4, 0xaf, 0xc8, 0x01, 0xf4,	/* bias current */
	0x9b, 0x82, 0x22, 0xd0, 0x8a, 0x99, 0x27, 0x10,	/* Tx power */
	0x07, 0xcb, 0x00, 0x0a, 0x06, 0x31, 0x00, 0x0d,	/* Rx power */
};

static void route(void)
{
	odi_reg_write(ODI_SW_PIN_GPIO_SELECT(0), ODI_OPTICS_GPIO_SELECT_VALUE);
}

static void fmt_word(int type, const uint8_t *w, char *out, int max)
{
	uint8_t raw[ODI_DDM_BUF_LEN] = { w[0], w[1] };

	ddm_format(type, raw, out, max);
}

int main(void)
{
	uint8_t raw[ODI_DDM_BUF_LEN];
	char got[64];

	(void)odi_optics_set_alarms;	/* the model's other setters are used by the other model test */
	(void)odi_optics_set_a2_word;

	puts("odi_ddm: monitoring type and module limits:");

	odi_mock_reset();
	odi_optics_model_reset();
	route();
	memcpy(odi_optics.a2, limits_a2, sizeof limits_a2);
	odi_optics.a0[92] = 0x68;	/* DDM, internally calibrated, average power */
	odi_optics.a0[93] = 0xb0;
	odi_optics.a0[94] = 0x03;

	CHECK(odi_ddm_get(ODI_DDM_MONITOR_TYPE, raw) == 0, "monitor type read");
	CHECK(raw[0] == 0x68 && raw[1] == 0xb0 && raw[2] == 0x03 && raw[3] == 0,
	      "A0h 92-94 in out[0..2], the rest zeroed");

	CHECK(odi_ddm_get(ODI_DDM_LIMITS_A, raw) == 0, "limits A read");
	CHECK(memcmp(raw, limits_a2, 24) == 0, "A2h 0-23 in out[0..23]");
	CHECK(odi_ddm_get(ODI_DDM_LIMITS_B, raw) == 0, "limits B read");
	CHECK(memcmp(raw, limits_a2 + 24, 16) == 0 && raw[16] == 0, "A2h 24-39 in out[0..15], the rest zeroed");

	/* The limits go through the same formatter as the readings. */
	fmt_word(DDM_TEMPERATURE, limits_a2 + 0, got, sizeof got);
	CHECK(strcmp(got, "100.000000 C") == 0, "temperature high alarm 100 C");
	fmt_word(DDM_TEMPERATURE, limits_a2 + 2, got, sizeof got);
	CHECK(strcmp(got, "-40.000000 C") == 0, "temperature low alarm -40 C");
	fmt_word(DDM_VOLTAGE, limits_a2 + 10, got, sizeof got);
	CHECK(strcmp(got, "3.000000 V") == 0, "voltage low alarm 3.0 V");
	fmt_word(DDM_BIAS_CURRENT, limits_a2 + 16, got, sizeof got);
	CHECK(strcmp(got, "100.000000 mA") == 0, "bias high alarm 100 mA");
	fmt_word(DDM_RX_POWER, limits_a2 + 34, got, sizeof got);
	CHECK(fabs(atof(got) - -30.0) < 1e-4, "Rx power low alarm -30 dBm");
	fmt_word(DDM_RX_POWER, limits_a2 + 38, got, sizeof got);
	CHECK(fabs(atof(got) - 10 * log10(13 / 10000.0)) < 1e-4, "Rx power low warning -28.86 dBm");

	/* Absent module: the new selectors fail like the others. */
	odi_optics_set_present(0);
	CHECK(odi_ddm_get(ODI_DDM_LIMITS_A, raw) == -ENXIO, "limits NACK with no module");
	CHECK(odi_ddm_get(ODI_DDM_MONITOR_TYPE, raw) == -ENXIO, "monitor type NACK with no module");
	odi_optics_set_present(1);

	CHECK(odi_mock_locks_idle(), "every lock released across every scenario");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
