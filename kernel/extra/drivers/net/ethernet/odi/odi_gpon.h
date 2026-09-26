/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon.h -- thin interface between the odi_gpon FSM/PLOAM core in this
 * same directory (odi_gpon_hw.h, odi_gpon_ploam.*, odi_gpon_fsm.*, an
 * in-progress cut on its own branch) and the kernel-integration glue this
 * directory also owns: the /proc/odi_init boot verbs, the proprietary
 * GPON module's compile-out, the ISR
 * registration, the /proc/odi_gpon status file (odi_gpon.c) and the
 * odi_switch/odi_omci call sites that used to reach into that GPON
 * module directly (odi_switch_cmd.c cmd 13/15, odi_switch_ds_gem.c, the
 * encrypt-port path).
 *
 * Deliberately thin and free of any FSM/PLOAM struct: the FSM/PLOAM core
 * and this glue meet only here, so either side can change without the
 * other. odi_gpon.c implements every function below against the core
 * (odi_gpon_drv.h and its .c files, host-tested against a captured
 * re-activation).
 */
#ifndef ODI_GPON_H
#define ODI_GPON_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

/* odi_gpon_init() -- the driver's own one-time bring-up (FSM/PLOAM state and
 * the three kernel timers only), called once from odi_gpon.c's own
 * module_init() at kernel boot, before rcS writes the first PON-step verb
 * to /proc/odi_init. It does NOT register the ISR any more: a stock boot
 * capture showed the "gpondrv" step itself is what enables the GPON
 * interrupt type at the switch-core interrupt-mask level (register
 * 0x1d00c, bit 10/0x400), so registering this early left the handler
 * registered while the interrupt type was still masked off, and the later
 * "intr" boot verb could in any case reset the interrupt registration
 * table. The ISR registration and the unmask of bit 10 instead run once,
 * from the "gpondrv" verb (odi_gpon_verb(), below; odi_gpon.c's own
 * odi_gpon_irq_attach() for the implementation).
 * Returns 0 on success, a negative errno otherwise.
 */
int odi_gpon_init(void);

/* The interrupt handler is static to odi_gpon.c: the line is requested
 * at boot, the GPON source unmasked by gpondrv.
 */

/* odi_gpon_verb() -- the /proc/odi_init
 * dispatch for the seven PON-step verbs: gpondrv, gpondev, gponsn, gponpw, gponact, gpondeact, gponstat. verb is
 * the bare verb text (no leading or trailing space); arg is the raw text
 * after the verb's own space, already NUL-terminated and trimmed the same
 * way rcS's own writes already are -- NULL if the verb line carried no
 * argument (gpondrv, gpondev, gponact, gpondeact, gponstat). odi_gpon owns
 * every parse (hex serial number, hex or ASCII PLOAM password) that
 * the /proc/odi_init dispatch used to do itself; none of
 * that parsing happens there any more.
 * Returns 0 on success, a negative errno otherwise -- odi_gpon owns its
 * own return convention now, not the stock error-code range
 * /proc/odi_init used to read back.
 */
int odi_gpon_verb(const char *verb, const char *arg);

/* odi_gpon_onu_state() -- odi_switch_cmd.c cmd 13 (GetOnuState): the FSM
 * own ONU activation state
 * (ITU-T G.984.3 O1-O7 states, odi_gpon_fsm.h's own enum odi_gpon_state,
 * restated here as a plain int so this header stays independent of that
 * one), 0 if the driver has never been initialized.
 */
int odi_gpon_onu_state(void);

/* odi_gpon_onu_id_get() -- the ONU-ID the OLT assigned during ranging,
 * distinct from the activation state above. Returns -1 if no ONU-ID has
 * been assigned yet, 0-253 otherwise (ITU-T G.984.3's own ONU-ID range).
 */
int odi_gpon_onu_id_get(void);

/* odi_gpon_sn_get() -- odi_switch_cmd.c cmd 15 (GetSerialNum): the 8-byte
 * GPON serial number (4 vendor-ID ASCII bytes, 4 specific bytes)
 * odi_gpon_verb("gponsn", ...)
 * was last given. sn must point at 8 writable bytes.
 */
void odi_gpon_sn_get(u8 sn[8]);

/* ---- /proc/odi_gpon status --------------------------------------------
 *
 * odi_gpon.c's own read-only procfile calls these getters; every one is a
 * plain snapshot read (no locking contract beyond what the implementation
 * itself needs internally), safe to call from process context only, the
 * same as an open+read of /proc/odi_gpon requires.
 */

/* Equalization delay the OLT last granted (register USF_EQ_DELAY), in
 * the hardware's own multiframe/subframe pair.
 */
struct odi_gpon_eqd {
	u32 multiframe;
	u32 inframe;
};
void odi_gpon_eqd_get(struct odi_gpon_eqd *eqd);

/* Lifetime PLOAM message counters: downstream received, upstream sent. Not
 * broken down by message type at this interface -- a per-type breakdown
 * needs the real PLOAM type enum, odi_gpon_ploam.h's own, and this header
 * stays independent of it (see file header, "thin interface").
 */
struct odi_gpon_ploam_counts {
	u32 ds_rx;
	u32 us_tx;
};
void odi_gpon_ploam_counts_get(struct odi_gpon_ploam_counts *counts);

/* The last ODI_GPON_PLOAM_RING_LEN PLOAM messages in either direction,
 * oldest first, as raw bytes -- odi_gpon.c's own procfile prints them as
 * hex; decoding them by type is the FSM/PLOAM core's own concern, not this
 * one job. *count on return is the number of valid entries
 * (<= ODI_GPON_PLOAM_RING_LEN).
 */
#define ODI_GPON_PLOAM_RING_LEN 32

enum odi_gpon_ploam_dir {
	ODI_GPON_PLOAM_DS = 0,	/* downstream, received from the OLT */
	ODI_GPON_PLOAM_US = 1,	/* upstream, sent to the OLT */
};

struct odi_gpon_ploam_entry {
	u32 timestamp_ms;	/* jiffies_to_msecs() at capture time */
	u8 direction;		/* enum odi_gpon_ploam_dir */
	u8 type;		/* raw PLOAM message-type byte */
	u8 content[10];		/* raw 10-byte PLOAM content field */
};

void odi_gpon_ploam_ring_get(struct odi_gpon_ploam_entry *ring, unsigned int *count);

#endif /* ODI_GPON_H */
