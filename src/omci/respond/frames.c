/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* The last OMCI frames, as bytes, for `omcli frames`.
 *
 * omcid.log has every frame decoded, as a <- or -> line, and is trimmed to
 * its last 128 KB once a minute; the bytes themselves show only with no line
 * side. For an interop problem with an OLT someone else runs, the frames are
 * what both sides can compare, and Wireshark with an OMCI dissector is the
 * tool most people have. So the last FRAMES_RING frames are kept here, in
 * RAM, in a ring of fixed size, filled at the points where omcid already
 * receives (rx_accept(), the line and the injection queue alike) and sends
 * (emit_frame(), the test result, the autonomous notifications). Nothing
 * else reads the channel, and nothing is written anywhere until asked.
 *
 * `omcli frames` prints them, oldest first, one line each: the time, <- for
 * a frame from the OLT and -> for one to it, and the bytes in hex.
 * `omcli frames --pcap FILE` writes them as a pcap file instead (the client
 * writes FILE; omcid only streams), each frame as an Ethernet II frame with
 * EtherType 0x88B5, which is what the common Wireshark OMCI Lua dissector
 * reads: OLT_MAC to ONU_MAC for a frame from the OLT, the reverse for one to
 * it, the 48 bytes unchanged, CRC included.
 *
 * Both are masked unless --full: in the classes omcid keeps out of its log
 * (me_is_secret(), mibstore.c) every attribute value a frame carries is
 * overwritten with MASK_BYTE, and so are the attributes every dump masks
 * (me_attr_secret()), the identity of the stick and the MACs learned on
 * the LAN side (ident_attr). The header, the attribute masks and the
 * result codes stay, so a masked capture still decodes. A frame with
 * masked values has its CRC zeroed: the original CRC could otherwise
 * reveal the values it covered. Only --full preserves that checksum.
 * A frame that is not a baseline frame (device id other than 0x0a, or
 * shorter than 48 bytes) has everything after its header masked: there is
 * no layout to read it by.
 *
 * Size: FRAMES_RING records of 60 bytes, 120 KB of the 32 MB. A
 * registration on ISP2, MIB upload included, is about 900 frames, so the
 * ring holds one whole and what followed it, not a whole day; a capture is
 * meant to be taken right after the problem shows.
 */
#include "omcid.h"

#define FRAMES_RING 2048
#define MASK_BYTE   0xee

