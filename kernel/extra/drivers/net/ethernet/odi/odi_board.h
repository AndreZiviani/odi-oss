/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_board.h -- the board init: the switch-core, LED and I2C setup the
 * stock board code makes before the first /proc/odi_init verb, replayed
 * from a clean stock boot (odi_board_data.c).
 *
 * The LED writes configure the switch LED controller once: which link
 * states (10/100/500/1000 Mb/s, activity) light LED port 15 (UTP0) and
 * port 2 (UTP1). The controller then drives the LEDs from link state by
 * itself, and nothing touches it again, so this replay is the whole LED
 * behaviour.
 *
 * The I2C master is first set up here; the `i2c`/`i2cen` PON steps later
 * replay their own part of the same capture (odi_switch_sdkinit.c).
 */
#ifndef ODI_BOARD_H
#define ODI_BOARD_H

#include "odi_replay.h" /* struct odi_replay_event */

extern const struct odi_replay_event odi_board_init_events[];
extern const unsigned int odi_board_init_event_count;

/* Maps the switch-core window if it is not mapped yet, replays
 * odi_board_init_events[] (register records as plain writes, SoC records
 * through odi_soc_write()) and logs one line. Never fails outward: a
 * refused SoC write is logged and skipped. Called from the board code
 * (arch/mips/rtl8686/board.c).
 */
void odi_board_init(void);

/* The optics, which the SDK-init verbs never set up on this board: the
 * odi_init verb `optics`, the first PON step. PIN_GPIO_SELECT = 0x08082001
 * (SoC pins 0, 13, 19 and 27 as plain GPIO; without it the port-1 I2C
 * pins, DDM and the module status, stay muxed away and every transaction
 * ends in NO_ACK), then GPIO 13, the TX-disable input of the module,
 * driven low as an output: as an input, its reset state, the laser stays
 * off and the OLT never hears the serial number. The caller holds
 * odi_switch_lock. 0, or the error of the SoC write that failed.
 */
int odi_board_optics(void);

#endif /* ODI_BOARD_H */
