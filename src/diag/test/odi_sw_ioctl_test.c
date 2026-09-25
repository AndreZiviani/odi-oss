/* Re-derives every ODI_SW_IOC_* request number (odi_sw_ioctl.h) from the
 * MIPS o32 ioctl composition formula rather than trusting the header's hex
 * literals: a wrong constant still compiles and still "works", just against
 * a different request. The formula itself is checked first against a
 * known-good vector from outside this codebase (MEMERASE).
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "odi_sw_ioctl.h"

static int failures;

static void check(int cond, const char *label)
{
	printf("  %-52s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

/* arch/rlx/include/asm/ioctl.h: _IOC_NONE 1, _IOC_READ 2, _IOC_WRITE 4,
 * _IOC_SIZEBITS 13 (not the asm-generic 0/1/2/14 an x86 header would
 * give these). dirshift 29 = nrbits(8) + typebits(8) + sizebits(13). */
static uint32_t mips_ioc(uint32_t dir, uint32_t type, uint32_t nr, uint32_t size)
{
	return (dir << 29) | (size << 16) | (type << 8) | nr;
}

int main(void)
{
	puts("MIPS o32 ioctl composition:");
	check(mips_ioc(4 /* WRITE */, 'M', 2, 8) == 0x80084D02U,
	      "known-good vector: MEMERASE = _IOW('M', 2, 8 bytes)");

	puts("odi_sw_ioctl.h request numbers, re-derived:");
	check(mips_ioc(2 | 4 /* READ|WRITE */, 'S', 1, sizeof(struct odi_sw_reg))
	      == ODI_SW_IOC_REG_GET, "ODI_SW_IOC_REG_GET");
	check(mips_ioc(4 /* WRITE */, 'S', 2, sizeof(struct odi_sw_reg))
	      == ODI_SW_IOC_REG_SET, "ODI_SW_IOC_REG_SET");
	check(mips_ioc(2 | 4, 'S', 3, sizeof(struct odi_sw_soc))
	      == ODI_SW_IOC_SOC_GET, "ODI_SW_IOC_SOC_GET");
	check(mips_ioc(2 | 4, 'S', 4, sizeof(struct odi_sw_mib))
	      == ODI_SW_IOC_MIB_GET, "ODI_SW_IOC_MIB_GET");
	check(mips_ioc(2 | 4, 'S', 5, sizeof(struct odi_sw_ddm))
	      == ODI_SW_IOC_DDM_GET, "ODI_SW_IOC_DDM_GET");

	check(mips_ioc(2 | 4, 'S', 6, sizeof(struct odi_sw_l2_row))
	      == ODI_SW_IOC_L2_GET, "ODI_SW_IOC_L2_GET");
	check(mips_ioc(2 | 4, 'S', 7, sizeof(struct odi_sw_l2_row))
	      == ODI_SW_IOC_L2_NEXT, "ODI_SW_IOC_L2_NEXT");
	check(mips_ioc(2 | 4, 'S', 8, sizeof(struct odi_sw_l2_mcast))
	      == ODI_SW_IOC_L2_MC_ADD, "ODI_SW_IOC_L2_MC_ADD");
	check(mips_ioc(2 | 4, 'S', 9, sizeof(struct odi_sw_l2_mcast))
	      == ODI_SW_IOC_L2_MC_DEL, "ODI_SW_IOC_L2_MC_DEL");
	check(mips_ioc(2 /* READ */, 'S', 10, sizeof(struct odi_sw_l2_mode))
	      == ODI_SW_IOC_L2_MODE, "ODI_SW_IOC_L2_MODE");

	/* Struct sizes the marshalling actually depends on -- a size that
	 * silently grew (padding, a wider field) would still compile and
	 * still ioctl() successfully; only the request number would then
	 * disagree with what odi_reg.c's copy of these structs expects. */
	check(sizeof(struct odi_sw_reg) == 8, "struct odi_sw_reg is 8 bytes");
	check(sizeof(struct odi_sw_soc) == 8, "struct odi_sw_soc is 8 bytes");
	check(sizeof(struct odi_sw_mib) == 16, "struct odi_sw_mib is 16 bytes");
	check(sizeof(struct odi_sw_ddm) == 28, "struct odi_sw_ddm is 28 bytes");
	check(sizeof(struct odi_sw_l2_row) == 48, "struct odi_sw_l2_row is 48 bytes");
	check(sizeof(struct odi_sw_l2_mcast) == 28, "struct odi_sw_l2_mcast is 28 bytes");
	check(sizeof(struct odi_sw_l2_mode) == 8, "struct odi_sw_l2_mode is 8 bytes");
	/* The same offsets test/odi_switch_l2_test.c asserts on the kernel
	 * copy: the two restatements agree field for field, not just in
	 * total size. */
	check(offsetof(struct odi_sw_l2_row, raw) == 4 &&
	      offsetof(struct odi_sw_l2_row, mac) == 16 &&
	      offsetof(struct odi_sw_l2_row, key) == 22 &&
	      offsetof(struct odi_sw_l2_row, type) == 24 &&
	      offsetof(struct odi_sw_l2_row, age) == 27 &&
	      offsetof(struct odi_sw_l2_row, flags) == 28 &&
	      offsetof(struct odi_sw_l2_row, group) == 44,
	      "struct odi_sw_l2_row field offsets");
	check(offsetof(struct odi_sw_l2_mcast_req, key) == 6 &&
	      offsetof(struct odi_sw_l2_mcast_req, ivl) == 8 &&
	      offsetof(struct odi_sw_l2_mcast_req, ext_ports) == 16 &&
	      offsetof(struct odi_sw_l2_mcast, index) == 20 &&
	      offsetof(struct odi_sw_l2_mcast, found) == 24,
	      "struct odi_sw_l2_mcast field offsets");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
