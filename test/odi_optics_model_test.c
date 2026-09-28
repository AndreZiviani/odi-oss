/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_optics_model_test.c -- odi_i2c_read_bytes()/odi_ddm_get() against the
 * scriptable SFF-8472 model (odi_optics_model.h), covering what the fixed
 * per-field tables in odi_i2c_test.c/odi_ddm_test.c cannot: the IO_GPIO_EN
 * routing gate, an absent module, and the alarm/warning/LOS selector
 * (odi_ddm.h's ODI_DDM_ALARM_STATUS) driven by a real I2C transaction
 * rather than a fixed byte table.
 */
#include <math.h>
#include <stdio.h>
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

/* odi_board_optics()'s own write, restated (see odi_optics_model.h): the
 * one register write that hands the port-1 I2C pins to the optics block. */
static void route_gpio_to_optics(void)
{
	odi_reg_write(ODI_SW_PIN_GPIO_SELECT(0), ODI_OPTICS_GPIO_SELECT_VALUE);
}

int main(void)
{
	uint8_t raw[ODI_DDM_BUF_LEN];

	puts("optics model: IO_GPIO_EN gate, absent module, alarm/warning/LOS:");

	/* 1. Before the GPIO route is set, every read NACKs -- looks exactly
	 * like a dead or missing module, the KB note's own words. */
	odi_mock_reset();
	odi_optics_model_reset();
	odi_optics_set_a2_word(96, 10844); /* 42.359375 C, unused once gated off */
	CHECK(odi_ddm_get(ODI_DDM_TEMPERATURE, raw) == -ENXIO,
	      "NACK before IO_GPIO_EN is routed to the optics block");

	/* 2. The wrong value routes the pins elsewhere, not to I2C: still NACK. */
	odi_reg_write(ODI_SW_PIN_GPIO_SELECT(0), 0x00000000U);
	CHECK(odi_ddm_get(ODI_DDM_TEMPERATURE, raw) == -ENXIO,
	      "NACK with PIN_GPIO_SELECT set to something else");

	/* 3. The real boot write (odi_board_optics()): reads succeed. */
	route_gpio_to_optics();
	{
		char got[64];
		int rc = odi_ddm_get(ODI_DDM_TEMPERATURE, raw);

		CHECK(rc == 0, "temperature read succeeds once IO_GPIO_EN is routed");
		ddm_format(DDM_TEMPERATURE, raw, got, sizeof got);
		CHECK(strcmp(got, "42.359375 C") == 0,
		      "modelled temperature matches what was scripted");
	}

	/* 4. Module physically absent: NACK regardless of routing. */
	odi_optics_set_present(0);
	CHECK(odi_ddm_get(ODI_DDM_TEMPERATURE, raw) == -ENXIO,
	      "NACK when the modelled transceiver is absent, even routed");
	odi_optics_set_present(1);
	CHECK(odi_ddm_get(ODI_DDM_TEMPERATURE, raw) == 0,
	      "reads resume once the module is present again");

	/* 5. Rx power drifting down to about -28 dBm: the module's own
	 * firmware would assert both the alarm and warning low-power bits at
	 * that level (see odi_optics_model.h's header comment on why this
	 * test states them rather than deriving them). */
	{
		uint16_t rx_raw = 16; /* 10*log10(16/10000) = -27.96 dBm */
		char got[64];

		odi_optics_set_a2_word(104, rx_raw);
		odi_optics_set_alarms(0x0040 /* Rx Power low alarm */,
				       0x0040 /* Rx Power low warning */, 0);

		CHECK(odi_ddm_get(ODI_DDM_RX_POWER, raw) == 0, "rx-power read succeeds");
		ddm_format(DDM_RX_POWER, raw, got, sizeof got);
		{
			double parsed = atof(got);
			double want = 10 * log10(rx_raw / 10000.0);

			CHECK(fabs(parsed - want) < 2e-5 && strstr(got, "dBm") != NULL,
			      "rx-power prints about -28 dBm");
		}

		CHECK(odi_ddm_get(ODI_DDM_ALARM_STATUS, raw) == 0,
		      "alarm-status read succeeds");
		CHECK(((raw[2] << 8) | raw[3]) == 0x0040,
		      "Rx Power low alarm bit set, nothing else");
		CHECK(((raw[6] << 8) | raw[7]) == 0x0040,
		      "Rx Power low warning bit set, nothing else");
		CHECK((raw[0] & 0x02) == 0, "LOS not asserted in this scenario");
	}

	/* 6. LOS asserted, no alarms/warnings: a receiver with no signal at
	 * all is a distinct condition from a low-but-present signal. */
	{
		odi_optics_set_alarms(0, 0, 1);
		CHECK(odi_ddm_get(ODI_DDM_ALARM_STATUS, raw) == 0,
		      "alarm-status read succeeds with LOS asserted");
		CHECK((raw[0] & 0x02) != 0, "LOS bit set");
		CHECK(((raw[2] << 8) | raw[3]) == 0, "no alarm bits set alongside LOS");
	}

	CHECK(odi_mock_locks_idle(), "every lock released across every scenario");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
