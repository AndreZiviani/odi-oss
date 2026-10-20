/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_dlog_test.c -- the held log lines of the GPON interrupt
 * (odi_gpon_dlog.h): lines are formatted into the buffer in order and with
 * their level, taken out whole, the buffer is empty after the take, and a
 * line past the end is counted, not written over another. Whether
 * odi_gpon.c prints them after the lock is released is read from the code;
 * it is kernel-only.
 */
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_dlog.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static void add(struct odi_gpon_dlog *d, enum odi_gpon_dlog_level level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	odi_gpon_dlog_vadd(d, level, fmt, ap);
	va_end(ap);
}

int main(void)
{
	static struct odi_gpon_dlog d, out;
	unsigned int i;

	add(&d, ODI_GPON_DLOG_NOTICE, "odi_gpon: event=%s n=%u\n", "onu_state", 5U);
	add(&d, ODI_GPON_DLOG_INFO, "second\n");
	odi_gpon_dlog_take(&d, &out);
	CHECK(out.n == 2U && out.lost == 0U, "two lines taken");
	CHECK(out.line[0].level == ODI_GPON_DLOG_NOTICE && out.line[1].level == ODI_GPON_DLOG_INFO,
	      "levels kept");
	CHECK(strcmp(out.line[0].text, "odi_gpon: event=onu_state n=5\n") == 0, "first line formatted");
	CHECK(strcmp(out.line[1].text, "second\n") == 0, "second line kept, in order");
	CHECK(d.n == 0U && d.lost == 0U, "buffer empty after the take");

	for (i = 0; i < ODI_GPON_DLOG_LINES + 3U; i++)
		add(&d, ODI_GPON_DLOG_INFO, "line %u\n", i);
	odi_gpon_dlog_take(&d, &out);
	CHECK(out.n == ODI_GPON_DLOG_LINES, "a full buffer holds its lines");
	CHECK(out.lost == 3U, "lines past the end are counted");
	CHECK(strcmp(out.line[ODI_GPON_DLOG_LINES - 1U].text, "line 7\n") == 0, "the last kept line is not overwritten");

	/* A line longer than the slot is cut, never run over into the next. */
	{
		char big[400];

		memset(big, 'x', sizeof(big) - 1U);
		big[sizeof(big) - 1U] = '\0';
		add(&d, ODI_GPON_DLOG_INFO, "%s", big);
		add(&d, ODI_GPON_DLOG_INFO, "next\n");
		odi_gpon_dlog_take(&d, &out);
		CHECK(strlen(out.line[0].text) == ODI_GPON_DLOG_LEN - 1U, "a long line is cut to the slot");
		CHECK(strcmp(out.line[1].text, "next\n") == 0, "the line after a long one is intact");
	}

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_dlog_test: ok\n");
	return 0;
}
