/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_l2_test.c -- host test for odi_switch_l2.c, the L2 lookup
 * table: the row layout (decode and multicast encode, bit by bit), the
 * TABLE_CMD words each access sends, the software walk over valid rows,
 * and multicast add/delete through a small model of the hashed table.
 *
 * The first unicast case restates one row in the shape of a stock `l2-table
 * get entry` listing (invented MAC 02:00:00:00:00:05, source port 2, FID 0, age 6,
 * VID 1, dynamic, SVL): the decode has to give that row back field for
 * field. test_stick_readout() replays a whole table as a stick read it
 * back row by row (four learned MACs, 1020 empty rows).
 *
 * The model (lut_hook below) sits on odi_switch_mock.h's write hook: a
 * TABLE_CMD write with START set runs one access against model_rows[] and
 * leaves TABLE_READ_WORD and TABLE_STATUS as the hardware does. It reads
 * back the way the stick does: every row with bit 77 set, an empty row as
 * its bucket (row / 4) in word 0 and nothing else, and only
 * TABLE_STATUS.HIT telling a used row from an empty one. A driver that
 * took bit 77 for validity lists all 1024 rows against it. Its hash is
 * its own (bucket = low MAC byte xor key, four ways); the driver never
 * computes a hash, so any function exercises the same driver code.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_l2.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_reg.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* ---- The model ------------------------------------------------------ */

#define MODEL_ROWS	(ODI_SW_L2_HASH_ROWS + ODI_SW_L2_CAM_ROWS)

static uint32_t model_rows[MODEL_ROWS][3];
static int model_used[MODEL_ROWS];
static uint32_t model_last_cmd;
static unsigned int model_cmds;
static uint32_t model_last_wr[3];
/* Model switches. strict: a delete removes a row only when every word but
 * VALID agrees, as the other implementation measured on hardware (a
 * key-only delete left a row behind the CPU port). refuse: a delete never
 * removes anything (an engine that keeps the row).
 */
static int model_strict;
static int model_refuse;

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

static void set_reg(uint32_t off, uint32_t v)
{
	odi_mock.regs[odi_mock_slot(off)] = v;
}

static int model_same_key(const uint32_t *a, const uint32_t *b)
{
	/* MAC 0..47, key 48..59, IVL 63: raw[0] whole, raw[1] bits 0..27
	 * and 31.
	 */
	return a[0] == b[0] && (a[1] & 0x8fffffffU) == (b[1] & 0x8fffffffU);
}

static void lut_hook(uint32_t off, uint32_t val)
{
	uint32_t row, method, i, bucket, hit_row = 0;
	int hit = 0, is_write;
	uint32_t wr[3];

	if (off != ODI_SW_TABLE_CMD_OFF || !(val & (1U << 31)))
		return;
	model_cmds++;
	model_last_cmd = val;
	/* The hardware clears START when it takes the command. */
	set_reg(ODI_SW_TABLE_CMD_OFF, val & ~(1U << 31));

	if ((val & 7U) != ODI_SW_L2_TABLE_KIND) {
		fprintf(stderr, "model: TABLE_KIND %u is not the LUT\n", val & 7U);
		failures++;
		return;
	}
	row = (val >> 9) & 0xfffU;
	method = (val >> 4) & 7U;
	is_write = (val >> 3) & 1U;
	for (i = 0; i < 3; i++)
		wr[i] = reg(ODI_SW_TABLE_WRITE_WORD(i));
	memcpy(model_last_wr, wr, sizeof(wr));

	if (method == ODI_SW_L2_METHOD_ROW && !is_write) {
		uint32_t sts = (row & ODI_SW_TABLE_STATUS_ROW_MASK) |
			       (row >= ODI_SW_L2_HASH_ROWS ? ODI_SW_TABLE_STATUS_IN_CAM : 0);
		uint32_t out[3] = { (row >> 2) & 0xffU, 0, 0 };

		if (row < MODEL_ROWS && model_used[row]) {
			memcpy(out, model_rows[row], sizeof(out));
			sts |= ODI_SW_TABLE_STATUS_HIT;
		}
		out[2] |= 1U << 13;	/* bit 77 reads back set on every row */
		for (i = 0; i < 3; i++)
			set_reg(ODI_SW_TABLE_READ_WORD(i), out[i]);
		set_reg(ODI_SW_TABLE_STATUS_OFF, sts);
		return;
	}
	if (method != ODI_SW_L2_METHOD_HASH) {
		fprintf(stderr, "model: method %u write %d not modelled\n", method, is_write);
		failures++;
		return;
	}

	bucket = ((wr[0] & 0xffU) ^ ((wr[1] >> 16) & 0xfffU)) & 0xffU;
	for (i = 0; i < 4 && !hit; i++) {
		uint32_t r = bucket * 4U + i;

		if (model_used[r] && model_same_key(model_rows[r], wr)) {
			hit = 1;
			hit_row = r;
		}
	}
	/* The CAM rows are searched too (CAM on: the tests that fill it). */
	for (i = ODI_SW_L2_HASH_ROWS; i < MODEL_ROWS && !hit; i++)
		if (model_used[i] && model_same_key(model_rows[i], wr)) {
			hit = 1;
			hit_row = i;
		}
	/* A delete word has to agree with the row it removes, VALID aside. */
	if (hit && !(wr[2] & (1U << 13)) && is_write &&
	    (model_refuse || (model_strict && (wr[1] != model_rows[hit_row][1] ||
					       wr[2] != (model_rows[hit_row][2] & ~(1U << 13)))))) {
		set_reg(ODI_SW_TABLE_STATUS_OFF, ODI_SW_TABLE_STATUS_HIT | hit_row);
		return;
	}
	/* A hash lookup read, keyed from TABLE_WRITE_WORD: HIT and the row
	 * for a present key, HIT clear for an absent one, as on the stick.
	 */
	if (!is_write) {
		for (i = 0; i < 3; i++)
			set_reg(ODI_SW_TABLE_READ_WORD(i), hit ? model_rows[hit_row][i] : 0);
		set_reg(ODI_SW_TABLE_STATUS_OFF, hit ? (ODI_SW_TABLE_STATUS_HIT | hit_row) : bucket * 4U);
		return;
	}
	/* A delete write of a key that is not there: the stick answers HIT
	 * with the row the key hashes to, and removes nothing.
	 */
	if (!(wr[2] & (1U << 13)) && !hit) {
		set_reg(ODI_SW_TABLE_STATUS_OFF, ODI_SW_TABLE_STATUS_HIT | (bucket * 4U));
		return;
	}
	if (wr[2] & (1U << 13)) {
		for (i = 0; i < 4 && !hit; i++) {
			uint32_t r = bucket * 4U + i;

			if (!model_used[r]) {
				hit = 1;
				hit_row = r;
			}
		}
		if (hit) {
			memcpy(model_rows[hit_row], wr, sizeof(wr));
			model_used[hit_row] = 1;
		}
	} else if (hit) {
		memset(model_rows[hit_row], 0, sizeof(model_rows[hit_row]));
		model_used[hit_row] = 0;
	}
	set_reg(ODI_SW_TABLE_STATUS_OFF, hit ? (ODI_SW_TABLE_STATUS_HIT | hit_row) : 0);
}

