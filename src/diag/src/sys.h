/*
 * Minimal o32 MIPS syscall layer for the freestanding diag.
 *
 * o32 convention: number in $v0, args in $a0-$a3, `syscall`, result in $v0 with
 * $a3 non-zero on error. Numbers are offset by 4000. Only four arguments travel
 * in registers -- anything wider goes through __syscall6 in start.S.
 *
 * Careful before adding anything here:
 * constants copied from x86 or ARM are wrong in ways that still "work".
 */
#ifndef ODI_SYS_H
#define ODI_SYS_H

#define __NR_exit   4001
#define __NR_read   4003
#define __NR_write  4004
#define __NR_open   4005
#define __NR_close  4006
#define __NR_ioctl  4054
/* Socket numbers are o32's own and are NOT the generic ones -- getsockopt is
 * 173 here, not 182. Taken from <asm/unistd.h> in the cross toolchain, never
 * from memory; a wrong number is a syscall that succeeds at doing the wrong
 * thing. */
#define __NR_getsockopt 4173
#define __NR_setsockopt 4181
#define __NR_socket     4183
/* Same source, same reason: the netlink capture needs these and a number from
 * memory would be a syscall that succeeds at doing the wrong thing. */
#define __NR_bind       4169
#define __NR_recvmsg    4177
#define __NR_sendmsg    4179
#define __NR_gettid     4222
/* This kernel has no signal(2) -- syscall 4048 returns -ENOSYS (89 here) -- so
 * handlers go in through rt_sigaction. MIPS puts sa_flags FIRST in struct
 * sigaction, ahead of the handler, which is the opposite of every other
 * architecture; the layout below is from the toolchain's asm/signal.h, not from
 * memory. MIPS supplies its own return trampoline, but a handler installed this
 * way still must not return here, because there is no libc restorer: ours
 * deregisters and exits. */
#define __NR_rt_sigaction 4194

struct mips_sigaction {
	unsigned int sa_flags;
	void (*sa_handler)(int);
	unsigned long sa_mask[4];      /* sigset_t: _NSIG 128, 32 bits a word */
};
#define __NR_getpid 4020
/* System V IPC goes through the `ipc` multiplexer here. The direct
 * msgget/msgsnd/msgrcv numbers a modern toolchain header offers (4399..4402)
 * return -ENOSYS on this kernel: o32 only grew them in Linux 5.x. */
#define __NR_ipc    4117
#define __NR_getdents64 4219
#define __NR_unlink 4010
#define __NR_rename 4038
#define __NR_nanosleep 4166
/* 4263, read twice: asm/unistd_o32.h in the cross toolchain gives
 * __NR_Linux + 263, and arch/rlx/kernel/scall32-o32.S in our own kernel tree
 * puts sys_clock_gettime two slots above the entry it labels 4265. There is no
 * clock_gettime64 here -- o32 grew 403 in Linux 5.x -- so the timespec is a
 * pair of 32-bit words and every timer above it is a 2038 problem, same as the
 * rest of this kernel ABI. */
#define __NR_clock_gettime 4263
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define __NR_mmap2  4210

/* MIPS does NOT share x86's terminal ioctl numbers -- TCGETS is 0x5401 there
 * and 0x540D here. This value is used only to ask "is this a tty?", where a
 * wrong number fails the ioctl and we fall back to treating stdin as a pipe.
 * That degrades to a duplicated echo on a terminal, never to wrong output on a
 * pipe. Verify it against the target's asm/ioctls.h before relying on it for
 * anything that actually configures the terminal. */
#define TCGETS_MIPS 0x540d

#define O_RDONLY 0
#define O_RDWR   2
#define O_SYNC   0x2000   /* MIPS value; differs from x86's 04010000 */

/* MIPS swaps SOCK_STREAM and SOCK_DGRAM relative to every other architecture;
 * SOCK_RAW happens to be 3 on both. */
#define AF_INET    2
#define SOCK_DGRAM 1
#define SOCK_STREAM 2
#define SOCK_RAW   3

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SYSCALL_CLOBBERS \
	"$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", \
	"$24", "$25", "hi", "lo", "memory"

static long syscall3(long n, long a, long b, long c)
{
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7");
	register long r2 __asm__("$2");

	__asm__ __volatile__ (
		"addu $2, $0, %2 ; syscall"
		: "=&r"(r2), "=r"(r7)
		: "ir"(n), "r"(r4), "r"(r5), "r"(r6)
		: SYSCALL_CLOBBERS);

	return (r7 && r2 > 0) ? -r2 : r2;
}

extern long __syscall6(long n, long a, long b, long c, long d, long e, long f);

static inline long sys_read(int fd, void *buf, unsigned long n)
{
	return syscall3(__NR_read, fd, (long)buf, (long)n);
}

static inline long sys_write(int fd, const void *buf, unsigned long n)
{
	return syscall3(__NR_write, fd, (long)buf, (long)n);
}

static inline long sys_open(const char *path, int flags)
{
	return syscall3(__NR_open, (long)path, flags, 0);
}

/* Creating a file needs a mode, and sys_open passes zero -- which makes a file
 * nobody can read, including the process that is about to be handed it. */
