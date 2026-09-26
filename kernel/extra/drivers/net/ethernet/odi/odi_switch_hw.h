/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_hw.h -- register offsets and field helpers of the RTL9602C
 * switch core (physical base 0x1B000000): the registers the leaves, the
 * table engine, the interrupt glue and the GEM-port provisioning touch.
 * Anything else a capture writes is replayed by address and needs no
 * name. Plain C, host-testable.
 *
 * Addresses and bit layouts are the register table the stock firmware
 * binary carries (src/diag/tools/regmap-extract.py reads it out; its -t
 * listing is the reference for any register not named here). The names
 * are ours: src/diag/tools/regnames.txt is the one list, shared with
 * diag, and test/regnames_kernel_test.py fails when a name here drifts
 * from it.
 *
 * Naming: ODI_SW_<register>_OFF for a single word, ODI_SW_<register>_BASE
 * plus ODI_SW_<register>(n) for an array or per-port register, and
 * ODI_SW_<register>_<field>_GET/_SET for a field.
 *
 * Two register shapes. A narrow field (width times item count at most 32
 * bits) packs every item into one word, item i at bit lsb + i * width:
 * the _BASE macro and item-indexed _GET/_SET. A wide field (more than 32
 * bits in all, every 32-bit register included) gives each item its own
 * word at BASE + 4n: the NAME(n) macro.
 */
#ifndef ODI_SWITCH_HW_H
#define ODI_SWITCH_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* The switch-core register file, physical. The stock driver reaches it
 * through its KSEG1 alias 0xBB000000; odi_switch.c ioremaps the physical
 * address. It is unrelated to the SoC window at 0xB8000000 and to the NIC
 * window at 0xB8012000 (docs/SWITCH.md#register-window).
 */
#define ODI_SWITCH_MMIO_BASE	0x1B000000UL

/* The part of the 32 MB window this driver maps: the switch-core
 * registers, the GPON block at +0x700000 and the PON queue block at
 * +0xF00000, up to just past the highest PONQ_COUNT_MASK slot in use.
 */
#define ODI_SWITCH_MMIO_SIZE	0xF10000UL

/* Every odi_reg_read()/odi_reg_write() offset must fall inside the mapped
 * window; the host mock aborts outside its modelled sub-windows too.
 */
static inline int odi_switch_mmio_offset_in_bounds(uint32_t off)
{
	return off < ODI_SWITCH_MMIO_SIZE;
}

/* ---- Chip-level: PHY access, pin mux, forced port ability */

/* PHY_ACCESS_DATA: 0x000000 -- data word for an indirect PHY write */
#define ODI_SW_PHY_ACCESS_DATA_OFF	0x00000U
static inline uint32_t ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffffU << 0)) | ((val & 0xffffU) << 0);
}

/* PHY_ACCESS_CMD: 0x000004 -- indirect PHY access command */
#define ODI_SW_PHY_ACCESS_CMD_OFF	0x00004U
static inline uint32_t ODI_SW_PHY_ACCESS_CMD_WRITE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 22)) | ((val & 0x1U) << 22);
}
static inline uint32_t ODI_SW_PHY_ACCESS_CMD_START_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 21)) | ((val & 0x1U) << 21);
}
static inline uint32_t ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1fffffU << 0)) | ((val & 0x1fffffU) << 0);
}

/* PIN_GPIO_SELECT: 0x000048, one bit per SoC pin, 1 = plain GPIO */
#define ODI_SW_PIN_GPIO_SELECT_BASE	0x00048U
#define ODI_SW_PIN_GPIO_SELECT(n)	(ODI_SW_PIN_GPIO_SELECT_BASE + 4U * (uint32_t)(n))	/* slots used: 29, 31 of 0..63 */

/* PORT_FORCE_SELECT: 0x0001b4, one word per port */
#define ODI_SW_PORT_FORCE_SELECT_BASE	0x001b4U
#define ODI_SW_PORT_FORCE_SELECT(n)	(ODI_SW_PORT_FORCE_SELECT_BASE + 4U * (uint32_t)(n))	/* slots used: 0,1,2,3 of 0..3 */

/* ---- Frame length, DSCP remark */