static void model_reset(int cam_disabled)
{
	odi_mock_reset();
	memset(model_rows, 0, sizeof(model_rows));
	memset(model_used, 0, sizeof(model_used));
	model_cmds = 0;
	model_last_cmd = 0;
	model_strict = 0;
	model_refuse = 0;
	odi_mock_write_hook = lut_hook;
	set_reg(ODI_SW_L2_LOOKUP_SETUP_OFF, cam_disabled ? 0x00600bb8U : 0x00400bb8U);
}

/* ---- Layout --------------------------------------------------------- */

/* 02:00:00:00:00:05, key (VID) 1, FID 0, SVL, dynamic, port 2, age 6:
 *   raw[0] = MAC bits 0..31              = 0x00000005
 *   raw[1] = MAC 32..47 0x0200 | key 1 << 16      = 0x00010200
 *   raw[2] = port 2 at 65 -> bit 1: 2 << 1 = 0x4, age 6 at 67 -> bit 3:
 *            6 << 3 = 0x30, bit 77 -> bit 13: 0x2000     = 0x00002034
 * Bit 77 is set, as it is on every row read back, and the decode must not
 * take it for validity: flags stay 0.
 */
static void test_decode_unicast(void)
{
	struct odi_sw_l2_row r;
	static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x05 };

	memset(&r, 0xa5, sizeof(r));
	r.raw[0] = 0x00000005U;
	r.raw[1] = 0x00010200U;
	r.raw[2] = 0x00002034U;
	odi_switch_l2_decode(&r);
	CHECK(r.type == ODI_SW_L2_UCAST, "unicast row decodes as unicast");
	CHECK(memcmp(r.mac, mac, 6) == 0, "unicast MAC, octet 0 in bits 47..40");
	CHECK(r.key == 1, "VID 1 in bits 48..59");
	CHECK(r.port == 2, "source port 2 in bits 65..66");
	CHECK(r.age == 6, "age 6 in bits 67..69");
	CHECK(r.fid == 0 && r.ext_port == 0, "FID and extension port zero");
	CHECK(r.flags == 0, "dynamic, SVL, no flag -- and bit 77 is not VALID");
	CHECK(r.ports == 0 && r.ext_ports == 0 && r.group == 0, "no multicast fields on unicast");

	/* Every unicast flag bit, one at a time, lands on its own flag. */
	{
		static const struct { unsigned int bit; uint32_t flag; } f[] = {
			{ 62, ODI_SW_L2_F_STATIC }, { 63, ODI_SW_L2_F_IVL },
			{ 64, ODI_SW_L2_F_CTAG }, { 70, ODI_SW_L2_F_AUTH },
			{ 71, ODI_SW_L2_F_SA_BLOCK }, { 72, ODI_SW_L2_F_DA_BLOCK },
			{ 73, ODI_SW_L2_F_ARP },
		};
		unsigned int i;

		for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
			memset(&r, 0, sizeof(r));
			r.raw[2] = 1U << 13;
			r.raw[f[i].bit / 32U] |= 1U << (f[i].bit % 32U);
			odi_switch_l2_decode(&r);
			CHECK(r.flags == f[i].flag, "one unicast flag bit, one flag");
		}
		memset(&r, 0, sizeof(r));
		r.raw[1] = 1U << 28;			/* FID, bit 60 */
		r.raw[2] = (1U << 13) | (5U << 10);	/* extension port 5 at 74 */
		odi_switch_l2_decode(&r);
		CHECK(r.fid == 1 && r.ext_port == 5 && r.port == 0 && r.age == 0,
		      "FID bit 60 and extension port 74..76");
	}
}

