/* The L2 listing line builder: pure, so test/l2_format_test.c runs it
 * natively. l2.c has the commands. */
#include "l2.h"

/* Column widths, the stock listing's where it has the field. */
#define W_MAC   18
#define W_SPA   4
#define W_FID   4
#define W_AGE   4
#define W_VID   5
#define W_STATE 7
#define W_EXT   4
#define W_HASH  5
#define W_TYPE  5
#define W_PORTS 6

const char l2_header[] =
	"MACAddress        Spa Fid Age Vid  State  Ext Hash Type Ports Index";

struct line {
	char *buf;
	int len;
};

static void put_c(struct line *l, char c)
{
	if (l->len < L2_LINE_MAX - 1)
		l->buf[l->len++] = c;
}

static void put_s(struct line *l, const char *s)
{
	while (*s)
		put_c(l, *s++);
}

static void put_dec(struct line *l, uint32_t v)
{
	char tmp[10];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && n < (int)sizeof tmp);
	while (n)
		put_c(l, tmp[--n]);
}

static void put_hex(struct line *l, uint32_t v, int digits, int upper)
{
	const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	for (int i = digits - 1; i >= 0; i--)
		put_c(l, d[(v >> (4 * i)) & 0xfu]);
}

/* Pads the field that started at `start` out to `width`, and always leaves
 * at least one space after it, so a long value never runs into the next. */
static void pad_from(struct line *l, int start, int width)
{
	do
		put_c(l, ' ');
	while (l->len - start < width);
}

static void put_mac(struct line *l, const uint8_t *m)
{
	for (int i = 0; i < 6; i++) {
		if (i)
			put_c(l, ':');
		put_hex(l, m[i], 2, 1);
	}
}

/* The low 28 bits of a group address; the top nibble is always 1110. */
static void put_group(struct line *l, uint32_t g28)
{
	uint32_t g = 0xe0000000u | (g28 & 0x0fffffffu);

	for (int s = 24; s >= 0; s -= 8) {
		put_dec(l, (g >> s) & 0xffu);
		if (s)
			put_c(l, '.');
	}
}

int l2_format_row(const struct odi_sw_l2_row *r, char *buf)
{
	struct line l = { buf, 0 };
	int uc = r->type == ODI_SW_L2_UCAST;
	int s;

	s = l.len;
	if (r->type == ODI_SW_L2_IPMC)
		put_group(&l, r->group);
	else
		put_mac(&l, r->mac);
	pad_from(&l, s, W_MAC);

	s = l.len;
	if (uc)
		put_dec(&l, r->port);
	else
		put_c(&l, '-');
	pad_from(&l, s, W_SPA);

	s = l.len;
	if (uc)
		put_dec(&l, r->fid);
	else
		put_c(&l, '-');
	pad_from(&l, s, W_FID);

	s = l.len;
	if (uc && !(r->flags & ODI_SW_L2_F_STATIC))
		put_dec(&l, r->age);
	else
		put_c(&l, '-');
	pad_from(&l, s, W_AGE);

	s = l.len;
	put_dec(&l, r->key);
	pad_from(&l, s, W_VID);

	s = l.len;
	put_s(&l, (r->flags & ODI_SW_L2_F_STATIC) ? "Static" : "Auto");
	pad_from(&l, s, W_STATE);

	s = l.len;
	if (uc)
		put_dec(&l, r->ext_port);
	else
		put_c(&l, '-');
	pad_from(&l, s, W_EXT);

	s = l.len;
	put_s(&l, (r->flags & ODI_SW_L2_F_IVL) ? "IVL" : "SVL");
	pad_from(&l, s, W_HASH);

	s = l.len;
	put_s(&l, uc ? "uc" : r->type == ODI_SW_L2_MCAST ? "mc" : "ipmc");
	pad_from(&l, s, W_TYPE);

	s = l.len;
	if (uc) {
		put_c(&l, '-');
	} else {
		put_s(&l, "0x");
		put_hex(&l, r->ports, 1, 0);
	}
	pad_from(&l, s, W_PORTS);

	put_s(&l, "0x");
	put_hex(&l, r->index, 3, 0);

	/* The unicast flags nobody sets on this image, named only when set so
	 * the ordinary line stays as short as the stock one. */
	if (uc) {
		static const struct { uint32_t f; const char *name; } fl[] = {
			{ ODI_SW_L2_F_CTAG, " ctag" },
			{ ODI_SW_L2_F_AUTH, " auth" },
			{ ODI_SW_L2_F_SA_BLOCK, " sa-block" },
			{ ODI_SW_L2_F_DA_BLOCK, " da-block" },
			{ ODI_SW_L2_F_ARP, " arp" },
		};

		for (unsigned i = 0; i < sizeof fl / sizeof fl[0]; i++)
			if (r->flags & fl[i].f)
				put_s(&l, fl[i].name);
	} else if (r->ext_ports) {
		put_s(&l, " ext 0x");
		put_hex(&l, r->ext_ports, 2, 0);
	}
	buf[l.len] = '\0';
	return l.len;
}
