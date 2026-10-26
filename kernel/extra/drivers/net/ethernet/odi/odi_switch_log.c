// SPDX-License-Identifier: GPL-2.0
/*
 * The ACL log counters and the one register their owners share
 * (odi_switch_log.h says who owns what, and why LOG_TYPE is never read).
 */
#include "odi_switch_log.h"
#include "odi_switch_dal.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"

static uint32_t log_type;

void odi_sw_log_unit(uint32_t counter, uint32_t bytes)
{
	uint32_t bit = 1U << ((counter % ODI_SW_LOG_COUNTERS) / 2U);

	if (bytes)
		log_type |= bit;
	else
		log_type &= ~bit;
	odi_reg_write(ODI_SW_LOG_TYPE_OFF, log_type);
}

uint32_t odi_sw_log_read(uint32_t counter)
{
	return odi_reg_read(ODI_SW_LOG_CNT_OFF + 4U * (counter % ODI_SW_LOG_COUNTERS));
}
