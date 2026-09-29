/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_ramlog_test.c -- host-side unit test for odi_ramlog.c, unity-build
 * (#include the .c directly, the same posture test/odi_wdt_test.c uses
 * against odi_wdt.c): the on-memory format primitives against a pair of
 * on-stack byte arrays standing in for the two DRAM pages, then a
 * from-scratch reimplementation of tools/memprobe/ramlog-read.sh own
 * decode logic (reader_decode_a()/reader_decode_b() below) to prove the
 * bytes odi_ramlog_write() leaves behind are exactly what that reader
 * already parses -- write, wrap, read back with the same parser the
 * reader uses.
 */
#include <stdio.h>
#include <string.h>

#include "../kernel/extra/drivers/net/ethernet/odi/odi_ramlog.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_ramlog.c"
/* Only the constants: CONFIG_ODI_EARLY_CRUMBS is not defined on the host. */
#include "../kernel/extra/arch/mips/include/asm/mach-rtl8686/odi-early-crumb.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* ---- reader_decode_a()/reader_decode_b() --------------------------------
 *
 * tools/memprobe/ramlog-read.sh, restated in C rather than copied line for
 * line (that file is bash + inline python, this is plain C), but the
 * arithmetic is identical:
 *
 *   page A: a[16:16+min(na,4080)]
 *   page B: ring=b[16:16+4080]; if nb>4080: p=nb%4080; txt=ring[p:]+ring[:p]
 *           else: txt=ring[:nb]
 */
static unsigned int reader_decode_a(const unsigned char *page, char *out)
{
	uint32_t n = odi_ramlog_rd32(page, 4);
	unsigned int len = n < ODI_RAMLOG_DATA ? n : ODI_RAMLOG_DATA;

	memcpy(out, (const void *)(page + ODI_RAMLOG_HDR), len);
	return len;
}

static unsigned int reader_decode_b(const unsigned char *page, char *out)
{
	uint32_t n = odi_ramlog_rd32(page, 4);

	if (n > ODI_RAMLOG_DATA) {
		unsigned int p = n % ODI_RAMLOG_DATA;

		memcpy(out, (const void *)(page + ODI_RAMLOG_HDR + p), ODI_RAMLOG_DATA - p);
		memcpy(out + (ODI_RAMLOG_DATA - p), (const void *)(page + ODI_RAMLOG_HDR), p);
		return ODI_RAMLOG_DATA;
	}
	memcpy(out, (const void *)(page + ODI_RAMLOG_HDR), n);
	return n;
}

/* ---- Format primitives --------------------------------------------------- */

static void test_rd32_wr32_roundtrip_is_big_endian(void)
{
	unsigned char page[8] = { 0 };

	odi_ramlog_wr32(page, 0, 0x11223344U);
	CHECK(page[0] == 0x11 && page[1] == 0x22 && page[2] == 0x33 && page[3] == 0x44,
	      "wr32 stores big-endian bytes, matching ramlog-read.sh struct.unpack(\">I\")");
	CHECK(odi_ramlog_rd32(page, 0) == 0x11223344U, "rd32 is the exact inverse of wr32");
}

static void test_page_reset_sets_magic_count_tags(void)
{
	unsigned char page[ODI_RAMLOG_PAGE_SIZE];

	memset((void *)page, 0xaa, sizeof(page)); /* stale/garbage, as a real page could hold */
	odi_ramlog_page_reset(page, ODI_RAMLOG_MAGIC_A, ODI_RAMLOG_TAG_HEAD, ODI_RAMLOG_TAG_JSK);
	CHECK(odi_ramlog_rd32(page, 0) == ODI_RAMLOG_MAGIC_A, "page_reset stamps the magic");
	CHECK(odi_ramlog_rd32(page, 4) == 0, "page_reset zeroes count");
	CHECK(odi_ramlog_rd32(page, 8) == ODI_RAMLOG_TAG_HEAD, "page_reset stamps tag1");
	CHECK(odi_ramlog_rd32(page, 12) == ODI_RAMLOG_TAG_JSK, "page_reset stamps tag2");
	CHECK(page[ODI_RAMLOG_HDR] == 0xaa, "page_reset does not touch the data region");
}

