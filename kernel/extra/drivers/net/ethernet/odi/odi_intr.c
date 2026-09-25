// SPDX-License-Identifier: GPL-2.0
/*
 * odi_intr.c -- odi_intr.h's own implementation. Two halves:
 *
 *  - Plain C, no kernel dependency, compiled on both builds: the handler
 *    table, odi_intr_register()/odi_intr_enable(), and
 *    odi_intr_dispatch_once() -- the actual demux loop (read IMS & IMR,
 *    call whichever handlers are registered for a set bit, W1C-ack every
 *    set bit regardless), modeled directly on odi_gpon_isr.c's own
 *    odi_gpon_isr_poll() shape (mask/read/dispatch/unmask) generalized
 *    from one fixed ordinal to N. test/odi_intr_test.c calls this function
 *    directly against test/odi_switch_mock.h, the same way
 *    odi_switch_sdkinit_test.c exercises odi_switch_sdkinit_apply().
 *
 *  - __KERNEL__ only: odi_intr_isr() (the real request_irq() trampoline,
 *    a spinlock around one odi_intr_dispatch_once() call) and
 *    odi_intr_init()/module_init(), which does everything the stock
 *    "intr"+"irq" /proc/rtk_init steps used to (odi_intr.h has the
 *    register-write and capture citations), plus /proc/odi_intr.
 */
#include "odi_intr.h"
#include "odi_switch_hw.h"
#include "odi_compat.h"
/*
 * odi_switch_reg.h: odi_reg_read()/odi_reg_write() prototypes, __KERNEL__
 * only -- a no-op on the host build; test/odi_switch_mock.h's own static
 * inline definitions (included ahead of this file in the unity-build
 * test .c) cover the host build instead, same posture as
 * odi_switch_sdkinit.c.
 */
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/printk.h>
#include <linux/errno.h>
#else
#include <stddef.h>
#include <errno.h>
#endif

/* ---- Portable core: handler table, register/enable, the dispatch loop -- */

static odi_intr_handler_t odi_intr_handlers[ODI_INTR_TYPE_COUNT];
static unsigned int odi_intr_dispatched[ODI_INTR_TYPE_COUNT];
static unsigned int odi_intr_unhandled[ODI_INTR_TYPE_COUNT];
static unsigned int odi_intr_total_count;
static unsigned int odi_intr_spurious_count;

int odi_intr_register(unsigned int type, odi_intr_handler_t fn)
{
	if (type >= ODI_INTR_TYPE_COUNT || !fn)
		return -EINVAL;
	odi_intr_handlers[type] = fn;
	return 0;
}

int odi_intr_enable(unsigned int type, int on)
{
	uint32_t reg;

	if (type >= ODI_INTR_TYPE_COUNT)
		return -EINVAL;

	/* Read-modify-write of one bit, same RMW-not-blind-write shape every
	 * other replay/trigger in this codebase uses (odi_switch_sdkinit.c
	 * own comment on odi_switch_sdkinit_apply()). Bit position equals
	 * the type ordinal directly: CHIP_IRQ_ENABLE bit 0 .. bit 18
	 * (odi_switch_hw.h) carry every ordinal 0..18 in exactly that order,
	 * one field per bit in the register table the stock binary carries.
	 *
	 * This has to be a real read-back, not a blind write, because the
	 * `acl` step's own sdkinit replay data (sdkinit.bin,
	 * the last of its 193 events) blindly overwrites the WHOLE of IMR to
	 * 0x00000080 (bit 7, ACL) -- odi_switch_sdkinit_apply() applies
	 * every replay register event as a captured full-word snapshot, not a
	 * partial merge. If odi_intr_enable() also wrote the whole register
	 * instead of reading it back first, whichever of `acl`'s replay and
	 * this call ran second would silently erase the other's bit.
	 *
	 * Ordering saves this in practice: "acl" is one step of the main
	 * /proc/rtk_init loop (rootfs/skeleton/etc/init.d/rcS, "intr irq
	 * switch svlan stp oam acl ..."), which runs to completion, in full,
	 * before rcS's SEPARATE pon-steps loop even starts -- "gpondrv" (the
	 * verb that calls odi_gpon_irq_attach(), the only caller of this
	 * function for ordinal 10/GPON) is one of those later pon-steps.
	 * So on every normal boot this call is the LAST write to IMR, and its
	 * read-back sees 0x00000080 (from acl) already sitting there: result
	 * 0x00000080 | (1 << 10) = 0x00000480 -- bits 7 (ACL) and 10 (GPON),
	 * the exact same IMR value already recorded as the known-working one
	 * on this board, arrived at the same way
	 * (acl sets its own bit first, GPON's enable OR's its own in after).
	 * A boot with the sdkinit `acl` bit cleared (bisection,
	 * /var/config/sdkinit.mask) falls through to the stock ACL init
	 * instead, which sets the same bit 7 by its own register write --
	 * same end state either way, just a different writer.
	 */
	reg = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	if (on)
		reg |= (1U << type);
	else
		reg &= ~(1U << type);
	odi_reg_write(ODI_SW_CHIP_IRQ_ENABLE_OFF, reg);
	return 0;
}

