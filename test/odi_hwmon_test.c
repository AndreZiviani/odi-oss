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

#define CTRL 0x00034ad7U
#define ST(c) ((uint32_t)(c) << 16)

static void expect(const char *what, int cond)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		fails++;
	}
}

/* One sample at c degrees; returns the events. */
static unsigned int samp(struct odi_therm_alarms *a, long c, int *rc)
{
	unsigned int ev;
	long t;

	*rc = odi_therm_alarm_sample(a, CTRL, ST(c), &t, &ev);
	return ev;
}

static void alarm_tests(void)
{
	struct odi_therm_alarms a;
	unsigned int ev;
	int rc;

	odi_therm_alarms_init(&a);
	expect("default max", a.max == 95000 && ODI_THERM_MAX_DEFAULT == 95000L);
	expect("default crit", a.crit == 110000 && ODI_THERM_CRIT_DEFAULT == 110000L);
	expect("alarms start clear", !a.max_alarm && !a.crit_alarm);

	/* Limit writes: range and ordering. */
	expect("max 0 ok", odi_therm_set_max(&a, 0) == 0 && a.max == 0);
	expect("max negative", odi_therm_set_max(&a, -1) == -EINVAL && a.max == 0);
	expect("max above range", odi_therm_set_max(&a, 150001) == -EINVAL);
	expect("max above crit", odi_therm_set_max(&a, 110001) == -EINVAL && a.max == 0);
	expect("max = crit ok", odi_therm_set_max(&a, 110000) == 0 && a.max == 110000);
	expect("crit below max", odi_therm_set_crit(&a, 109999) == -EINVAL && a.crit == 110000);
	expect("crit above range", odi_therm_set_crit(&a, 150001) == -EINVAL);
	expect("crit 150000 ok", odi_therm_set_crit(&a, 150000) == 0 && a.crit == 150000);
	expect("max 95000 ok", odi_therm_set_max(&a, 95000) == 0);

	odi_therm_alarms_init(&a);
	/* Below the limit: nothing. */
	ev = samp(&a, 94, &rc);
	expect("94 C no event", rc == 0 && ev == 0 && !a.max_alarm);
	/* At the limit: set, once. */
	ev = samp(&a, 95, &rc);
	expect("95 C sets max", ev == ODI_THERM_EV_MAX_SET && a.max_alarm && !a.crit_alarm);
	ev = samp(&a, 96, &rc);
	expect("96 C no second event", ev == 0 && a.max_alarm);
	/* Hysteresis: 94 and 93 keep it, 93 is limit - 2 so it clears. */
	ev = samp(&a, 94, &rc);
	expect("94 C holds", ev == 0 && a.max_alarm);
	ev = samp(&a, 93, &rc);
	expect("93 C clears", ev == ODI_THERM_EV_MAX_CLR && !a.max_alarm);
	ev = samp(&a, 93, &rc);
	expect("93 C quiet", ev == 0 && !a.max_alarm);
	ev = samp(&a, 94, &rc);
	expect("94 C stays clear", ev == 0 && !a.max_alarm);

	/* Crit, and both at once. */
	ev = samp(&a, 111, &rc);
	expect("111 C sets both", ev == (ODI_THERM_EV_MAX_SET | ODI_THERM_EV_CRIT_SET) &&
	       a.max_alarm && a.crit_alarm);
	ev = samp(&a, 108, &rc);
	expect("108 C clears crit only", ev == ODI_THERM_EV_CRIT_CLR &&
	       a.max_alarm && !a.crit_alarm);
	ev = samp(&a, 90, &rc);
	expect("90 C clears max", ev == ODI_THERM_EV_MAX_CLR && !a.max_alarm);

	/* An invalid reading changes nothing, in either state. */
	samp(&a, 100, &rc);
	{
		long t = 777;
		unsigned int e2 = 99;

		rc = odi_therm_alarm_sample(&a, 0x00024ad7U, ST(20), &t, &e2);
		expect("invalid: -ENODATA", rc == -ENODATA);
		expect("invalid: no event, alarm kept", e2 == 0 && a.max_alarm && !a.crit_alarm);
		expect("invalid: value untouched", t == 777);
		odi_therm_alarms_init(&a);
		rc = odi_therm_alarm_sample(&a, 0, ST(120), &t, &e2);
		expect("invalid when clear: stays clear", rc == -ENODATA && e2 == 0 &&
		       !a.max_alarm && !a.crit_alarm);
	}

	/* Lowering a limit below the reading raises the alarm at the next sample. */
	odi_therm_alarms_init(&a);
	samp(&a, 62, &rc);
	expect("max 60 ok", odi_therm_set_max(&a, 60000) == 0);
	ev = samp(&a, 62, &rc);
	expect("lowered limit trips", ev == ODI_THERM_EV_MAX_SET && a.max_alarm);
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

	alarm_tests();

	if (fails)
		return 1;
	printf("odi_hwmon_test: ok\n");
	return 0;
}