static void test_boot_stamp_matches_head_jsk_layout(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];

	odi_ramlog_boot_stamp(page_a, page_b);
	CHECK(odi_ramlog_rd32(page_a, 0) == ODI_RAMLOG_MAGIC_A, "boot_stamp: page A magic RLGA");
	CHECK(odi_ramlog_rd32(page_a, 8) == ODI_RAMLOG_TAG_HEAD, "boot_stamp: page A HEAD tag at +8");
	CHECK(odi_ramlog_rd32(page_a, 12) == ODI_RAMLOG_TAG_JSK, "boot_stamp: page A JSK tag at +12");
	CHECK(odi_ramlog_rd32(page_b, 0) == ODI_RAMLOG_MAGIC_B, "boot_stamp: page B magic RLGB");
	CHECK(odi_ramlog_rd32(page_b, 4) == 0, "boot_stamp: page B count starts at 0");
}

/* ---- odi_ramlog_write() -- append, cap, wrap ----------------------------- */

static void test_write_short_line_readable_on_both_pages(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];
	char out[ODI_RAMLOG_DATA];
	const char *line = "odi_ramlog: console registered\n";
	unsigned int len = (unsigned int)strlen(line);

	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_write(page_a, page_b, line, len);

	CHECK(odi_ramlog_rd32(page_a, 4) == len, "page A count advances by the written length");
	CHECK(reader_decode_a(page_a, out) == len && memcmp(out, line, len) == 0,
	      "page A reader decode returns the line unchanged");
	CHECK(odi_ramlog_rd32(page_b, 4) == len, "page B count advances by the written length");
	CHECK(reader_decode_b(page_b, out) == len && memcmp(out, line, len) == 0,
	      "page B reader decode returns the line unchanged (no wrap yet)");
	/* HEAD/JSK untouched by an ordinary write -- only boot_stamp()/a stale-magic
	 * lazy re-init ever rewrite them. */
	CHECK(odi_ramlog_rd32(page_a, 8) == ODI_RAMLOG_TAG_HEAD, "HEAD tag survives a normal write");
	CHECK(odi_ramlog_rd32(page_a, 12) == ODI_RAMLOG_TAG_JSK, "JSK tag survives a normal write");
}

static void test_multiple_writes_concatenate_in_order(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];
	char out[ODI_RAMLOG_DATA];
	unsigned int len;

	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_write(page_a, page_b, "line one\n", 9);
	odi_ramlog_write(page_a, page_b, "line two\n", 9);
	odi_ramlog_write(page_a, page_b, "line three\n", 11);

	len = reader_decode_a(page_a, out);
	CHECK(len == 29, "page A count is the sum of three writes (9+9+11)");
	CHECK(memcmp(out, "line one\nline two\nline three\n", 29) == 0,
	      "page A holds the three lines concatenated, in write order");
}

/* Page A: capped at ODI_RAMLOG_DATA, then frozen -- bytes past the cap are
 * dropped, count stops advancing, whatever was captured up to the cap stays
 * exactly as it was.
 */
static void test_a_append_freezes_at_capacity(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];
	char big[ODI_RAMLOG_DATA + 500];
	char out[ODI_RAMLOG_DATA];
	unsigned int i;

	for (i = 0; i < sizeof(big); i++)
		big[i] = (char)('A' + (i % 26));

	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_write(page_a, page_b, big, sizeof(big));

	CHECK(odi_ramlog_rd32(page_a, 4) == ODI_RAMLOG_A_DATA,
	      "page A count freezes at ODI_RAMLOG_A_DATA once the page is full");
	/* reader_decode_a() is the pre-metadata reader, min(count, 4080): it
	 * still gets exactly the text, because count never passes 4016. */
	CHECK(reader_decode_a(page_a, out) == ODI_RAMLOG_A_DATA &&
	      memcmp(out, big, ODI_RAMLOG_A_DATA) == 0,
	      "page A holds exactly the first ODI_RAMLOG_A_DATA bytes, nothing past the cap");

	/* A second write after the page is already full changes nothing further. */
	odi_ramlog_write(page_a, page_b, "more\n", 5);
	CHECK(odi_ramlog_rd32(page_a, 4) == ODI_RAMLOG_A_DATA,
	      "page A count does not move again once frozen");
}