/* 01:00:5E:01:02:03, key (VID) 10, IVL, ports 0x1, no extension port:
 *   raw[0] = 0x5e010203
 *   raw[1] = 0x0100 | 10 << 16 | static bit 62 (bit 30) | IVL bit 63
 *            (bit 31)                                  = 0xc00a0100
 *   raw[2] = member ports 0x1 at 66 -> bit 2: 0x4, valid bit 13: 0x2000
 *                                                      = 0x00002004
 * and the delete of the same key: the key alone -- MAC, key 10, IVL --
 * with static, valid and the member masks clear
 *                                    raw[1] = 0x800a0100, raw[2] = 0.
 */
static void test_encode_mcast(void)
{
	struct odi_sw_l2_mcast_req q = {
		{ 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 }, 10, 1, 0x1, 0,
	};
	struct odi_sw_l2_row r;
	uint32_t raw[3];

	CHECK(odi_switch_l2_mcast_encode(&q, 1, raw) == 0, "multicast encode accepted");
	CHECK(raw[0] == 0x5e010203U, "multicast raw[0]");
	CHECK(raw[1] == 0xc00a0100U, "multicast raw[1]: MAC high, key 10, static, IVL");
	CHECK(raw[2] == 0x00002004U, "multicast raw[2]: member port 0, valid");
	memset(&r, 0, sizeof(r));
	memcpy(r.raw, raw, sizeof(raw));
	odi_switch_l2_decode(&r);
	CHECK(r.type == ODI_SW_L2_MCAST && r.ports == 0x1 && r.key == 10 &&
	      r.flags == (ODI_SW_L2_F_STATIC | ODI_SW_L2_F_IVL),
	      "multicast row decodes back to what was encoded");

	CHECK(odi_switch_l2_mcast_encode(&q, 0, raw) == 0, "multicast delete encode accepted");
	CHECK(raw[0] == 0x5e010203U && raw[1] == 0x800a0100U && raw[2] == 0,
	      "delete: the key alone, static, valid and members clear");

	/* Extension ports 0..6 at 70..76: 0x41 -> bits 6 and 12 of raw[2]. */
	q.ports = 0xf;
	q.ext_ports = 0x41;
	CHECK(odi_switch_l2_mcast_encode(&q, 1, raw) == 0 &&
	      raw[2] == ((0xfU << 2) | (0x41U << 6) | (1U << 13)),
	      "all four member ports and extension ports 0 and 6");

	q.ports = 0x10;
	q.ext_ports = 0;
	CHECK(odi_switch_l2_mcast_encode(&q, 1, raw) == -22, "a member bit past port 3 is refused");
	q.ports = 1;
	q.ext_ports = 0x80;
	CHECK(odi_switch_l2_mcast_encode(&q, 1, raw) == -22, "an extension bit past 6 is refused");
	q.ext_ports = 0;
	q.key = 4096;
	CHECK(odi_switch_l2_mcast_encode(&q, 1, raw) == -22, "a key past 12 bits is refused");
	q.key = 1;
	q.ivl = 2;
	CHECK(odi_switch_l2_mcast_encode(&q, 1, raw) == -22, "ivl must be 0 or 1");
}

static void test_ipmc_decode(void)
{
	struct odi_sw_l2_row r;

	/* L3 bit 61 (raw[1] bit 29): group 0x0010203 (from 224.1.2.3 without
	 * its top nibble), members port 0, valid. No MAC is decoded.
	 */
	memset(&r, 0, sizeof(r));
	r.raw[0] = 0x00010203U;
	r.raw[1] = 1U << 29;
	r.raw[2] = (1U << 2) | (1U << 13);
	odi_switch_l2_decode(&r);
	CHECK(r.type == ODI_SW_L2_IPMC && r.group == 0x00010203U && r.ports == 1,
	      "an L3 row decodes as a group and a member mask");
	CHECK(r.mac[0] == 0 && r.mac[5] == 0, "an L3 row has no MAC");
}