/* odi_intr_dispatch_once() -- one demux pass: read IMS and IMR, act on
 * every bit set in BOTH (pending AND enabled -- a bit pending but masked
 * is not this switch's problem to report, and cannot be what raised
 * the switch IRQ in the first place). For every such bit: call the
 * registered handler if there is one (bumping dispatched[bit]), otherwise
 * just count it (unhandled[bit]) -- and W1C-ack the bit at CHIP_IRQ_PENDING
 * EITHER WAY, unconditionally, before moving to the next bit.
 *
 * That unconditional ack is the one thing this function must never get
 * wrong: the `acl` init step still sets the ACL enable bit (bit 7) in
 * IMR, independently of this module, and still runs under
 * CONFIG_ODI_INTR (only the
 * "intr"/"irq" /proc/rtk_init verbs became no-ops -- "acl" did not). So
 * bit 7 can be enabled in hardware with nothing ever calling
 * odi_intr_register(7, ...) here. Leaving that bit un-acked when it goes
 * pending would leave the switch IRQ (a level-triggered shared line)
 * permanently asserted with no software servicing it -- exactly the
 * ~16,500/s IRQ storm already hit once on the unmodified stock path
 * before the GPON ISR's own GTC_US_INTR_DLT read was found to be the
 * missing ack.
 * Acking every pending&enabled bit regardless of whether a handler
 * exists is what keeps an as-yet-unfolded type (acl today, others later)
 * from ever being able to reproduce that storm through this module.
 *
 * Returns 1 if at least one bit was serviced, 0 if the pass found nothing
 * pending&enabled (spurious). Bumps odi_intr_total_count/
 * odi_intr_spurious_count itself (not left to the caller), so a host test
 * calling this function directly (test/odi_intr_test.c) sees the same
 * counters odi_intr_status_get() would after a real hard-IRQ entry --
 * declared in odi_intr.h, not static, for exactly that reason (the
 * __KERNEL__ trampoline below, odi_intr_isr(), is the only other caller).
 */
int odi_intr_dispatch_once(void)
{
	uint32_t imr, ims, pending;
	unsigned int bit;
	int serviced = 0;

	imr = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	ims = odi_reg_read(ODI_SW_CHIP_IRQ_PENDING_OFF);
	pending = ims & imr;

	for (bit = 0; bit < ODI_INTR_TYPE_COUNT; bit++) {
		if (!(pending & (1U << bit)))
			continue;
		serviced = 1;
		if (odi_intr_handlers[bit]) {
			odi_intr_dispatched[bit]++;
			odi_intr_handlers[bit]();
		} else {
			odi_intr_unhandled[bit]++;
		}
		odi_reg_write(ODI_SW_CHIP_IRQ_PENDING_OFF, (1U << bit));
	}

	if (serviced)
		odi_intr_total_count++;
	else
		odi_intr_spurious_count++;

	return serviced;
}

void odi_intr_status_get(struct odi_intr_status *st)
{
	unsigned int i;

	st->total_count = odi_intr_total_count;
	st->spurious_count = odi_intr_spurious_count;
	for (i = 0; i < ODI_INTR_TYPE_COUNT; i++) {
		st->dispatched[i] = odi_intr_dispatched[i];
		st->unhandled[i] = odi_intr_unhandled[i];
	}
}

#ifndef __KERNEL__
/* Host-test-only (odi_intr.h has the full rationale): not compiled into
 * vmlinux, there is no re-init path for a built-in driver to call it on.
 */
void odi_intr_test_reset(void)
{
	unsigned int i;

	for (i = 0; i < ODI_INTR_TYPE_COUNT; i++) {
		odi_intr_handlers[i] = NULL;
		odi_intr_dispatched[i] = 0;
		odi_intr_unhandled[i] = 0;
	}
	odi_intr_total_count = 0;
	odi_intr_spurious_count = 0;
}
#endif /* !__KERNEL__ */

#ifdef __KERNEL__

#define ODI_INTR_LOG(fmt, ...) pr_info("odi_intr: " fmt, ##__VA_ARGS__)