/* Page B: a ring of the last ODI_RAMLOG_DATA bytes, count advancing without
 * limit -- write more than the page holds and confirm the reader
 * reconstruction returns exactly the tail of what was written, in order.
 */
static void test_b_append_wraps_and_reader_reconstructs_tail(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];
	char big[ODI_RAMLOG_DATA + 1500];
	char out[ODI_RAMLOG_DATA];
	unsigned int i, len;

	for (i = 0; i < sizeof(big); i++)
		big[i] = (char)('a' + (i % 26));

	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_write(page_a, page_b, big, sizeof(big));

	CHECK(odi_ramlog_rd32(page_b, 4) == sizeof(big),
	      "page B count is the full write length, unbounded (unlike page A)");
	len = reader_decode_b(page_b, out);
	CHECK(len == ODI_RAMLOG_DATA, "reader_decode_b returns a full ring once count exceeds it");
	CHECK(memcmp(out, big + (sizeof(big) - ODI_RAMLOG_DATA), ODI_RAMLOG_DATA) == 0,
	      "reader_decode_b reconstructs exactly the last ODI_RAMLOG_DATA bytes written, in order");
}

/* Several separate writes that cross the wrap boundary, not one big one --
 * the ring math (count % ODI_RAMLOG_DATA) has to hold across calls, the way
 * separate printk lines do on the real driver.
 */
static void test_b_wraps_across_several_writes(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];
	char expect[ODI_RAMLOG_DATA + 300];
	char out[ODI_RAMLOG_DATA];
	unsigned int total = 0;
	int line = 0;

	odi_ramlog_boot_stamp(page_a, page_b);
	while (total < sizeof(expect)) {
		char buf[40];
		int n = snprintf(buf, sizeof(buf), "line %04d 0123456789abcdef\n", line++);
		unsigned int take = (unsigned int)n;

		if (total + take > sizeof(expect))
			take = (unsigned int)(sizeof(expect) - total);
		memcpy(expect + total, buf, take);
		odi_ramlog_write(page_a, page_b, buf, take);
		total += take;
	}

	CHECK(odi_ramlog_rd32(page_b, 4) == total, "page B count matches the total bytes written across many calls");
	CHECK(reader_decode_b(page_b, out) == ODI_RAMLOG_DATA &&
	      memcmp(out, expect + (total - ODI_RAMLOG_DATA), ODI_RAMLOG_DATA) == 0,
	      "ring reconstruction across many small writes matches one big write of the same bytes");
}

/* A cold power cycle wipes DRAM;
 * the preloader fills it with 0x55, not the RLGA/RLGB magic -- the next
 * write must re-stamp rather than append onto garbage.
 */
static void test_ensure_reinits_on_cold_wipe(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];
	char out[ODI_RAMLOG_DATA];

	memset((void *)page_a, 0x55, sizeof(page_a));
	memset((void *)page_b, 0x55, sizeof(page_b));
	odi_ramlog_write(page_a, page_b, "fresh\n", 6);

	CHECK(odi_ramlog_rd32(page_a, 0) == ODI_RAMLOG_MAGIC_A, "a write on cold-wiped DRAM re-stamps page A own magic");
	CHECK(reader_decode_a(page_a, out) == 6 && memcmp(out, "fresh\n", 6) == 0,
	      "the re-stamped page starts a clean log, not the 0x55 filler");
	CHECK(odi_ramlog_rd32(page_b, 0) == ODI_RAMLOG_MAGIC_B, "a write on cold-wiped DRAM re-stamps page B own magic too");
}

/* A watchdog reset, unlike a cold power cycle, does NOT wipe DRAM: the
 * next boot own odi_ramlog_boot_stamp() deliberately resets the
 * magic anyway so this boot own log starts fresh rather than
 * appending onto the previous, already-read, trial own bytes.
 */