static void test_mac_ok(void)
{
	static const struct { uint8_t mac[6]; int ok; const char *what; } c[] = {
		{ { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 }, 1, "IPv4 group MAC" },
		{ { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 }, 1, "IPv6 group MAC" },
		{ { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x10 }, 1, "01:80:c2:00:00:10 is outside the reserved block" },
		{ { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 }, 0, "unicast" },
		{ { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 0, "broadcast" },
		{ { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 }, 0, "STP BPDU address" },
		{ { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e }, 0, "LLDP address" },
	};
	unsigned int i;

	for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		CHECK(odi_switch_l2_mcast_mac_ok(c[i].mac) == c[i].ok, c[i].what);
}

/* ---- The engine ----------------------------------------------------- */

/* Row 5 by number: START | ROW 5 << 9 | METHOD 1 << 4 | read | KIND 0
 *   = 0x80000000 | 0xa00 | 0x10 = 0x80000a10.
 */
static void test_read(void)
{
	struct odi_sw_l2_row r;
	unsigned int n;
	int rc;

	model_reset(1);
	model_rows[5][0] = 0x00000005U;
	model_rows[5][1] = 0x00010200U;
	model_rows[5][2] = 0x00002034U;
	model_used[5] = 1;
	/* Stale SPA bits in TABLE_CMD from an earlier user must be cleared. */
	set_reg(ODI_SW_TABLE_CMD_OFF, 3U << 7);
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_read(5, &r);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == 0, "row read succeeds");
	CHECK(model_last_cmd == 0x80000a10U, "row read TABLE_CMD == 0x80000a10");
	CHECK(r.index == 5 && r.raw[0] == 0x00000005U && r.raw[1] == 0x00010200U &&
	      r.raw[2] == 0x00002034U, "raw words come back in TABLE_READ_WORD order");
	CHECK(r.type == ODI_SW_L2_UCAST && r.port == 2 && r.age == 6, "and decoded");
	CHECK(r.flags == ODI_SW_L2_F_VALID, "a row with HIT set reads as valid");

	/* An empty row, as the stick reads one back: bucket 0x1b of row 0x6d
	 * in the low MAC octet, bit 77 set, HIT clear.
	 */
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_read(0x6d, &r);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == 0 && r.raw[0] == 0x1bU && r.raw[1] == 0 && r.raw[2] == 0x00002000U,
	      "an empty row reads back as its bucket, bit 77 set");
	CHECK(!(r.flags & ODI_SW_L2_F_VALID), "and is not valid: HIT was clear");
	CHECK(r.mac[5] == 0x1b && r.type == ODI_SW_L2_UCAST,
	      "even though it decodes as unicast MAC 00:00:00:00:00:1B");

	mutex_lock(&odi_switch_lock);
	n = model_cmds;
	rc = odi_switch_l2_read(1024, &r);
	CHECK(rc == -22 && model_cmds == n, "a CAM row is not read while the CAM is off");
	set_reg(ODI_SW_L2_LOOKUP_SETUP_OFF, 0x00400bb8U);	/* CAM on */
	rc = odi_switch_l2_read(1087, &r);
	CHECK(rc == 0 && model_last_cmd == (0x80000010U | (1087U << 9)),
	      "the last CAM row is read while the CAM is on");
	rc = odi_switch_l2_read(1088, &r);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == -22, "a row past the table is refused");
	CHECK(odi_mock_locks_idle(), "locks idle after the reads");
}

static void test_walk(void)
{
	struct odi_sw_l2_row r;
	uint32_t idx;
	int rc;

	model_reset(1);
	model_used[3] = 1;
	model_rows[4][0] = 0x12345678U;		/* data but no HIT: skipped */
	model_used[700] = 1;
	model_used[1030] = 1;			/* a CAM row */

	mutex_lock(&odi_switch_lock);
	CHECK(odi_switch_l2_rows() == 1024, "CAM rows off: 1024 rows");
	idx = 0;
	rc = odi_switch_l2_next(&idx, &r);
	CHECK(rc == 0 && idx == 3 && r.index == 3, "first valid row is 3");
	idx = 4;
	rc = odi_switch_l2_next(&idx, &r);
	CHECK(rc == 0 && idx == 700, "row 4 holds data but no HIT; next is 700");
	idx = 701;
	rc = odi_switch_l2_next(&idx, &r);
	CHECK(rc == -2 && idx == 1024, "nothing after 700 with the CAM off");
	mutex_unlock(&odi_switch_lock);

	set_reg(ODI_SW_L2_LOOKUP_SETUP_OFF, 0x00400bb8U);	/* CAM on */
	mutex_lock(&odi_switch_lock);
	CHECK(odi_switch_l2_rows() == 1088, "CAM rows on: 1088 rows");
	idx = 701;
	rc = odi_switch_l2_next(&idx, &r);
	CHECK(rc == 0 && idx == 1030, "the CAM row is walked when the CAM is on");
	mutex_unlock(&odi_switch_lock);
	CHECK(odi_mock_locks_idle(), "locks idle after the walk");
}

/* The table as ISP1 read it back, row by row, with four learned MACs
 * (image 618p2, diag l2-table get index 0..1023 and TABLE_STATUS after
 * each):
 *   row 0x06c  0x00002038 0x00000200 0x00000001  02:00:00:00:00:01 port 0 age 7
 *   row 0x270  0x0000203d 0x000e0200 0x00000002  02:00:00:00:00:02 port 2 VID 14 ctag
 *   row 0x364  0x00002038 0x00000200 0x00000003  02:00:00:00:00:03 port 0
 *   row 0x390  0x0000203e 0x00000200 0x00000004  02:00:00:00:00:04 port 3
 * (words 2, 1, 0), HIT set after each of those four reads and after no
 * other; every other row read 0x00002000 0x00000000 and its bucket. The
 * walk must give exactly the four back.
 */
