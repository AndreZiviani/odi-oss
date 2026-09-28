/* Scripted scenario: optical RX_LOS asserted (SFF-8472 A2h byte 110, bit 1)
 * -- no signal at all, as distinct from hw_fake_rx_drift.c's low-but-present
 * signal. The five numeric DDM fields still answer (a real module keeps
 * reporting its last sample, or zero, while LOS is up; the exact value does
 * not matter here, so the baseline's own values are reused), and every
 * other accessor is hw_fake.c's baseline scenario.
 */
#include <stdint.h>
#include "hw.h"
#include "gpon_status.h"

#define FAKE_PORTS 4

static const uint16_t ddm_word[DDM_SEL_COUNT] = {
	[DDM_TEMPERATURE]  = 10844,
	[DDM_VOLTAGE]      = 31703,
	[DDM_BIAS_CURRENT] = 7175,
	[DDM_TX_POWER]     = 16933,
	[DDM_RX_POWER]     = 51,
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

int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los);
int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los)
{
	*alarms = 0;
	*warnings = 0;
	*los = 1;
	return 0;
}

#include "hw_fake_common.inc"