static void test_boot_stamp_starts_fresh_even_with_valid_prior_content(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];

	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_write(page_a, page_b, "previous trial own boot log\n", 27);
	CHECK(odi_ramlog_rd32(page_a, 4) == 27, "sanity: the previous trial did leave 27 bytes");

	odi_ramlog_boot_stamp(page_a, page_b); /* next boot */
	CHECK(odi_ramlog_rd32(page_a, 4) == 0, "the next boot own stamp resets page A count to 0");
	CHECK(odi_ramlog_rd32(page_b, 4) == 0, "the next boot own stamp resets page B count to 0");
}

/* ---- Boot metadata, previous-boot copy, render ------------------------- */

static void test_layout_constants_agree(void)
{
	CHECK(ODI_RAMLOG_META_OFF == 4032 && ODI_RAMLOG_REASON_OFF == 4000 && ODI_RAMLOG_A_DATA == 3984,
	      "metadata block is the last 64 bytes of page A, the reason block the 32 before, text capped at 3984");
	CHECK(ODI_RAMLOG_HDR + ODI_RAMLOG_A_DATA == ODI_RAMLOG_REASON_OFF &&
	      ODI_RAMLOG_REASON_OFF + ODI_RAMLOG_REASON_SIZE == ODI_RAMLOG_META_OFF,
	      "page A text ends where the reason block starts, which ends where the metadata block starts");
	CHECK(ODI_RAMLOG_A_DATA_FMT1 == 4016, "a format 1 page A ran text to 4016 bytes");
	CHECK(ODI_CRUMB_STASH == ODI_RAMLOG_PAGE_A_KSEG1 + ODI_RAMLOG_META_OFF + ODI_RAMLOG_M_CRUMB_TAG,
	      "the asm crumb stash address is the metadata crumb tag word");
	CHECK(ODI_CRUMB_PAGE_B == ODI_RAMLOG_PAGE_B_KSEG1, "crumbs and ramlog agree on page B");
}

static void test_parse_slot_last_root_wins(void)
{
	CHECK(odi_ramlog_parse_slot("console=ttyS0 root=31:5 mtdparts=x") == 0, "root=31:5 is slot 0");
	CHECK(odi_ramlog_parse_slot("root=31:7") == 1, "root=31:7 is slot 1");
	CHECK(odi_ramlog_parse_slot("console=ttyS0,115200 root=31:5 mtdparts=a root=31:7 x=1") == 1,
	      "BUILTIN_EXTEND line: built-in root=31:5 first, bootloader root=31:7 last, slot 1");
	CHECK(odi_ramlog_parse_slot("root=31:7 root=31:5") == 0, "the last root= wins either way");
	CHECK(odi_ramlog_parse_slot("root=/dev/mtdblock7") == 1, "the /dev/mtdblock form");
	CHECK(odi_ramlog_parse_slot("nfsroot=31:7 console=ttyS0") == ODI_RAMLOG_SLOT_UNKNOWN,
	      "nfsroot= is not root=");
	CHECK(odi_ramlog_parse_slot("root=31:5 root=/dev/sda1") == ODI_RAMLOG_SLOT_UNKNOWN,
	      "a last root= naming neither slot is unknown, not the earlier one");
	CHECK(odi_ramlog_parse_slot("") == ODI_RAMLOG_SLOT_UNKNOWN, "empty line");
	CHECK(odi_ramlog_parse_slot(NULL) == ODI_RAMLOG_SLOT_UNKNOWN, "no line");
}