/* PORT_MAX_FRAME_SEL: 0x011008, one word per port -- which max-length
 * profile each link speed checks frames against
 */
#define ODI_SW_PORT_MAX_FRAME_SEL_BASE	0x11008U
#define ODI_SW_PORT_MAX_FRAME_SEL(n)	(ODI_SW_PORT_MAX_FRAME_SEL_BASE + 4U * (uint32_t)(n))	/* slots used: 0,1,2,3 of 0..3 */
static inline uint32_t ODI_SW_PORT_MAX_FRAME_SEL_GIGA_PROFILE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 1)) | ((val & 0x1U) << 1);
}
static inline uint32_t ODI_SW_PORT_MAX_FRAME_SEL_FE_PROFILE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* MAX_FRAME_LEN_1: 0x011018 -- the max frame length (bytes) length-check
 * profile 1 accepts (14 bits; profile 0 is a separate word at 0x023034).
 * At its power-on 0, every non-empty frame checked against this profile
 * is oversize and the MAC drops it.
 */
#define ODI_SW_MAX_FRAME_LEN_1_OFF	0x11018U
static inline uint32_t ODI_SW_MAX_FRAME_LEN_1_BYTES_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3fffU << 0)) | ((val & 0x3fffU) << 0);
}

/* DSCP_REMARK_MAP: 0x01101c, array 0..63 -- DSCP value per internal
 * priority entry
 */
#define ODI_SW_DSCP_REMARK_MAP_BASE	0x1101cU
#define ODI_SW_DSCP_REMARK_MAP(n)	(ODI_SW_DSCP_REMARK_MAP_BASE + 4U * (uint32_t)(n))	/* slots used: 15,16,17,18,19 of 0..63 */
static inline uint32_t ODI_SW_DSCP_REMARK_MAP_DSCP_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3fU << 0)) | ((val & 0x3fU) << 0);
}

/* ---- Indirect table access, 0x012000-0x01202c */

/* TABLE_CMD: 0x012000 -- fires one table row read or write */
#define ODI_SW_TABLE_CMD_OFF	0x12000U
static inline uint32_t ODI_SW_TABLE_CMD_START_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 31)) | ((val & 0x1U) << 31);
}
static inline uint32_t ODI_SW_TABLE_CMD_ROW_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfffU << 9)) | ((val & 0xfffU) << 9);
}
static inline uint32_t ODI_SW_TABLE_CMD_METHOD_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x7U << 4)) | ((val & 0x7U) << 4);
}
static inline uint32_t ODI_SW_TABLE_CMD_IS_WRITE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 3)) | ((val & 0x1U) << 3);
}
static inline uint32_t ODI_SW_TABLE_CMD_TABLE_KIND_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x7U << 0)) | ((val & 0x7U) << 0);
}

/* TABLE_STATUS: 0x012004 -- poll/status, read-only in our capture */
#define ODI_SW_TABLE_STATUS_OFF	0x12004U
#define ODI_SW_TABLE_STATUS_IN_PROGRESS	(1U << 13)	/* clears when the op is done */

/* TABLE_WRITE_WORD: 0x012008, array 0..4, 5 words */
#define ODI_SW_TABLE_WRITE_WORD_BASE	0x12008U
#define ODI_SW_TABLE_WRITE_WORD(n)	(ODI_SW_TABLE_WRITE_WORD_BASE + 4U * (uint32_t)(n))	/* n = 0..4 */

/* TABLE_READ_WORD: 0x01201c, array 0..4, 5 words */
#define ODI_SW_TABLE_READ_WORD_BASE	0x1201cU
#define ODI_SW_TABLE_READ_WORD(n)	(ODI_SW_TABLE_READ_WORD_BASE + 4U * (uint32_t)(n))	/* n = 0..4 */

/* ---- VLAN and SVLAN */

