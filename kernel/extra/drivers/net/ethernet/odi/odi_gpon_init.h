/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_init.h -- the GPON boot-init replay: the writes the
 * gpondrv/gpondev/gponsn/gponpw/gponact PON steps make, generated from a
 * boot capture by tools/regtrace/mkgponinit.py into
 * /lib/firmware/odi/gpon_init.bin (odi_replay_blob.h has the format;
 * regenerate rather than hand-edit). The table covers the whole
 * switch-core address space, not only the GPON MAC window
 * 0x700000-0x706fff: the steps also write plain switch-core registers such
 * as MAX_FRAME_LEN_1 and PORT_MAX_FRAME_SEL.
 *
 * A register record is a plain write, with no read before it; a table
 * record is one switch-core table row.
 */
#ifndef ODI_GPON_INIT_H
#define ODI_GPON_INIT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

struct odi_replay_blob;	/* odi_replay_blob.h */

/* Replays a parsed gpon_init.bin in capture order. A register record with
 * an sn_word (1..4) writes that word of this board serial number instead
 * of the captured value: the Serial_Number_ONU content the gponsn step
 * writes is six words, word 0 the fixed 0xff01 ONU id and type header,
 * words 1..4 serial_number[0..7] two bytes each, big-endian, and word 5
 * the fixed 0x0005 trailer. password is unused: the capture had an empty
 * password, so its gponpw step wrote nothing.
 *
 * No allocation, no sleeping: odi_gpon.c calls this with its spinlock
 * held, on a table it loaded before taking the lock.
 */
void odi_gpon_init_apply(const struct odi_replay_blob *table,
			 const uint8_t serial_number[8], const uint8_t password[10]);

/* Writes a new serial number into the PLOAM slot the boot replay armed,
 * after boot: only the run of records that arms that slot, not the whole
 * table. The hardware answers the OLT ranging with what this slot holds, so
 * a new serial number reaches the line only through it. Returns the
 * records applied, 0 when the table has no such run. Same locking rules as
 * odi_gpon_init_apply().
 */
unsigned int odi_gpon_init_apply_serial(const struct odi_replay_blob *table,
					const uint8_t serial_number[8]);

#endif /* ODI_GPON_INIT_H */
