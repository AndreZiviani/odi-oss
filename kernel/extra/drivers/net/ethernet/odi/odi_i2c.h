/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_i2c.h -- byte-at-a-time access to the SoC switch-core I2C indirect
 * controller, port-1 instance: the optics port (dfp34x-optics-on-i2c-
 * port1-gated-by-io-gpio-en.md), the only one a live capture ever showed
 * driven. Reads serve the DDM selectors (odi_ddm.c) and the I2C adapter
 * (odi_i2c_adapter.c, /dev/i2c-0); the one write it makes is the SFF-8472
 * page select, A2h byte 127 (odi_i2c_write_byte()).
 *
 * Register offsets: the register table the stock binary carries, names
 * from src/diag/tools/regnames.txt: I2C_WRITE_DATA, I2C_BYTE_ADDR, I2C_CMD
 * and I2C_READ_DATA ("array 0..1", i.e. two instances four bytes apart --
 * port 1 is base+4: 0xb0+4=0xb4, 0xb8+4=0xbc, 0xc0+4=0xc4, 0xc8+4=0xcc)
 * plus I2C_MASTER_SETUP (base 0x023004, port 1 instance 0x023008). I2C_CMD's
 * bit names are used directly; I2C_MASTER_SETUP's fields have no names
 * of ours, so ODI_I2C_SEL_A0/_A2 below are the two words a capture of a
 * working DDM read shows written verbatim; the fields they are now built
 * from are a decode of those words, asserted equal to them.
 *
 * Read shape, from a capture of `pon get transceiver <keyword>`
 * against a stock boot that still ran its own I2C init (one bracket
 * per keyword, every byte the same shape): write I2C_MASTER_SETUP (device
 * select), write I2C_BYTE_ADDR (target byte address), write I2C_CMD
 * with START set and WRITE clear (a read), poll I2C_CMD until IN_PROGRESS
 * clears, read I2C_READ_DATA. Repeated once per byte; the address increments
 * by one each time within a run (vendor-name: bytes 20-35, part-number:
 * 40-55, the five numeric fields: two bytes each at their SFF-8472 A2h
 * offset). odi_ddm.c owns that per-keyword address/length table.
 *
 * Write shape, observed on the device: the same device select, the byte
 * in I2C_WRITE_DATA, the target address in I2C_BYTE_ADDR, then I2C_CMD
 * with START and WRITE set, and the same poll. Writing 0, 2, 4, 5 and 6
 * to A2h byte 127 this way selected each of those pages of the module
 * (the upper half read back different contents), and byte 127 read back
 * the value written.
 */
#ifndef ODI_I2C_H
#define ODI_I2C_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define ODI_I2C_MASTER_SETUP    0x00023008u	/* I2C_MASTER_SETUP, port-1 instance */
#define ODI_I2C_WRITE_DATA      0x000000b4u	/* I2C_WRITE_DATA, port-1 instance: low byte is the byte to write */
#define ODI_I2C_BYTE_ADDR       0x000000bcu	/* I2C_BYTE_ADDR, port-1 instance: target byte address */
#define ODI_I2C_CMD             0x000000c4u	/* I2C_CMD, port-1 instance */
#define ODI_I2C_READ_DATA       0x000000ccu	/* I2C_READ_DATA, port-1 instance: low byte is the byte just read */

/* I2C_CMD bits (register id 44 in the table). */
#define ODI_I2C_CMD_START       0x00000001u	/* START -- write 1 to start */
#define ODI_I2C_CMD_WRITE       0x00000002u	/* WRITE -- 1 = write, 0 = read */
#define ODI_I2C_CMD_IN_PROGRESS 0x00000004u	/* IN_PROGRESS -- 1 while the transaction runs */
#define ODI_I2C_CMD_NOT_ACKED   0x00000008u	/* NOT_ACKED -- device did not acknowledge */

/* I2C_MASTER_SETUP fields. The decode is cross-checked against an
 * independent driver for a later chip of the same family; the values are
 * unchanged from the captured words. Both words carry data width and
 * memory-address width 0; what the other codes mean is not decoded.
 *   9:0    CLK_DIV, clock divider (0x13a in both words)
 *   11:10  data width code
 *   13:12  memory-address width code
 *   20:14  DEV_ADDR, the 7-bit bus address: 0x50 (A0h) or 0x51 (A2h)
 *   21, 25 set in both captured words; not decoded
 */
