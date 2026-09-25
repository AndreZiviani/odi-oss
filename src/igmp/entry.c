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

void igmp_mac_entry(uint32_t *e, uint16_t vid, uint32_t group, uint32_t ports,
		    int vid_valid)
{
	/* Aliasing a word array through unsigned char is what the standard
	 * allows, and the target is big-endian, so the byte offsets below are
	 * the offsets librtk copies. */
	uint8_t *b = (uint8_t *)e;
	uint32_t flags = 0;

	for (int i = 0; i < IGMP_MAC_ENTRY_WORDS; i++)
		e[i] = 0;

	if (vid_valid) {
		b[0] = (uint8_t)(vid >> 8);
		b[1] = (uint8_t)vid;
		flags |= IGMP_ENTRY_F_VID;
	}
	igmp_group_mac(b + 2, group);
	/* +8 is left zero. The vendor zeroes it explicitly in one branch and
	 * writes nothing to it in the other, so zero is the only value it has
	 * ever been observed to hold. */
	b[12] = (uint8_t)(ports >> 24);
	b[13] = (uint8_t)(ports >> 16);
	b[14] = (uint8_t)(ports >> 8);
	b[15] = (uint8_t)ports;
	b[24] = (uint8_t)(flags >> 24);
	b[25] = (uint8_t)(flags >> 16);
	b[26] = (uint8_t)(flags >> 8);
	b[27] = (uint8_t)flags;
}
