/* The same conversion vectors as ddm_test.c, but built for the target and run
 * under qemu. The host test proves the maths; this proves it survives
 * big-endian MIPS, 32-bit longs, and the absence of libgcc -- which is where
 * fixed-point arithmetic tends to go wrong. Output is diffed against the host
 * test's expectations by `make selftest`. */
#include "ddm.h"
#include "hw.h"
#include "io.h"

static void show(int type, unsigned raw)
{
	unsigned char blk[24];
	char got[64];
	int i;

	for (i = 0; i < 24; i++)
		blk[i] = 0;
	blk[0] = (unsigned char)(raw >> 8);
	blk[1] = (unsigned char)(raw & 0xff);
	ddm_format(type, blk, got, sizeof got);
	out_fmt("%d %u %s\n", (long)type, (unsigned long)raw, got);
}

int main(void)
{
	show(DDM_TEMPERATURE, 0x2A00);
	show(DDM_TEMPERATURE, 0x1980);
	show(DDM_TEMPERATURE, 0xF600);
	show(DDM_VOLTAGE, 32800);
	show(DDM_VOLTAGE, 33000);
	show(DDM_BIAS_CURRENT, 6200);
	show(DDM_BIAS_CURRENT, 1);
	show(DDM_RX_POWER, 111);
	show(DDM_RX_POWER, 744);
	show(DDM_TX_POWER, 17400);
	show(DDM_RX_POWER, 1);
	show(DDM_RX_POWER, 65535);
	show(DDM_RX_POWER, 10000);
	out_flush();
	return 0;
}
