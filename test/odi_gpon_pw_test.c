/* odi_gpon_pw_test.c -- odi_gpon_parse_pw (the gponpw argument) on the host:
 * every length 1..20, both forms, and the ones that must be refused. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "odi_gpon_pw.h"

static int fails, checks;

static void expect(const char *name, const char *arg, int want_rc,
		   const uint8_t want[10])
{
	uint8_t pw[10];
	int rc;

	memset(pw, 0xAA, sizeof(pw));
	rc = odi_gpon_parse_pw(arg, pw);
	checks++;
	if (rc != want_rc || memcmp(pw, want, 10) != 0) {
		int i;

		fails++;
		printf("FAIL  %s: \"%s\" rc=%d want %d, bytes", name, arg ? arg : "(null)", rc, want_rc);
		for (i = 0; i < 10; i++)
			printf(" %02x", pw[i]);
		printf("\n");
	}
}

int main(void)
{
	static const uint8_t zero[10];
	const char *hexdig = "0123456789abcdef";
	const char *text = "Zq!Zq!Zq!Zq!";	/* never all hex digits */
	char buf[32];
	uint8_t want[10];
	int len, i;

	expect("null is no password", NULL, 0, zero);
	expect("empty is no password", "", 0, zero);

	/* 20 hex digits: hex, as before. */
	for (i = 0; i < 20; i++)
		buf[i] = hexdig[(i * 5 + 3) & 15];
	buf[20] = 0;
	memset(want, 0, sizeof(want));
	for (i = 0; i < 10; i++)
		want[i] = (uint8_t)((((2 * i) * 5 + 3) & 15) << 4 | (((2 * i + 1) * 5 + 3) & 15));
	expect("20 hex digits", buf, 0, want);

	/* 1..10 characters are text, hex-looking or not. */
	for (len = 1; len <= 10; len++) {
		memset(want, 0, sizeof(want));
		memcpy(want, text, len);
		memcpy(buf, text, len);
		buf[len] = 0;
		expect("text", buf, 0, want);
	}
	for (len = 1; len <= 10; len++) {
		const char *digs = "1234567890";

		memset(want, 0, sizeof(want));
		memcpy(want, digs, len);
		memcpy(buf, digs, len);
		buf[len] = 0;
		expect("hex-looking text stays text", buf, 0, want);
	}
	memset(want, 0, sizeof(want));
	memcpy(want, "123456", 6);
	expect("123456 is six ASCII bytes", "123456", 0, want);
	memset(want, 0, sizeof(want));
	memcpy(want, "deadbeef", 8);
	expect("deadbeef is text", "deadbeef", 0, want);

	/* Explicit short hex: 0x and 2..20 digits, zero-padded. */
	for (len = 2; len <= 20; len += 2) {
		char h[24] = "0x";

		memset(want, 0, sizeof(want));
		for (i = 0; i < len; i++)
			h[2 + i] = hexdig[(i * 7 + 1) & 15];
		h[2 + len] = 0;
		for (i = 0; i < len / 2; i++)
			want[i] = (uint8_t)(((((2 * i) * 7 + 1) & 15) << 4) | (((2 * i + 1) * 7 + 1) & 15));
		expect("0x hex", h, 0, want);
		h[1] = 'X';
		expect("0X hex", h, 0, want);
	}

	/* Vendor omci_app form. */
	memset(want, 0, sizeof(want));
	want[0] = 0xab; want[1] = 0xcd;
	expect("0x prefix wins over text", "0xabcd", 0, want);
	expect("0X prefix, upper case", "0XABCD", 0, want);
	memset(want, 0, sizeof(want));
	memcpy(want, "0xzz", 4);
	expect("0x then non-hex is text", "0xzz", 0, want);

	/* Refused: nothing is half-written. */
	expect("11 characters of text", "abcdefghijk", -EINVAL, zero);
	expect("0x with an odd digit count, 11 chars", "0x123456789", -EINVAL, zero);
	expect("11 hex digits (odd)", "12345678901", -EINVAL, zero);
	expect("19 hex digits (odd)", "1234567890123456789", -EINVAL, zero);
	expect("21 hex digits", "123456789012345678901", -EINVAL, zero);
	expect("20 chars, one not hex", "1234567890123456789g", -EINVAL, zero);
	expect("22 hex digits", "1234567890123456789012", -EINVAL, zero);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
