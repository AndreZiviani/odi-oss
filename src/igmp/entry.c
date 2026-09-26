#include "entry.h"

void igmp_group_mac(uint8_t *mac, uint32_t group)
{
	mac[0] = 0x01;
	mac[1] = 0x00;
	mac[2] = 0x5e;
	/* 23 bits, not 24: bit 23 of the address is dropped, which is why
	 * 224.1.1.1 and 225.1.1.1 share a MAC address. That collision is the
	 * hardware truth a MAC-keyed snooper lives with. */
	mac[3] = (uint8_t)((group >> 16) & 0x7f);
	mac[4] = (uint8_t)((group >> 8) & 0xff);
	mac[5] = (uint8_t)(group & 0xff);
}

int igmp_mac_entry(struct odi_sw_l2_mcast *m, uint16_t vid, uint32_t group,
		   uint32_t ports, int vid_valid)
{
	uint8_t *b = (uint8_t *)m;

	/* Zeroed whole, the output fields with it: nothing left over from the
	 * previous group can reach the kernel. */
	for (unsigned i = 0; i < sizeof *m; i++)
		b[i] = 0;
	if (ports & ~IGMP_HW_PORTS_MASK)
		return -1;
	igmp_group_mac(m->req.mac, group);
	if (vid_valid) {
		m->req.key = (uint16_t)(vid & 0xfff);
		m->req.ivl = 1;
	}
	m->req.ports = ports;
	return 0;
}

int igmp_parse_num(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	int n = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (s += 2; *s; s++, n++) {
			uint32_t d;

			if (*s >= '0' && *s <= '9')
				d = (uint32_t)(*s - '0');
			else if (*s >= 'a' && *s <= 'f')
				d = (uint32_t)(*s - 'a' + 10);
			else if (*s >= 'A' && *s <= 'F')
				d = (uint32_t)(*s - 'A' + 10);
			else
				return 0;
			if (n >= 8)
				return 0;
			v = (v << 4) | d;
		}
	} else {
		for (; *s; s++, n++) {
			if (*s < '0' || *s > '9')
				return 0;
			if (v > 429496729u || (v == 429496729u && *s > '5'))
				return 0;
			v = v * 10 + (uint32_t)(*s - '0');
		}
	}
	if (n == 0)
		return 0;
	*out = v;
	return 1;
}

int igmp_parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t a = 0;

	for (int part = 0; part < 4; part++) {
		uint32_t v = 0;
		int n = 0;

		for (; *s >= '0' && *s <= '9'; s++, n++) {
			v = v * 10 + (uint32_t)(*s - '0');
			if (v > 255 || n >= 3)
				return 0;
		}
		if (n == 0)
			return 0;
		a = (a << 8) | v;
		if (part < 3) {
			if (*s != '.')
				return 0;
			s++;
		}
	}
	if (*s)
		return 0;
	*out = a;
	return 1;
}
