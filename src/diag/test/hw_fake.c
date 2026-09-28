/* Fixed hardware answers for the exporter contract test -- the baseline
 * scenario: a healthy module, no alarm, no warning, no LOS.
 *
 * Linked into build/diag_fake with -Wl,--wrap for each accessor the
 * exporter's commands reach, so every call from the command handlers lands
 * here instead of on /dev/odi_sw or /proc/odi_gpon, which qemu-user does not
 * have. The real parser, handlers, formatting, prompt and echo all run
 * unchanged; only the values are fixed, so the output can be compared byte
 * for byte against test/exporter.golden.
 *
 * The values are shaped like a real stick's: the DDM words read off isp1,
 * state O5, LOF asserted so both alarm words appear, and four ports whose
 * driver refuses the counters it has no register for.
 *
 * The three scripted optics scenarios (rx power drifting low, LOS asserted,
 * module absent) are their own fixture files -- hw_fake_rx_drift.c,
 * hw_fake_los.c, hw_fake_absent.c -- differing only in the transceiver and
 * alarm-status wraps below; everything past that point is shared, in
 * hw_fake_common.inc.
 */
#include <stdint.h>
#include "hw.h"
#include "gpon_status.h"

/* Ports 0-3 answer, as on the board. */
#define FAKE_PORTS 4

/* Raw DDMI words, big-endian, as the ioctl returns them. */
static const uint16_t ddm_word[DDM_SEL_COUNT] = {
	[DDM_TEMPERATURE]  = 10844,    /* 42.359375 C */
	[DDM_VOLTAGE]      = 31703,    /* 3.170300 V */
	[DDM_BIAS_CURRENT] = 7175,     /* 14.350000 mA */
	[DDM_TX_POWER]     = 16933,    /* 2.288 dBm */
	[DDM_RX_POWER]     = 51,       /* -22.924 dBm */
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

/* All clear: no alarm, no warning, no LOS. */
int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los);
int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los)
{
	*alarms = 0;
	*warnings = 0;
	*los = 0;
	return 0;
}

#include "hw_fake_common.inc"
