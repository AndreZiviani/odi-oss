/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 -- CONFIG_ODI_EARLY_CRUMBS: the IRQE crumb.
 *
 * arch_init_irq() (board.c) installs odi_crumb_handle_int as the
 * EXCCODE_INT handler. The general exception vector dispatches through
 * exception_handlers[], so every interrupt passes through here: store
 * "IRQE" in the page B tag word, count the interrupt in the step word,
 * then jump to mainline handle_int, which saves the interrupted context
 * itself. k0/k1 are the only registers free this early: every other one
 * still belongs to the interrupted context. A later crumb (IRQD, TICK)
 * overwrites the tag, so the tag read back after a revert is the last
 * thing that ran; a non-zero count with an IRQE tag means interrupts
 * arrive but the dispatch never does.
 */
#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/mach-rtl8686/odi-early-crumb.h>

	.text
	.set	push
	.set	noreorder
	.set	noat
LEAF(odi_crumb_handle_int)
	li	k0, ODI_CRUMB_PAGE_B
	li	k1, ODI_CRUMB_IRQE
	sw	k1, ODI_CRUMB_TAG(k0)
	lw	k1, ODI_CRUMB_STEP(k0)
	nop				# load delay
	addiu	k1, k1, 1
	sw	k1, ODI_CRUMB_STEP(k0)
	j	handle_int
	 nop
	END(odi_crumb_handle_int)
	.set	pop
