/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_unity.h -- the switch core as one host unity build: the mock
 * register file, then every .c file the core links on target. A test
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
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_qos.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_ds_gem.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cf.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_platform.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_mib.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay.c"

#endif /* ODI_SWITCH_UNITY_H */
