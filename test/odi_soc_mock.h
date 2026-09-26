/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_soc_mock.h -- the SoC window of odi_soc.c on the host: a flat
 * register file behind the allowlist, with access counts. Included before
 * odi_soc.c in a unity build. When test/odi_switch_mock.h is in the same
 * unit, every store also goes into its write log as kind 'w', at the KSEG1
 * address the replay tables carry, so a golden of the switch writes shows
 * the SoC writes in their place.
 */
#ifndef ODI_SOC_MOCK_H
#define ODI_SOC_MOCK_H

#include <stdint.h>
#include <string.h>
#include "../kernel/extra/drivers/net/ethernet/odi/odi_soc.h"

struct odi_soc_mock_state {
	uint32_t regs[ODI_SOC_SIZE / 4];
	unsigned int reads;
	unsigned int writes;
};

static struct odi_soc_mock_state odi_soc_mock;

static inline void odi_soc_mock_reset(void)
{
	memset(&odi_soc_mock, 0, sizeof(odi_soc_mock));
}

static inline uint32_t odi_soc_mock_read(uint32_t off)
{
	odi_soc_mock.reads++;
	return odi_soc_mock.regs[off / 4];
}

static inline void odi_soc_mock_write(uint32_t off, uint32_t val)
{
	odi_soc_mock.writes++;
	odi_soc_mock.regs[off / 4] = val;
#ifdef ODI_MOCK_LOG_MAX
	if (odi_mock.log_n < ODI_MOCK_LOG_MAX) {
		struct odi_mock_write *e = &odi_mock.log[odi_mock.log_n++];

		e->ns = odi_mock.ns;
		e->kind = 'w';
		e->addr = ODI_SOC_KSEG1 + off;
		e->val = val;
	}
	odi_mock.ns += 10000;
#endif
}

#endif /* ODI_SOC_MOCK_H */
