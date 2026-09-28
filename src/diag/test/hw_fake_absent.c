/* Scripted scenario: transceiver absent. Every I2C transaction NACKs
 * (odi_i2c_read_bytes() -> -ENXIO, odi_ddm_get() propagates it, the ioctl
 * answers EOPNOTSUPP) regardless of selector, matching what test/
 * odi_optics_model.h's write-hook model does for the kernel-driver path;
 * this fixture is the same failure at the diag-CLI layer. Every other
 * accessor (gpon, mib, l2) is hw_fake.c's baseline scenario: they are on a
 * different bus and do not depend on the optical module being present.
 */
#include <stdint.h>
#include "hw.h"
#include "gpon_status.h"

#define FAKE_PORTS 4

int __wrap_hw_transceiver_get(int sel, uint8_t out[DDM_RAW_LEN]);
int __wrap_hw_transceiver_get(int sel, uint8_t out[DDM_RAW_LEN])
{
	(void)sel;
	for (int i = 0; i < DDM_RAW_LEN; i++)
		out[i] = 0;
	return -1;
}

int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los);
int __wrap_hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los)
{
	*alarms = 0;
	*warnings = 0;
	*los = 0;
	return -1;
}

#include "hw_fake_common.inc"
