/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_omci_parity_parse_test.c -- host-side unit test for the
 * parity_add token parser in kernel/extra/drivers/net/ethernet/odi/
 * odi_omci.c (odi_omci_next_uint_token()). No kernel, no target
 * toolchain: that function is kernel-only (uses kstrtouint(),
 * copy_from_user()), so this test reimplements its exact parsing
 * contract on the host -- strip one optional leading "0x"/"0X", then
 * require one or more hex digits and nothing else -- and checks it
 * against both synthetic cases and every field of every real parity
 * table this repo ships, byte for byte.
 *
 * The bug: the
 * kernel function called kstrtouint(p, 0, out) -- base 0. Every table
 * file (tools/regdump/mkparity.py own output format) writes fields as
 * zero-padded 8-digit hex with no "0x" prefix, e.g. "0080c101". Base 0
 * treats a leading '0' as an octal prefix, and '8' is not a valid octal
 * digit, so kstrtouint returned -EINVAL for every such field and
 * parity-load.sh own parity_add write failed with "Invalid argument" --
 * confirmed live on the stick. The fix changed the call to
 * kstrtouint(p, 16, out) explicitly: base 16 still strips a leading
 * "0x"/"0X" if present (the kernel own _parse_integer_fixup_radix does
 * that unconditionally for base 16, not only for the auto-detecting
 * base 0), so both the zero-padded form the table files actually use and
 * an explicit "0x..." form parse the same way. This test own
 * host_hex_token() mirrors that post-fix behaviour; test_regression_bug()
 * documents the pre-fix failure this test would have caught.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* Host reimplementation of kstrtouint(str, 16, out) own contract, as used
 * by odi_omci_next_uint_token() after the fix: an optional one-time
 * "0x"/"0X" prefix, then one or more hex digits, nothing else -- an
 * empty string, a bare prefix with no digits, any non-hex character, or
 * unsigned-int overflow are all refused. Returns 0 and *out on success,
 * -1 on any parse failure -- same signature odi_omci_next_uint_token()
 * itself uses for kstrtouint own return.
 */
static int host_hex_token(const char *s, unsigned int *out)
{
	unsigned long acc = 0;
	int saw_digit = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	if (*s == '\0')
		return -1;
	for (; *s != '\0'; s++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -1;
		saw_digit = 1;
		acc = acc * 16 + (unsigned long)d;
		if (acc > UINT_MAX)
			return -1;
	}
	if (!saw_digit)
		return -1;
	*out = (unsigned int)acc;
	return 0;
}

/* Pre-fix behaviour, kept only to document the bug this test would have
 * caught: kstrtouint(str, 0, out) own contract -- base auto-detected
 * from the prefix ("0x.." -> 16, a bare leading '0' with more digits
 * after it -> 8, anything else -> 10), same "digits only, whole string
 * consumed" strictness otherwise. Zero-padded hex without "0x" (the
 * the table files own format) is octal under this rule, so a byte outside
 * 0-7 (any hex digit 8, 9, a-f) fails it -- exactly the bug.
 */
static int host_hex_token_base0_buggy(const char *s, unsigned int *out)
{
	unsigned long acc = 0;
	int base = 10;
	int saw_digit = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	} else if (s[0] == '0' && s[1] != '\0') {
		base = 8;
	} else if (s[0] == '0') {
		*out = 0;
		return 0;
	}
	if (*s == '\0')
		return -1;
	for (; *s != '\0'; s++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -1;
		if (d >= base)
			return -1;
		saw_digit = 1;
		acc = acc * (unsigned long)base + (unsigned long)d;
		if (acc > UINT_MAX)
			return -1;
	}
	if (!saw_digit)
		return -1;
	*out = (unsigned int)acc;
	return 0;
}

