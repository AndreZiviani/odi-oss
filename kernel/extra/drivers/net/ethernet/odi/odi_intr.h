/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_intr.h -- our own demux for IRQ 8 ("apl_sw", the one interrupt line
 * the switch, GPON, ACL and the other switch-core blocks share), for every
 * interrupt type this image actually services.
 *
 * Register ground truth (the register table the stock binary carries;
 * names from src/diag/tools/regnames.txt):
 * CHIP_IRQ_SETUP (0x01d000, one field, POLARITY_SEL at lsp 0), CHIP_IRQ_ENABLE
 * (0x01d00c, one enable bit per type, bit N = ordinal N for the 19 types
 * this switch declares -- odi_switch_hw.h's own CHIP_IRQ_ENABLE accessors
 * pack every one of them), CHIP_IRQ_PENDING
 * (0x01d010, sibling to IMR, write-1-to-clear per bit, same bit layout).
 * A captured clean stock boot trace shows the "intr" step writing IMR=0,
 * IMS=0x7ffff (every one of the 19 bits, clear-all) plus four writes to
 * SW_0x01d014 (0x01d014, not replayed here -- nothing in this image has ever
 * needed it, easy to add if a future ordinal proves otherwise) and the
 * "irq" step writing IMR=0, IMS=0x7ffff again (redundant with "intr"'s own
 * pair) then CHIP_IRQ_SETUP=0 (polarity high) -- odi_intr_init() below
 * replays the IMR/IMS/CTRL writes itself, since patch 0007 makes both
 * verbs no-ops under CONFIG_ODI_INTR.
 *
 * The SAME capture's "irq" window also carries five more writes into
 * 0xb8003310-0xb8003338 (kind 'w', the raw SoC-window KSEG1 store, a
 * different bus entirely from the switch-core one above): the GPIO
 * interrupt controller being masked and its status cleared, for the two
 * GPIO interrupt lines ("apl_gpio") -- SEPARATE IRQ lines from the IRQ 8
 * this file owns. Nothing in this image uses a GPIO interrupt, so odi_intr
 * does NOT replay them: GPIO interrupts are out of scope for this guard,
 * and losing the stock apl_gpio registration when "irq" becomes a no-op
 * costs nothing this image uses.
 *
 * Only IRQ 8 ("apl_sw") is requested here, non-shared (IRQF_SHARED, not
 * used: under CONFIG_ODI_INTR patch 0007 turns BOTH the "intr" and "irq"
 * /proc/rtk_init verbs into no-ops, and "irq" is the step that requests
 * this line on the stock firmware, so there is only ever one handler on
 * it). IRQF_DISABLED is a documented no-op on every kernel this driver
 * builds for -- not used, no behaviour to lose.
 */
#ifndef ODI_INTR_H
#define ODI_INTR_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Bit/ordinal range IMR and IMS both declare (odi_switch_hw.h's own
 * CHIP_IRQ_ENABLE bits 0..18) -- every interrupt-type
 * ordinal this switch has, whether or not this image ever registers a
 * handler for it.
 */
#define ODI_INTR_TYPE_COUNT	19U

/* Restated ordinals this codebase actually names elsewhere (odi_gpon.c's own
 * ODI_GPON_INTR_TYPE_GPON header comment has the full ordinal list);
 * only the ones a caller of this header needs are given names here.
 */
#define ODI_INTR_TYPE_ACL_ACTION	7U
#define ODI_INTR_TYPE_GPON		10U

typedef void (*odi_intr_handler_t)(void);

/* odi_intr_register() -- installs the handler for one interrupt type,
 * generalized from one fixed ordinal to any of the 19. Does NOT touch
 * IMR: odi_intr_enable() below is the separate call that actually
 * unmasks the type at hardware, same two-call shape
 * odi_gpon_irq_attach() already used. Returns 0, or -EINVAL for a type
 * out of range.
 */
