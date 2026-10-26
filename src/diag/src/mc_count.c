/* `mc-count` over /dev/odi_sw. */
#include "mc_count.h"
#include "table.h"
#include "hw.h"
#include "io.h"
#include "odi_sw_ioctl.h"

static void clear(struct odi_sw_mc_count *c)
{
	uint8_t *b = (uint8_t *)c;

	for (unsigned i = 0; i < sizeof *c; i++)
		b[i] = 0;
}

static void out_ip(uint32_t a)
{
	out_udec((a >> 24) & 0xff);
	out_char('.');
	out_udec((a >> 16) & 0xff);
	out_char('.');
	out_udec((a >> 8) & 0xff);
	out_char('.');
	out_udec(a & 0xff);
}

/* The leading one bits of a mask n bytes long. */
static unsigned prefix_len(const uint8_t *m, unsigned n)
{
	unsigned bits = 0;

	for (unsigned i = 0; i < n; i++)
		for (int b = 7; b >= 0; b--) {
			if (!(m[i] & (1u << b)))
				return bits;
			bits++;
		}
	return bits;
}

static void print(const struct odi_sw_mc_count *c)
{
	out("slot=");
	out_udec(c->slot);
	out(" entry=");
	out_udec(c->entry);
	out(" counter=");
	out_udec(c->counter);
	if (!c->in_use) {
		out(" empty\n");
		return;
	}
	out(" port=");
	out_udec(c->port);
	if (c->match == ODI_SW_MCC_MATCH_MAC) {
		out(" match=mac dst=");
		for (int i = 0; i < 6; i++) {
			if (i)
				out_char(':');
			out_hex(c->mac[i], 2);
		}
		out_char('/');
		out_udec(prefix_len(c->mac_mask, 6));
	} else {
		uint8_t m[4];

		m[0] = (uint8_t)(c->dst_mask >> 24);
		m[1] = (uint8_t)(c->dst_mask >> 16);
		m[2] = (uint8_t)(c->dst_mask >> 8);
		m[3] = (uint8_t)c->dst_mask;
		out(" match=ipv4 dst=");
		out_ip(c->dst);
		out_char('/');
		out_udec(prefix_len(m, 4));
	}
	out(c->bytes ? " unit=bytes" : " unit=frames");
	out(" counted=");
	out_u64(((uint64_t)c->total_hi << 32) | c->total_lo);
	out(" now=0x");
	out_hex(c->now, 8);
	out_char('\n');
}

static int failed(int rc)
{
	switch (rc) {
	case -22:
		out("% refused: a slot is 0 to 15, a MAC prefix 1 to 48 bits,\n"
		    "% an IPv4 prefix 1 to 32 bits\n");
		break;
	case -16:
		out("% busy: the slot is in use, another process holds the counters,\n"
		    "% an ACL probe is in, or the other slot of the pair counts the\n"
		    "% other unit (slots 2k and 2k+1 share one)\n");
		break;
	case -25:
		out("% this kernel has no multicast group counters on /dev/odi_sw\n");
		break;
	default:
		out_fmt("%% mc-count failed (%d)\n", (long)rc);
		break;
	}
	return 1;
}

/* A prefix of `len` bits from the top of an n-byte mask. */
static void mask_bits(uint8_t *m, unsigned n, uint32_t len)
{
	for (unsigned i = 0; i < n; i++) {
		uint32_t b = len > 8 * i ? len - 8 * i : 0;

		m[i] = b >= 8 ? 0xff : (uint8_t)(0xff00u >> b);
	}
}

/* u: the slot, then the MAC as its first two octets and last four, and the
 * prefix length; or the IPv4 address and the prefix length. */
int cmd_mcc_set(uint32_t sel, const uint32_t *u)
{
	struct odi_sw_mc_count c;
	int rc;

	clear(&c);
	c.op = ODI_SW_MCC_SET;
	c.slot = u[0];
	c.port = (sel & MCC_SEL_PON) ? 2u : 0u;
	c.bytes = (sel & MCC_SEL_BYTES) ? 1u : 0u;
	if (sel & MCC_SEL_IPV4) {
		if (u[2] == 0 || u[2] > 32)
			return failed(-22);
		c.match = ODI_SW_MCC_MATCH_IPV4;
		c.dst_mask = 0xffffffffu << (32 - u[2]);
		c.dst = u[1] & c.dst_mask;
	} else {
		if (u[1] > 0xffff || u[3] == 0 || u[3] > 48)
			return failed(-22);
		c.match = ODI_SW_MCC_MATCH_MAC;
		c.mac[0] = (uint8_t)(u[1] >> 8);
		c.mac[1] = (uint8_t)u[1];
		c.mac[2] = (uint8_t)(u[2] >> 24);
		c.mac[3] = (uint8_t)(u[2] >> 16);
		c.mac[4] = (uint8_t)(u[2] >> 8);
		c.mac[5] = (uint8_t)u[2];
		mask_bits(c.mac_mask, 6, u[3]);
	}
	rc = hw_mc_count(&c);
	if (rc)
		return failed(rc);
	print(&c);
	return 0;
}

int cmd_mcc_show(uint32_t slot)
{
	struct odi_sw_mc_count c;
	int rc;

	clear(&c);
	c.op = ODI_SW_MCC_GET;
	c.slot = slot;
	rc = hw_mc_count(&c);
	if (rc)
		return failed(rc);
	print(&c);
	return 0;
}

int cmd_mcc_delete(uint32_t slot)
{
	struct odi_sw_mc_count c;
	int rc;

	clear(&c);
	c.op = ODI_SW_MCC_DEL;
	c.slot = slot;
	rc = hw_mc_count(&c);
	return rc ? failed(rc) : 0;
}
