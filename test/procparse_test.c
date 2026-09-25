/* procparse_test.c -- src/omci/procparse.h against the shapes our drivers
 * print: /proc/odi_omci "registered:" (odi_omci.c) and the /proc/odi_gpon
 * "sn" line (odi_gpon.c), plus the config store label form of GPON_SN.
 * Host-side: the header touches no syscalls. Part of `make test-host`. */
#include <stdio.h>
#include <string.h>
#include "procparse.h"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-52s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static int holder(const char *s, unsigned type, uint32_t *pid)
{
	return pp_redirect_holder(s, (unsigned)strlen(s), type, pid);
}

int main(void)
{
	uint32_t pid = 0;
	uint8_t sn[8];
	static const uint8_t want[8] = { 'A', 'B', 'C', 'D', 0x00, 0x11, 0xaa, 0xff };

	puts("/proc/odi_omci registered: line");
	ok(holder("registered: type=1 pid=593\nframes_delivered 1\n", 1, &pid) == 1 &&
	   pid == 593, "type 1 held by 593");
	pid = 0;
	ok(holder("registered: type=4 pid=7 type=1 pid=1200\n", 1, &pid) == 1 &&
	   pid == 1200, "second entry on the line");
	ok(holder("registered: type=4 pid=7\n", 1, &pid) == 0, "another type only: free");
	ok(holder("registered:\nframes_delivered 0\n", 1, &pid) == 0, "empty line: free");
	ok(holder("registered: type=11 pid=5\n", 1, &pid) == 0, "type 11 is not type 1");
	ok(holder("frames_delivered 0\n", 1, &pid) == -1, "no registered line: cannot tell");
	ok(holder("", 1, &pid) == -1, "empty read: cannot tell");
	ok(holder("xregistered: type=1 pid=5\n", 1, &pid) == -1, "only at a line start");
	ok(holder("registered: type=1 pidx=5 type=1 pid=6\n", 1, &pid) == 1 && pid == 6,
	   "malformed entry skipped");
	ok(holder("registered: type=1 pid=", 1, &pid) == 0, "truncated pid: not a hit");

	puts("/proc/odi_gpon sn line");
	{
		static const char f[] = "state 5 (O5)\nonu_id 26\nsn 414243440011aaff00\n";
		static const char g[] = "state 5 (O5)\nonu_id 26\nsn 414243440011AAFF\neqd x\n";

		memset(sn, 0, sizeof sn);
		ok(pp_gpon_sn(g, (unsigned)strlen(g), sn) == 0 && !memcmp(sn, want, 8),
		   "third line, upper-case hex");
		ok(pp_gpon_sn(f, (unsigned)strlen(f), sn) == -1, "18 digits rejected");
		ok(pp_gpon_sn("sn 0000000000000000\n", 20, sn) == 0 && pp_sn_is_zero(sn),
		   "all-zero read back as zero");
		ok(pp_gpon_sn("sn 00112233445566\n", 18, sn) == -1, "short value rejected");
		ok(pp_gpon_sn("snx 0011223344556677\n", 21, sn) == -1, "other key rejected");
		ok(pp_gpon_sn("state 5\n", 8, sn) == -1, "no sn line");
		ok(pp_gpon_sn("sn 0011223344556677", 19, sn) == 0, "no trailing newline");
	}

	puts("GPON_SN label form");
	memset(sn, 0, sizeof sn);
	ok(pp_label_sn("ABCD0011aaFF", 12, sn) == 0 && !memcmp(sn, want, 8), "vendor + 8 hex");
	ok(pp_label_sn("ABCD0011aaF", 11, sn) == -1, "11 characters rejected");
	ok(pp_label_sn("ABCD0011aaFG", 12, sn) == -1, "non-hex rejected");
	ok(pp_sn_is_zero((const uint8_t *)"\0\0\0\0\0\0\0\0") &&
	   !pp_sn_is_zero(want), "zero test");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