int odi_intr_register(unsigned int type, odi_intr_handler_t fn);

/* odi_intr_enable() -- odi_intr's own IMR policy: only the types a caller
 * has actually registered a handler for are enabled, on is 1 (unmask)
 * or 0 (mask). A
 * read-modify-write of one bit of CHIP_IRQ_ENABLE (0x1d00c), through odi_switch's
 * existing odi_reg_read()/odi_reg_write() primitive -- switch-core MMIO,
 * the same bus/window odi_switch.c already owns, no new allowlist entry
 * needed. The read-back matters: the `acl` sdkinit replay's own last event
 * blindly overwrites all of IMR (bit 7 only) -- odi_intr.c's own comment on
 * this function has the full ordering argument for why that is always
 * safe here (short version: IMR ends up 0x480, ACL + GPON, matching the
 * known-good value already recorded for this board). Returns 0, or -EINVAL
 * for a type out of range.
 */
int odi_intr_enable(unsigned int type, int on);

/* odi_intr_dispatch_once() -- one demux pass against the CURRENT IMS/IMR
 * register content: read both, act on every bit set in both (dispatch a
 * registered handler, or just count an unhandled one), W1C-ack every such
 * bit regardless (odi_intr.c's own comment on this function has the why --
 * an un-acked enabled-but-unhandled type storms the shared line). Bumps
 * the total/spurious counters itself. This is the whole ISR body: the
 * real hard-IRQ trampoline (odi_intr.c, __KERNEL__ only) is a spinlock
 * around exactly one call to this function, and test/odi_intr_test.c
 * calls it directly against test/odi_switch_mock.h to exercise the
 * dispatch loop without a real IRQ. Returns 1 if at least one bit was
 * serviced, 0 if the pass was spurious (nothing pending&enabled).
 */
int odi_intr_dispatch_once(void);

/* odi_intr_test_reset() -- host-test-only: clears the handler table and
 * every counter back to their module-load state. Real target code never
 * calls this (there is no re-init path for a built-in driver); it exists
 * so test/odi_intr_test.c can run one case per odi_mock_reset() without
 * a previous case's own registrations leaking into the next.
 */
void odi_intr_test_reset(void);

/* odi_intr_init() -- odi_intr's own one-time bring-up, called once from
 * this file's own module_init() at kernel boot: CHIP_IRQ_SETUP=0 (polarity
 * high), IMR=0 and IMS=0x7ffff (mask everything, clear any latched
 * status -- the same pair the stock "intr" AND "irq" steps both wrote),
 * then request_irq(8, ..., "apl_sw"). No caller needs to
 * invoke this directly; it is not exported for that reason.
 */

/* ---- /proc/odi_intr ----------------------------------------------------
 *
 * Read-only snapshot getters, the same posture as odi_gpon.c's own
 * /proc/odi_gpon getters: safe to call from process context only.
 */
struct odi_intr_status {
	unsigned int total_count;	/* every hard-IRQ entry that found >=1 pending&enabled bit */
	unsigned int spurious_count;	/* every hard-IRQ entry that found NONE (IMS & IMR == 0) */
	/* Per type, indexed 0..ODI_INTR_TYPE_COUNT-1: dispatched (a handler
	 * was registered and called) vs. unhandled (the bit was pending AND
	 * enabled in hardware -- e.g. the stock `acl` init step, still enabling
	 * bit 7 independently of this module -- but nothing here is
	 * registered for it; still W1C-acked every time, see odi_intr.c's own
	 * ISR comment for why: an un-acked enabled type storms the shared
	 * line exactly like the g2/g3 regression already hit once on the
	 * unmodified stock path).
	 */
	unsigned int dispatched[ODI_INTR_TYPE_COUNT];
	unsigned int unhandled[ODI_INTR_TYPE_COUNT];
};

void odi_intr_status_get(struct odi_intr_status *st);

#endif /* ODI_INTR_H */
