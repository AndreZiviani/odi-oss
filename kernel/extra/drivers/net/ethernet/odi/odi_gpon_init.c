// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_init.c -- odi_gpon_init_apply(), the replay of gpon_init.bin
 * through odi_replay_run() with the serial number substituted
 * (odi_gpon_init.h has the rule).
 */
#include "odi_gpon_init.h"
#include "odi_replay.h"
#include "odi_replay_blob.h"

/* USF_PLOAM_TX_CTL (odi_gpon_hw.h), the register whose write opens and arms a
 * PLOAM transmit slot; the serial-number slot of the table is bracketed by it. */
#define GPON_INIT_SLOT_CTL_OFF 0x7050c0U

/* sn_word 1..4 is serial-number bytes 0-1, 2-3, 4-5, 6-7. */
static uint32_t odi_gpon_init_value(const struct odi_replay_event *e, const void *ctx)
{
	const uint8_t *serial_number = ctx;
	unsigned int b;

	if (!e->sn_word)
		return e->value;
	b = (e->sn_word - 1U) * 2U;
	return ((uint32_t)serial_number[b] << 8) | (uint32_t)serial_number[b + 1U];
}

void odi_gpon_init_apply(const struct odi_replay_blob *table,
			 const uint8_t serial_number[8], const uint8_t password[10])
{
	const struct odi_replay_opts opts = {
		.rmw = 0,
		.verb = ODI_REPLAY_ALL_VERBS,
		.value = odi_gpon_init_value,
		.ctx = serial_number,
	};

	(void)password;
	(void)odi_replay_run(table, &opts);
}

/* The serial-number slot write of the boot table, replayed alone: the run
 * of records from the USF_PLOAM_TX_CTL write that opens the slot to the
 * one that arms it again, holding the six message words between them.
 * Found by the sn_word records inside it, so it follows the table if a
 * regeneration moves it. The other records of the table are not touched.
 */
unsigned int odi_gpon_init_apply_serial(const struct odi_replay_blob *table,
					const uint8_t serial_number[8])
{
	const struct odi_replay_opts opts = {
		.rmw = 0,
		.verb = ODI_REPLAY_ALL_VERBS,
		.value = odi_gpon_init_value,
		.ctx = serial_number,
	};
	struct odi_replay_event e;
	struct odi_replay_blob slot = *table;
	uint32_t i, first = table->count, last = 0, from, to;

	for (i = 0; i < table->count; i++) {
		odi_replay_blob_event(table, i, &e);
		if (!e.sn_word)
			continue;
		if (first == table->count)
			first = i;
		last = i;
	}
	if (first == table->count)
		return 0;
	for (from = first; from > 0; from--) {
		odi_replay_blob_event(table, from - 1U, &e);
		if (e.kind == ODI_REPLAY_REG && e.offset == GPON_INIT_SLOT_CTL_OFF)
			break;
	}
	if (from == 0)
		return 0;
	from--;
	for (to = last + 1U; to < table->count; to++) {
		odi_replay_blob_event(table, to, &e);
		if (e.kind == ODI_REPLAY_REG && e.offset == GPON_INIT_SLOT_CTL_OFF)
			break;
	}
	if (to == table->count)
		return 0;
	slot.records += (size_t)from * ODI_REPLAY_BLOB_RECORD_SIZE;
	slot.count = to - from + 1U;
	return odi_replay_run(&slot, &opts);
}
