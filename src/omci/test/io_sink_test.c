/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* out_drop_sink(), the call omcid's signal handler makes: what is buffered
 * for a sink is not sent through it, and output goes to stdout again. */
#include "sys.h"
#include "io.h"

static int calls;

static void counting_sink(const char *s, int n)
{
	(void)s;
	(void)n;
	calls++;
}

int main(void)
{
	int failures = 0;

	out_set_sink(counting_sink);
	out("half a reply, no newline");
	out_drop_sink();
	out_flush();
	if (calls) {
		out("FAIL  out_drop_sink sent the buffered bytes through the sink\n");
		failures++;
	} else {
		out("ok    out_drop_sink sends nothing through the sink\n");
	}

	out_set_sink(counting_sink);
	out("a reply");
	out_set_sink(0);
	if (calls != 1) {
		out("FAIL  out_set_sink(0) no longer flushes through the sink\n");
		failures++;
	} else {
		out("ok    out_set_sink(0) still flushes through the sink\n");
	}
	out_flush();
	return failures ? 1 : 0;
}
