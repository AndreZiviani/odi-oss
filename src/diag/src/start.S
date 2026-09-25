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
 * Same shape as sfp-exporter's src/start.S; kept separate because the two
 * repos do not share a build.
 */

	.text
	.globl	_start
	.ent	_start
	.set	nomips16
	.set	noreorder

_start:
	lw	$a0, 0($sp)		/* argc */
	addiu	$a1, $sp, 4		/* argv */

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
 * that shuffle. Needed for mmap2, which takes six; the three-argument inline
 * version in sys.h covers everything else.
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