static void test_stick_readout(void)
{
	static const struct {
		uint32_t row, w[3];
		uint8_t mac[6], port;
		uint16_t key;
		uint32_t flags;
	} e[] = {
		{ 0x06c, { 0x00000001U, 0x00000200U, 0x00002038U },
		  { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }, 0, 0, 0 },
		{ 0x270, { 0x00000002U, 0x000e0200U, 0x0000203dU },
		  { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 }, 2, 14, ODI_SW_L2_F_CTAG },
		{ 0x364, { 0x00000003U, 0x00000200U, 0x00002038U },
		  { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 }, 0, 0, 0 },
		{ 0x390, { 0x00000004U, 0x00000200U, 0x0000203eU },
		  { 0x02, 0x00, 0x00, 0x00, 0x00, 0x04 }, 3, 0, 0 },
	};
	struct odi_sw_l2_row r;
	uint32_t idx = 0;
	unsigned int n = 0, i;
	int rc;

	model_reset(1);
	for (i = 0; i < 4; i++) {
		memcpy(model_rows[e[i].row], e[i].w, sizeof(e[i].w));
		model_used[e[i].row] = 1;
	}
	mutex_lock(&odi_switch_lock);
	for (;;) {
		rc = odi_switch_l2_next(&idx, &r);
		if (rc)
			break;
		if (n < 4) {
			CHECK(idx == e[n].row, "the walk stops on a learned row");
			CHECK(memcmp(r.mac, e[n].mac, 6) == 0, "with its MAC");
			CHECK(r.type == ODI_SW_L2_UCAST && r.port == e[n].port &&
			      r.key == e[n].key && r.age == 7 &&
			      r.flags == (ODI_SW_L2_F_VALID | e[n].flags),
			      "port, VID, age 7, dynamic SVL");
		}
		n++;
		idx++;
	}
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == -2 && n == 4, "exactly the four learned rows, not 1024");
}

static void test_mode(void)
{
	model_reset(1);
	CHECK(odi_switch_l2_ipmc_mode() == 0, "stock L2_LOOKUP_SETUP 0x00600bb8: IPv4 multicast on MAC + VID");
	set_reg(ODI_SW_L2_LOOKUP_SETUP_OFF, 0x00e00bb8U);
	CHECK(odi_switch_l2_ipmc_mode() == 1, "bit 23 set: on the group address");
}

/* Add: START | METHOD 0 | write 1 << 3 | KIND 0 = 0x80000008, the three
 * encoded words in TABLE_WRITE_WORD[0..2] as they are.
 */
static void test_mcast_add_del(void)
{
	struct odi_sw_l2_mcast_req q = {
		{ 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 }, 10, 1, 0x1, 0,
	};
	uint32_t idx = 9999, idx2 = 9999, raw[3];
	int rc, found = -1;
	unsigned int n;

	model_reset(1);
	odi_switch_l2_mcast_encode(&q, 1, raw);
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_mcast_add(&q, &idx);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == 0, "multicast add succeeds");
	CHECK(model_last_cmd == 0x80000008U, "add TABLE_CMD == 0x80000008 (hash, write)");
	CHECK(memcmp(model_last_wr, raw, sizeof(raw)) == 0, "add writes the encoded words unreversed");
	CHECK(idx < 1024 && model_rows[idx][0] == 0x5e010203U, "add reports the row the entry landed on");

	/* The same key again, other members: the same row is updated. */
	q.ports = 0x5;
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_mcast_add(&q, &idx2);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == 0 && idx2 == idx && (model_rows[idx][2] & 0x3cU) == (0x5U << 2),
	      "re-adding a key updates its row in place");

	/* Fill the rest of that bucket with other keys that hash the same
	 * (the model bucket is the low MAC byte xor the key), then one more.
	 */
	for (n = 1; n <= 4; n++) {
		struct odi_sw_l2_mcast_req o = q;

		o.mac[4] = (uint8_t)(0x10 + n);	/* different MAC, same low byte */
		mutex_lock(&odi_switch_lock);
		rc = odi_switch_l2_mcast_add(&o, &idx2);
		mutex_unlock(&odi_switch_lock);
		if (n < 4)
			CHECK(rc == 0, "the bucket still has a free way");
		else
			CHECK(rc == -28, "a full bucket is ENOSPC, not a silent overwrite");
	}

	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_mcast_del(&q, &found);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == 0 && found == 1, "delete finds the key");
	CHECK(model_last_cmd == 0x80000008U, "delete ends with the same hash write");
	CHECK(model_last_wr[2] == 0 && model_last_wr[0] == 0x5e010203U,
	      "delete sends the key with valid clear");
	CHECK(!model_used[idx], "and the row is gone");
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_mcast_del(&q, &found);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == 0 && found == 0, "deleting an absent key is not an error, and says so");
	CHECK(model_last_cmd == 0x80000000U, "an absent key gets the hash lookup (read) and no write");

	/* A refused address never reaches the engine. */
	n = model_cmds;
	q.mac[0] = 0x00;
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_mcast_add(&q, &idx2);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == -22 && model_cmds == n, "a unicast MAC is refused before any register write");
	CHECK(odi_mock_locks_idle(), "locks idle after add and delete");
}

/* The static row for the stick's MAC, against two rows a stick read back:
 * - the stick's own MAC as the hardware learned it from the CPU port
 *   (docs/KERNEL.md readout, row 0x390: 0x0000203e 0x00000200 0x00000004,
 *   port 3, age 7, SVL, FID 0, VID 0). The static row must be that row
 *   word for word plus bit 62, so its key (and hash) is the learned one's;
 * - the static multicast row igmpd placed on ISP1 (0x00002004 0x40000100
 *   0x5e010203), which shows where bit 62 sits in word 1 on the hardware.
 */