/* VLAN_SETUP: 0x013008 */
#define ODI_SW_VLAN_SETUP_OFF	0x13008U
static inline uint32_t ODI_SW_VLAN_SETUP_VID4095_MODE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 4)) | ((val & 0x1U) << 4);
}
static inline uint32_t ODI_SW_VLAN_SETUP_VID0_MODE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 3)) | ((val & 0x1U) << 3);
}
static inline uint32_t ODI_SW_VLAN_SETUP_FILTER_ON_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* VLAN_ACCEPT_FRAMES: 0x013000, 4 ports packed 2 bits each */
#define ODI_SW_VLAN_ACCEPT_FRAMES_BASE	0x13000U	/* 4 items packed, 2 bits each */
#define ODI_SW_VLAN_ACCEPT_FRAMES(n)	((n) == 2U ? ODI_SW_VLAN_SETUP_OFF : ODI_SW_VLAN_ACCEPT_FRAMES_BASE)	/* n = 2 is VLAN_SETUP (0x013008), the register cmd 51 writes there */

/* VLAN_INGRESS_CHECK: 0x013004, 4 ports packed 1 bit each */
#define ODI_SW_VLAN_INGRESS_CHECK_BASE	0x13004U	/* 4 items packed, 1 bits each */
#define ODI_SW_VLAN_INGRESS_CHECK(n)	((void)(uint32_t)(n), ODI_SW_VLAN_INGRESS_CHECK_BASE)	/* compat: same word for every item, 0..3 */
static inline uint32_t ODI_SW_VLAN_INGRESS_CHECK_ON_SET(uint32_t reg, uint32_t item, uint32_t val)
{
	uint32_t sh = 0 + item * 1U;
	return (reg & ~(0x1U << sh)) | ((val & 0x1U) << sh);
}

/* PORT_VLAN_INDEX: 0x01300c, one word per port */
#define ODI_SW_PORT_VLAN_INDEX_BASE	0x1300cU
#define ODI_SW_PORT_VLAN_INDEX(n)	(ODI_SW_PORT_VLAN_INDEX_BASE + 4U * (uint32_t)(n))	/* slots used: 0,1 of 0..3 */

/* PROTO_VLAN_GROUP: 0x013020, array 0..3 */
#define ODI_SW_PROTO_VLAN_GROUP_BASE	0x13020U
#define ODI_SW_PROTO_VLAN_GROUP(n)	(ODI_SW_PROTO_VLAN_GROUP_BASE + 4U * (uint32_t)(n))	/* slots used: 0..3 of 0..3 */

/* PORT_PROTO_VLAN: 0x013030, array 0..3 per port 0..3 */
#define ODI_SW_PORT_PROTO_VLAN_BASE	0x13030U
#define ODI_SW_PORT_PROTO_VLAN(n)	(ODI_SW_PORT_PROTO_VLAN_BASE + 4U * (uint32_t)(n))	/* slots used: 0..15 of 0..15 */
static inline uint32_t ODI_SW_PORT_PROTO_VLAN_VLAN_INDEX_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfffU << 1)) | ((val & 0xfffU) << 1);
}

/* SVLAN_SETUP: 0x01400c. FILTER_ON (bit 27) turns SVLAN handling on;
 * UNTAGGED_ACTION (bits 3:2) is what a frame without an S-tag gets (0
 * drop, 1 trap, 2 the port SVID); PRIO_SOURCE (bits 1:0) is where the
 * S-tag priority comes from (0 internal, 1 the C-tag, 3 the port).
 */
#define ODI_SW_SVLAN_SETUP_OFF	0x1400cU
static inline uint32_t ODI_SW_SVLAN_SETUP_FILTER_ON_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 27)) | ((val & 0x1U) << 27);
}
static inline uint32_t ODI_SW_SVLAN_SETUP_UNTAGGED_ACTION_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3U << 2)) | ((val & 0x3U) << 2);
}
static inline uint32_t ODI_SW_SVLAN_SETUP_PRIO_SOURCE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3U << 0)) | ((val & 0x3U) << 0);
}

/* SVLAN_UPLINK_PORTS: 0x0230c4, 4 ports packed 1 bit each */
#define ODI_SW_SVLAN_UPLINK_PORTS_BASE	0x230c4U	/* 4 items packed, 1 bits each */
static inline uint32_t ODI_SW_SVLAN_UPLINK_PORTS_UPLINK_SET(uint32_t reg, uint32_t item, uint32_t val)
{
	uint32_t sh = 0 + item * 1U;
	return (reg & ~(0x1U << sh)) | ((val & 0x1U) << sh);
}

