// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_init.c -- odi_gpon_init_apply(), the replay of gpon_init.bin
 * through odi_replay_run() with the serial number substituted
 * (odi_gpon_init.h has the rule).
 */
#include "odi_gpon_init.h"
#include "odi_replay.h"
#include "odi_replay_blob.h"

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