static void test_ucast_static_encode(void)
{
	static const uint8_t stick[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x04 };
	static const uint8_t bad_mc[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
	static const uint8_t zero[6] = { 0 };
	uint32_t raw[3];

	CHECK(odi_switch_l2_ucast_static_encode(stick, 3, 1, raw) == 0, "encode the stick MAC on port 3");
	CHECK(raw[0] == 0x00000004U && raw[1] == (0x00000200U | 0x40000000U) && raw[2] == 0x0000203eU,
	      "the learned port-3 row plus bit 62 (static), bit 77 (valid) as written");
	CHECK((raw[1] & 0x40000000U) == (0x40000100U & 0x40000000U),
	      "bit 62 is where the stick showed it on igmpd's static row");
	{
		struct odi_sw_l2_row r;

		memcpy(r.raw, raw, sizeof(raw));
		odi_switch_l2_decode(&r);
		CHECK(r.type == ODI_SW_L2_UCAST && r.port == 3 && r.age != 0 &&
		      (r.flags & ODI_SW_L2_F_STATIC) && !(r.flags & ODI_SW_L2_F_IVL) &&
		      r.fid == 0 && r.key == 0 && memcmp(r.mac, stick, 6) == 0,
		      "decodes as static unicast, port 3, age non-zero, SVL, FID 0");
	}
	CHECK(odi_switch_l2_ucast_static_encode(stick, 3, 0, raw) == 0 &&
	      raw[0] == 0x00000004U && raw[1] == 0x00000200U && raw[2] == 0,
	      "a delete sends the key alone, valid clear");
	CHECK(odi_switch_l2_ucast_static_encode(bad_mc, 3, 1, raw) == -22, "a multicast MAC is refused");
	CHECK(odi_switch_l2_ucast_static_encode(zero, 3, 1, raw) == -22, "the zero MAC is refused");
	CHECK(odi_switch_l2_ucast_static_encode(stick, 4, 1, raw) == -22, "a port past 3 is refused");
}

/* Pinning through the model: the learned row of the stick's MAC (as the
 * stick showed it, port 3) is replaced in place by the static one; a MAC
 * change moves the pin; an unchanged set rewrites the same rows; a failed
 * write is retried by the next sync.
 */
static void test_pins_sync(void)
{
	static const uint8_t mac_a[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x04 };
	static const uint8_t mac_b[6] = { 0x02, 0x00, 0x00, 0x00, 0x11, 0x04 };
	static const uint32_t learned[3] = { 0x00000004U, 0x00000200U, 0x0000203eU };
	struct odi_sw_l2_pins pinned = { 0 }, want = { 0 };
	struct odi_sw_l2_pin_report rep;
	struct odi_sw_l2_row r;
	uint32_t row_a, i, before;
	int n, found = -1;

	model_reset(1);
	/* The learned row where the model hashes it: put it there by adding
	 * then overwriting the words with the stick's.
	 */
	mutex_lock(&odi_switch_lock);
	CHECK(odi_switch_l2_ucast_static_add(mac_a, 3, &row_a) == 0, "model: place the key");
	memcpy(model_rows[row_a], learned, sizeof(learned));
	CHECK(odi_switch_l2_read(row_a, &r) == 0 && !(r.flags & ODI_SW_L2_F_STATIC) && r.port == 3,
	      "the stick MAC starts as a learned row on port 3");

	CHECK(odi_switch_l2_pins_add(&want, mac_a) == 1, "want: the stick MAC");
	CHECK(odi_switch_l2_pins_add(&want, mac_a) == 0, "a duplicate is not added twice (eth0, eth0.2, br0)");
	odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
	CHECK(rep.failed == 0 && rep.added == 1U && rep.row[0] == row_a,
	      "pinned on the learned row's own row");
	CHECK(odi_switch_l2_read(row_a, &r) == 0 && (r.flags & ODI_SW_L2_F_STATIC) && r.port == 3,
	      "now static on port 3");
	for (n = 0, i = 0; i < MODEL_ROWS; i++)
		n += model_used[i];
	CHECK(n == 1, "one row for the MAC, not two");

	/* The same set again: rewritten in place, nothing reported as new. */
	before = model_cmds;
	odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
	CHECK(rep.added == 0 && rep.failed == 0 && rep.removed_n == 0 && rep.row[0] == row_a,
	      "an unchanged set: same row, no news");
	CHECK(model_cmds == before + 1, "one hash write");

	/* ELAN_MAC_ADDR changed: the old row goes, the new MAC is pinned. */
	memset(&want, 0, sizeof(want));
	odi_switch_l2_pins_add(&want, mac_b);
	odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
	CHECK(rep.removed_n == 1 && memcmp(rep.removed[0], mac_a, 6) == 0, "the old MAC reported removed");
	CHECK(odi_switch_l2_ucast_del(mac_a, &found) == 0 && found == 0, "and its key is no longer in the table");
	CHECK(rep.added == 1U && pinned.n == 1 && memcmp(pinned.mac[0], mac_b, 6) == 0, "the new MAC pinned");
	CHECK(odi_switch_l2_read(rep.row[0], &r) == 0 && (r.flags & ODI_SW_L2_F_STATIC) &&
	      memcmp(r.mac, mac_b, 6) == 0, "static, with the new MAC");

	/* Every device down: nothing wanted, the pin is removed. */
	memset(&want, 0, sizeof(want));
	odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
	CHECK(rep.removed_n == 1 && pinned.n == 0, "no device up: unpinned");
	CHECK(odi_switch_l2_ucast_del(mac_b, &found) == 0 && found == 0, "and absent from the table");

	/* A full bucket: the write fails, the MAC is not counted as pinned,
	 * and the next sync, with room again, pins it.
	 */
	model_reset(1);
	odi_switch_l2_pins_add(&want, mac_a);
	{
		uint32_t b = (mac_a[5] ^ 0U) & 0xffU, w;

		for (w = 0; w < 4; w++) {
			model_rows[b * 4U + w][0] = 0x000000a0U + w;	/* other keys, same bucket */
			model_used[b * 4U + w] = 1;
		}
		odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
		CHECK(rep.failed == 1U && rep.rc[0] == -28 && pinned.n == 0,
		      "a full bucket: reported failed, not pinned");
		model_used[b * 4U + 3U] = 0;
		odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
		CHECK(rep.failed == 0 && rep.added == 1U && pinned.n == 1, "retried by the next sync");
	}
	mutex_unlock(&odi_switch_lock);
	CHECK(odi_mock_locks_idle(), "locks idle after the pin syncs");
}

/* The stick's pin is a static row on the CPU port. With a model that, like
 * the hardware another implementation measured, removes a row only when
 * the whole delete word agrees, the old key-only word leaves it behind and
 * the own-words delete removes it.
 */
static void test_ucast_del_own_words(void)
{
	static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x04 };
	uint32_t raw[3], row;
	int found = -1;

	model_reset(1);
	model_strict = 1;
	mutex_lock(&odi_switch_lock);
	CHECK(odi_switch_l2_ucast_static_add(mac, 3, &row) == 0 && model_used[row], "pin placed");

	/* What the driver sent before: the key alone, valid clear. */
	CHECK(odi_switch_l2_ucast_static_encode(mac, 0, 0, raw) == 0, "key-only word");
	{
		unsigned int i;

		for (i = 0; i < 3; i++)
			odi_reg_write(ODI_SW_TABLE_WRITE_WORD(i), raw[i]);
		odi_reg_write(ODI_SW_TABLE_CMD_OFF, (1U << 31) | (ODI_SW_L2_METHOD_HASH << 4) | (1U << 3));
	}
	CHECK(model_used[row], "model: a key-only delete leaves the row behind the CPU port");

	CHECK(odi_switch_l2_ucast_del(mac, &found) == 0 && found == 1, "own-words delete reports the row found");
	CHECK(!model_used[row], "and the row is gone");
	CHECK(odi_switch_l2_ucast_del(mac, &found) == 0 && found == 0, "a second delete finds nothing and writes nothing");

	/* An engine that keeps the row: -5, never a quiet success. */
	CHECK(odi_switch_l2_ucast_static_add(mac, 3, &row) == 0, "pin placed again");
	model_refuse = 1;
	CHECK(odi_switch_l2_ucast_del(mac, &found) == -5 && model_used[row],
	      "a row that stays is reported, not assumed gone");
	{
		struct odi_sw_l2_pins pinned = { 0 }, want = { 0 };
		struct odi_sw_l2_pin_report rep;

		odi_switch_l2_pins_add(&pinned, mac);
		odi_switch_l2_pins_sync(&pinned, &want, 3, &rep);
		CHECK(rep.removed_n == 0 && rep.stuck_n == 1 && memcmp(rep.stuck[0], mac, 6) == 0,
		      "a pin sync names the MAC whose row did not go");
	}
	mutex_unlock(&odi_switch_lock);
}

