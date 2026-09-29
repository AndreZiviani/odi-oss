/* OMCI baseline message format (G.988 11.2) and the CRC this OLT uses.
 *
 * A baseline frame is 48 bytes:
 *
 *   +0  u16 transaction id      +8  .. message contents (32 bytes)
 *   +2  u8  message type        +40 u32 trailer, always 0x00000028
 *   +3  u8  device id, 0x0a     +44 u32 CRC-32 over the first 44
 *   +4  u16 class id
 *   +6  u16 instance id
 *
 * The CRC variant was not guessed: poly 0x04C11DB7, init 0xFFFFFFFF, no
 * reflection either way, final XOR 0xFFFFFFFF -- the only combination that
 * reproduces the trailer of every frame captured from the OLT.
 */
#ifndef ODI_OMCI_H
#define ODI_OMCI_H

#include <stdint.h>

#define OMCI_FRAME_LEN   48
#define OMCI_TRAILER     0x00000028u
#define OMCI_DEVICE_ID   0x0a

#define OMCI_AR          0x40
#define OMCI_AK          0x20
#define OMCI_DB          0x80
#define OMCI_MT(x)       ((x) & 0x1f)

#define OMCI_MT_CREATE   4
#define OMCI_MT_DELETE   6
#define OMCI_MT_SET      8
#define OMCI_MT_GET      9
#define OMCI_MT_MIB_UPLOAD      13
#define OMCI_MT_MIB_UPLOAD_NEXT 14
#define OMCI_MT_MIB_RESET       15
#define OMCI_MT_GET_ALL_ALARMS      11
#define OMCI_MT_GET_ALL_ALARMS_NEXT 12
#define OMCI_MT_TEST                18
#define OMCI_MT_START_SW_DOWNLOAD   19
#define OMCI_MT_DOWNLOAD_SECTION    20
#define OMCI_MT_END_SW_DOWNLOAD     21
#define OMCI_MT_ACTIVATE_SW         22
#define OMCI_MT_COMMIT_SW           23
#define OMCI_MT_REBOOT              25
#define OMCI_MT_SYNC_TIME           24
#define OMCI_MT_TEST_RESULT         27
#define OMCI_MT_GET_NEXT        26

/* Result codes, G.988 table 11.2.2-2. */
#define OMCI_OK                 0
#define OMCI_ERR_CMD            1
#define OMCI_ERR_BAD_PARAM          3
#define OMCI_ERR_UNKNOWN_ME     4
#define OMCI_ERR_UNKNOWN_INST   5
#define OMCI_ERR_ATTR_FAILED    9

/* Managed-entity class ids this userland names and handles, G.988 table
 * 11.2.4 except where noted. Our own names, prefixed OMCI_ME_; G.988 itself
 * is a public standard and the numbers are its own. Not every class this
 * device carries has a name here -- only the ones apply.c, omcimsg.c,
 * mibstore.c and show.c compare against. */
enum omci_me_class {
	OMCI_ME_ONT_DATA                      = 2,   /* ONT data */
	OMCI_ME_SOFTWARE_IMAGE                = 7,   /* software image */
	OMCI_ME_PPTP_ETH_UNI                  = 11,  /* PPTP Ethernet UNI */
	OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE    = 45,  /* MAC bridge service profile */
	OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA      = 47,  /* MAC bridge port config data */
	OMCI_ME_VLAN_TAGGING_FILTER_DATA      = 84,  /* VLAN tagging filter data */
	OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE  = 130, /* 802.1p mapper service profile */
	OMCI_ME_OLT_G                         = 131, /* OLT-G */
	OMCI_ME_IP_HOST_CONFIG_DATA           = 134, /* IP host config data */
	OMCI_ME_AUTH_SECURITY_METHOD          = 148, /* authentication security method */
	OMCI_ME_LARGE_STRING                  = 157, /* large string */
	OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA  = 171, /* extended VLAN tagging op cfg */
	OMCI_ME_ONU_G                         = 256, /* ONU-G */
	OMCI_ME_ONU2_G                        = 257, /* ONU2-G */
	OMCI_ME_TCONT                         = 262, /* T-CONT */
	OMCI_ME_ANI_G                         = 263, /* ANI-G */
	OMCI_ME_GEM_IW_TP                     = 266, /* GEM interworking TP */
	OMCI_ME_GEM_PORT_CTP                  = 268, /* GEM port network CTP */
	OMCI_ME_PRIORITY_QUEUE                = 277, /* priority queue */
	OMCI_ME_TRAFFIC_SCHEDULER             = 278, /* traffic scheduler */
	OMCI_ME_TRAFFIC_DESCRIPTOR            = 280, /* traffic descriptor */
	OMCI_ME_MCAST_GEM_IW_TP               = 281, /* multicast GEM interworking TP */
	OMCI_ME_DOT1_RATE_LIMITER             = 298, /* dot1 rate limiter */
	OMCI_ME_VEIP                          = 329, /* virtual Ethernet interface point */
	OMCI_ME_TR069_MGMT_SERVER             = 340, /* TR-069 management server params */
	/* Vendor-private range (G.988 11.2.4: 65280-65535), not a G.988 class:
	 * CTC (China Telecom) LOID authentication, as generated/omci_mib.c
	 * names it ("LoIdAuth"). */
	OMCI_ME_CTC_LOID_AUTH                 = 65530,
};

static inline uint32_t omci_crc(const uint8_t *d, uint32_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint32_t i = 0; i < n; i++) {
		c ^= (uint32_t)d[i] << 24;
		for (int k = 0; k < 8; k++)
			c = (c & 0x80000000u) ? ((c << 1) ^ 0x04c11db7u) : (c << 1);
	}
	return c ^ 0xffffffffu;
}



#endif
