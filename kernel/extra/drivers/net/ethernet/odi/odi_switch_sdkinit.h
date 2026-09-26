/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_sdkinit.h -- the SDK-init replay: the writes of each odi_init
 * verb (switch svlan stp oam acl qos sec rate classify stat trunk l2 vlan
 * port mirror cpu rldp trap gpio time ponmac) and PON step (i2c i2cen gpon
 * rxsd), captured from a clean boot. intr and irq have no records: they
 * set up the switch interrupt, which odi_gpon does at boot.
 *
 * The records (struct odi_replay_event: register, table row or SoC
 * write) live in /lib/firmware/odi/sdkinit.bin, grouped by verb in the
 * enum order below and in capture order within a verb.
 */
#ifndef ODI_SWITCH_SDKINIT_H
#define ODI_SWITCH_SDKINIT_H

#include "odi_replay.h" /* uint32_t */

/* The verb ids, in /proc/odi_init order (odi_init verbs first, PON steps
 * after): enum index N is the verb byte of each sdkinit.bin record. A verb
 * the capture recorded nothing for keeps its id and has no records. Never
 * renumber this list without tools/regtrace/replayblob.py SDKINIT_VERBS
 * and odi_switch_sdkinit_verb_names[]: the three agree index for index.
 */
enum odi_sw_sdkinit_verb_id {
	ODI_SDKINIT_SWITCH = 0,
	ODI_SDKINIT_SVLAN,
	ODI_SDKINIT_STP,
	ODI_SDKINIT_OAM,
	ODI_SDKINIT_ACL,
	ODI_SDKINIT_QOS,
	ODI_SDKINIT_SEC,
	ODI_SDKINIT_RATE,
	ODI_SDKINIT_CLASSIFY,
	ODI_SDKINIT_STAT,
	ODI_SDKINIT_TRUNK,
	ODI_SDKINIT_L2,
	ODI_SDKINIT_VLAN,
	ODI_SDKINIT_PORT,
	ODI_SDKINIT_MIRROR,
	ODI_SDKINIT_CPU,
	ODI_SDKINIT_RLDP,
	ODI_SDKINIT_TRAP,
	ODI_SDKINIT_GPIO,
	ODI_SDKINIT_TIME,
	ODI_SDKINIT_PONMAC,
	ODI_SDKINIT_I2C,
	ODI_SDKINIT_I2CEN,
	ODI_SDKINIT_GPON,
	ODI_SDKINIT_RXSD,
	ODI_SDKINIT_VERB_ID_COUNT
};

/* The verb names, indexed by enum odi_sw_sdkinit_verb_id. */
extern const char *const odi_switch_sdkinit_verb_names[ODI_SDKINIT_VERB_ID_COUNT];

/* Replays the records of `verb`: loads sdkinit.bin (this sleeps), applies
 * them in order, logs one line, releases the file and returns 0. An
 * unknown verb (intr, irq) returns -ENOENT without loading anything, a
 * verb with no records -ENOENT, a missing or invalid file the loader error
 * (logged). /proc/odi_init answers -ENOSYS for all three.
 */
int odi_switch_sdkinit_verb(const char *verb);

#endif /* ODI_SWITCH_SDKINIT_H */
