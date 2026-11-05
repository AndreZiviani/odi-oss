/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* SFF-8472 optical diagnostics, formatted the way the vendor formats them. */
#ifndef ODI_DDM_H
#define ODI_DDM_H

#include <stdint.h>

/* Render the DDMI block for `type` into `out`. Returns 0 on success. */
int ddm_format(int type, const uint8_t raw[24], char *out, int max);

#endif
