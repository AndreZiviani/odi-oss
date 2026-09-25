/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_reg.h -- the /dev/odi_sw ioctl ABI: switch-core register get/set,
 * SoC-address get, and per-port MIB counter get. The replacement
 * for the four register-only sockopts the stock kernel answers (RTK_OPT_
 * REGISTER, RTK_OPT_ADDRESS_GET/SET, RTK_OPT_SOC_GET, RTK_OPT_STAT_PORT),
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

/* SoC-address get (RTK_OPT_SOC_GET). Same shape as odi_sw_reg; kept as
 * its own struct/ioctl rather than reusing odi_sw_reg because the two
 * address spaces are unrelated (SoC window vs switch-core window) and
 * must never be confused at a call site.
 */
struct odi_sw_soc {
	uint32_t addr;
	uint32_t value;
};

/*
 * Per-port MIB/RMON counter get (RTK_OPT_STAT_PORT). counter is
 * src/diag/src/mib.h's mib_names[] index (0-68), not a register offset --
 * odi_switch_dal.c's odi_sw_mib_get() owns the mapping from that index to
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

#define ODI_SW_IOC_REG_GET	0xC0085301U	/* _IOWR('S', 1, struct odi_sw_reg) */
#define ODI_SW_IOC_REG_SET	0x80085302U	/* _IOW ('S', 2, struct odi_sw_reg) */
#define ODI_SW_IOC_SOC_GET	0xC0085303U	/* _IOWR('S', 3, struct odi_sw_soc) */
#define ODI_SW_IOC_MIB_GET	0xC0105304U	/* _IOWR('S', 4, struct odi_sw_mib) */
#define ODI_SW_IOC_DDM_GET	0xC01C5305U	/* _IOWR('S', 5, struct odi_sw_ddm) */

#endif /* ODI_REG_H */