/* PORT_EGRESS_TAG_MODE: 0x02a000, one word per port */
#define ODI_SW_PORT_EGRESS_TAG_MODE_BASE	0x2a000U
#define ODI_SW_PORT_EGRESS_TAG_MODE(n)	(ODI_SW_PORT_EGRESS_TAG_MODE_BASE + 4U * (uint32_t)(n))	/* slots used: 0,1,2,3 of 0..3 */

/* ---- ACL and classification */

/* ACL_PORT_ENABLE: 0x015040, 4 ports packed 1 bit each. The odi_init
 * "switch" verb clears every port; the platform init turns the UNI and
 * the PON back on.
 */
#define ODI_SW_ACL_PORT_ENABLE_OFF	0x15040U

/* CLASSIFY_SETUP: 0x01600c. US_NO_MATCH_ACTION (bits 1:0) is what an
 * upstream frame no CF rule matched gets: 0 permit, 1 permit without a
 * PON match, 2 drop.
 */
#define ODI_SW_CLASSIFY_SETUP_OFF	0x1600cU
static inline uint32_t ODI_SW_CLASSIFY_SETUP_US_NO_MATCH_ACTION_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3U << 0)) | ((val & 0x3U) << 0);
}

/* CLASSIFY_PATTERN_SEL: 0x016010, one bit per classification rule */
#define ODI_SW_CLASSIFY_PATTERN_SEL_BASE	0x16010U
#define ODI_SW_CLASSIFY_PATTERN_SEL(n)	(ODI_SW_CLASSIFY_PATTERN_SEL_BASE + 4U * (uint32_t)(n))	/* word n: pattern bits of rules 32n..32n+31 */
static inline uint32_t ODI_SW_CLASSIFY_PATTERN_SEL_PATTERN_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* ---- L2 lookup, flooding, reserved multicast */

/* L2_LOOKUP_SETUP: 0x017000 */
#define ODI_SW_L2_LOOKUP_SETUP_OFF	0x17000U
static inline uint32_t ODI_SW_L2_LOOKUP_SETUP_AGE_ON_LINK_DOWN_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 22)) | ((val & 0x1U) << 22);
}
static inline uint32_t ODI_SW_L2_LOOKUP_SETUP_CAM_OFF_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 21)) | ((val & 0x1U) << 21);
}
static inline uint32_t ODI_SW_L2_LOOKUP_SETUP_AGE_TICKS_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1fffffU;
}
static inline uint32_t ODI_SW_L2_LOOKUP_SETUP_AGE_TICKS_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1fffffU << 0)) | ((val & 0x1fffffU) << 0);
}

/* FLOOD_UNKN_UCAST_PORTS: 0x01c028, 4 ports packed 1 bit each -- the
 * unknown-unicast flood mask; the broadcast (0x01c020) and
 * unknown-multicast (0x01c024) masks sit just before it
 */
#define ODI_SW_FLOOD_UNKN_UCAST_PORTS_BASE	0x1c028U	/* 4 items packed, 1 bits each */

/* IPMC_VLAN_LEAK: 0x01c030, 4 ports packed 1 bit each */
#define ODI_SW_IPMC_VLAN_LEAK_BASE	0x1c030U	/* 4 items packed, 1 bits each */
#define ODI_SW_IPMC_VLAN_LEAK(n)	((void)(uint32_t)(n), ODI_SW_IPMC_VLAN_LEAK_BASE)	/* compat: same word for every item, 0..3 */

/* LINK_MCAST_*: 0x01c038-0x01c080 -- the action for each reserved
 * link-local multicast group, 01:80:C2:00:00:xx by its last byte, then
 * the CDP and SSTP groups
 */