/* Bring-up: whatever a previous run left, every kind of row, in the hash
 * and in the CAM, is gone, and the report counts it.
 */
static void test_clean(void)
{
	static const uint8_t pin_a[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x04 };
	static const uint8_t pin_b[6] = { 0x02, 0x00, 0x00, 0x00, 0x11, 0x05 };
	static const uint8_t grp[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 };
	struct odi_sw_l2_mcast_req q = { .ports = 0x5, .ivl = 1, .key = 10 };
	struct odi_sw_l2_clean_report rep;
	uint32_t row, cam = ODI_SW_L2_HASH_ROWS + 3U, i;
	int found = -1;

	memcpy(q.mac, grp, 6);
	model_reset(0);		/* CAM on */
	model_strict = 1;
	mutex_lock(&odi_switch_lock);
	CHECK(odi_switch_l2_clean(&rep) == 0 && rep.found == 0 && rep.left == 0 && rep.passes == 1,
	      "an empty table: nothing found, one pass");

	CHECK(odi_switch_l2_ucast_static_add(pin_a, 3, &row) == 0, "static pin a");
	CHECK(odi_switch_l2_ucast_static_add(pin_b, 3, &row) == 0, "static pin b");
	CHECK(odi_switch_l2_mcast_add(&q, &row) == 0, "static multicast group");
	/* A learned row (dynamic, port 0) and one in the CAM. */
	{
		static const uint32_t learned[3] = { 0x00000032U, 0x00000200U, 0x00002008U };
		static const uint32_t incam[3] = { 0x0000000cU, 0x00000200U, 0x0000203eU };

		memcpy(model_rows[200], learned, sizeof(learned));
		model_used[200] = 1;
		memcpy(model_rows[cam], incam, sizeof(incam));
		model_used[cam] = 1;
	}
	CHECK(odi_switch_l2_clean(&rep) == 0, "clean runs");
	CHECK(rep.found == 5 && rep.found_static == 3, "five rows found, three of them static");
	CHECK(rep.left == 0 && rep.passes == 2, "none left; a second pass confirmed it");
	for (i = 0; i < MODEL_ROWS; i++)
		if (model_used[i])
			break;
	CHECK(i == MODEL_ROWS, "no row in use, the CAM included");
	CHECK(odi_switch_l2_ucast_del(pin_a, &found) == 0 && found == 0, "the pin is gone");

	/* A row the engine will not delete is counted, and the boot goes on. */
	CHECK(odi_switch_l2_ucast_static_add(pin_a, 3, &row) == 0, "pin again");
	model_refuse = 1;
	CHECK(odi_switch_l2_clean(&rep) == 0 && rep.found == 1 && rep.left == 1 && rep.passes == 2,
	      "a stubborn row: reported left after two passes");
	mutex_unlock(&odi_switch_lock);
	CHECK(odi_mock_locks_idle(), "locks idle after the clean");
}

