/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_i2c.h -- byte-at-a-time read of the SoC switch-core I2C indirect
 * controller, port-1 instance: the optics port (dfp34x-optics-on-i2c-
 * port1-gated-by-io-gpio-en.md), the only one a live capture ever showed
 * driven.
 *
 * Register offsets: the register table the stock binary carries, names
 * from src/diag/tools/regnames.txt: I2C_BYTE_ADDR, I2C_CMD and
 * I2C_READ_DATA ("array 0..1", i.e. two instances four bytes apart --
 * port 1 is base+4: 0xb8+4=0xbc, 0xc0+4=0xc4, 0xc8+4=0xcc) plus
 * I2C_MASTER_SETUP (base 0x023004, port 1 instance 0x023008). I2C_CMD's
 * bit names are used directly; I2C_MASTER_SETUP's fields have no names
 * of ours, so ODI_I2C_SEL_A0/_A2 below are the two words a capture of a
 * working DDM read shows written verbatim, never reconstructed field by
 * field.
 *
 * Transaction shape, from a capture of `pon get transceiver <keyword>`
 * against a stock boot that still ran its own I2C init (one bracket
 * per keyword, every byte the same shape): write I2C_MASTER_SETUP (device
 * select), write I2C_BYTE_ADDR (target byte address), write I2C_CMD
 * with START set and WRITE clear (a read), poll I2C_CMD until IN_PROGRESS
 * clears, read I2C_READ_DATA. Repeated once per byte; the address increments
 * by one each time within a run (vendor-name: bytes 20-35, part-number:
 * 40-55, the five numeric fields: two bytes each at their SFF-8472 A2h
 * offset). odi_ddm.c owns that per-keyword address/length table.
 */
#ifndef ODI_I2C_H
#define ODI_I2C_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define ODI_I2C_MASTER_SETUP    0x00023008u	/* I2C_MASTER_SETUP, port-1 instance */
#define ODI_I2C_BYTE_ADDR       0x000000bcu	/* I2C_BYTE_ADDR, port-1 instance: target byte address */
#define ODI_I2C_CMD             0x000000c4u	/* I2C_CMD, port-1 instance */
#define ODI_I2C_READ_DATA       0x000000ccu	/* I2C_READ_DATA, port-1 instance: low byte is the byte just read */

/* I2C_CMD bits (register id 44 in the table). */
#define ODI_I2C_CMD_START       0x00000001u	/* START -- write 1 to start */
#define ODI_I2C_CMD_WRITE       0x00000002u	/* WRITE -- 1 = write, 0 = read; always 0 here */
#define ODI_I2C_CMD_IN_PROGRESS 0x00000004u	/* IN_PROGRESS -- 1 while the transaction runs */
#define ODI_I2C_CMD_NOT_ACKED   0x00000008u	/* NOT_ACKED -- device did not acknowledge */

/* The two I2C_MASTER_SETUP words the capture writes: SFF-8472 device address
 * A0h (identification: vendor name, part number) and A2h (DDMI:
 * temperature, voltage, bias current, Tx/Rx power). Captured verbatim,
 * see this file's own header comment.
 */
#define ODI_I2C_SEL_A0   0x0234013au
#define ODI_I2C_SEL_A2   0x0234413au

/* Bound on the IN_PROGRESS poll: about 5 ms, 512 reads 10 us apart. The
 * longest run the capture shows was about 116 consecutive IN_PROGRESS
 * reads of the stock loop before the clear.
 */
#define ODI_I2C_POLL_DELAY_US	10u
#define ODI_I2C_POLL_US		5120u

/* Reads `n` sequential bytes starting at `addr` from the SFF-8472 device
 * `sel` selects (ODI_I2C_SEL_A0/_A2), through the port-1 I2C indirect
 * controller, one byte per captured transaction. Returns 0 on success,
 * -ETIMEDOUT if IN_PROGRESS never cleared within ODI_I2C_POLL_US, -ENXIO
 * if the device did not acknowledge.
 */
int odi_i2c_read_bytes(uint32_t sel, uint32_t addr, uint8_t *out, unsigned int n);

#endif /* ODI_I2C_H */