static void test_meta_stamp_counts_boots(void)
{
	unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char *m = page_a + ODI_RAMLOG_META_OFF;
	char big[ODI_RAMLOG_DATA + 10];
	unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];

	memset((void *)page_a, 0x55, sizeof(page_a)); /* cold start filler */
	CHECK(odi_ramlog_meta_stamp(page_a, 0, "odi-oss-260924-618k1") == 1,
	      "invalid magic (power cycle): the counter starts at 1");
	CHECK(odi_ramlog_rd32(m, ODI_RAMLOG_M_MAGIC) == ODI_RAMLOG_MAGIC_M, "RLGM magic");
	CHECK(odi_ramlog_rd32(m, ODI_RAMLOG_M_SLOT) == 0, "slot stored");
	CHECK(odi_ramlog_rd32(m, ODI_RAMLOG_M_FMT) == ODI_RAMLOG_META_FMT, "format stored");
	CHECK(memcmp((const void *)(m + ODI_RAMLOG_M_BUILD), "odi-oss-260924-618k1", 21) == 0,
	      "build id stored with its NUL");
	CHECK(m[ODI_RAMLOG_M_BUILD + ODI_RAMLOG_BUILD_LEN - 1] == 0, "build id NUL padded");
	CHECK(m[ODI_RAMLOG_M_CRUMB_TAG] == 0x55, "the stash words are not the driver's to write");
	CHECK(odi_ramlog_rd32(page_a, ODI_RAMLOG_REASON_OFF + ODI_RAMLOG_R_MAGIC) == ODI_RAMLOG_MAGIC_R &&
	      odi_ramlog_rd32(page_a, ODI_RAMLOG_REASON_OFF + ODI_RAMLOG_R_CODE) == ODI_RAMLOG_REASON_NONE &&
	      page_a[ODI_RAMLOG_REASON_OFF + ODI_RAMLOG_R_DETAIL] == 0,
	      "the stamp starts the reason block empty");

	/* A whole boot's worth of text does not reach the block. */
	memset(big, 'x', sizeof(big));
	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_write(page_a, page_b, big, sizeof(big));
	CHECK(odi_ramlog_meta_stamp(page_a, 1, "b") == 2, "a warm reset: previous counter + 1");
	CHECK(odi_ramlog_rd32(m, ODI_RAMLOG_M_SLOT) == 1, "slot updated");
	CHECK(m[ODI_RAMLOG_M_BUILD] == 'b' && m[ODI_RAMLOG_M_BUILD + 1] == 0,
	      "a shorter build id leaves no tail of the longer one");

	{
		char longid[80];

		memset(longid, 'L', sizeof(longid) - 1);
		longid[sizeof(longid) - 1] = 0;
		odi_ramlog_meta_stamp(page_a, 1, longid);
		CHECK(m[ODI_RAMLOG_M_BUILD + ODI_RAMLOG_BUILD_LEN - 2] == 'L' &&
		      m[ODI_RAMLOG_M_BUILD + ODI_RAMLOG_BUILD_LEN - 1] == 0,
		      "a long build id is cut to 39 characters and stays terminated");
	}
}

struct render_buf {
	char s[3 * ODI_RAMLOG_PAGE_SIZE];
	unsigned int len;
};

static void render_emit(void *ctx, const char *s, unsigned int len)
{
	struct render_buf *r = ctx;

	if (r->len + len < sizeof(r->s)) {
		memcpy(r->s + r->len, s, len);
		r->len += len;
		r->s[r->len] = 0;
	}
}

/* One boot, then the next boot's kernel_entry stash and save, the way the
 * kernel runs them: the saved copy carries the previous boot's crumb and
 * metadata, and renders to its head, tail and a metadata line.
 */