static void test_clean_busy(void)
{
	struct odi_sw_l2_clean_report rep;

	odi_mock_reset();
	odi_mock_write_hook = NULL;
	odi_mock.poll_stuck = 1;
	set_reg(ODI_SW_TABLE_STATUS_OFF, ODI_SW_TABLE_STATUS_IN_PROGRESS);
	mutex_lock(&odi_switch_lock);
	CHECK(odi_switch_l2_clean(&rep) == -1, "a busy engine stops the clean with -1");
	mutex_unlock(&odi_switch_lock);
}

static void test_busy(void)
{
	struct odi_sw_l2_row r;
	int rc;

	/* An engine that never finishes: TABLE_STATUS keeps IN_PROGRESS set,
	 * so the bounded wait must give up rather than spin forever, and no
	 * TABLE_CMD is fired.
	 */
	odi_mock_reset();
	odi_mock_write_hook = NULL;
	odi_mock.poll_stuck = 1;
	set_reg(ODI_SW_TABLE_STATUS_OFF, ODI_SW_TABLE_STATUS_IN_PROGRESS);
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_l2_read(0, &r);
	mutex_unlock(&odi_switch_lock);
	CHECK(rc == -1, "an engine that stays busy is -1");
	CHECK(reg(ODI_SW_TABLE_CMD_OFF) == 0, "and no command was fired into it");
}

/* The ioctl ABI: the kernel structs, at the offsets src/diag restates
 * (src/diag/test/odi_sw_ioctl_test.c asserts the same numbers on its
 * copy), and the request numbers composed from them.
 */
static void test_abi(void)
{
	CHECK(sizeof(struct odi_sw_l2_row) == 48, "struct odi_sw_l2_row is 48 bytes");
	CHECK(offsetof(struct odi_sw_l2_row, raw) == 4 &&
	      offsetof(struct odi_sw_l2_row, mac) == 16 &&
	      offsetof(struct odi_sw_l2_row, key) == 22 &&
	      offsetof(struct odi_sw_l2_row, type) == 24 &&
	      offsetof(struct odi_sw_l2_row, age) == 27 &&
	      offsetof(struct odi_sw_l2_row, flags) == 28 &&
	      offsetof(struct odi_sw_l2_row, group) == 44,
	      "struct odi_sw_l2_row field offsets");
	CHECK(sizeof(struct odi_sw_l2_mcast) == 28, "struct odi_sw_l2_mcast is 28 bytes");
	CHECK(offsetof(struct odi_sw_l2_mcast_req, key) == 6 &&
	      offsetof(struct odi_sw_l2_mcast_req, ivl) == 8 &&
	      offsetof(struct odi_sw_l2_mcast_req, ext_ports) == 16 &&
	      offsetof(struct odi_sw_l2_mcast, index) == 20 &&
	      offsetof(struct odi_sw_l2_mcast, found) == 24,
	      "struct odi_sw_l2_mcast field offsets");
	CHECK(sizeof(struct odi_sw_l2_mode) == 8, "struct odi_sw_l2_mode is 8 bytes");
	/* (dir << 29) | (size << 16) | ('S' << 8) | nr, MIPS dir bits. */
	CHECK(ODI_SW_IOC_L2_GET == ((6U << 29) | (48U << 16) | (0x53U << 8) | 6U), "L2_GET number");
	CHECK(ODI_SW_IOC_L2_NEXT == ((6U << 29) | (48U << 16) | (0x53U << 8) | 7U), "L2_NEXT number");
	CHECK(ODI_SW_IOC_L2_MC_ADD == ((6U << 29) | (28U << 16) | (0x53U << 8) | 8U), "L2_MC_ADD number");
	CHECK(ODI_SW_IOC_L2_MC_DEL == ((6U << 29) | (28U << 16) | (0x53U << 8) | 9U), "L2_MC_DEL number");
	CHECK(ODI_SW_IOC_L2_MODE == ((2U << 29) | (8U << 16) | (0x53U << 8) | 10U), "L2_MODE number");
}

int main(void)
{
	test_abi();
	test_decode_unicast();
	test_encode_mcast();
	test_ipmc_decode();
	test_mac_ok();
	test_read();
	test_walk();
	test_stick_readout();
	test_mode();
	test_mcast_add_del();
	test_ucast_static_encode();
	test_pins_sync();
	test_ucast_del_own_words();
	test_clean();
	test_clean_busy();
	test_busy();

	if (failures) {
		fprintf(stderr, "odi_switch_l2_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_switch_l2_test: all checks passed\n");
	return 0;
}
