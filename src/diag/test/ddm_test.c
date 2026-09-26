/* Checks the fixed-point SFF-8472 conversions against double-precision
 * reference values computed the way the vendor does. Host-side: ddm.c touches
 * no syscalls, so it builds natively. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "ddm.h"
#include "hw.h"

static int failures;

static void check(int type, unsigned raw, double expect, const char *unit)
{
	unsigned char blk[24] = {0};
	char got[64];
	double parsed;
	const char *p;

	blk[0] = (unsigned char)(raw >> 8);
	blk[1] = (unsigned char)(raw & 0xff);
	ddm_format(type, blk, got, sizeof got);

	parsed = atof(got);
	p = strstr(got, unit);
	double diff = fabs(parsed - expect);
	int ok = p && diff < 2e-5;

	printf("  type %d raw %5u -> %-22s expect %12.6f%-5s diff %.2e  %s\n",
	       type, raw, got, expect, unit, diff, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

int main(void)
{
	puts("SFF-8472 conversions (reference computed in double):");

	/* temperature: signed, 1/256 C */
	check(DDM_TEMPERATURE, 0x2A00, 10752 / 256.0, " C");
	check(DDM_TEMPERATURE, 0x1980, 6528 / 256.0, " C");
	check(DDM_TEMPERATURE, 0xF600, (double)(short)0xF600 / 256.0, " C");
	/* 11191/256 = 43.71484375: the quarter that exposed a truncation bug
	 * against real hardware. */
	check(DDM_TEMPERATURE, 11191, 11191 / 256.0, " C");
	check(DDM_TEMPERATURE, 11193, 11193 / 256.0, " C");

	/* voltage: 100 uV units */
	check(DDM_VOLTAGE, 32800, 32800 / 10000.0, " V");
	check(DDM_VOLTAGE, 33000, 33000 / 10000.0, " V");

	/* bias: 2 uA units */
	check(DDM_BIAS_CURRENT, 6200, 6200 * 2 / 1000.0, " mA");
	check(DDM_BIAS_CURRENT, 1, 2 / 1000.0, " mA");

	/* power: 10*log10(v/10000) dBm */
	check(DDM_RX_POWER, 111, 10 * log10(111 / 10000.0), " dBm");
	check(DDM_RX_POWER, 744, 10 * log10(744 / 10000.0), " dBm");
	check(DDM_TX_POWER, 17400, 10 * log10(17400 / 10000.0), " dBm");
	check(DDM_RX_POWER, 1, 10 * log10(1 / 10000.0), " dBm");
	check(DDM_RX_POWER, 65535, 10 * log10(65535 / 10000.0), " dBm");
	check(DDM_RX_POWER, 10000, 0.0, " dBm");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