#define ODI_SW_LINK_MCAST_00_OFF	0x1c038U
#define ODI_SW_LINK_MCAST_01_OFF	0x1c03cU
#define ODI_SW_LINK_MCAST_02_OFF	0x1c040U
#define ODI_SW_LINK_MCAST_03_OFF	0x1c044U
#define ODI_SW_LINK_MCAST_04_OFF	0x1c048U
#define ODI_SW_LINK_MCAST_08_OFF	0x1c04cU
#define ODI_SW_LINK_MCAST_0D_OFF	0x1c050U
#define ODI_SW_LINK_MCAST_0E_OFF	0x1c054U
#define ODI_SW_LINK_MCAST_10_OFF	0x1c058U
#define ODI_SW_LINK_MCAST_11_OFF	0x1c05cU
#define ODI_SW_LINK_MCAST_12_OFF	0x1c060U
#define ODI_SW_LINK_MCAST_13_OFF	0x1c064U
#define ODI_SW_LINK_MCAST_18_OFF	0x1c068U
#define ODI_SW_LINK_MCAST_1A_OFF	0x1c06cU
#define ODI_SW_LINK_MCAST_20_OFF	0x1c070U
#define ODI_SW_LINK_MCAST_21_OFF	0x1c074U
#define ODI_SW_LINK_MCAST_22_OFF	0x1c078U
#define ODI_SW_LINK_MCAST_CDP_OFF	0x1c07cU
#define ODI_SW_LINK_MCAST_SSTP_OFF	0x1c080U

/* PORT_QUEUE_MAP: 0x01c0c0, 4 ports packed 2 bits each */
#define ODI_SW_PORT_QUEUE_MAP_BASE	0x1c0c0U	/* 4 items packed, 2 bits each */

/* ---- Chip interrupt controller, 0x01d000-0x01d010 */

/* CHIP_IRQ_SETUP: 0x01d000 -- POLARITY_SEL is its one field; 0, active
 * high, is what the stock irq step writes and what odi_gpon_chip_irq_reset()
 * writes.
 */
#define ODI_SW_CHIP_IRQ_SETUP_OFF	0x1d000U
static inline uint32_t ODI_SW_CHIP_IRQ_SETUP_POLARITY_SEL_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* CHIP_IRQ_ENABLE: 0x01d00c -- one enable bit per interrupt source, bit n
 * for source ordinal n, 19 sources: GPON is bit 10, ACL bit 7, link
 * change bit 0
 */
#define ODI_SW_CHIP_IRQ_ENABLE_OFF	0x1d00cU
#define ODI_SW_CHIP_IRQ_ENABLE_GPON	(1U << 10)
#define ODI_SW_CHIP_IRQ_ENABLE_ACL	(1U << 7)
#define ODI_SW_CHIP_IRQ_SOURCES		19U	/* bits 0..18 */
static inline uint32_t ODI_SW_CHIP_IRQ_ENABLE_GPON_GET(uint32_t reg)
{
	return (reg >> 10) & 0x1U;
}
static inline uint32_t ODI_SW_CHIP_IRQ_ENABLE_ACL_GET(uint32_t reg)
{
	return (reg >> 7) & 0x1U;
}

/* CHIP_IRQ_PENDING: 0x01d010 -- write-1-to-clear per-source interrupt
 * status, same bit layout as CHIP_IRQ_ENABLE.
 */
#define ODI_SW_CHIP_IRQ_PENDING_OFF	0x1d010U

/* ---- I2C master */

/* I2C_MASTER_SETUP: 0x023004, array 0..1 */
#define ODI_SW_I2C_MASTER_SETUP_BASE	0x23004U
#define ODI_SW_I2C_MASTER_SETUP(n)	(ODI_SW_I2C_MASTER_SETUP_BASE + 4U * (uint32_t)(n))	/* slots used: 1 of 0..1 */

/* ---- MIB counters */

/* PORT_TX_COUNTERS/PORT_RX_COUNTERS/PORT_OAM_COUNTERS: the per-port
 * counter blocks, one word per counter row. The register table gives no
 * per-port stride, so it is the declared width of a block (1024 bits,
 * 0x80, for TX and RX; 64 bits, 8, for OAM). odi_switch_mib.c maps the
 * diag counter indexes onto rows.
 */
#define ODI_SW_PORT_TX_COUNTERS_BASE	0x32000U
#define ODI_SW_PORT_TX_COUNTERS(port, n) \
	(ODI_SW_PORT_TX_COUNTERS_BASE + 0x80U * (uint32_t)(port) + 4U * (uint32_t)(n))
#define ODI_SW_PORT_RX_COUNTERS_BASE	0x32200U
#define ODI_SW_PORT_RX_COUNTERS(port, n) \
	(ODI_SW_PORT_RX_COUNTERS_BASE + 0x80U * (uint32_t)(port) + 4U * (uint32_t)(n))
