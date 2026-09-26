/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_reg.h -- the /dev/odi_sw ioctl ABI: switch-core register get/set,
 * per-port MIB counter get, and the L2 lookup table
 * (row readback, the valid-row walk, L2 multicast add/delete). The replacement
 * for the register-only sockopts the stock kernel answers (RTK_OPT_
 * REGISTER, RTK_OPT_ADDRESS_GET/SET, RTK_OPT_STAT_PORT),
 * plus RTK_OPT_TRANSCEIVER (DDM, ODI_SW_IOC_DDM_GET -- odi_ddm.c/odi_i2c.c
 * read it directly over the SoC I2C controller, no vendor call at all).
 * RTK_OPT_GPON_STATUS already moved to /proc/odi_gpon, so that one has no
 * ioctl here.
 *
 * Shared verbatim between the kernel driver (odi_reg.c) and src/diag
 * (src/odi_sw_ioctl.h, restated rather than #included -- the two trees
 * build separately, kernel/extra under kbuild, src/diag freestanding
 * under its own cross Makefile, see src/build.sh's own header comment for
 * why the latter carries no shared build path to kernel/extra). Keep the
 * two copies identical; a divergence is silently a different ioctl on one
 * side (see below).
 *
 * Request numbers are hand-composed, not built from <linux/ioctl.h>'s
 * _IOWR() et al: MIPS o32 (arch/rlx, our fork of arch/mips) does not use
 * the asm-generic direction bits or size width those macros assume --
 * _IOC_NONE is 1 here, not 0, _IOC_READ 2, _IOC_WRITE 4, and
 * _IOC_SIZEBITS is 13, not 14 (confirmed against arch/rlx/include/asm/ioctl.h).
 * A literal
 * computed once here and used verbatim on both sides sidesteps the whole
 * class of "looks right, means a different request" bug that trap
 * describes; src/diag/test/odi_sw_ioctl_test.c re-derives each one from
 * the same formula and checks it against a known-good
 * example (MEMERASE) as a known-good vector.
 *
 * Composition: (dir << 29) | (size << 16) | (type << 8) | nr, type 'S'
 * (0x53, "odi Switch"), dir NONE=1/READ=2/WRITE=4 (IOWR is READ|WRITE=6).
 */
#ifndef ODI_REG_H
#define ODI_REG_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "odi_ddm.h" /* ODI_DDM_BUF_LEN, for struct odi_sw_ddm below */
#include "odi_switch_l2.h" /* struct odi_sw_l2_row / _mcast_req, below */

/* Switch-core register get/set -- one word at a caller-given offset, no
 * field decode. Covers RTK_OPT_ADDRESS_GET/SET and RTK_OPT_REGISTER
 * alike: both are the same flat MMIO word on this driver's side, whatever
 * distinction the stock sockopt makes between them (odi_switch_dal.h's
 * odi_sw_reg_get/_set doc). addr is a switch-core MMIO offset (from
 * ODI_SWITCH_MMIO_BASE), not a switch-core-relative "address" needing a
 * base added first -- callers translate their own RTK_SWCORE_BASE-
 * relative addresses to this offset space themselves (they are the same
 * numbers; RTK_SWCORE_BASE is librtk's own KSEG1 alias, not a different
 * origin).
 */
struct odi_sw_reg {
	uint32_t addr;
	uint32_t value;
};

/*
 * Per-port MIB/RMON counter get (RTK_OPT_STAT_PORT). counter is
 * src/diag/src/mib.h's mib_names[] index (0-68), not a register offset --
 * odi_sw_mib_get() (odi_switch_mib.c) owns the mapping from that index to
 * a register.
 */
struct odi_sw_mib {
	uint32_t port;
	uint32_t counter;
	uint64_t value;
};

/* DDM get (RTK_OPT_TRANSCEIVER). type is one of odi_ddm.h's ODI_DDM_*
 * selectors (0-6; ODI_DDM_* mirrors src/diag/src/hw.h's enum rtk_ddm in
 * the same order the stock sockopt uses). raw is the same 24-byte DDMI block the
 * vendor sockopt's own +0x28 output was -- ddm_format() in src/diag/src/
 * ddm.c is unchanged either way.
 */
struct odi_sw_ddm {
	uint32_t type;
	uint8_t raw[ODI_DDM_BUF_LEN];
};

/* The L2 lookup table (odi_switch_l2.h has the row layout).
 *
 * L2_GET reads the row at .index, valid or not, and decodes it. L2_NEXT
 * returns the first VALID row at or after .index (and sets .index to it),
 * or fails with ENOENT past the last one: a caller walks the table with
 * .index = found + 1. Both return the three raw words next to the decode,
 * so a reader can check the layout against the hardware. The argument is
 * struct odi_sw_l2_row itself (odi_switch_l2.h), 48 bytes.
 */

/* L2 multicast add/delete, through the hardware hash of (mac, key, ivl).
 * Out: .index, the row the entry landed on (add); .found, whether a
 * delete matched a row. Fails EINVAL for a unicast, broadcast or reserved
 * 01:80:c2:00:00:0x address and for member bits past the ports; ENOSPC
 * when the key hash bucket is full.
 */
struct odi_sw_l2_mcast {
	struct odi_sw_l2_mcast_req req;
	uint32_t index;
	uint32_t found;
};

/* ipmc_on_group: L2_LOOKUP_SETUP.IPMC_ON_GROUP (0 = IPv4 multicast is switched on MAC +
 * VID/FID, the mode MAC-keyed multicast entries serve). rows: how many
 * rows the walk covers, 1024 with the CAM rows off, 1088 with them on.
 */
struct odi_sw_l2_mode {
	uint32_t ipmc_on_group;
	uint32_t rows;
};

#define ODI_SW_IOC_REG_GET	0xC0085301U	/* _IOWR('S', 1, struct odi_sw_reg) */
#define ODI_SW_IOC_REG_SET	0x80085302U	/* _IOW ('S', 2, struct odi_sw_reg) */
/* Request 3 was a SoC-window get that no userland used; not reused. */
#define ODI_SW_IOC_MIB_GET	0xC0105304U	/* _IOWR('S', 4, struct odi_sw_mib) */
#define ODI_SW_IOC_DDM_GET	0xC01C5305U	/* _IOWR('S', 5, struct odi_sw_ddm) */
#define ODI_SW_IOC_L2_GET	0xC0305306U	/* _IOWR('S', 6, struct odi_sw_l2_row) */
#define ODI_SW_IOC_L2_NEXT	0xC0305307U	/* _IOWR('S', 7, struct odi_sw_l2_row) */
#define ODI_SW_IOC_L2_MC_ADD	0xC01C5308U	/* _IOWR('S', 8, struct odi_sw_l2_mcast) */
#define ODI_SW_IOC_L2_MC_DEL	0xC01C5309U	/* _IOWR('S', 9, struct odi_sw_l2_mcast) */
#define ODI_SW_IOC_L2_MODE	0x4008530AU	/* _IOR ('S', 10, struct odi_sw_l2_mode) */

#endif /* ODI_REG_H */