static void test_zero_padded_hex_no_prefix(void)
{
	unsigned int v = 0xdeadbeefU;

	CHECK(host_hex_token("0080c101", &v) == 0, "0080c101: base-16 parse accepted");
	CHECK(v == 0x0080c101U, "0080c101: parsed value is correct");

	v = 0xdeadbeefU;
	CHECK(host_hex_token("00000000", &v) == 0, "00000000: base-16 parse accepted");
	CHECK(v == 0U, "00000000: parsed value is zero");

	v = 0xdeadbeefU;
	CHECK(host_hex_token("ffffffff", &v) == 0, "ffffffff: base-16 parse accepted");
	CHECK(v == 0xffffffffU, "ffffffff: parsed value is all-ones");
}

static void test_0x_prefixed_hex_still_works(void)
{
	unsigned int v = 0xdeadbeefU;

	CHECK(host_hex_token("0x0080c101", &v) == 0, "0x0080c101: base-16 parse accepted");
	CHECK(v == 0x0080c101U, "0x0080c101: parsed value matches the unprefixed form");

	v = 0xdeadbeefU;
	CHECK(host_hex_token("0X3F", &v) == 0, "0X3F: uppercase prefix accepted");
	CHECK(v == 0x3fU, "0X3F: parsed value is correct");
}

static void test_rejects_garbage(void)
{
	unsigned int v;

	CHECK(host_hex_token("", &v) == -1, "empty string refused");
	CHECK(host_hex_token("0x", &v) == -1, "bare 0x prefix with no digits refused");
	CHECK(host_hex_token("0080g101", &v) == -1, "'g' is not a hex digit, refused");
	CHECK(host_hex_token("12 34", &v) == -1, "embedded space refused (token parsing splits on it, not this)");
}

/* Documents the bug: the same zero-padded string that host_hex_token()
 * (the fix) accepts is refused by the pre-fix base-0 behaviour whenever
 * it contains an 8 or 9 digit -- exactly parity-v6.table own first
 * data line.
 */
static void test_regression_bug(void)
{
	unsigned int v;

	CHECK(host_hex_token_base0_buggy("0080c101", &v) == -1,
	      "pre-fix base-0 parsing of 0080c101 fails (the s9 bug, reproduced)");
	CHECK(host_hex_token("0080c101", &v) == 0,
	      "post-fix base-16 parsing of the same string succeeds");
}

/* Every field of every real table this repo ships must parse cleanly
 * under the fix -- the actual regression surface, not just synthetic
 * cases. Mirrors parity-load.sh own line format: "offset value mask
 * name", comments starting with '#' and blank lines skipped.
 */
static int check_table_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	int n = 0;

	if (!f) {
		fprintf(stderr, "FAIL: could not open %s\n", path);
		failures++;
		return 0;
	}
	while (fgets(line, sizeof line, f)) {
		char offset[64], value[64], mask[64], name[128];
		unsigned int ov, vv, mv;
		int got;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		got = sscanf(line, "%63s %63s %63s %127s", offset, value, mask, name);
		if (got < 3)
			continue;
		if (host_hex_token(offset, &ov) != 0) {
			fprintf(stderr, "FAIL: %s: offset field %s did not parse\n", path, offset);
			failures++;
			continue;
		}
		if (host_hex_token(value, &vv) != 0) {
			fprintf(stderr, "FAIL: %s: value field %s did not parse\n", path, value);
			failures++;
			continue;
		}
		if (host_hex_token(mask, &mv) != 0) {
			fprintf(stderr, "FAIL: %s: mask field %s did not parse\n", path, mask);
			failures++;
			continue;
		}
		n++;
	}
	fclose(f);
	return n;
}

int main(int argc, char **argv)
{
	int i, total = 0;

	test_zero_padded_hex_no_prefix();
	test_0x_prefixed_hex_still_works();
	test_rejects_garbage();
	test_regression_bug();

	for (i = 1; i < argc; i++)
		total += check_table_file(argv[i]);

	if (failures) {
		fprintf(stderr, "odi_omci_parity_parse_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_omci_parity_parse_test: ok (%d table entries parsed across %d file(s))\n",
	       total, argc - 1);
	return 0;
}
