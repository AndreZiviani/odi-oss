/* Scripted scenario: rx power has drifted down to about -28 dBm. A real
 * SFF-8472 module would assert both its low-power alarm and warning at
 * that level (see odi_ddm.c/odi_ddm.h for the byte layout, and
 * test/odi_optics_model.h's header comment for why this states the flag
 * bits directly rather than deriving them from thresholds this repo does
 * not model). Everything else -- vendor identity, the other four DDM
 * fields, gpon, mib, l2 -- is the same as hw_fake.c's baseline scenario.
 */
#include <stdint.h>
#include "hw.h"
#include "gpon_status.h"

#define FAKE_PORTS 4

/* raw 16: 10*log10(16/10000) = -27.9588 dBm, about -28 dBm. */
static const uint16_t ddm_word[DDM_SEL_COUNT] = {
	[DDM_TEMPERATURE]  = 10844,
	[DDM_VOLTAGE]      = 31703,
	[DDM_BIAS_CURRENT] = 7175,
	[DDM_TX_POWER]     = 16933,
	[DDM_RX_POWER]     = 16,
};

static void put_text(uint8_t *out, const char *s)
{
	int i = 0;

	for (; s[i] && i < DDM_RAW_LEN; i++)
		out[i] = (uint8_t)s[i];
	for (; i < 16; i++)
		out[i] = ' ';
}

int __wrap_hw_transceiver_get(int sel, uint8_t out[DDM_RAW_LEN]);
int __wrap_hw_transceiver_get(int sel, uint8_t out[DDM_RAW_LEN])
{
	for (int i = 0; i < DDM_RAW_LEN; i++)
		out[i] = 0;
	if (sel == DDM_VENDOR_NAME) {
		put_text(out, "ODI");
		return 0;
	}
	if (sel == DDM_PART_NUMBER) {
		put_text(out, "DFP-34X-2C2");
		return 0;
	}
	if (sel < 0 || sel >= DDM_SEL_COUNT)
		return -1;
	out[0] = (uint8_t)(ddm_word[sel] >> 8);
	out[1] = (uint8_t)ddm_word[sel];
	return 0;
}

/* Rx Power low alarm and warning (bit 0x0040 -- see
 * commands.c's cmd_transceiver_alarms() field table), nothing else, no
 * LOS: a low but still-present signal, not a lost one. */
int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los);
int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los)
{
	*alarms = 0x0040;
	*warnings = 0x0040;
	*los = 0;
	return 0;
}

#include "hw_fake_common.inc"