static void test_save_prev_and_render_previous_boot(void)
{
	static unsigned char page_a[ODI_RAMLOG_PAGE_SIZE], page_b[ODI_RAMLOG_PAGE_SIZE];
	static unsigned char prev[ODI_RAMLOG_PREV_SIZE];
	static struct render_buf r;
	char expect[ODI_RAMLOG_DATA + 2000];
	unsigned int total = 0, i;
	int line = 0;

	/* Boot 1 (slot 1): a log long enough to wrap page B, then a last crumb. */
	memset((void *)page_a, 0x55, sizeof(page_a));
	memset((void *)page_b, 0x55, sizeof(page_b));
	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_meta_stamp(page_a, 1, "odi-oss-260924-618k1");
	while (total + 40 < sizeof(expect)) {
		char buf[40];
		int n = snprintf(buf, sizeof(buf), "line %04d 0123456789abcdef\n", line++);

		memcpy(expect + total, buf, (size_t)n);
		odi_ramlog_write(page_a, page_b, buf, (unsigned int)n);
		total += (unsigned int)n;
	}
	odi_ramlog_wr32(page_b, 8, 0x5449434bU);	/* "TICK" */
	odi_ramlog_wr32(page_b, 12, 4242);

	/* Boot 2: kernel_entry_setup moves the crumb to the stash, then K1EN. */
	for (i = 0; i < 8; i++)
		page_a[ODI_RAMLOG_META_OFF + ODI_RAMLOG_M_CRUMB_TAG + i] = page_b[8 + i];
	odi_ramlog_wr32(page_b, 8, 0x4b31454eU);	/* "K1EN" */
	odi_ramlog_wr32(page_b, 12, 1);

	odi_ramlog_save_prev(prev, page_a, page_b, 1);
	CHECK(odi_ramlog_rd32(prev + ODI_RAMLOG_PAGE_SIZE, 8) == 0x5449434bU &&
	      odi_ramlog_rd32(prev + ODI_RAMLOG_PAGE_SIZE, 12) == 4242,
	      "the saved page B carries the previous boot's crumb, not K1EN");
	CHECK(memcmp(prev + ODI_RAMLOG_PAGE_SIZE + ODI_RAMLOG_HDR,
		     (const void *)(page_b + ODI_RAMLOG_HDR), ODI_RAMLOG_DATA) == 0,
	      "the saved page B ring is byte for byte the page");
	odi_ramlog_boot_stamp(page_a, page_b);
	CHECK(odi_ramlog_meta_stamp(page_a, 0, "next") == 2, "boot 2 counts on from boot 1");
	CHECK(odi_ramlog_rd32(prev, 4) == ODI_RAMLOG_A_DATA, "the copy survives this boot's stamp");

	odi_ramlog_render(prev, 2, 0, render_emit, &r);
	CHECK(strstr(r.s, "this boot: boot=2 slot=0\n") == r.s, "first line: the running boot");
	CHECK(strstr(r.s, "previous boot: boot=1 slot=1 build=odi-oss-260924-618k1 crumb=TICK/4242 reason=unknown\n") != NULL,
	      "second line: the previous boot's metadata, last crumb, and no recorded reason");
	{
		const char *ha = strstr(r.s, "---- page A: first 3984 bytes ----\n");
		const char *hb = strstr(r.s, "---- page B: last 4080 of ");

		CHECK(ha && memcmp(ha + strlen("---- page A: first 3984 bytes ----\n"), expect,
				   ODI_RAMLOG_A_DATA) == 0,
		      "page A section is the head of the previous log");
		CHECK(hb != NULL, "page B section present");
		if (hb) {
			const char *tb = strchr(hb, '\n') + 1;

			CHECK(memcmp(tb, expect + total - ODI_RAMLOG_DATA, ODI_RAMLOG_DATA) == 0,
			      "page B section is the tail, in order");
			CHECK(tb + ODI_RAMLOG_DATA == r.s + r.len, "nothing after the tail (it ends in a newline)");
		}
	}
}