#define ODI_I2C_SETUP_CLK_DIV(v)	(((uint32_t)(v) & 0x3ffu) << 0)
#define ODI_I2C_SETUP_DATA_WIDTH(v)	(((uint32_t)(v) & 0x3u) << 10)
#define ODI_I2C_SETUP_MEM_ADDR_WIDTH(v)	(((uint32_t)(v) & 0x3u) << 12)
#define ODI_I2C_SETUP_DEV_ADDR(v)	(((uint32_t)(v) & 0x7fu) << 14)
#define ODI_I2C_SETUP_BIT21		(1u << 21)	/* set, not decoded */
#define ODI_I2C_SETUP_BIT25		(1u << 25)	/* set, not decoded */

#define ODI_I2C_SETUP_CLK_DIV_CAPTURED	0x13au
#define ODI_I2C_SETUP(dev_addr) \
	(ODI_I2C_SETUP_CLK_DIV(ODI_I2C_SETUP_CLK_DIV_CAPTURED) | \
	 ODI_I2C_SETUP_DATA_WIDTH(0) | ODI_I2C_SETUP_MEM_ADDR_WIDTH(0) | \
	 ODI_I2C_SETUP_DEV_ADDR(dev_addr) | ODI_I2C_SETUP_BIT21 | ODI_I2C_SETUP_BIT25)

/* The two I2C_MASTER_SETUP words the capture writes: SFF-8472 device address
 * A0h (identification: vendor name, part number) and A2h (DDMI:
 * temperature, voltage, bias current, Tx/Rx power).
 */
#define ODI_I2C_SEL_A0   ODI_I2C_SETUP(0x50)
#define ODI_I2C_SEL_A2   ODI_I2C_SETUP(0x51)

/* The values are the captured literals, bit for bit. */
_Static_assert(ODI_I2C_SEL_A0 == 0x0234013au, "I2C A0h setup word changed");
_Static_assert(ODI_I2C_SEL_A2 == 0x0234413au, "I2C A2h setup word changed");

/* Bound on the IN_PROGRESS poll: about 5 ms, 512 reads 10 us apart. The
 * longest run the capture shows was about 116 consecutive IN_PROGRESS
 * reads of the stock loop before the clear.
 */
#define ODI_I2C_POLL_DELAY_US	10u
#define ODI_I2C_POLL_US		5120u

/* SFF-8472 bus addresses and the page select byte: A2h byte 127 picks
 * which page the upper half of A2h (bytes 128-255) shows. It is the only
 * byte this driver set ever writes. The other pages belong to the laser
 * driver chip behind A2h (its calibration and look-up tables); a stray
 * write there could change how the laser is driven, so none is possible
 * from here.
 */
#define ODI_I2C_DEV_A0		0x50u
#define ODI_I2C_DEV_A2		0x51u
#define ODI_I2C_A2_PAGE_SELECT	0x7fu

/* Reads `n` sequential bytes starting at `addr` from the device `sel`
 * selects (ODI_I2C_SEL_A0/_A2, or ODI_I2C_SETUP() of any 7-bit address),
 * through the port-1 I2C indirect controller, one byte per captured
 * transaction. Returns 0 on success, -ETIMEDOUT if IN_PROGRESS never
 * cleared within ODI_I2C_POLL_US, -ENXIO if the device did not
 * acknowledge, -EINVAL for a run past byte 255.
 */
int odi_i2c_read_bytes(uint32_t sel, uint32_t addr, uint8_t *out, unsigned int n);

/* Writes one byte, `val`, at `addr` of the device `sel` selects. Only the
 * page select (ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT) is accepted; any
 * other target returns -EPERM without touching the controller. Otherwise
 * the same returns as odi_i2c_read_bytes().
 */
int odi_i2c_write_byte(uint32_t sel, uint32_t addr, uint8_t val);

#endif /* ODI_I2C_H */
