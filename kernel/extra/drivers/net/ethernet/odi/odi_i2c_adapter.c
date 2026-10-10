// SPDX-License-Identifier: GPL-2.0
/*
 * odi_i2c_adapter.c -- the optics I2C port as an i2c adapter, i2c-0, so
 * the module's own devices (A0h = 0x50, A2h = 0x51: the identification
 * EEPROM and the laser driver with its DDM) are readable with the
 * standard tools: i2cget, i2cdump (busybox) on /dev/i2c-0 (i2c-dev).
 *
 * The controller is not a general I2C master: one transaction is a device
 * select, one byte address and one data byte (odi_i2c.h), so it cannot
 * carry an arbitrary i2c_msg sequence and offers no master_xfer. What it
 * does is exactly an SMBus byte-data access, so the adapter has an
 * smbus_xfer and nothing else:
 *
 *   read byte data       one transaction (i2cget, i2cdump mode b)
 *   read I2C block       one transaction per byte, under one lock hold
 *                        (i2cdump mode i)
 *   write byte data      the page select, A2h byte 127, only
 *                        (i2cset -y 0 0x51 0x7f <page>); any other
 *                        target is refused in odi_i2c_write_byte()
 *
 * No quick command and no receive byte: there is no transaction without a
 * byte address, so i2cdetect cannot scan this bus.
 *
 * Only 0x50 and 0x51 are answered; any other address gets -ENXIO without
 * a transaction. The controller does not report a missing device: a read
 * of an address nothing answers on ends with NOT_ACKED clear, and
 * I2C_READ_DATA still holds the byte of the transaction before it (on
 * the ISP1 stick, 0x52 read back the byte just read from 0x50). Passing
 * those through would show a device that is not there.
 *
 * Every transfer goes through odi_i2c_read_bytes()/odi_i2c_write_byte(),
 * which take odi_i2c_lock themselves: the DDM ioctl, cmd 10, the sdkinit
 * I2C verbs and this adapter all serialise on that one mutex, each
 * transaction carrying its own device select. The i2c core bus lock sits
 * outside it (bus lock -> odi_i2c_lock); nothing takes them the other way
 * round.
 *
 * No i2c client is registered and the adapter class is 0, so the i2c core
 * never probes this bus on its own: the only traffic on it is the DDM
 * reads and what userland asks for.
 */
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/errno.h>

#include "odi_i2c.h"

static int odi_i2c_adapter_xfer(struct i2c_adapter *adap, u16 addr,
				unsigned short flags, char read_write,
				u8 command, int size,
				union i2c_smbus_data *data)
{
	uint32_t sel = ODI_I2C_SETUP(addr);

	(void)adap;
	(void)flags;
	if (addr != ODI_I2C_DEV_A0 && addr != ODI_I2C_DEV_A2)
		return -ENXIO;
	switch (size) {
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ)
			return odi_i2c_read_bytes(sel, command, &data->byte, 1);
		return odi_i2c_write_byte(sel, command, data->byte);
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (read_write != I2C_SMBUS_READ)
			return -EOPNOTSUPP;
		if (data->block[0] == 0 || data->block[0] > I2C_SMBUS_BLOCK_MAX)
			return -EINVAL;
		return odi_i2c_read_bytes(sel, command, &data->block[1],
					  data->block[0]);
	default:
		return -EOPNOTSUPP;
	}
}

static u32 odi_i2c_adapter_func(struct i2c_adapter *adap)
{
	(void)adap;
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_READ_I2C_BLOCK;
}

static const struct i2c_algorithm odi_i2c_adapter_algo = {
	.smbus_xfer	= odi_i2c_adapter_xfer,
	.functionality	= odi_i2c_adapter_func,
};

static struct i2c_adapter odi_i2c_adapter = {
	.owner	= THIS_MODULE,
	.algo	= &odi_i2c_adapter_algo,
	.name	= "odi optics (SoC I2C port 1)",
	.nr	= 0,
};

/* Registration touches no register: a transfer before the sdkinit i2c
 * and i2cen verbs have run (rcS) times out or is not acknowledged, and
 * returns that.
 */
static int __init odi_i2c_adapter_init(void)
{
	return i2c_add_numbered_adapter(&odi_i2c_adapter);
}
device_initcall(odi_i2c_adapter_init);
