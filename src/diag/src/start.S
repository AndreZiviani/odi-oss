/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Entry point for a freestanding o32 MIPS binary.
 *
 * There is no libc and no crt0, so the kernel jumps straight here with the
 * stack set up and nothing else. At entry $sp points at argc, followed by the
 * argv pointers, so main(argc, argv) costs two instructions.
 *
 * We owe main() 16 bytes of argument save area (o32 requires the caller to
 * reserve it) and an exit syscall on the way out, since returning from _start
 * would fall off the end of .text.
 *
 * Same shape as odi-sfp-exporter's src/start.S; kept separate because the two
 * repos do not share a build.
 *
 * The runtime support of -fstack-protector-strong (src/harden.mk) lives here
 * too, in assembly, because it is the code that must not be protected itself:
 * ssp_seed runs before the guard holds its value, and __stack_chk_fail runs
 * on a frame already known to be overwritten. docs/CROSS-COMPILING.md,
 * "Hardening", has the reasoning.
 */

	.text
	.globl	_start
	.ent	_start
	.set	nomips16
	.set	noreorder

_start:
	lw	$a0, 0($sp)		/* argc */
	addiu	$a1, $sp, 4		/* argv */

	jal	ssp_seed		/* before any protected function runs */
	nop

	addiu	$sp, $sp, -24		/* 16B arg save area, 8B-aligned */
	move	$fp, $zero		/* terminate the frame chain */

	jal	main
	nop

	move	$a0, $v0		/* main()'s return value is the status */
	li	$v0, 4001		/* __NR_exit: o32 syscall base 4000 + 1 */
	syscall
	nop

1:	b	1b			/* unreachable; exit never returns */
	nop

	.end	_start

/*
 * long __syscall6(long n, a1, a2, a3, a4, a5, a6)
 *
 * o32 passes the first four arguments in $a0-$a3 and the rest on the stack, and
 * the kernel wants the same shape one register to the left, with args 5 and 6
 * at 16($sp)/20($sp) as seen from the `syscall` itself. Everything below is
 * that shuffle. Used for every syscall with more than three arguments; the
 * inline syscall3 in sys.h covers the rest.
 */
	.globl	__syscall6
	.ent	__syscall6
	.set	nomips16
	.set	noreorder

__syscall6:
	move	$v0, $a0		/* syscall number */
	move	$a0, $a1
	move	$a1, $a2
	move	$a2, $a3
	lw	$a3, 16($sp)		/* arg4 — read before moving $sp */
	lw	$t0, 20($sp)		/* arg5 */
	lw	$t1, 24($sp)		/* arg6 */

	addiu	$sp, $sp, -32
	sw	$t0, 16($sp)
	sw	$t1, 20($sp)
	syscall
	addiu	$sp, $sp, 32

	beqz	$a3, 1f			/* $a3 != 0 means error */
	nop
	subu	$v0, $zero, $v0		/* return -errno, like the inline version */
1:	jr	$ra
	nop

	.end	__syscall6

/*
 * The stack protector guard. gcc on MIPS compares every protected frame
 * against this global (there is no TLS slot for it on this target), so it
 * is a plain word, written once by ssp_seed before main() runs. In .bss, not
 * .data: a binary with no other initialised data then needs no data
 * segment in the file.
 */
	.bss
	.globl	__stack_chk_guard
	.type	__stack_chk_guard, @object
	.size	__stack_chk_guard, 4
	.align	2
__stack_chk_guard:
	.space	4

	.section .rodata
	.align	2
/* struct sigaction for MIPS: sa_flags, sa_handler (SIG_DFL), sa_mask. */
ssp_dfl:
	.word	0, 0, 0, 0, 0, 0
ssp_kmsg:
	.asciz	"/dev/kmsg"
/* "<2>" makes the kernel log record critical; stderr gets the rest. */
ssp_msg:
	.ascii	"<2>stack-protector: stack smashing detected, aborting\n"
	.set	ssp_msg_len, . - ssp_msg

	.text

