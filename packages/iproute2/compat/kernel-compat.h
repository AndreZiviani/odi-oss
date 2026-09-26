/*
 * Types the device kernel headers predate, force-included into every iproute2
 * translation unit.
 *
 * __kernel_old_time_t is the only one, and it is a naming change rather than a
 * new type. Linux 5.6 split the old 32-bit time types out under explicit
 * names when it added 64-bit time_t; before that the same field was plain
 * long. iproute2 bundles include/uapi/linux/ppp_defs.h, which is new enough to
 * use the new name, while <linux/types.h> and asm/posix_types.h come from this
 * device 2.6.30 sysroot, which only has the old one.
 *
 * long is the correct definition and not an approximation: on every 32-bit
 * architecture, and on o32 in particular, __kernel_old_time_t IS long upstream.
 *
 * This is a kernel-vintage gap, not a libc one. No toolchain change fixes it
 * -- the headers come from the kernel this firmware targets, and that is the
 * point of taking them from there.
 */
#ifndef ODI_KERNEL_COMPAT_H
#define ODI_KERNEL_COMPAT_H

#ifndef __ASSEMBLER__
#ifndef __kernel_old_time_t
typedef long __kernel_old_time_t;
#define __kernel_old_time_t __kernel_old_time_t
#endif
#endif

#endif
