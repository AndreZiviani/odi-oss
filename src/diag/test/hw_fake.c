/* Fixed hardware answers for the exporter contract test.
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

int __wrap_hw_gpon_status_get(uint32_t *state);
int __wrap_hw_gpon_status_get(uint32_t *state)
{
	*state = 5;
	return 0;
}

int __wrap_hw_gpon_alarms_get(uint32_t *alarms, uint32_t *known);
int __wrap_hw_gpon_alarms_get(uint32_t *alarms, uint32_t *known)
{
	*alarms = GPON_ALARM_LOF;
	*known = GPON_ALARM_KNOWN;
	return 0;
}

/* The counters the driver has no register for, by mib.h index: the real
 * odi_switch refuses about a third of them, and a refused counter must be
 * left out of the dump, not printed as zero. */
static int refused(uint32_t counter)
{
	return counter % 3 == 1 && counter != 1;
}

int __wrap_hw_stat_port_get(uint32_t port, uint32_t counter, uint64_t *value);
int __wrap_hw_stat_port_get(uint32_t port, uint32_t counter, uint64_t *value)
{
	if (port >= FAKE_PORTS || counter >= MIB_COUNT || refused(counter))
		return -22;
	/* Port 1 is all zeros, as an unpopulated port reads; port 3 carries
	 * one value past 32 bits, which the 25-wide column must hold. */
	if (port == 1)
		*value = 0;
	else if (port == 3 && counter == 0)
		*value = 0x123456789abcULL;
	else
		/* 32-bit arithmetic: a 64-bit multiply is a libgcc call. */
		*value = (port + 1) * 1000u * (counter + 1) + counter;
	return 0;
}
