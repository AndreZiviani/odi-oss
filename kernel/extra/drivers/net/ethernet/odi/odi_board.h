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

/* The optics pins, which the SDK-init verbs never set up on this board:
 * the odi_init verb `optics`, the first PON step. PIN_GPIO_SELECT =
 * 0x08082001 (SoC pins 0, 13, 19 and 27 as plain GPIO; without it the
 * port-1 I2C pins, DDM and the module status, stay muxed away and every
 * transaction ends in NO_ACK). The laser is left alone (odi_board_laser()).
 * The caller holds odi_switch_lock. Returns 0.
 */
int odi_board_optics(void);

/* The laser: the odi_init verb `laser`, run after `i2c` and `i2cen` (the
 * I2C controller answers only after them: before, a read returns 0 with
 * no error) and before `gponact`. It identifies the laser driver, and only
 * then drives GPIO 13, the TX-disable input of the module, low as an
 * output: as an input, its reset state, the laser stays off and the OLT
 * never hears the serial number.
 *
 * The laser is switched on only for the laser driver this image was
 * validated with, a Semtech GN25L95 that loads its own calibration from
 * its EEPROM: A2h page 2 byte 0xa0 reads 0x6a (the page select is put
 * back afterwards). Any other answer, or none, leaves GPIO 13 alone and
 * returns -ENODEV: on another board the laser may need set-up this image
 * does not do, or be wired differently, and a laser that lights outside
 * its timeslots takes down every other ONU on the OLT port. arg
 * "unverified" switches it on anyway, for a user testing on an OLT port
 * of their own. The caller holds odi_switch_lock. 0, -ENODEV, or the
 * error of the SoC write that failed.
 */
#define ODI_BOARD_LDD_ID_PAGE		2U	/* A2h page that holds the id byte */
#define ODI_BOARD_LDD_ID_OFF		0xa0U	/* the id byte, in that page */
#define ODI_BOARD_LDD_ID_GN25L95	0x6aU
int odi_board_laser(const char *arg);

/* What the last laser verb found and did, for /proc/odi_gpon/status and the
 * GPON activation gate: laser_on is 1 only after the verb drove the
 * TX-disable line low; ldd_id is the id byte read, or -1 when the probe
 * failed or never ran.
 */
struct odi_board_laser {
	int laser_on;
	int ldd_id;
	int unverified;
};
void odi_board_laser_get(struct odi_board_laser *out);

#endif /* ODI_BOARD_H */
