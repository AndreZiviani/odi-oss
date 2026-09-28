/* hw_fake_common.inc -- the MIB and L2 fixture code every exporter/optics
 * scenario fixture shares (hw_fake.c and the scripted scenario fixtures:
 * hw_fake_rx_drift.c, hw_fake_los.c, hw_fake_absent.c). None of the
 * scenarios below touch MIB counters or the L2 table, only the transceiver
 * and its alarm/warning/LOS status, so this half of the fixture is common
 * and included verbatim rather than repeated four times.
 *
 * A .inc, not a .c: it defines symbols (the __wrap_* functions), so it must
 * be #include'd into exactly one translation unit, never built or linked
 * on its own.
 */

/* The counters the driver has no register for, by mib.h index: the real
 * odi_switch refuses about a third of them, and a refused counter must be
 * left out of the dump, not printed as zero. */
static int refused(uint32_t counter)
{
	return counter % 3 == 1 && counter != 1;
}

int __wrap_hw_stat_port_get(uint32_t port, uint32_t counter, uint64_t *value);
int __wrap_hw_stat_port_get(uint32_t port, uint32_t counter, uint64_t *value)
{
	if (port >= FAKE_PORTS || counter >= MIB_COUNT || refused(counter))
		return -22;
	/* Port 1 is all zeros, as an unpopulated port reads; port 3 carries
	 * one value past 32 bits, which the 25-wide column must hold. */
	if (port == 1)
		*value = 0;
	else if (port == 3 && counter == 0)
		*value = 0x123456789abcULL;
	else
		/* 32-bit arithmetic: a 64-bit multiply is a libgcc call. */
		*value = (port + 1) * 1000u * (counter + 1) + counter;
	return 0;
}

/* The L2 table for test/l2.txt: 1024 rows (the CAM rows off, as the stock
 * init leaves them), three valid rows -- a learned host on the PON side,
 * one on the UNI, and a multicast group with the UNI as its only member.
 * Row 9 holds data but is not valid, so the listing must skip it and the
 * single-row read must still show it. */
#include "odi_sw_ioctl.h"

static void fake_l2_row(uint32_t index, struct odi_sw_l2_row *r)
{
	static const uint8_t pon_host[6] = { 0x78, 0x54, 0x2e, 0x07, 0x64, 0x63 };
	static const uint8_t uni_host[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const uint8_t group[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 };
	const uint8_t *mac = 0;

	for (unsigned i = 0; i < sizeof *r; i++)
		((uint8_t *)r)[i] = 0;
	r->index = index;
	if (index == 8) {
		mac = pon_host;
		r->type = ODI_SW_L2_UCAST;
		r->port = 2;
		r->age = 6;
		r->key = 1;
		r->flags = ODI_SW_L2_F_VALID;
		r->raw[0] = 0x2e076463u;
		r->raw[1] = 0x00017854u;
		r->raw[2] = 0x00002034u;
	} else if (index == 9) {
		mac = uni_host;
		r->type = ODI_SW_L2_UCAST;
		r->raw[0] = 0x22334455u;
		r->raw[1] = 0x00000211u;
	} else if (index == 0x4c) {
		mac = uni_host;
		r->type = ODI_SW_L2_UCAST;
		r->port = 0;
		r->age = 7;
		r->key = 1;
		r->flags = ODI_SW_L2_F_VALID;
		r->raw[0] = 0x22334455u;
		r->raw[1] = 0x00010211u;
		r->raw[2] = 0x00002038u;
	} else if (index == 0x3a1) {
		mac = group;
		r->type = ODI_SW_L2_MCAST;
		r->key = 1;
		r->ports = 0x1;
		r->flags = ODI_SW_L2_F_VALID | ODI_SW_L2_F_STATIC | ODI_SW_L2_F_IVL;
		r->raw[0] = 0x5e010203u;
		r->raw[1] = 0xc0010100u;
		r->raw[2] = 0x00002004u;
	}
	if (mac)
		for (int i = 0; i < 6; i++)
			r->mac[i] = mac[i];
}

int __wrap_hw_l2_mode(uint32_t *rows, uint32_t *ipmc_on_group);
int __wrap_hw_l2_mode(uint32_t *rows, uint32_t *ipmc_on_group)
{
	*rows = 1024;
	*ipmc_on_group = 0;
	return 0;
}

int __wrap_hw_l2_get(uint32_t index, struct odi_sw_l2_row *row);
int __wrap_hw_l2_get(uint32_t index, struct odi_sw_l2_row *row)
{
	if (index >= 1024)      /* the CAM rows are off */
		return -22;
	fake_l2_row(index, row);
	return 0;
}

int __wrap_hw_l2_next(uint32_t *index, struct odi_sw_l2_row *row);
int __wrap_hw_l2_next(uint32_t *index, struct odi_sw_l2_row *row)
{
	for (uint32_t i = *index; i < 1024; i++) {
		fake_l2_row(i, row);
		if (row->flags & ODI_SW_L2_F_VALID) {
			*index = i;
			return 0;
		}
	}
	return 1;
}

/* gpon: unchanged across every optics scenario -- O5, LOF asserted, same
 * as hw_fake.c always answered before the optics scenarios existed. */
int __wrap_hw_gpon_status_get(uint32_t *state);
int __wrap_hw_gpon_status_get(uint32_t *state)
{
	*state = 5;
	return 0;
}

int __wrap_hw_gpon_alarms_get(uint32_t *alarms, uint32_t *known);
int __wrap_hw_gpon_alarms_get(uint32_t *alarms, uint32_t *known)
{
	*alarms = GPON_ALARM_LOF;
	*known = GPON_ALARM_KNOWN;
	return 0;
}
