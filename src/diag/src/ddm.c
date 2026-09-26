#include "ddm.h"
#include "hw.h"

/*
 * The stock diag does this in double-precision softfloat and prints with "%f".
 * We use fixed point, in millionths: three of the four conversions are exact in
 * integer arithmetic, and the fourth is accurate well past the input
 * quantisation:
 *
 *   temperature   raw / 256          = raw * 15625 / 4   micro-degrees (rounded)
 *   voltage       raw / 10000        = raw * 100         micro-volts
 *   bias          raw * 2 / 1000     = raw * 2000        micro-amps
 *   power         10 * log10(raw / 10000) dBm            -- see log10_micro
 *
 * The first three are exact. The dBm path agrees with the vendor to within
 * about 1e-5 dB, which is far below the 0.1 uW quantisation of the DDMI word
 * itself, but it is NOT bit-identical to the vendor's double output in the
 * last decimals. Nothing is lost that the transceiver ever measured.
 */

/* Integer log2, no clz: the RLX5281 traps on it, and __builtin_clz would become
 * a libgcc call. Returns the position of the highest set bit. */
static int highest_bit(uint32_t x)
{
	int n = -1;

	while (x) {
		n++;
		x >>= 1;
	}
	return n;
}

/* log2(x) in Q30, for x >= 1. Every multiply is 32x32 into 64, which MIPS-I
 * does natively with multu -- a 64x64 multiply would pull in libgcc. Q30, not
 * Q24, so that an exact input prints exactly: at Q24, 1.0 mW prints as
 * -0.000002 dBm. */
static int64_t log2_q30(uint32_t x)
{
	int e = highest_bit(x);
	uint32_t m = x << (31 - e);          /* normalise into [2^31, 2^32) */
	int64_t r = (int64_t)e << 30;
	int i;

	for (i = 0; i < 30; i++) {
		uint64_t sq = (uint64_t)m * m;   /* Q62 */

		sq >>= 31;                       /* back to Q31 */
		if (sq >> 32) {
			sq >>= 1;
			/* The shift is always under 32, so keep it 32-bit: a variable
			 * 64-bit shift would call __ashldi3 in libgcc. */
			r += (uint32_t)1 << (29 - i);
		}
		m = (uint32_t)sq;
	}
	return r;
}

/* 10 * log10(v / 10000) in millionths of a dB.
 *
 * log10(x) = log2(x) * log10(2), and log10(2) in Q30 is 323228497. Working in
 * Q30 throughout keeps every intermediate inside 64 bits. */
static int64_t dbm_micro(uint32_t v)
{
	int64_t l2, l10_q30;

	if (v == 0)
		return 0;                        /* caller reports this as absent */
	l2 = log2_q30(v);
	/* Round at both shifts: a right shift of a negative value rounds toward
	 * -inf, and 0 dBm would print as -0.000003. */
	l10_q30 = (l2 * 323228497 + (1 << 29)) >> 30;   /* log10(v), Q30 */
	l10_q30 -= (int64_t)4 << 30;                    /* / 10000 */
	/* * 10, and Q30 -> millionths */
	return (l10_q30 * 10 * 1000000 + (1 << 29)) >> 30;
}

/* Append a fixed-point value given in millionths, with six decimals, matching
 * the width the vendor's "%f" produces.
 *
 * Deliberately 32-bit: every value here is at most about 1.3e8 millionths, and
 * a 64-bit divide would pull __udivdi3 out of libgcc, which a -nostdlib link
 * does not have. */
static int put_micro(char *out, int max, int32_t micro)
{
	char digits[24];
	int n = 0, i, neg = micro < 0;
	uint32_t whole, frac, u;

	if (neg)
		micro = -micro;
	u = (uint32_t)micro;
	whole = u / 1000000;
	frac = u % 1000000;

	if (neg && n < max - 1)
		out[n++] = '-';
	if (!whole) {
		if (n < max - 1)
			out[n++] = '0';
	} else {
		int d = 0;

		while (whole) {
			digits[d++] = (char)('0' + (whole % 10));
			whole /= 10;
		}
		while (d-- && n < max - 1)
			out[n++] = digits[d];
	}
	if (n < max - 1)
		out[n++] = '.';
	for (i = 100000; i > 0 && n < max - 1; i /= 10) {
		out[n++] = (char)('0' + (frac / (unsigned)i) % 10);
	}
	out[n] = '\0';
	return n;
}

static int put_str(char *out, int max, int n, const char *s)
{
	while (*s && n < max - 1)
		out[n++] = *s++;
	out[n] = '\0';
	return n;
}

int ddm_format(int type, const uint8_t raw[24], char *out, int max)
{
	uint16_t v = (uint16_t)((raw[0] << 8) | raw[1]);
	int64_t micro;
	const char *unit;
	int n;

	switch (type) {
	case DDM_VENDOR_NAME:
	case DDM_PART_NUMBER: {
		/* ASCII, space-padded in the block; trim the trailing padding. */
		int len = 16, i;

		if (len > 24)
			len = 24;
		while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\0'))
			len--;
		for (i = 0; i < len && i < max - 1; i++)
			out[i] = (char)raw[i];
		out[i] = '\0';
		return 0;
	}
	case DDM_TEMPERATURE:
		/* Signed, 1/256 degree, sign-extended (the stock diag treats 0x80xx,
		 * about -128 C, as positive). raw * 15625 / 4 is rounded to match %f:
		 * raw 11191 is 43.71484375 C, printed 43.714844, not 43.714843. */
		micro = (int64_t)(int16_t)v * 15625;
		micro = (micro < 0 ? micro - 2 : micro + 2) / 4;   /* C truncates toward zero */
		unit = " C";
		break;
	case DDM_VOLTAGE:
		micro = (int64_t)v * 100;
		unit = " V";
		break;
	case DDM_BIAS_CURRENT:
		micro = (int64_t)v * 2000;
		unit = " mA";
		break;
	case DDM_TX_POWER:
	case DDM_RX_POWER:
		if (v == 0) {
			put_str(out, max, 0, "-inf  dBm");
			return 0;
		}
		micro = dbm_micro(v);
		unit = "  dBm";   /* two spaces, as the vendor's format string has */
		break;
	default:
		put_str(out, max, 0, "N/A");
		return 0;
	}
	n = put_micro(out, max, (int32_t)micro);
	put_str(out, max, n, unit);
	return 0;
}
