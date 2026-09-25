/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_init.h -- the GPON MAC block's own init-write replay table: the
 * gpondrv/gpondev/gponsn/gponpw/gponact PON-step writes, generated from a
 * real boot capture by tools/regtrace/mkgponinit.py into
 * /lib/firmware/odi/gpon_init.bin (odi_replay_blob.h has the format;
 * checked in under rootfs/skeleton, regenerate rather than hand-edit).
 *
 * This table covers every write those five steps make across the WHOLE
 * switch-core address space, not only the GPON MAC block's own
 * 0x700000-0x706fff window -- mkgponinit.py's own docstring has why (a
 * later capture found the stock steps also touch plain switch-core
 * registers outside that block, e.g. MAX_FRAME_LEN_1/PORT_MAX_FRAME_SEL).
 * Two entry kinds, same split odi_switch_dal.h's own odi_sw_modload_event
 * already uses for a different generated table: a REG entry is a plain
 * register write (odi_reg_write()); a TABLE entry is one table row
 * (odi_switch_table_write(), odi_switch_tbl.h), for the switch-core
 * indirect tables (VLAN, L2_UNICAST, CF-family, ...) that live behind the
 * TABLE_CMD/TABLE_*_WORD handshake rather than a plain MMIO offset.
 *
 * odi_gpon_init_apply() is the one function this header declares: it
 * replays every entry of the loaded table, in the capture's own order, substituting this
 * box's own configured serial number for the ODI_GPON_INIT_SN_WORD REG
 * entries instead of the generating capture's own board identity. No
 * read-modify-write, no polling: a REG entry is exactly "write value at
 * offset" and a TABLE entry exactly "write this row at this index",
 * nothing else.
 */
#ifndef ODI_GPON_INIT_H
#define ODI_GPON_INIT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* sn_word: 0xff for a literal REG entry (replay value as generated), or the
 * 1..4 word index into the six-word Serial_Number_ONU content the gponsn
 * step writes (odi_gpon_hw.h ODI_GPON_US_PLOAM_WORDS -- word 0 is the
 * fixed 0xff01 onu_id/type header and word 5 the fixed 0x0005 trailer,
 * neither serial-number-derived, so both always replay as literals) --
 * value is then ignored and the substituted word is written instead.
 * Meaningless for a TABLE entry (always 0xff there too, for a uniform
 * generated literal).
 */
#define ODI_GPON_INIT_SN_WORD_NONE	0xffU

enum odi_gpon_init_event_kind {
	ODI_GPON_INIT_REG = 0,
	ODI_GPON_INIT_TABLE = 1,
};

struct odi_gpon_init_event {
	uint8_t kind;		/* enum odi_gpon_init_event_kind */
	uint8_t sn_word;	/* REG entries only, see above */
	uint16_t n_words;	/* TABLE entries only; 0 for a REG entry */
	uint16_t table;		/* TABLE entries only: enum odi_sw_table id (odi_switch_hw.h) */
	uint32_t offset;	/* REG: MMIO offset. TABLE: row index */
	uint32_t value;		/* REG entries only */
	uint32_t words[5];	/* TABLE entries only, zero-padded past n_words --
				 * 5 is odi_sw_modload_event's own widest-row bound
				 * (odi_switch_dal.h), reused here for the same reason
				 */
};

struct odi_replay_blob;	/* odi_replay_blob.h */

/* Replays a parsed gpon_init.bin (table) in the capture's own order: a REG entry
 * through odi_reg_write(), substituting serial_number 8 bytes (wire order,
 * matching gponsn's own layout: word0 = 0xff01 fixed onu_id/type, words 1..4
 * = serial_number[0..7] two bytes per word big-endian, word5 = 0x0005 fixed
 * trailer) for the ODI_GPON_INIT_SN_WORD entries; a TABLE entry through
 * odi_switch_table_write(table, index, words, n_words). password is
 * currently unused (the capturing board's own gponpw step captured zero
 * entries -- nothing in the generated table to substitute); kept as a
 * parameter so a future board with a real password shows up as ordinary
 * generated entries without an API change here.
 *
 * No allocation, no sleeping: odi_gpon.c calls this with its spinlock
 * held, on a table it loaded (odi_replay_fw_load()) before taking the
 * lock.
 */
void odi_gpon_init_apply(const struct odi_replay_blob *table,
			 const uint8_t serial_number[8], const uint8_t password[10]);

#endif /* ODI_GPON_INIT_H */
