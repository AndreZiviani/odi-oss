/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_sfp_mirror.h -- the host-side SFP EEPROM: the module's A0h and A2h,
 * with live diagnostics, served to the host (the router the stick is
 * plugged into) by the SoC's own SFP EEPROM emulation.
 *
 * The RTL9602C has an I2C slave on the SFP edge pins that answers the host
 * at 0x50 (A0h) and 0x51 (A2h) from a 512-byte SRAM: A0h bytes 0-255 at
 * SRAM 0-255, A2h bytes 0-255 at SRAM 256-511. Whether the host reaches it
 * depends on the board: on board revisions that wire it (HSGQ v08 /
 * DFP-34X-2C2 v08) the stock firmware turns it on and the host shows the
 * module's diagnostics; on v06 the host pins go to a separate fixed
 * EEPROM, and turning the slave on there changes nothing the host reads
 * (measured: the SRAM and the indirect access work, the host kept reading
 * the fixed EEPROM).
 *
 * Registers, from the chip's register table (offsets of the switch-core
 * window, odi_reg_read()/odi_reg_write(); diag shows them under the
 * mechanical names SW_0x0002a8 and so on, regnames.txt curates none):
 *
 *   0x268-0x2a4  host write protect, 16 words, one bit per SRAM byte:
 *                1 = the host cannot write that byte. Element i is bit
 *                i % 32 of word i / 32 (the table gives a one-bit stride
 *                from the base; the packing inside a word is the one
 *                assumption here not yet checked on hardware, and only
 *                the last word, A2h 224-255, depends on it).
 *   0x2a8        CPU access to the SRAM, one byte per command: CMD[26]
 *                starts it and reads 1 until it is done, WRITE[25] (1 =
 *                write), ADDR[24:16] the SRAM byte, WDATA[15:8] the byte
 *                to write, RDATA[7:0] the byte read.
 *   0x2ac        slave control: SLAVE_EN[0] answers the host; bits 1 and 2
 *                (standard mode, byte order) are already set as the reset
 *                leaves them and are not touched.
 *   0x23018      pin modes: bit 23 gives the SFP edge I2C pins to the slave.
 *
 * The enable is read-modify-write of exactly those two bits, nothing else.
 * Measured on a stock v08 stick with host diagnostics working (measurements
 * by Thaad-QR): 0x23018 = 0x01824040 and 0x2ac = 0x7; this image has
 * 0x01026040 and 0x6. Bit 13 of 0x23018 differs too: set here, clear on
 * that stock stick. It is the low bit of the two-bit I2C master pin field
 * (bits 14:13; the board init sets it around the first I2C master's setup
 * word) and is left as the boot sets it: why the stock image clears it is
 * not known. It is the first thing to try should the host still read
 * nothing on a v08 board with the slave on.
 *
 * Order, a hard rule: the whole SRAM is written and read back, and the
 * write protect set, before either enable bit, so the host never sees an
 * empty or partial EEPROM (it would lose the module identity, not just the
 * diagnostics). Nothing here writes the module: the mirror only reads it.
 *
 * Pure helpers here are shared with the host test (test/odi_sfp_mirror_test.c).
 */
#ifndef ODI_SFP_MIRROR_H
#define ODI_SFP_MIRROR_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#define ODI_SFPM_HOST_RO_OFF		0x000268U	/* 16 words */
#define ODI_SFPM_HOST_RO_WORDS		16U
#define ODI_SFPM_ACCESS_OFF		0x0002a8U
#define ODI_SFPM_ACCESS_CMD		(1U << 26)
#define ODI_SFPM_ACCESS_WRITE		(1U << 25)
#define ODI_SFPM_ACCESS_ADDR_SHIFT	16
#define ODI_SFPM_ACCESS_ADDR_MASK	0x1ffU
#define ODI_SFPM_ACCESS_WDATA_SHIFT	8
#define ODI_SFPM_ACCESS_RDATA_MASK	0xffU
#define ODI_SFPM_CTRL_OFF		0x0002acU
#define ODI_SFPM_CTRL_SLAVE_EN		(1U << 0)
#define ODI_SFPM_IO_MODE_OFF		0x023018U
#define ODI_SFPM_IO_MODE_SLAVE_PINS	(1U << 23)

/* One SRAM access is a few bus clocks; bounded at about 1 ms. */
#define ODI_SFPM_ACCESS_POLL_DELAY_US	1U
#define ODI_SFPM_ACCESS_POLL_US		1000U

#define ODI_SFPM_SIZE			512U
#define ODI_SFPM_A2(b)			(256U + (b))

/* SFF-8472 bytes this file touches. */
#define ODI_SFPM_A0_OPTIONS		93U	/* enhanced options */
#define ODI_SFPM_A0_CC_EXT		95U	/* checksum of A0h 64-94 */
#define ODI_SFPM_A2_LIVE_FIRST		96U	/* A2h 96-119: the five diagnostics, */
#define ODI_SFPM_A2_LIVE_LEN		24U	/* status/control, alarm and warning flags */
#define ODI_SFPM_A2_PASSWORD		123U	/* 123-126, write-only password entry */
#define ODI_SFPM_A2_PAGE_SELECT		127U
#define ODI_SFPM_A2_USER_FIRST		128U	/* 128-247: the user EEPROM, */
#define ODI_SFPM_A2_USER_LAST		247U	/* the only bytes the host may write */