#define ODI_SW_PORT_OAM_COUNTERS_BASE	0x32400U
#define ODI_SW_PORT_OAM_COUNTERS(port, n) \
	(ODI_SW_PORT_OAM_COUNTERS_BASE + 8U * (uint32_t)(port) + 4U * (uint32_t)(n))

/* ---- GPON GEM-port and T-CONT CAMs (the GPON block, switch-core space) */

/* DSF_ALLOC_CAM_CTL: 0x7010c0 -- alloc-ID/T-CONT CAM control */
#define ODI_SW_DSF_ALLOC_CAM_CTL_OFF	0x7010c0U
#define ODI_SW_DSF_ALLOC_CAM_CTL_DONE	(1U << 14)	/* set when the request is done */
static inline uint32_t ODI_SW_DSF_ALLOC_CAM_CTL_REQ_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 15)) | ((val & 0x1U) << 15);
}
static inline uint32_t ODI_SW_DSF_ALLOC_CAM_CTL_OP_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3U << 8)) | ((val & 0x3U) << 8);
}
static inline uint32_t ODI_SW_DSF_ALLOC_CAM_CTL_CAM_ROW_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1fU << 0)) | ((val & 0x1fU) << 0);
}

/* DSF_ALLOC_CAM_WDATA: 0x7010c4 -- write-data half of the alloc-ID CAM
 * handshake
 */
#define ODI_SW_DSF_ALLOC_CAM_WDATA_OFF	0x7010c4U
static inline uint32_t ODI_SW_DSF_ALLOC_CAM_WDATA_ALLOC_ID_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfffU << 0)) | ((val & 0xfffU) << 0);
}

/* DSF_GEM_CAM_CTL: 0x701100 -- downstream GEM-port CAM control */
#define ODI_SW_DSF_GEM_CAM_CTL_OFF	0x701100U
#define ODI_SW_DSF_GEM_CAM_CTL_DONE	(1U << 14)	/* set when the request is done */
static inline uint32_t ODI_SW_DSF_GEM_CAM_CTL_REQ_GET(uint32_t reg)
{
	return (reg >> 15) & 0x1U;
}
static inline uint32_t ODI_SW_DSF_GEM_CAM_CTL_REQ_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 15)) | ((val & 0x1U) << 15);
}
static inline uint32_t ODI_SW_DSF_GEM_CAM_CTL_OP_GET(uint32_t reg)
{
	return (reg >> 8) & 0x3U;
}
static inline uint32_t ODI_SW_DSF_GEM_CAM_CTL_OP_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3U << 8)) | ((val & 0x3U) << 8);
}
static inline uint32_t ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_GET(uint32_t reg)
{
	return (reg >> 0) & 0x7fU;
}
static inline uint32_t ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x7fU << 0)) | ((val & 0x7fU) << 0);
}

/* DSF_GEM_CAM_WDATA: 0x701104 -- write-data half of the GEM-port CAM
 * handshake
 */
#define ODI_SW_DSF_GEM_CAM_WDATA_OFF	0x701104U
static inline uint32_t ODI_SW_DSF_GEM_CAM_WDATA_GEM_PORT_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfffU << 0)) | ((val & 0xfffU) << 0);
}

/* DSF_GEM_FLOW_TYPE: 0x701400, array 0..127, one word per GEM-port CAM
 * row
 */
#define ODI_SW_DSF_GEM_FLOW_TYPE_BASE	0x701400U
#define ODI_SW_DSF_GEM_FLOW_TYPE(n)	(ODI_SW_DSF_GEM_FLOW_TYPE_BASE + 4U * (uint32_t)(n))	/* slots used: 0,1,2,3,4,5 of 0..127 */
static inline uint32_t ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1fU;
}
static inline uint32_t ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1fU << 0)) | ((val & 0x1fU) << 0);
}

/* FLAGS: bit 0 multicast, 1 Ethernet (data), 2 OMCI, 3 unused, 4 decrypt
 * downstream (AES). cmd 25 writes the whole field (2 data, 3
 * OMCI/broadcast); the AES path flips bit 4 alone.
 */
