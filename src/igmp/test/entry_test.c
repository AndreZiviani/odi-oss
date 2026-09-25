/* The 36-byte L2 multicast entry, byte by byte.
 *
 * Nothing here talks to the driver -- that cannot be tested without a stick --
 * so what is pinned is the one thing that can be: that the bytes land at the
 * offsets a vendor caller was observed to write, and that nothing else moves.
 *
 * Checking the whole 36 bytes rather than the five fields is the point. A
 * field written at the wrong offset still passes a test that only reads the
 * offset it wrote, and an entry with a stray byte in it is the failure that
 * forwards multicast to the wrong port.
 */
#include "entry.h"
#include "io.h"

static int failures;

static void ok(int cond, const char *what)
{
	if (cond) {
		out_fmt("ok    %s\n", what);
	} else {
		out_fmt("FAIL  %s\n", what);
		failures++;
	}
}

/* Every byte except the ones named, which must be zero. */
static int only(const uint8_t *b, const int *keep, int nkeep)
{
	for (int i = 0; i < 36; i++) {
		int named = 0;

		for (int k = 0; k < nkeep; k++)
			if (keep[k] == i)
				named = 1;
		if (!named && b[i] != 0) {
			out_fmt("      byte %d is 0x%x, expected 0\n", i, b[i]);
			return 0;
		}
	}
	return 1;
}

int main(void)
{
	uint32_t e[IGMP_MAC_ENTRY_WORDS];
	uint8_t *b = (uint8_t *)e;
	uint8_t mac[6];

	/* 239.1.2.3 -- the low 23 bits, so the leading 1 of 239 is dropped. */
	igmp_group_mac(mac, 0xef010203);
	ok(mac[0] == 0x01 && mac[1] == 0x00 && mac[2] == 0x5e,
	   "the multicast MAC starts 01:00:5e");
	ok(mac[3] == 0x01 && mac[4] == 0x02 && mac[5] == 0x03,
	   "and carries the low 23 bits");
	/* 224.1.2.3 and 225.1.2.3 differ in bit 23, which is the bit dropped.
	 * They share a MAC address, and a MAC-keyed snooper cannot tell them
	 * apart. Asserted so nobody later mistakes it for a bug here. */
	{
		uint8_t m2[6];

		igmp_group_mac(mac, 0xe0010203);
		igmp_group_mac(m2, 0xe1010203);
		ok(mac[3] == m2[3] && mac[4] == m2[4] && mac[5] == m2[5],
		   "224.1.2.3 and 225.1.2.3 map to the same MAC, as the hardware does");
	}

	/* The full entry, vid applying. */
	igmp_mac_entry(e, 0x0064, 0xef010203, 0x0000000a, 1);
	ok(b[0] == 0x00 && b[1] == 0x64, "the vid is a big-endian halfword at +0");
	ok(b[2] == 0x01 && b[3] == 0x00 && b[4] == 0x5e &&
	   b[5] == 0x01 && b[6] == 0x02 && b[7] == 0x03,
	   "the MAC is six bytes at +2, not three halfwords somewhere else");
	ok(b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0,
	   "+8 is left zero, which is the only value it was seen to hold");
	ok(b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 0x0a,
	   "the port mask is a big-endian word at +12");
	ok(b[24] == 0 && b[25] == 0 && b[26] == 0 && b[27] == IGMP_ENTRY_F_VID,
	   "the flags word at +24 has bit 1 set when the vid applies");
	{
		static const int keep[] = {1, 2, 3, 4, 5, 6, 7, 15, 27};
		ok(only(b, keep, 9), "and nothing else in the 36 bytes is touched");
	}

	/* ipmcMode not 0: no vid, and the flag goes with it. */
	igmp_mac_entry(e, 0x0064, 0xef010203, 0x0000000a, 0);
	ok(b[0] == 0 && b[1] == 0, "with the vid not applying the field is zero");
	ok(b[27] == 0, "and the flag is clear, not left over from the vid case");
	ok(b[15] == 0x0a, "the port mask is unaffected by the mode");

	/* A delete passes no mask. The vendor never writes +12 on that path,
	 * and a zeroed entry is the same bytes -- which is what makes one
	 * builder correct for both. */
	igmp_mac_entry(e, 0x0064, 0xef010203, 0, 1);
	ok(b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 0,
	   "a delete entry carries no port mask");
	ok(b[2] == 0x01 && b[7] == 0x03 && b[1] == 0x64,
	   "and still carries the key: the vid and the MAC");

	/* A full 32-bit mask, to catch a builder that truncated to a byte. */
	igmp_mac_entry(e, 1, 0xe0000001, 0xdeadbeef, 1);
	ok(b[12] == 0xde && b[13] == 0xad && b[14] == 0xbe && b[15] == 0xef,
	   "a wide port mask survives all four bytes");

	/* The entry is nine words, and the accessor reads it as words. If the
	 * byte writes above landed in the wrong word the wrapper would stage
	 * the wrong offsets. */
	igmp_mac_entry(e, 0x0102, 0xe0000001, 0x00030004, 1);
	ok(e[0] == 0x01020100 && e[3] == 0x00030004 && e[6] == IGMP_ENTRY_F_VID,
	   "read back as words, the fields are in words 0, 3 and 6");

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