/* Locally administered, and easy to filter on (eth.src == ...). */
static const uint8_t OLT_MAC[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t ONU_MAC[6] = { 0x02, 0, 0, 0, 0, 0x02 };

struct frame_rec {
	uint32_t sec, usec;             /* CLOCK_REALTIME */
	uint16_t len;                   /* as received or sent, may exceed 48 */
	uint8_t dir;                    /* FRAME_RX or FRAME_TX */
	uint8_t f[OMCI_FRAME_LEN];      /* the first 48 bytes */
};

static struct frame_rec ring[FRAMES_RING];
static unsigned long ring_total;        /* frames noted since start */

void frames_note(int dir, const uint8_t *f, uint32_t len)
{
	struct frame_rec *r = &ring[ring_total % FRAMES_RING];
	long s = 0, ns = 0;
	uint32_t n = len < OMCI_FRAME_LEN ? len : OMCI_FRAME_LEN;

	sys_clock_gettime(CLOCK_REALTIME, &s, &ns);
	r->sec = (uint32_t)s;
	r->usec = (uint32_t)(ns / 1000);
	r->len = (uint16_t)(len > 0xffff ? 0xffff : len);
	r->dir = (uint8_t)dir;
	for (uint32_t i = 0; i < n; i++)
		r->f[i] = f[i];
	for (uint32_t i = n; i < OMCI_FRAME_LEN; i++)
		r->f[i] = 0;
	ring_total++;
}

/* ------------------------------------------------------------------ masking */

static void fill(uint8_t *f, unsigned from, unsigned to)
{
	for (unsigned i = from; i < to && i < 40; i++)
		f[i] = MASK_BYTE;
	/* A checksum of the original values can disclose what was hidden. */
	nl_put32(f + 44, 0);
}

/* The identity of the stick, masked as the diag bundle masks it: the serial
 * number, in ONU-G and in every circuit pack, and the MACs the OLT reads. */
static const struct { uint16_t cls; uint8_t k; } ident_attr[] = {
	{ OMCI_ME_ONU_G, 3 },                    /* serial number */
	{ OMCI_ME_CIRCUIT_PACK, 3 },             /* serial number */
	{ OMCI_ME_MAC_BRIDGE_CFG_DATA, 1 },      /* bridge MAC address */
	{ OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA, 10 }, /* port MAC address */
	/* Not the stick's own: the bridge table of a port, the MACs the
	 * switch learned on it (the router's among them). The bundle leaves
	 * the class out of its MIB dumps; the capture masks it the same way. */
	{ 50, 1 },
};

static int attr_hidden(uint16_t cls, unsigned k)
{
	for (unsigned i = 0; i < sizeof ident_attr / sizeof ident_attr[0]; i++)
		if (ident_attr[i].cls == cls && ident_attr[i].k == k)
			return 1;
	return me_attr_secret(cls, k);
}

/* Any attribute of the class masked: then a Create of it, whose values have
 * no mask to walk, is masked whole. */
static int class_has_hidden(uint16_t cls)
{
	for (unsigned k = 1; k <= 16; k++)
		if (attr_hidden(cls, k))
			return 1;
	return 0;
}

/* The attribute values from byte off on, in the order of the mask. */
static void mask_values(uint8_t *f, uint16_t cls, uint16_t amask, unsigned off)
{
	const struct omci_class *c;

	if (me_is_secret(cls)) {
		fill(f, off, 40);
		return;
	}
	if (!class_has_hidden(cls))
		return;
	c = find_class(cls);
	for (unsigned k = 1; k <= 16 && off < 40; k++) {
		unsigned w;

		if (!(amask & (0x8000u >> (k - 1))))
			continue;
		w = c ? attr_width(c, k) : 0;
		if (!w) {
			/* No width to step over: the rest is unknown. */
			fill(f, off, 40);
			return;
		}
		if (attr_hidden(cls, k))
			fill(f, off, off + w);
		off += w;
	}
}

/* G.988 11.2, the baseline layouts: contents are bytes 8 to 39. */
void frames_mask(uint8_t *f, uint32_t len)
{
	uint8_t mt = OMCI_MT(f[2]);
	int ak = (f[2] & OMCI_AK) != 0;
	uint16_t cls = nl_get16(f + 4);

	if (len < OMCI_FRAME_LEN || f[3] != OMCI_DEVICE_ID) {
		for (unsigned i = 8; i < OMCI_FRAME_LEN; i++)
			f[i] = MASK_BYTE;
		return;
	}
	if (!ak) {
		if (mt == OMCI_MT_CREATE &&
		    (me_is_secret(cls) || class_has_hidden(cls)))
			fill(f, 8, 40);
		/* A Set, and an attribute value change, which has its
		 * layout: the mask, then the values. */
		else if (mt == OMCI_MT_SET || mt == OMCI_MT_AVC)
			mask_values(f, cls, nl_get16(f + 8), 10);
		return;
	}
	switch (mt) {
	case OMCI_MT_GET:
	case OMCI_MT_GET_CURRENT_DATA:
		/* result, mask, values */
		mask_values(f, cls, nl_get16(f + 9), 11);
		break;
	case OMCI_MT_GET_NEXT:
		/* result, mask, a piece of one table: all of it or none. */
		if (me_is_secret(cls) || class_has_hidden(cls))
			fill(f, 11, 40);
		break;
	case OMCI_MT_MIB_UPLOAD_NEXT:
		/* the entity it describes, its mask, its values */
		mask_values(f, nl_get16(f + 8), nl_get16(f + 12), 14);
		break;
	default:
		break;
	}
}

/* --------------------------------------------------------------------- CLI */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void out_bytes(const uint8_t *p, unsigned n)
{
	out_n((const char *)p, (int)n);
}

/* The pcap file header (big endian, as the magic says), microseconds,
 * link type 1 (Ethernet). */
static void pcap_header(void)
{
	uint8_t h[24];

	put32(h + 0, 0xa1b2c3d4u);
	h[4] = 0; h[5] = 2;              /* version 2.4 */
	h[6] = 0; h[7] = 4;
	put32(h + 8, 0);                 /* GMT, as the stamps are */
	put32(h + 12, 0);
	put32(h + 16, 65535);            /* snaplen */
	put32(h + 20, 1);                /* LINKTYPE_ETHERNET */
	out_bytes(h, sizeof h);
}

static void pcap_record(const struct frame_rec *r, const uint8_t *f)
{
	uint8_t h[16 + 14];
	uint32_t cap = r->len < OMCI_FRAME_LEN ? r->len : OMCI_FRAME_LEN;
	const uint8_t *src = r->dir == FRAME_RX ? OLT_MAC : ONU_MAC;
	const uint8_t *dst = r->dir == FRAME_RX ? ONU_MAC : OLT_MAC;

	put32(h + 0, r->sec);
	put32(h + 4, r->usec);
	put32(h + 8, 14 + cap);
	put32(h + 12, 14u + r->len);
	for (unsigned i = 0; i < 6; i++) {
		h[16 + i] = dst[i];
		h[22 + i] = src[i];
	}
	h[28] = 0x88;
	h[29] = 0xb5;
	out_bytes(h, sizeof h);
	out_bytes(f, cap);
}

static void out_dec_pad(unsigned long v, int width)
{
	char d[12];
	int n = 0;

	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < (int)sizeof d);
	while (width-- > n)
		out_char('0');
	while (n--)
		out_char(d[n]);
}

