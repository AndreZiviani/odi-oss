/* Checks gpon_proc_parse_state against the actual /proc/odi_gpon first-line
 * shape (kernel/extra/drivers/net/ethernet/odi/odi_gpon.c: "state %d\n").
 * Host-side: gpon_status.c touches no syscalls, so it builds natively. */
#include <stdio.h>
#include <string.h>
#include "gpon_status.h"

static int failures;

static void check_ok(const char *buf, unsigned len, uint32_t expect, const char *label)
{
	uint32_t got = 0xffffffffu;
	int rc = gpon_proc_parse_state(buf, len, &got);
	int ok = rc == 0 && got == expect;

	printf("  %-28s rc %d state %u (want %u)  %s\n", label, rc, (unsigned)got,
	       (unsigned)expect, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

static void check_fail(const char *buf, unsigned len, const char *label)
{
	uint32_t got = 0;
	int rc = gpon_proc_parse_state(buf, len, &got);
	int ok = rc != 0;

	printf("  %-28s rc %d  %s\n", label, rc, ok ? "ok (rejected)" : "FAIL (accepted)");
	if (!ok)
		failures++;
}

int main(void)
{
	puts("/proc/odi_gpon first-line parsing:");

	/* The real shapes odi_gpon_proc_show() prints, O1..O7 as 1..7 and
	 * unknown as 0, always followed by the rest of the file. */
	check_ok("state 0\nonu_id 0\n", strlen("state 0\nonu_id 0\n"), 0, "unknown (0)");
	check_ok("state 5\nonu_id 3\n", strlen("state 5\nonu_id 3\n"), 5, "O5 (5)");
	check_ok("state 7\n", strlen("state 7\n"), 7, "O7 (7), no trailing data");
	/* A short read that still lands past the last digit is fine -- only the
	 * first line is ever used. */
	check_ok("state 2", 7, 2, "short read, no newline");

	/* The file absent (SWITCH=vendor) is sys_open failing, never this
	 * function; these are content it must still refuse. */
	check_fail("", 0, "empty read");
	check_fail("stat 5\n", 7, "wrong prefix");
	check_fail("state \n", 7, "no digits");
	check_fail("state 8\n", 8, "out of range (8)");
	check_fail("state 99\n", 9, "out of range (99)");

	puts("/proc/odi_gpon last_los_ms line:");
	{
		/* The shape odi_gpon_proc_show() prints, embedded as it is in
		 * the rest of the file. */
		static const char file[] =
			"state 5 (O5)\nonu_id 26\nsn 0011223344556677\n"
			"irq_attached 1 isr_rc 0 imr_rc 0\n"
			"last_los_ms 0 state 0\nploam_ring 1 entries\n"
			"  4294897600 ds type 0x01\n";
		static const char set[] = "x 1\nlast_los_ms 123456 state 1\n";
		const char *bad[] = {
			"last_los_ms state 0\n", "last_los_ms 5 state 2\n",
			"last_los_ms 5 stat 1\n", "xlast_los_ms 5 state 1\n",
			"state 5\nonu_id 1\n", "last_los_ms 5 state 10\n",
		};
		uint32_t los = 9;
		int rc = gpon_proc_parse_los(file, (unsigned)strlen(file), &los);

		printf("  %-28s rc %d los %u  %s\n", "clear, mid-file", rc,
		       (unsigned)los, rc == 0 && los == 0 ? "ok" : "FAIL");
		failures += !(rc == 0 && los == 0);
		los = 9;
		rc = gpon_proc_parse_los(set, (unsigned)strlen(set), &los);
		printf("  %-28s rc %d los %u  %s\n", "set", rc, (unsigned)los,
		       rc == 0 && los == 1 ? "ok" : "FAIL");
		failures += !(rc == 0 && los == 1);
		for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			rc = gpon_proc_parse_los(bad[i], (unsigned)strlen(bad[i]), &los);
			printf("  bad #%u                       rc %d  %s\n", i, rc,
			       rc != 0 ? "ok (rejected)" : "FAIL (accepted)");
			failures += rc == 0;
		}
	}

	puts("GPON_GTC_DS_INTR_STS decoding:");
	{
		/* Bit 2 is FEC state, not an alarm, and must never show up. */
		uint32_t a = gpon_alarms_from_sts(0xfffffff4u);
		int ok = a == 0 && gpon_alarms_from_sts(0x0bu) ==
			(GPON_ALARM_LOS | GPON_ALARM_LOF | GPON_ALARM_LOM) &&
			gpon_alarms_from_sts(0x02u) == GPON_ALARM_LOF;

		printf("  %-28s %s\n", "LOS/LOF/LOM only, FEC ignored", ok ? "ok" : "FAIL");
		failures += !ok;
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
