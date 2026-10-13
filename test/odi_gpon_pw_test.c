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

	/* Hex form, every even length 2..20, with a value that tells the bytes apart. */
	for (len = 2; len <= 20; len += 2) {
		memset(want, 0, sizeof(want));
		for (i = 0; i < len; i++)
			buf[i] = hexdig[(i * 5 + 3) & 15];
		buf[len] = 0;
		for (i = 0; i < len / 2; i++)
			want[i] = (uint8_t)(((((i * 2) * 5 + 3) & 15) << 4) | ((((i * 2 + 1) * 5 + 3) & 15)));
		expect("hex", buf, 0, want);
	}

	/* The documented stored form: hex of ASCII text, 1..10 characters. */
	for (len = 1; len <= 10; len++) {
		char hex[24];
		const char *p = "pw9Kx2Lm4Q";

		memset(want, 0, sizeof(want));
		memcpy(want, p, len);
		for (i = 0; i < len; i++)
			sprintf(&hex[2 * i], "%02x", (unsigned char)p[i]);
		expect("hex of text", hex, 0, want);
	}

	/* Text form, 1..10 characters that are not all hex digits (odd lengths
	 * of plain digits too: they cannot be hex). */
	for (len = 1; len <= 10; len++) {
		memset(want, 0, sizeof(want));
		memcpy(want, text, len);
		memcpy(buf, text, len);
		buf[len] = 0;
		expect("text", buf, 0, want);
	}
	memset(want, 0, sizeof(want));
	memcpy(want, "12345", 5);
	expect("odd digits are text", "12345", 0, want);

	/* Vendor omci_app form. */
	memset(want, 0, sizeof(want));
	want[0] = 0xab; want[1] = 0xcd;
	expect("0x prefix", "0xabcd", 0, want);
	expect("0X prefix", "0XABCD", 0, want);
	memset(want, 0, sizeof(want));
	memcpy(want, "0xzz", 4);
	expect("0x then non-hex is text", "0xzz", 0, want);

	/* Refused: nothing is half-written. */
	expect("11 characters of text", "abcdefghijk", -EINVAL, zero);
	expect("11 hex digits (odd)", "12345678901", -EINVAL, zero);
	expect("19 hex digits (odd)", "1234567890123456789", -EINVAL, zero);
	expect("21 hex digits", "123456789012345678901", -EINVAL, zero);
	expect("20 chars, one not hex", "1234567890123456789g", -EINVAL, zero);
	expect("22 hex digits", "1234567890123456789012", -EINVAL, zero);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
