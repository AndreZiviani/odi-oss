/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* sys.h for a build of omcid that runs on the build host.
 *
 * src/diag/src/sys.h is the system call layer of the freestanding binaries:
 * o32 MIPS inline assembly, which no host compiler takes. This header stands
 * in for it when omcid is built for the fuzzers (Makefile here). It declares
 * the same sys_*() functions with the same signatures and gives them no
 * body: fakeos.c has the bodies, and they are a small model of the kernel
 * (files, message queues, the netlink transport, a clock, signals) with
 * nothing of the real one behind it.
 *
 * How it gets in: the Makefile passes `-include sys_host.h` for every
 * translation unit of omcid. It defines ODI_SYS_H, the include guard of the
 * real sys.h, so when a source file then includes "sys.h" the body of that
 * file is skipped. Nothing in the shipped sources knows about this header,
 * and a build without the -include does not see it. Should the guard of
 * sys.h ever be renamed the host build stops at the first `$4` register
 * name, which is loud enough.
 *
 * The constants are the MIPS ones, on purpose. omcid passes them to
 * sys_open() and compares return values against them (the errno numbers in
 * omci_msgq.h), so the model speaks MIPS numbers and the code under test is
 * compiled exactly as shipped. Only what omcid, io.c and the shared headers
 * of src/omci use is here; a new sys_*() call in omcid needs a line here and
 * a body in fakeos.c, and the link fails until it has both.
 */
#ifndef ODI_SYS_H
#define ODI_SYS_H

#ifdef __mips__
#error "sys_host.h is the host stand-in for sys.h; a target build must not include it"
#endif

#define __NR_ipc    4117        /* the one number omcid passes itself */

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_APPEND  0x0008
#define O_CREAT   0x0100
#define O_TRUNC   0x0200
#define O_SYNC    0x2000

#define AF_INET     2
#define SOCK_DGRAM  1
#define SOCK_STREAM 2
#define SOCK_RAW    3

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

long sys_read(int fd, void *buf, unsigned long n);
long sys_write(int fd, const void *buf, unsigned long n);
long sys_open(const char *path, int flags);
long sys_create(const char *path, int flags, int mode);
long sys_close(int fd);
void sys_exit(int status) __attribute__((noreturn));
long sys_socket(int domain, int type, int protocol);
long sys_bind(int fd, const void *addr, unsigned len);
long sys_sendmsg(int fd, const void *msg, int flags);
long sys_recvmsg(int fd, void *msg, int flags);
long sys_setsockopt(int fd, int level, int opt, const void *val, unsigned len);
long sys_gettid(void);
long sys_getpid(void);
long sys_signal(int sig, void (*handler)(int));
long sys_alarm(unsigned sec);
long sys_kill(long pid, int sig);
long sys_getdents64(int fd, void *buf, unsigned n);
long sys_rename(const char *from, const char *to);
long sys_unlink(const char *path);
long sys_nanosleep(long sec, long nsec);
long sys_clock_gettime(int clk, long *sec, long *nsec);

/* System V IPC goes through the ipc multiplexer on the target, and omcid
 * calls it by number (omci_msgq.h, vqsrv.c). The model takes __NR_ipc and
 * nothing else. */
long __syscall6(long n, long a, long b, long c, long d, long e, long f);

#endif
