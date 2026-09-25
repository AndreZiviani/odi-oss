/* The L2 multicast request igmpd hands /dev/odi_sw, byte by byte, and the
 * command-line number parsers the one-shot mode uses.
 *
 * Nothing here talks to the driver -- that cannot be tested without a stick
 * (the kernel side has its own host test against a model of the table) -- so
 * what is pinned is the request: every byte of the 28, not only the fields
 * written. A field at the wrong offset still passes a test that only reads the
 * offset it wrote, and a stray byte in the key is an entry the switch files
 * under a different group.
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
static int only(const uint8_t *b, unsigned len, const int *keep, int nkeep)
{
	for (unsigned i = 0; i < len; i++) {
		int named = 0;

		for (int k = 0; k < nkeep; k++)
			if (keep[k] == (int)i)
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
	struct odi_sw_l2_mcast m;
	uint8_t *b = (uint8_t *)&m;
	uint8_t mac[6];
	uint32_t v;

	ok(sizeof m == 28, "the request is the 28-byte ioctl argument");

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

	/* Keyed on the VID (lookup on MAC + VID/FID). Big-endian target:
	 *   +0  mac[6]         01 00 5e 01 02 03
	 *   +6  key (u16)      00 64
	 *   +8  ivl (u32)      00 00 00 01
	 *   +12 ports (u32)    00 00 00 05
	 *   +16 ext_ports, +20 index, +24 found: zero */
	for (unsigned i = 0; i < sizeof m; i++)
		b[i] = 0xa5;
	ok(igmp_mac_entry(&m, 0x0064, 0xef010203, 0x5, 1) == 0, "a two-port join builds");
	ok(b[0] == 0x01 && b[1] == 0x00 && b[2] == 0x5e &&
	   b[3] == 0x01 && b[4] == 0x02 && b[5] == 0x03, "the group MAC at +0");
	ok(b[6] == 0x00 && b[7] == 0x64, "the VID key at +6");
	ok(b[11] == 1, "IVL set at +8: keyed on the VID");
	ok(b[15] == 0x05, "the member mask at +12");
	{
		static const int keep[] = {0, 2, 3, 4, 5, 7, 11, 15};
		ok(only(b, sizeof m, keep, 8), "and nothing else, output fields included, is set");
	}

	/* Lookup on the group address: filtering id 0, no VID, no IVL. */
	ok(igmp_mac_entry(&m, 0x0064, 0xef010203, 0x1, 0) == 0, "an SVL join builds");
	ok(b[6] == 0 && b[7] == 0 && b[11] == 0, "without the vid the key and IVL are zero");
	ok(b[15] == 0x01, "the member mask is unaffected by the mode");

	/* A delete passes no mask; the key is the MAC and the VID. */
	ok(igmp_mac_entry(&m, 0x0064, 0xef010203, 0, 1) == 0, "a delete builds");
	ok(b[15] == 0 && b[5] == 0x03 && b[7] == 0x64 && b[11] == 1,
	   "a delete carries the key and no members");

	/* A VID wider than 12 bits is cut to the field, not carried into IVL. */
	igmp_mac_entry(&m, 0xf123, 0xef010203, 1, 1);
	ok(b[6] == 0x01 && b[7] == 0x23, "the key is 12 bits");

	/* Bit 4 and up name ports the switch does not have: refused, and the
	 * request is left zeroed rather than half built. */
	ok(igmp_mac_entry(&m, 1, 0xef010203, 0x10, 1) == -1,
	   "a mask past the four switch ports is refused");
	{
		static const int none[] = {-1};
		ok(only(b, sizeof m, none, 0), "and the refused request is all zero");
	}

	/* The one-shot parsers. */
	ok(igmp_parse_ipv4("239.1.2.3", &v) && v == 0xef010203, "a dotted group parses");
	ok(igmp_parse_ipv4("224.0.0.1", &v) && v == 0xe0000001, "224.0.0.1 parses");
	ok(!igmp_parse_ipv4("239.1.2", &v), "three parts are refused");
	ok(!igmp_parse_ipv4("239.1.2.3.4", &v), "five parts are refused");
	ok(!igmp_parse_ipv4("239.1.256.3", &v), "a part over 255 is refused");
	ok(!igmp_parse_ipv4("239..2.3", &v), "an empty part is refused");
	ok(!igmp_parse_ipv4("0239.1.2.3", &v), "a four-digit part is refused");
	ok(igmp_parse_num("0x1", &v) && v == 1, "0x1 parses");
	ok(igmp_parse_num("10", &v) && v == 10, "decimal parses");
	ok(igmp_parse_num("4294967295", &v) && v == 0xffffffffu, "the largest decimal parses");
	ok(!igmp_parse_num("4294967296", &v), "one past it is refused");
	ok(!igmp_parse_num("0x", &v) && !igmp_parse_num("", &v), "no digits is refused");
	ok(!igmp_parse_num("0x123456789", &v), "nine hex digits are refused");
	ok(!igmp_parse_num("12a", &v), "trailing junk is refused");

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