/* A0h byte 93 bits this mirror serves, all from the module's own A2h as
 * refreshed (A2h 96-119): bit 7 alarm and warning flags (112-117), bit 5
 * soft TX_FAULT and bit 4 soft RX_LOS monitoring (byte 110, bits 2 and 1).
 * The other bits advertise controls the host would write (soft TX_DISABLE,
 * rate and application select, through bytes 110 and 118), and the host
 * cannot write A2h below 128 here.
 */
#define ODI_SFPM_A0_OPTIONS_SERVED	0xb0U

/* The refresh period of A2h 96-119. */
#define ODI_SFPM_REFRESH_MS		1000U

/* Word w of the host write protect: every byte read-only except A2h
 * 128-247.
 */
static inline uint32_t odi_sfpm_host_ro_word(unsigned int w)
{
	uint32_t v = 0;
	unsigned int b;

	for (b = 0; b < 32U; b++) {
		unsigned int i = w * 32U + b;

		if (i < ODI_SFPM_A2(ODI_SFPM_A2_USER_FIRST) || i > ODI_SFPM_A2(ODI_SFPM_A2_USER_LAST))
			v |= 1U << b;
	}
	return v;
}

/* A0h as the host gets it: the module's bytes, except that byte 93 keeps
 * only the options this mirror serves, with CC_EXT moved by the same
 * amount so that a checksum that was right stays right. Returns true when
 * byte 93 changed (the module advertised a control this mirror refuses).
 */
static inline bool odi_sfpm_a0_adjust(uint8_t a0[256])
{
	uint8_t old = a0[ODI_SFPM_A0_OPTIONS];
	uint8_t new = old & ODI_SFPM_A0_OPTIONS_SERVED;

	if (new == old)
		return false;
	a0[ODI_SFPM_A0_OPTIONS] = new;
	a0[ODI_SFPM_A0_CC_EXT] = (uint8_t)(a0[ODI_SFPM_A0_CC_EXT] - (uint8_t)(old - new));
	return true;
}

/* A2h as the host gets it: the password entry (write-only by SFF-8472) and
 * the page select read 0, the upper half is the module's page 0. When the
 * copy was taken with another page selected, the upper half is left
 * empty rather than show another page (nothing here selects a page: the
 * mirror never writes the module).
 */
static inline void odi_sfpm_a2_adjust(uint8_t a2[256], bool page0)
{
	unsigned int i;

	for (i = ODI_SFPM_A2_PASSWORD; i <= ODI_SFPM_A2_PAGE_SELECT; i++)
		a2[i] = 0;
	if (!page0)
		for (i = 128; i < 256; i++)
			a2[i] = 0;
}

/* The mirror image and what has been logged, so that a failure that lasts
 * is logged once, and its end once.
 */
#define ODI_SFPM_W_READ		0x1U	/* the module's A2h does not answer */
#define ODI_SFPM_W_SRAM		0x2U	/* an SRAM write did not complete */
#define ODI_SFPM_W_LOST		0x4U	/* an enable bit found cleared */

struct odi_sfpm {
	uint8_t img[ODI_SFPM_SIZE];	/* what the SRAM holds, as written */
	unsigned int warned;
};

/* One byte of the SRAM through the CPU access register. 0, or -ETIMEDOUT
 * when the command did not complete within ODI_SFPM_ACCESS_POLL_US.
 */
int odi_sfpm_sram_write(unsigned int addr, uint8_t val);
int odi_sfpm_sram_read(unsigned int addr, uint8_t *val);

/* The module's A2h 96-119, the diagnostic words read so that a refresh by
 * the module between their two bytes is caught (odi_ddm_get()).
 */
int odi_sfpm_read_live(uint8_t live[ODI_SFPM_A2_LIVE_LEN]);

/* Fills the SRAM from the module (A0h, A2h), reads it all back, sets the
 * host write protect, then the two enable bits, then reads a few bytes and
 * the bits back. 0 with the mirror serving the host; otherwise an error,
 * logged, and both enable bits as they were before (a failure after the
 * enable clears them again). Process context: sleeps on odi_i2c_lock.
 */
int odi_sfpm_start(struct odi_sfpm *m);

/* Clears the two enable bits, pin mode first. The SRAM is left as it is. */
void odi_sfpm_stop(void);

/* One refresh: A2h 96-119 from the module into the bytes of the SRAM that
 * changed, and the enable bits checked (set again, and logged once, when
 * something cleared them). A failed module read leaves the previous
 * values. 0, or the error of the module read.
 */
int odi_sfpm_refresh(struct odi_sfpm *m);

#ifdef __KERNEL__
/* The kernel side (odi_sfp_mirror.c): odi_switch.sfp_mirror and
 * odi_switch.sfp_mirror_state (odi_switch.c). A request on starts the fill
 * in a work item and returns; off stops the refresh and clears the enable
 * bits before it returns.
 */
void odi_sfp_mirror_request(bool on);
bool odi_sfp_mirror_requested(void);
const char *odi_sfp_mirror_state(void);
#endif

#endif /* ODI_SFP_MIRROR_H */