static inline uint32_t ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT_GET(uint32_t reg)
{
	return (reg >> 4) & 0x1U;
}
static inline uint32_t ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 4)) | ((val & 0x1U) << 4);
}

/* US_GEM_PORT_MAP: 0x706400, array 0..127 -- upstream flow to GEM port */
#define ODI_SW_US_GEM_PORT_MAP_BASE	0x706400U
#define ODI_SW_US_GEM_PORT_MAP(n)	(ODI_SW_US_GEM_PORT_MAP_BASE + 4U * (uint32_t)(n))	/* slots used: 0,1,2,3,4 of 0..127 */
static inline uint32_t ODI_SW_US_GEM_PORT_MAP_GEM_PORT_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfffU << 0)) | ((val & 0xfffU) << 0);
}

/* ---- PON queue block, 0xf00000 */

/* PONQ_COUNT_MASK: 0xf020a8, one word per slot. The table declares a
 * 1-bit field, but the captures write wider values to several slots
 * (0xf020f8, 0xf020fc, 0xf023e8, 0xf023f8, 0xf023fc): another, unnamed
 * register owns those words, so the word is written raw, never a field.
 */
#define ODI_SW_PONQ_COUNT_MASK_BASE	0xf020a8U
#define ODI_SW_PONQ_COUNT_MASK(n)	(ODI_SW_PONQ_COUNT_MASK_BASE + 4U * (uint32_t)(n))	/* slots: 0..259, the ones used are in odi_switch_qos.c */

/* PONQ_STREAM_VALID: 0xf0213c, array 0..127, one bit per row: the stream
 * gate a GEM port and T-CONT bring-up turns on before traffic flows and
 * clears at teardown, where the stock driver writes it. This SoC has one
 * PON stream, row 2 (0xf02144 in the capture).
 */
#define ODI_SW_PONQ_STREAM_VALID_BASE	0xf0213cU
#define ODI_SW_PONQ_STREAM_VALID(n)	(ODI_SW_PONQ_STREAM_VALID_BASE + 4U * (uint32_t)(n))	/* slot used: 2 of 0..127 */
static inline uint32_t ODI_SW_PONQ_STREAM_VALID_STREAM_ON_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* PORT_VLAN_INDEX: cmd 51 writes 0x1000 to 0x01300c and 0x013010, one
 * bit past the 12-bit field the table declares, so it is written raw.
 */

/* Table ids for odi_switch_table_write(): the numbering of the stock
 * driver, which is also the table id of a T line in a register capture.
 * The names are ours, the same list tools/regtrace/decode.py prints.
 */
enum odi_sw_table {
	ODI_SW_TBL_PARSER_SNAP = 0,
	ODI_SW_TBL_PARSER_SNAP_PARAM,
	ODI_SW_TBL_ACTION_SNAP_NATMC,
	ODI_SW_TBL_ACTION_SNAP_OMCI,
	ODI_SW_TBL_ACTION_SNAP_PTP,
	ODI_SW_TBL_ACTION_SNAP,
	ODI_SW_TBL_ACTION_SNAP_V1,
	ODI_SW_TBL_ACTION_SNAP_DEBUG,
	ODI_SW_TBL_ACL_ACTIONS,
	ODI_SW_TBL_ACL_PATTERN,
	ODI_SW_TBL_ACL_PATTERN_MASK,
	ODI_SW_TBL_CLS_DS_ACTION,
	ODI_SW_TBL_CLS_US_ACTION,
	ODI_SW_TBL_CLS_MASK_A,
	ODI_SW_TBL_CLS_MASK_B,
	ODI_SW_TBL_CLS_MASK_C,
	ODI_SW_TBL_CLS_RULE_A,
	ODI_SW_TBL_CLS_RULE_B,
	ODI_SW_TBL_CLS_RULE_C,
	ODI_SW_TBL_L2_MCAST_DSL,
	ODI_SW_TBL_L2_UNICAST,
	ODI_SW_TBL_L3_IP6_MCAST,
	ODI_SW_TBL_L3_MCAST_ROUTE,
	ODI_SW_TBL_VLAN_MEMBERS,
	ODI_SW_TBL_EPON_GRANTS,
	ODI_SW_TBL_NAT_ARP_CAM,
	ODI_SW_TBL_NAT_BINDING,
	ODI_SW_TBL_NAT_EXT_IP,
	ODI_SW_TBL_NAT_FLOW_V4,
	ODI_SW_TBL_NAT_FLOW_V6,
	ODI_SW_TBL_NAT_FLOW_V6_EXT,
	ODI_SW_TBL_NAT_ROUTE_V6,
	ODI_SW_TBL_NAT_ROUTE_DROP,
	ODI_SW_TBL_NAT_ROUTE_GLOBAL,
	ODI_SW_TBL_NAT_ROUTE_LOCAL,
	ODI_SW_TBL_NAT_NAPT,
	ODI_SW_TBL_NAT_NAPTR,
	ODI_SW_TBL_NAT_NEIGHBOR,
	ODI_SW_TBL_NAT_NETIF,
	ODI_SW_TBL_NAT_NEXT_HOP,
	ODI_SW_TBL_NAT_PPPOE,
	ODI_SW_TBL_NAT_WAN_TYPE,
	ODI_SW_TBL_NAT_ACTION_SNAP,
	ODI_SW_TBL_NAT_PARSER_SNAP,
};

