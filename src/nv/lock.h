/* flock(2) for the o32 ABI, the one thing nv's writer lock needs.
 *
 * __NR_flock is __NR_Linux + 143 (asm/unistd_o32.h), EAGAIN (EWOULDBLOCK) is
 * 11 on MIPS, LOCK_EX 2 and LOCK_NB 4 as everywhere. test/flock_probe.c runs
 * the real syscall under qemu-user to hold these numbers to the kernel's. */
#ifndef ODI_NV_LOCK_H
#define ODI_NV_LOCK_H

#define NV_NR_FLOCK 4143
#define NV_LOCK_EX  2
#define NV_LOCK_NB  4
#define NV_EAGAIN   11

#endif