static void test_render_no_previous_and_older_image(void)
{
	static unsigned char prev[ODI_RAMLOG_PREV_SIZE];
	static struct render_buf r;
	unsigned char *a = prev, *b = prev + ODI_RAMLOG_PAGE_SIZE;

	memset(prev, 0x55, sizeof(prev));
	odi_ramlog_render(prev, 1, ODI_RAMLOG_SLOT_UNKNOWN, render_emit, &r);
	CHECK(strstr(r.s, "this boot: boot=1 slot=?\n") == r.s, "unknown slot prints as ?");
	CHECK(strstr(r.s, "previous boot: none (page A magic 0x55555555, page B magic 0x55555555) reason=power\n") != NULL,
	      "cold start: no previous boot, why, and the reason a power cycle");
	CHECK(strstr(r.s, "---- page") == NULL, "no page sections without a valid page");

	/* An older image: text up to 4080 bytes in page A, no metadata block,
	 * zeroed crumb words, and a last line without a newline. */
	memset(prev, 'y', sizeof(prev));
	odi_ramlog_page_reset(a, ODI_RAMLOG_MAGIC_A, ODI_RAMLOG_TAG_HEAD, ODI_RAMLOG_TAG_JSK);
	odi_ramlog_wr32(a, 4, ODI_RAMLOG_DATA);
	odi_ramlog_page_reset(b, ODI_RAMLOG_MAGIC_B, 0, 0);
	odi_ramlog_wr32(b, 4, 5);
	r.len = 0;
	odi_ramlog_render(prev, 3, 1, render_emit, &r);
	CHECK(strstr(r.s, "previous boot: no metadata block (an older image) crumb=0x00000000/0 reason=unknown\n") != NULL,
	      "older image: said so, crumb in hex, reason unknown");
	CHECK(strstr(r.s, "---- page A: first 4080 bytes ----\n") != NULL,
	      "older image: page A text runs to 4080 bytes");
	CHECK(strstr(r.s, "---- page B: last 5 of 5 bytes ----\nyyyyy\n") != NULL,
	      "a short unwrapped ring, newline added after it");
}

/* Boot 1 of this image with the reason block, then boot 2 renders it. */
static const char *render_after(uint32_t code, const char *detail, int fmt)
{
	static unsigned char page_a[ODI_RAMLOG_PAGE_SIZE], page_b[ODI_RAMLOG_PAGE_SIZE];
	static unsigned char prev[ODI_RAMLOG_PREV_SIZE];
	static struct render_buf r;
	const char *line;

	memset((void *)page_a, 0x55, sizeof(page_a));
	memset((void *)page_b, 0x55, sizeof(page_b));
	odi_ramlog_boot_stamp(page_a, page_b);
	odi_ramlog_meta_stamp(page_a, 1, "v1.0.9");
	odi_ramlog_write(page_a, page_b, "hello\n", 6);
	if (fmt == 1)
		odi_ramlog_wr32(page_a, ODI_RAMLOG_META_OFF + ODI_RAMLOG_M_FMT, 1);
	if (code != ODI_RAMLOG_REASON_NONE)
		odi_ramlog_reason_set(page_a, code, detail);
	odi_ramlog_save_prev(prev, page_a, page_b, 0);
	r.len = 0;
	r.s[0] = 0;
	odi_ramlog_render(prev, 2, 1, render_emit, &r);
	line = strstr(r.s, "previous boot: ");
	return line ? line : "";
}

static int line_is(const char *got, const char *want)
{
	size_t n = strlen(want);

	return strncmp(got, want, n) == 0 && got[n] == '\n';
}

static void test_reason_recorded_and_rendered(void)
{
	static const struct { uint32_t code; const char *detail, *want; } cases[] = {
		{ ODI_RAMLOG_REASON_WDT_CLIENT, "omcid", "wdt_client:omcid" },
		{ ODI_RAMLOG_REASON_WDT_MEM, NULL, "wdt_mem" },
		{ ODI_RAMLOG_REASON_WDT_USERLAND, NULL, "wdt_userland" },
		{ ODI_RAMLOG_REASON_REBOOT, NULL, "reboot" },
		{ ODI_RAMLOG_REASON_HALT, NULL, "halt" },
		{ ODI_RAMLOG_REASON_POWEROFF, NULL, "poweroff" },
		{ ODI_RAMLOG_REASON_PANIC, NULL, "panic" },
		{ ODI_RAMLOG_REASON_OOPS, NULL, "oops" },
		{ 99, NULL, "unknown" },
		{ ODI_RAMLOG_REASON_WDT_CLIENT, "bad name=x", "wdt_client:bad_name_x" },
		{ ODI_RAMLOG_REASON_WDT_CLIENT, "", "wdt_client:?" },
		{ ODI_RAMLOG_REASON_WDT_CLIENT, "0123456789abcdefXYZ", "wdt_client:0123456789abcde" },
	};
	char want[160];
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		snprintf(want, sizeof(want),
			 "previous boot: boot=1 slot=1 build=v1.0.9 crumb=0x00000000/0 reason=%s",
			 cases[i].want);
		CHECK(line_is(render_after(cases[i].code, cases[i].detail, 2), want), cases[i].want);
	}
	CHECK(line_is(render_after(ODI_RAMLOG_REASON_NONE, NULL, 2),
		      "previous boot: boot=1 slot=1 build=v1.0.9 crumb=0x00000000/0 reason=unknown"),
	      "nothing recorded: unknown");
	CHECK(line_is(render_after(ODI_RAMLOG_REASON_PANIC, NULL, 1),
		      "previous boot: boot=1 slot=1 build=v1.0.9 crumb=0x00000000/0 reason=unknown"),
	      "a format 1 block has no reason, whatever bytes sit where format 2 keeps one");
}