/*
 * ssp_seed: copy three of the 16 random bytes the kernel put on the stack
 * (the AT_RANDOM entry of the aux vector) into the guard. The first byte in
 * memory stays zero, as glibc does it: a string read cannot leak the guard
 * past it and a string write cannot reproduce it.
 *
 * In: $a0 argc, $a1 argv, exactly as _start has them; both are preserved.
 * The aux vector starts after the NULL that ends envp, which starts after
 * the NULL that ends argv. Every walk below ends on a NULL the kernel always
 * writes (argv, envp, AT_NULL), so it is bounded by the vectors themselves.
 * Assembled in reorder mode: the assembler fills the delay slots and the
 * MIPS-I load delays.
 */
	.ent	ssp_seed
	.set	reorder
ssp_seed:
	sll	$t0, $a0, 2
	addu	$t0, $t0, $a1		/* &argv[argc], the NULL ending argv */
1:	addiu	$t0, $t0, 4
	lw	$t1, 0($t0)
	bnez	$t1, 1b			/* $t0 ends on the NULL ending envp */

	addiu	$t0, $t0, 4		/* the aux vector: (type, value) pairs */
	li	$t2, 25			/* AT_RANDOM */
2:	lw	$t1, 0($t0)		/* a_type */
	lw	$t3, 4($t0)		/* a_val */
	addiu	$t0, $t0, 8
	beqz	$t1, 3f			/* AT_NULL: no AT_RANDOM */
	bne	$t1, $t2, 2b

	lbu	$t1, 1($t3)		/* byte by byte: no alignment assumed */
	lbu	$t4, 2($t3)
	sll	$t1, $t1, 16
	sll	$t4, $t4, 8
	or	$t1, $t1, $t4
	lbu	$t4, 3($t3)
	or	$t1, $t1, $t4		/* big-endian: the first byte in memory is 0 */
	b	4f

	/* A kernel that passes no AT_RANDOM (every Linux since 2.6.29 does):
	 * zero, newline and 0xff, the terminator guard glibc falls back to,
	 * which at least stops a string copy. */
3:	li	$t1, 0x00000aff
4:	lui	$t4, %hi(__stack_chk_guard)
	sw	$t1, %lo(__stack_chk_guard)($t4)
	jr	$ra
	.end	ssp_seed

/*
 * __stack_chk_fail: called by a protected function whose guard no longer
 * matches, instead of returning into an overwritten frame. Says so on
 * stderr (the log file respawn.sh gives each daemon) and in the kernel log
 * (logread, dmesg, the ramlog), then dies of SIGABRT, which
 * print-fatal-signals=1 records with the pid and the registers. Nothing
 * here reads the stack or returns. SIGABRT is reset to its default action
 * first, so no handler can run on the damaged process; were it blocked,
 * exit_group ends the process anyway. init respawns it as for any exit.
 */
	.globl	__stack_chk_fail
	.type	__stack_chk_fail, @function
	.ent	__stack_chk_fail
	.set	reorder
__stack_chk_fail:
	li	$a0, 2			/* stderr, without the kmsg level */
	la	$a1, ssp_msg + 3
	li	$a2, ssp_msg_len - 3
	li	$v0, 4004		/* __NR_write */
	syscall

	la	$a0, ssp_kmsg
	li	$a1, 1			/* O_WRONLY */
	li	$v0, 4005		/* __NR_open */
	syscall
	bnez	$a3, 1f			/* no /dev/kmsg: stderr was enough */
	move	$a0, $v0
	la	$a1, ssp_msg
	li	$a2, ssp_msg_len
	li	$v0, 4004		/* __NR_write: one write, one log record */
	syscall

1:	li	$a0, 6			/* SIGABRT */
	la	$a1, ssp_dfl
	move	$a2, $zero
	li	$a3, 16			/* sizeof (sigset_t) */
	li	$v0, 4194		/* __NR_rt_sigaction */
	syscall
	li	$v0, 4020		/* __NR_getpid */
	syscall
	move	$a0, $v0
	li	$a1, 6			/* SIGABRT */
	li	$v0, 4037		/* __NR_kill */
	syscall
	li	$a0, 127
	li	$v0, 4246		/* __NR_exit_group */
	syscall
2:	b	2b			/* unreachable */
	.end	__stack_chk_fail

/* No executable stack: start.o has no trampolines, and without this note
 * the linker marks the whole program as needing one (PT_GNU_STACK RWE). */
	.section .note.GNU-stack, "", @progbits
