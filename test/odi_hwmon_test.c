/* odi_hwmon_test.c -- the die temperature decode (odi_hwmon.h), against the
 * words read on a stick and under the stock firmware. Host cc only.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/extra/drivers/net/ethernet/odi/odi_hwmon.h"

static int fails;

static void check_ok(const char *what, uint32_t ctrl, uint32_t st, long want)
{
	long got = -1;
	int rc = odi_therm_decode(ctrl, st, &got);

	if (rc != 0 || got != want) {
		printf("FAIL %s: rc %d, got %ld, want %ld\n", what, rc, got, want);
		fails++;
	}
}

static void check_nodata(const char *what, uint32_t ctrl, uint32_t st)
{
	long got = 12345;
	int rc = odi_therm_decode(ctrl, st, &got);

	if (rc != -ENODATA || got != 12345) {
		printf("FAIL %s: rc %d, got %ld (want -ENODATA, untouched)\n",
		       what, rc, got);
		fails++;
	}
}

int main(void)
{
	/* A booted stick: 0x138 = 0x00034ad7, 0x13c = 0x003e003e (62 C). */
	check_ok("stick", 0x00034ad7U, 0x003e003eU, 62000);
	/* Stock firmware dumps, the two halves differ by a degree or two:
	 * the reading is the upper half only. */
	check_ok("stock 63/65", 0x00034ad7U, 0x003f0041U, 63000);
	check_ok("stock 64/66", 0x00034ad7U, 0x00400042U, 64000);
	check_ok("stock 65/66", 0x00034ad7U, 0x00410042U, 65000);
	/* Bits above 22 and the low half do not leak in. */
	check_ok("high bits", 0x00034ad7U, 0xff3e00ffU, 62000);
	check_ok("zero", ODI_THERM_CTRL_ENABLE, 0, 0);
	check_ok("max", ODI_THERM_CTRL_ENABLE, 0x007f0000U, 127000);
	/* Sensor not enabled: no reading, output untouched. */
	check_nodata("disabled", 0x00024ad7U, 0x003e003eU);
	check_nodata("all zero", 0, 0);

	/* Offsets are the ones the register dumps use. */
	if (ODI_THERM_CTRL_OFF != 0x138U || ODI_THERM_STATUS_OFF != 0x13cU) {
		printf("FAIL offsets\n");
		fails++;
	}
	/* Neither word is in the DoS flood-threshold block. */
	if ((ODI_THERM_CTRL_OFF >= 0x026008U && ODI_THERM_CTRL_OFF <= 0x026010U) ||
	    (ODI_THERM_STATUS_OFF >= 0x026008U && ODI_THERM_STATUS_OFF <= 0x026010U)) {
		printf("FAIL hazard block\n");
		fails++;
	}

	if (fails)
		return 1;
	printf("odi_hwmon_test: ok\n");
	return 0;
}
