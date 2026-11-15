/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* procparse_test.c -- src/omci/procparse.h against the shapes our drivers
 * print: /proc/odi_omci "registered:" (odi_omci.c) and the
 * /proc/odi_gpon/status "sn" and "alloc_ids" lines (odi_gpon.c), plus the config store label
 * form of GPON_SN.
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

	puts("/proc/odi_gpon/status state line");
	{
		static const char f[] = "state 5 (O5)\nonu_id 26\n";
		static const char g[] = "onu_id 26\nstate 1 (O1)\nsn 00\n";

		ok(pp_gpon_state(f, (unsigned)strlen(f)) == 5, "state 5");
		ok(pp_gpon_state(g, (unsigned)strlen(g)) == 1, "state on a later line");
		ok(pp_gpon_state("state 12 (?)\n", 14) == -1, "two digits rejected");
		ok(pp_gpon_state("state x\n", 8) == -1, "not a digit rejected");
		ok(pp_gpon_state("last_los_ms 5 state 3\n", 22) == -1, "only at a line start");
		ok(pp_gpon_state("", 0) == -1, "empty read: cannot tell");
	}

	puts("/proc/odi_gpon/status sn line");
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

	puts("/proc/odi_gpon/status alloc_ids line");
	{
		static const char f[] = "state 5 (O5)\nsn 414243440011aaff\n"
			"ploam ds_rx 9 us_tx 9\nalloc_ids 5 282 794 1050 1306 538\n"
			"ploam_type ds 0x0a 15\n";
		uint16_t ids[8];
		int n;

		n = pp_gpon_alloc_ids(f, (unsigned)strlen(f), ids, 8);
		ok(n == 5 && ids[0] == 282 && ids[1] == 794 && ids[4] == 538,
		   "five ids, in order");
		ok(pp_gpon_alloc_ids(f, (unsigned)strlen(f), ids, 2) == 2 && ids[1] == 794,
		   "capped at max");
		ok(pp_gpon_alloc_ids("alloc_ids 0\n", 12, ids, 8) == 0, "none assigned: 0");
		ok(pp_gpon_alloc_ids("alloc_ids 0", 11, ids, 8) == 0, "no trailing newline");
		ok(pp_gpon_alloc_ids("state 5\n", 8, ids, 8) == -1, "no line: cannot tell");
		ok(pp_gpon_alloc_ids("alloc_ids 2 282\n", 16, ids, 8) == -1,
		   "short of its count rejected");
		ok(pp_gpon_alloc_ids("alloc_ids 1 4096\n", 17, ids, 8) == -1,
		   "past 12 bits rejected");
		ok(pp_gpon_alloc_ids("alloc_ids 1 282 7\n", 18, ids, 8) == -1,
		   "more than its count rejected");
		ok(pp_gpon_alloc_ids("alloc_ids 1 28x\n", 16, ids, 8) == -1,
		   "not a number rejected");
		ok(pp_gpon_alloc_ids("alloc_idsx 1 5\n", 15, ids, 8) == -1, "other key rejected");
		ok(pp_gpon_alloc_ids("alloc_ids 1 282", 12, ids, 8) == -1,
		   "cut inside an id rejected");
	}

	puts("/proc/odi_gpon/status enc_ports line");
	{
		static const char f[] = "alloc_ids 1 348\nenc_ports 3 1434 1690 1818\nrei_count 0\n";
		uint16_t ids[8];
		int n = pp_gpon_enc_ports(f, (unsigned)strlen(f), ids, 8);

		ok(n == 3 && ids[0] == 1434 && ids[1] == 1690 && ids[2] == 1818,
		   "three encrypted ports, in order");
		ok(pp_gpon_enc_ports("enc_ports 0\n", 13, ids, 8) == 0, "none encrypted: 0");
		ok(pp_gpon_enc_ports("alloc_ids 1 348\n", 16, ids, 8) == -1,
		   "no line (an older kernel): cannot tell");
		ok(pp_gpon_enc_ports("enc_ports 1 4096\n", 17, ids, 8) == -1, "past 12 bits rejected");
		ok(pp_gpon_alloc_ids(f, (unsigned)strlen(f), ids, 8) == 1 && ids[0] == 348,
		   "the alloc_ids line beside it is unchanged");
	}

	puts("/proc/odi_gpon/counters totals");
	{
		static const char f[] = "ctr_samples 9\nctr ds_fec_corrected_cw 12\n"
			"ctr ds_fec_corrected_bytes 34\nctr big 18446744073709551615\n"
			"ctr over 18446744073709551616\nctr bad 1x\nctr cut 5";
		uint64_t v = 7;

		ok(pp_ctr(f, (unsigned)strlen(f), "ds_fec_corrected_cw", &v) == 0 && v == 12,
		   "a total");
		ok(pp_ctr(f, (unsigned)strlen(f), "ds_fec_corrected_bytes", &v) == 0 && v == 34,
		   "a name that shares a prefix is its own line");
		ok(pp_ctr(f, (unsigned)strlen(f), "big", &v) == 0 && v == UINT64_MAX, "2^64 - 1");
		ok(pp_ctr(f, (unsigned)strlen(f), "over", &v) == -1, "2^64 rejected");
		ok(pp_ctr(f, (unsigned)strlen(f), "bad", &v) == -1, "trailing junk rejected");
		ok(pp_ctr(f, (unsigned)strlen(f), "cut", &v) == 0 && v == 5, "no trailing newline");
		ok(pp_ctr(f, (unsigned)strlen(f), "ds_fec", &v) == -1, "a prefix is not a name");
		ok(pp_ctr(f, (unsigned)strlen(f), "samples", &v) == -1, "only ctr lines");
		ok(pp_ctr("", 0, "x", &v) == -1, "empty read: cannot tell");
	}

	puts("GPON_SN label form");
	memset(sn, 0, sizeof sn);
	ok(pp_label_sn("ABCD0011aaFF", 12, sn) == 0 && !memcmp(sn, want, 8), "vendor + 8 hex");
	ok(pp_label_sn("ABCD0011aaF", 11, sn) == -1, "11 characters rejected");
	ok(pp_label_sn("ABCD0011aaFG", 12, sn) == -1, "non-hex rejected");
	ok(pp_sn_is_zero((const uint8_t *)"\0\0\0\0\0\0\0\0") &&
	   !pp_sn_is_zero(want), "zero test");

	puts("/proc/odi_gpon/ponq and cam");
	{
		static struct pp_ponq p;
		static struct pp_cam c;
		const char *pq =
			"tcont 0 en 1 qmap 0x00000003 block 0+2\n"
			"tcont 8 en 0 qmap 0x00000000 block 0+1\n"
			"tcont 16 en 1 qmap 0x00000001 block -\n"
			"tcont 9 en 1 qmap 0x00000001 block 30+5\n"
			"queue 1 wfq 1 weight 30 cir 99 pir 199\n"
			"queue 99 wfq 0 weight 1 cir 0 pir 0\n"
			"queues 2 wfq 0 weight 1 cir 0 pir 0\n"
			"flow 3 valid 1 gem 1562 queue 33 recorded 1\n"
			"flow 4 valid 1 gem 1563\n"
			"flow 64 valid 1 gem 0 queue 64 recorded 0";
		const char *cm =
			"alloc_cam 0 hw 1024 recorded 1024\n"
			"alloc_cam 1 hw - recorded 1025\n"
			"alloc_cam 2 hw timeout recorded -\n"
			"alloc_cam_valid 1\n"
			"gem_cam 0 hw 1562 type 0x02\n"
			"gem_cam 64 hw 26 type 0x03\n";
		uint32_t v = 0;

		ok(pp_word_u32("a 12 b 0x1f", 11, "b", &v) == 1 && v == 31, "a hex value");
		ok(pp_word_u32("ab 12 b 7", 9, "b", &v) == 1 && v == 7, "a whole word only");
		ok(pp_word_u32("a 12x", 5, "a", &v) == 0, "trailing junk rejected");
		ok(pp_ponq(pq, (unsigned)strlen(pq), &p) == 7, "seven rows; the malformed and out of range skipped");
		ok(p.t[0].seen && p.t[0].en && p.t[0].qmap == 3 && p.t[0].block &&
		   p.t[0].qbase == 0 && p.t[0].qn == 2, "a T-CONT with its block");
		ok(p.t[8].seen && !p.t[8].en && p.t[8].block && p.t[8].qn == 1, "a block and no queue");
		ok(p.t[16].seen && !p.t[16].block, "no block recorded");
		ok(p.t[9].seen && !p.t[9].block, "a block past its group of 32 is not taken");
		ok(p.q[1].seen && p.q[1].wfq && p.q[1].weight == 30 && p.q[1].cir == 99 &&
		   p.q[1].pir == 199, "a queue");
		ok(!p.q[2].seen, "\"queues\" is not \"queue\"");
		ok(p.f[3].valid && p.f[3].gem == 1562 && p.f[3].queue == 33 && p.f[3].rec,
		   "a flow");
		ok(!p.f[4].seen, "a flow line cut short is skipped");
		ok(p.f[64].seen && !p.f[64].rec, "the last line, no newline");
		ok(pp_ponq("", 0, &p) == -1, "empty read: cannot tell");
		ok(pp_cam(cm, (unsigned)strlen(cm), &c) == 5, "five rows");
		ok(c.alloc[0] == 1024 && c.alloc[1] == PP_CAM_NONE &&
		   c.alloc[2] == PP_CAM_TIMEOUT && c.alloc[3] == PP_CAM_UNLISTED, "Alloc-ID rows");
		ok(c.gem[0] == 1562 && c.gem[64] == 26 && c.gem[1] == PP_CAM_UNLISTED, "GEM rows");
		ok(pp_cam("x", 1, &c) == -1, "nothing of ours: cannot tell");
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