/* The switch interrupt: hardware bit 8 of the SoC interrupt controller,
 * the "apl_sw" line -- a board-level constant defined here, the same
 * posture this codebase already uses for every other one
 * (odi_gpon.c's own header comment has the general rule). Not IRQF_SHARED:
 * odi_intr.h has why nothing else ever requests this line under
 * CONFIG_ODI_INTR.
 *
 * ODI_INTR_IRQ is the Linux irq NUMBER, not the hardware bit position --
 * odi_compat.h's ODI_INTR_IRQ_NUM resolves that mapping per kernel/board.
 * With a flat irq controller and no domain offset, the switch bit
 * (hardware bit 8) IS the Linux irq number, no offset -- confirmed
 * live ("IRQ 8" on a booted stick). On the 6.18 port's board irqchip
 * (arch/mips/rtl8686/irq.c), the board
 * controller is an irq_domain_create_legacy() with RTL8686_IRQ_BASE=8
 * added on top of the hwirq, so the same physical switch bit
 * (RTL8686_IRQ_SWITCH=8) is Linux irq 16, not 8 -- using the literal 8
 * there would request the wrong line. Not hardware-verified (no build in
 * this tree ever touches a stick): a guess to verify before any 6.18
 * trial, same posture as the other step-1 board guesses.
 */
#define ODI_INTR_IRQ	ODI_INTR_IRQ_NUM

static int odi_intr_request_irq_rc = -1;
static DEFINE_SPINLOCK(odi_intr_lock);
static int odi_intr_dev_id; /* request_irq() needs a unique non-NULL token, not a real device */

static irqreturn_t odi_intr_isr(int irq, void *dev_id)
{
	unsigned long flags;
	int serviced;

	(void)irq;
	(void)dev_id;

	spin_lock_irqsave(&odi_intr_lock, flags);
	serviced = odi_intr_dispatch_once();
	spin_unlock_irqrestore(&odi_intr_lock, flags);

	return serviced ? IRQ_HANDLED : IRQ_NONE;
}

/* ---- /proc/odi_intr ---------------------------------------------------- */

static int odi_intr_proc_show(struct seq_file *seq, void *v)
{
	struct odi_intr_status st;
	unsigned int i;

	(void)v;
	odi_intr_status_get(&st);

	seq_printf(seq, "irq %d request_irq_rc %d\n", ODI_INTR_IRQ, odi_intr_request_irq_rc);
	seq_printf(seq, "total %u spurious %u\n", st.total_count, st.spurious_count);
	for (i = 0; i < ODI_INTR_TYPE_COUNT; i++) {
		if (st.dispatched[i] || st.unhandled[i])
			seq_printf(seq, "type %u dispatched %u unhandled %u\n",
				   i, st.dispatched[i], st.unhandled[i]);
	}
	return 0;
}

static int odi_intr_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_intr_proc_show, NULL);
}

static const struct proc_ops odi_intr_proc_fops = {
	.proc_open = odi_intr_proc_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

/* odi_intr_init() -- everything the stock "intr" and "irq" /proc/rtk_init
 * steps used to do for the switch IRQ line (odi_intr.h has the full register
 * and capture citations): CHIP_IRQ_SETUP=0 (polarity high), IMR=0 and
 * IMS=0x7ffff (mask everything, clear any latched status), then
 * request_irq(). Runs once, unconditionally, at kernel boot -- there is no
 * verb left to defer to any more, both "intr" and "irq" are no-ops under
 * this config (patch 0007). odi_switch_init() (CONFIG_ODI_SWITCH, this
 * same driver set, linked before this file in the Makefile's own obj-y
 * list) has already mapped odi_switch_base by the time this runs.
 */
static int __init odi_intr_init(void)
{
	odi_reg_write(ODI_SW_CHIP_IRQ_SETUP_OFF, ODI_SW_CHIP_IRQ_SETUP_POLARITY_SEL_SET(0, 0));
	odi_reg_write(ODI_SW_CHIP_IRQ_ENABLE_OFF, 0);
	odi_reg_write(ODI_SW_CHIP_IRQ_PENDING_OFF, (1U << ODI_INTR_TYPE_COUNT) - 1U);

	odi_intr_request_irq_rc = request_irq(ODI_INTR_IRQ, odi_intr_isr, 0,
					       "apl_sw", &odi_intr_dev_id);
	if (odi_intr_request_irq_rc)
		pr_err("odi_intr: request_irq(%d) failed, rc=%d\n",
		       ODI_INTR_IRQ, odi_intr_request_irq_rc);
	else
		ODI_INTR_LOG("attached to IRQ %d (apl_sw), IMR/IMS clear, polarity high\n",
			     ODI_INTR_IRQ);

	if (!proc_create("odi_intr", 0444, NULL, &odi_intr_proc_fops))
		ODI_INTR_LOG("/proc/odi_intr not created\n");

	return 0; /* never fail boot over a failed IRQ attach -- odi_intr_request_irq_rc
		   * is visible in /proc/odi_intr either way
		   */
}

static void __exit odi_intr_exit(void)
{
	if (!odi_intr_request_irq_rc)
		free_irq(ODI_INTR_IRQ, &odi_intr_dev_id);
}

module_init(odi_intr_init);
module_exit(odi_intr_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss IRQ 8 (apl_sw) switch interrupt demux");

#endif /* __KERNEL__ */