/* What the table engine needs per table. Every table below goes through
 * one register block, TABLE_CMD/TABLE_STATUS/TABLE_WRITE_WORD/
 * TABLE_READ_WORD at 0x012000-0x01202c, told apart by TABLE_KIND, a small
 * hardware type id that is neither our enum index nor unique (CLS_RULE_B
 * and CLS_MASK_B share 4; the L2 unicast table is 0).
 *
 *   type         the TABLE_KIND value
 *   size         rows
 *   datareg_num  words per row
 *   addr_offset  added to the row index for TABLE_CMD.ROW: +128 for
 *                ACL_PATTERN, +256 for every CLS_RULE_*, none elsewhere
 *
 * Confirmed against captured writes: VLAN_MEMBERS, CLS_RULE_B, CLS_MASK_B
 * and both CLS actions (cmd 51); ACL_ACTIONS, ACL_PATTERN,
 * ACL_PATTERN_MASK, CLS_MASK_A, CLS_RULE_A and L2_UNICAST (the module-load
 * replay). CLS_RULE_C and CLS_MASK_C are not written by any capture. The
 * other L2/L3 tables would take the same handshake with no row offset and
 * stay out until a capture writes them; the NAT tables use another
 * register pair, and the *_SNAP and EPON tables are not handled.
 */
struct odi_sw_table_desc {
	uint32_t type;
	uint32_t size;
	uint32_t datareg_num;
	uint32_t addr_offset;
};

#define ODI_SW_TABLE_DESC_INIT \
	[ODI_SW_TBL_ACL_ACTIONS]        = { 3, 96,   3, 0 }, \
	[ODI_SW_TBL_ACL_PATTERN]        = { 2, 96,   5, 128 }, \
	[ODI_SW_TBL_ACL_PATTERN_MASK]   = { 2, 96,   5, 0 }, \
	[ODI_SW_TBL_CLS_DS_ACTION]      = { 5, 256,  3, 0 }, \
	[ODI_SW_TBL_CLS_US_ACTION]      = { 5, 256,  3, 0 }, \
	[ODI_SW_TBL_CLS_MASK_A]         = { 4, 256,  2, 0 }, \
	[ODI_SW_TBL_CLS_MASK_B]         = { 4, 256,  2, 0 }, \
	[ODI_SW_TBL_CLS_MASK_C]         = { 4, 256,  2, 0 }, \
	[ODI_SW_TBL_CLS_RULE_A]         = { 4, 256,  2, 256 }, \
	[ODI_SW_TBL_CLS_RULE_B]         = { 4, 256,  2, 256 }, \
	[ODI_SW_TBL_CLS_RULE_C]         = { 4, 256,  2, 256 }, \
	[ODI_SW_TBL_VLAN_MEMBERS]       = { 1, 4096, 1, 0 }, \
	[ODI_SW_TBL_L2_UNICAST]         = { 0, 1088, 3, 0 }

#endif /* ODI_SWITCH_HW_H */