static inline long sys_create(const char *path, int flags, int mode)
{
	return syscall3(__NR_open, (long)path, flags, mode);
}

#define O_WRONLY  1
#define O_CREAT   0x0100
#define O_TRUNC   0x0200

static inline long sys_close(int fd)
{
	return syscall3(__NR_close, fd, 0, 0);
}

static inline void sys_exit(int status)
{
	syscall3(__NR_exit, status, 0, 0);
	__builtin_unreachable();
}

static inline long sys_socket(int domain, int type, int protocol)
{
	return syscall3(__NR_socket, domain, type, protocol);
}

/* Both take five arguments, so both go through the stack shuffle in start.S.
 * getsockopt's optlen is in/out; setsockopt's is by value. */
static inline long sys_getsockopt(int fd, int level, int opt, void *val, unsigned *len)
{
	return __syscall6(__NR_getsockopt, fd, level, opt, (long)val, (long)len, 0);
}

static inline long sys_bind(int fd, const void *addr, unsigned len)
{
	return syscall3(__NR_bind, fd, (long)addr, (long)len);
}

static inline long sys_sendmsg(int fd, const void *msg, int flags)
{
	return syscall3(__NR_sendmsg, fd, (long)msg, flags);
}

static inline long sys_recvmsg(int fd, void *msg, int flags)
{
	return syscall3(__NR_recvmsg, fd, (long)msg, flags);
}

/* libpr.so uses the thread id, not the process id, as the netlink port. */
static inline long sys_gettid(void)
{
	return syscall3(__NR_gettid, 0, 0, 0);
}

static inline long sys_signal(int sig, void (*handler)(int))
{
	struct mips_sigaction sa;

	sa.sa_flags = 0;
	sa.sa_handler = handler;
	for (int i = 0; i < 4; i++)
		sa.sa_mask[i] = 0;
	return __syscall6(__NR_rt_sigaction, sig, (long)&sa, 0, sizeof sa.sa_mask,
			  0, 0);
}

static inline long sys_setsockopt(int fd, int level, int opt, const void *val, unsigned len)
{
	return __syscall6(__NR_setsockopt, fd, level, opt, (long)val, (long)len, 0);
}

/* SysV IPC, /tmp scanning and a sleep -- what the omcicli replacement needs
 * beyond the socket layer. getdents64's record is variable length and the name
 * runs to the record's end, so a reader steps by d_reclen and nothing else. */
static inline long sys_getdents64(int fd, void *buf, unsigned n)
{
	return syscall3(__NR_getdents64, fd, (long)buf, (long)n);
}

static inline long sys_getpid(void)
{
	return syscall3(__NR_getpid, 0, 0, 0);
}

/* Atomic within a directory, which is what makes a config write survivable: the
 * new file is complete before the name moves. jffs2 supports it. */
static inline long sys_rename(const char *from, const char *to)
{
	return syscall3(__NR_rename, (long)from, (long)to, 0);
}

static inline long sys_unlink(const char *path)
{
	return syscall3(__NR_unlink, (long)path, 0, 0);
}

struct mips_timespec { long tv_sec; long tv_nsec; };

static inline long sys_nanosleep(long sec, long nsec)
{
	struct mips_timespec t = { sec, nsec };

	return syscall3(__NR_nanosleep, (long)&t, 0, 0);
}

/* A clock that does not step.
 *
 * CLOCK_MONOTONIC, not gettimeofday: every timer in a snooper is a duration,
 * and a membership that expires because ntp stepped the clock forwards drops
 * traffic somebody is still watching. Seconds are returned separately from
 * nanoseconds because that is what the kernel writes, and because folding them
 * into one number overflows a 32-bit long in four seconds.
 */
static inline long sys_clock_gettime(int clk, long *sec, long *nsec)
{
	struct mips_timespec t = { 0, 0 };
	long rc = syscall3(__NR_clock_gettime, clk, (long)&t, 0);

	if (rc == 0) {
		*sec = t.tv_sec;
		*nsec = t.tv_nsec;
	}
	return rc;
}

/* The general ioctl. Note that MIPS does not use the asm-generic direction
 * bits when COMPOSING a request number -- see arch/rlx/include/asm/ioctl.h in
 * the kernel tree, where _IOC_NONE is 1, _IOC_READ 2 and _IOC_WRITE 4. A
 * constant copied from an x86 header is silently the wrong request. */
static inline long sys_ioctl(int fd, unsigned long req, void *arg)
{
	return syscall3(__NR_ioctl, fd, (long)req, (long)arg);
}

static inline int sys_isatty(int fd)
{
	char termios_buf[64];   /* larger than struct termios on any 2.6 MIPS */

	return syscall3(__NR_ioctl, fd, TCGETS_MIPS, (long)termios_buf) == 0;
}

/* mmap2's offset is in 4096-byte pages, not bytes -- that is the whole
 * difference from mmap, and getting it wrong maps the wrong window silently. */
static inline void *sys_mmap2(void *addr, unsigned long len, int prot,
			      int flags, int fd, unsigned long pgoff)
{
	return (void *)__syscall6(__NR_mmap2, (long)addr, (long)len, prot,
				  flags, fd, (long)pgoff);
}

#endif
