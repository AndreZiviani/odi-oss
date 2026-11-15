/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Mutate the real MIB between an upstream rebuild and qos_check(), without
 * a daemon loop or a timer that could rebuild it first. The proc files are
 * a synthetic two-queue chip, and no driver is opened. */
#include "omcid.h"

static char captured[8192];
static unsigned used;
static int failures;

static void capture(const char *s, int n)
{
	while (n-- > 0 && used + 1 < sizeof captured)
		captured[used++] = *s++;
	captured[used] = 0;
}

static int contains(const char *s)
{
	for (unsigned i = 0; captured[i]; i++) {
		unsigned j = 0;

		while (s[j] && captured[i + j] == s[j])
			j++;
		if (!s[j])
			return 1;
	}
	return 0;
}

static void check(const char *name, const char *want)
{
	used = 0;
	captured[0] = 0;
	out_set_sink(capture);
	qos_check();
	out_flush();
	out_set_sink(0);
	if (contains(want))
		out_fmt("ok    %s\n", name);
	else {
		out_fmt("FAIL  %s\n%s", name, captured);
		failures++;
	}
}

static int file(const char *path, const char *text)
{
	unsigned n = 0;
	long fd = sys_create(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0)
		return 0;
	while (text[n])
		n++;
	long written = sys_write((int)fd, text, n);
	sys_close((int)fd);
	return written == (long)n;
}

static void put(struct mib_row *r, unsigned k, uint32_t v)
{
	const struct omci_class *c = find_class(r->classId);
	unsigned n = attr_width(c, k), o = attr_offset(c, k);

	for (unsigned i = 0; i < n; i++)
		r->data[o + n - 1 - i] = (uint8_t)(v >> (8 * i));
	r->written |= (uint16_t)(1u << (16 - k));
}

int main(void)
{
	struct mib_row *tc, *q, *q2, *ctp;

	gpon_proc_path = "/tmp/qos-check-status";
	if (!file(gpon_proc_path, "state 5 (O5)\nalloc_ids 1 1024\n") ||
	    !file("/tmp/cam", "alloc_cam 0 hw 1024 recorded 1024\n") ||
	    !file("/tmp/ponq",
		"tcont 0 en 1 qmap 0x00000003 block 0+2\n"
		"queue 0 wfq 0 weight 1 cir 0 pir 262143\n"
		"queue 1 wfq 0 weight 1 cir 0 pir 262143\n"
		"flow 0 valid 1 gem 1562 queue 0 recorded 1\n"
		"flow 1 valid 1 gem 1563 queue 1 recorded 1\n"))
		return 1;
	apply_hw = 0;
	aut_rebuild();
	tc = mib_add(262, 0x8000);
	put(tc, 1, 1024);
	q = mib_add(277, 0x8006);
	put(q, 6, 0x80000001);
	q2 = mib_add(277, 0x8007);
	put(q2, 6, 0x80000000);
	for (unsigned i = 0; i < 2; i++) {
		ctp = mib_add(268, 2 + i);
		put(ctp, 1, 1562 + i);
		put(ctp, 2, 0x8000);
		put(ctp, 3, 1);
		put(ctp, 4, 0x8006 + i);
	}
	us_qos_rebuild();
	check("unchanged MIB matches both programmed queues", "5 ok, 0 differ");
	put(q, 6, 0x80010001);
	check("pending RelatedPort move is a difference",
	      "mib t-cont 8001 ordinal 0, programmed t-cont 8000 ordinal 0");
	check("flow cannot accept the old queue mapping",
	      "us gem 1562 (ctp 0002) flow 0: differs: mib queue mapping has changed");
	put(q, 6, 0x80000002);
	check("priority change without reordering still matches", "5 ok, 0 differ");
	put(q2, 6, 0x80000003);
	check("a higher priority sibling moves the first queue",
	      "mib t-cont 8000 ordinal 1, programmed t-cont 8000 ordinal 0");
	check("the sibling moves too",
	      "mib t-cont 8000 ordinal 0, programmed t-cont 8000 ordinal 1");
	put(q, 6, 0x80000001);
	put(q2, 6, 0x80000000);
	check("restoring the MIB matches without programming", "5 ok, 0 differ");
	put(q, 7, 0x8001);
	check("scheduler pointer also changes the queue owner",
	      "mib t-cont 8001 ordinal 0, programmed t-cont 8000 ordinal 0");
	out_flush();
	return failures ? 1 : 0;
}
