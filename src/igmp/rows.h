/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Track every potentially programmed MAC until its delete succeeds. */
#ifndef ODI_IGMP_ROWS_H
#define ODI_IGMP_ROWS_H
#include <stdint.h>
#define IGMP_ROWS_MAX 256
struct igmp_row {
	uint8_t used, dirty, mac[6];
	uint16_t vid;
};
struct igmp_rows { struct igmp_row row[IGMP_ROWS_MAX]; };
void igmp_rows_clear(struct igmp_rows *rows);
int igmp_rows_write(struct igmp_rows *rows, uint16_t vid,
		    const uint8_t mac[6], uint32_t ports);
int igmp_rows_retry(struct igmp_rows *rows,
		    uint32_t (*wanted)(void *, const uint8_t *), void *ctx);
#endif