static void text_record(const struct frame_rec *r, const uint8_t *f)
{
	uint32_t cap = r->len < OMCI_FRAME_LEN ? r->len : OMCI_FRAME_LEN;

	out_udec(r->sec);
	out_char('.');
	out_dec_pad(r->usec, 6);
	out(r->dir == FRAME_RX ? " <- " : " -> ");
	for (uint32_t i = 0; i < cap; i++)
		out_hex(f[i], 2);
	if (r->len > cap)
		out_fmt(" (%u bytes, first 48 kept)", (unsigned long)r->len);
	out_char('\n');
}

/* omcli frames [--pcap] [--full]. The client lifts --pcap FILE out of its
 * command line, writes the stream to FILE and sends --pcap alone. */
uint32_t cli_frames(void)
{
	int pcap = 0, full = 0;
	unsigned long n, first;

	for (unsigned i = 1; cli_arg(i); i++) {
		if (str_eq(cli_arg(i), "--pcap"))
			pcap = 1;
		else if (str_eq(cli_arg(i), "--full"))
			full = 1;
		else {
			out_fmt("frames: unknown argument %s\n", cli_arg(i));
			return OMCLI_EARGS;
		}
	}
	n = ring_total < FRAMES_RING ? ring_total : FRAMES_RING;
	first = ring_total - n;
	if (pcap)
		pcap_header();
	for (unsigned long i = first; i < ring_total; i++) {
		const struct frame_rec *r = &ring[i % FRAMES_RING];
		uint8_t f[OMCI_FRAME_LEN];

		for (unsigned j = 0; j < OMCI_FRAME_LEN; j++)
			f[j] = r->f[j];
		if (!full)
			frames_mask(f, r->len);
		if (pcap)
			pcap_record(r, f);
		else
			text_record(r, f);
	}
	if (!pcap)
		out_fmt("%u frames%s, %u since start\n", n,
			full ? "" : ", masked", ring_total);
	return OMCLI_OK;
}