static void test_reason_last_writer_wins_and_needs_a_stamp(void)
{
	static unsigned char page_a[ODI_RAMLOG_PAGE_SIZE];
	unsigned char *r = page_a + ODI_RAMLOG_REASON_OFF;

	memset((void *)page_a, 0x55, sizeof(page_a));
	odi_ramlog_reason_set(page_a, ODI_RAMLOG_REASON_PANIC, NULL);
	CHECK(odi_ramlog_rd32(r, ODI_RAMLOG_R_CODE) == 0x55555555U,
	      "no metadata block yet: nothing is written");

	odi_ramlog_meta_stamp(page_a, 0, "b");
	odi_ramlog_reason_set(page_a, ODI_RAMLOG_REASON_OOPS, NULL);
	odi_ramlog_reason_set(page_a, ODI_RAMLOG_REASON_WDT_CLIENT, "omcid");
	odi_ramlog_reason_set(page_a, ODI_RAMLOG_REASON_PANIC, NULL);
	CHECK(odi_ramlog_rd32(r, ODI_RAMLOG_R_CODE) == ODI_RAMLOG_REASON_PANIC && r[ODI_RAMLOG_R_DETAIL] == 0,
	      "the last reason wins and clears an earlier detail");

	odi_ramlog_reason_set(page_a, ODI_RAMLOG_REASON_WDT_CLIENT, "omcid");
	CHECK(odi_ramlog_meta_stamp(page_a, 0, "b") == 2, "the next boot counts on");
	CHECK(odi_ramlog_rd32(r, ODI_RAMLOG_R_CODE) == ODI_RAMLOG_REASON_NONE && r[ODI_RAMLOG_R_DETAIL] == 0,
	      "the next boot own stamp clears the reason");

	{
		char big[ODI_RAMLOG_DATA + 10];
		unsigned char page_b[ODI_RAMLOG_PAGE_SIZE];

		odi_ramlog_reason_set(page_a, ODI_RAMLOG_REASON_REBOOT, NULL);
		memset(big, 'x', sizeof(big));
		odi_ramlog_write(page_a, page_b, big, sizeof(big));
		CHECK(odi_ramlog_rd32(r, ODI_RAMLOG_R_MAGIC) == ODI_RAMLOG_MAGIC_R &&
		      odi_ramlog_rd32(r, ODI_RAMLOG_R_CODE) == ODI_RAMLOG_REASON_REBOOT,
		      "a full page of text does not reach the reason block");
	}
}

int main(void)
{
	test_rd32_wr32_roundtrip_is_big_endian();
	test_page_reset_sets_magic_count_tags();
	test_boot_stamp_matches_head_jsk_layout();

	test_write_short_line_readable_on_both_pages();
	test_multiple_writes_concatenate_in_order();
	test_a_append_freezes_at_capacity();
	test_b_append_wraps_and_reader_reconstructs_tail();
	test_b_wraps_across_several_writes();

	test_ensure_reinits_on_cold_wipe();
	test_boot_stamp_starts_fresh_even_with_valid_prior_content();

	test_layout_constants_agree();
	test_parse_slot_last_root_wins();
	test_meta_stamp_counts_boots();
	test_save_prev_and_render_previous_boot();
	test_render_no_previous_and_older_image();
	test_reason_recorded_and_rendered();
	test_reason_last_writer_wins_and_needs_a_stamp();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_ramlog_test: ok\n");
	return 0;
}
