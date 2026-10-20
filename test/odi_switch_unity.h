/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_unity.h -- the switch core as one host unity build: the mock
 * register file, then every .c file the core links on target (the I2C
 * master and the DDM read included, with a module to answer them). A test
 * includes this instead of listing the files itself, so a file added to
 * or split out of the core changes this list and no test.
 *
 * A test that drives more than the core (the command layer, sdkinit, the
 * board replay, GPON) includes those .c files after this header.
 */
#ifndef ODI_SWITCH_UNITY_H
#define ODI_SWITCH_UNITY_H

#include "odi_switch_mock.h"
#include "odi_soc_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_soc.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_port.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_l2.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_qos.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_ds_gem.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cf.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_platform.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_mib.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_ddm.c"

/* The optics behind the port-1 I2C master, for odi_i2c.c's host hook:
 * the A0h and A2h pages, all zero unless a test fills them (cmd 10 reads
 * A2h through odi_ddm_get()). test/odi_optics_model.h is the fuller model
 * (NACK, the GPIO gate) for the tests that build odi_i2c.c on their own.
 */
static uint8_t odi_unity_sfp_a0[256];
static uint8_t odi_unity_sfp_a2[256];

uint8_t odi_i2c_mock_byte(uint32_t sel, uint32_t addr)
{
	const uint8_t *page = sel == ODI_I2C_SEL_A0 ? odi_unity_sfp_a0 : odi_unity_sfp_a2;

	return page[addr & 0xffU];
}

#endif /* ODI_SWITCH_UNITY_H */
