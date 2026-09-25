/* Restated copy of kernel/extra/drivers/net/ethernet/odi/odi_reg.h -- the
 * /dev/odi_sw ioctl ABI. The two trees build separately (this one
 * freestanding, no libc, under src/diag's own cross Makefile; the kernel
 * one under kbuild), so this is a second reading of the same numbers
 * rather than a shared #include -- keep the two identical. See that
 * header for what each ioctl does and why the request numbers are
 * hand-composed instead of built from <linux/ioctl.h> macros (MIPS o32's
 * direction bits differ from the asm-generic ones the macros assume).
 */
#ifndef ODI_SW_IOCTL_H
#define ODI_SW_IOCTL_H

#include <stdint.h>

struct odi_sw_reg {
	uint32_t addr;
	uint32_t value;
};

struct odi_sw_soc {
	uint32_t addr;
	uint32_t value;
};

struct odi_sw_mib {
	uint32_t port;
	uint32_t counter;
	uint64_t value;
};

/* type is one of hw.h's enum ddm_sel, 0-6. raw is the 24-byte DDMI
 * block. */
struct odi_sw_ddm {
	uint32_t type;
	uint8_t raw[24];
};

#define ODI_SW_IOC_REG_GET	0xC0085301U
#define ODI_SW_IOC_REG_SET	0x80085302U
#define ODI_SW_IOC_SOC_GET	0xC0085303U
#define ODI_SW_IOC_MIB_GET	0xC0105304U
#define ODI_SW_IOC_DDM_GET	0xC01C5305U

#endif
