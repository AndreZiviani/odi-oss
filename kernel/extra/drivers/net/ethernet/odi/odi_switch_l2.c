// SPDX-License-Identifier: GPL-2.0
/*
 * The L2 lookup table: row readback, the valid-row walk behind the MAC
 * table, and L2 multicast add/delete for igmpd. odi_switch_l2.h has the
 * row layout and the register fields.
 *
 * The read side is checked against the hardware: a stick with four
 * learned MACs, read row by row, gives those four MACs back field for
 * field, and TABLE_STATUS.HIT set on exactly their rows (odi_switch_l2.h,
 * "Which rows are in use"). The write side follows the same layout; what
 * the host test pins there is the arithmetic (every field at its bit, the
 * handshake register values).
 */
#include "odi_switch_l2.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"

#ifndef ODI_SWITCH_TBL_MAX_SPINS
#define ODI_SWITCH_TBL_MAX_SPINS	1000U
#endif

/* Plain negative numbers, like ODI_SW_EOPNOTSUPP in odi_switch_tbl.h:
 * the host tests assert them back, and these three happen to be the same
 * on Linux and macOS, but spelling them out keeps the file free of a
 * host errno.h either way.
 */
#define ODI_SW_L2_ENOENT	(-2)
#define ODI_SW_L2_EINVAL	(-22)
#define ODI_SW_L2_ENOSPC	(-28)
#define ODI_SW_L2_EBUSY		(-1)

/* ---- Bit access over a three-word row ------------------------------- */

static uint32_t l2_bits(const uint32_t raw[ODI_SW_L2_WORDS], unsigned int lsb, unsigned int len)
{
	uint32_t v = 0;
	unsigned int i;

	for (i = 0; i < len; i++) {
		unsigned int b = lsb + i;

		if (raw[b / 32U] & (1U << (b % 32U)))
			v |= 1U << i;
	}
	return v;
}

static void l2_set_bits(uint32_t raw[ODI_SW_L2_WORDS], unsigned int lsb, unsigned int len, uint32_t v)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		unsigned int b = lsb + i;
		uint32_t m = 1U << (b % 32U);

		if (v & (1U << i))
			raw[b / 32U] |= m;
		else
			raw[b / 32U] &= ~m;
	}
}

/* Field positions: odi_switch_l2.h has the table. */
#define L2_MAC_LSB	0U
#define L2_KEY_LSB	48U
#define L2_KEY_LEN	12U
#define L2_FID_BIT	60U
#define L2_L3_BIT	61U
#define L2_STATIC_BIT	62U
#define L2_IVL_BIT	63U
#define L2_CTAG_BIT	64U
#define L2_SPA_LSB	65U
#define L2_SPA_LEN	2U
#define L2_AGE_LSB	67U
#define L2_AGE_LEN	3U
#define L2_AUTH_BIT	70U
#define L2_SABLK_BIT	71U
#define L2_DABLK_BIT	72U
#define L2_ARP_BIT	73U
#define L2_EXTSPA_LSB	74U
#define L2_EXTSPA_LEN	3U
#define L2_MBR_LSB	66U
#define L2_MBR_LEN	4U
#define L2_EXTMBR_LSB	70U
#define L2_EXTMBR_LEN	7U
#define L2_GIP_LEN	28U
#define L2_VALID_BIT	77U	/* written, never read: see odi_switch_l2.h */

#define L2_PORTS_MASK		((1U << L2_MBR_LEN) - 1U)
#define L2_EXT_PORTS_MASK	((1U << L2_EXTMBR_LEN) - 1U)

void odi_switch_l2_decode(struct odi_sw_l2_row *row)
{
	const uint32_t *raw = row->raw;
	uint32_t lo = l2_bits(raw, L2_MAC_LSB, 32);
	uint32_t hi = l2_bits(raw, L2_MAC_LSB + 32U, 16);
	unsigned int i;

	row->flags = 0;
	if (l2_bits(raw, L2_STATIC_BIT, 1))
		row->flags |= ODI_SW_L2_F_STATIC;
	if (l2_bits(raw, L2_IVL_BIT, 1))
		row->flags |= ODI_SW_L2_F_IVL;
	row->key = (uint16_t)l2_bits(raw, L2_KEY_LSB, L2_KEY_LEN);
	row->port = 0;
	row->ext_port = 0;
	row->age = 0;
	row->fid = 0;
	row->ports = 0;
	row->ext_ports = 0;
	row->group = 0;
	for (i = 0; i < 6; i++)
		row->mac[i] = 0;

	if (l2_bits(raw, L2_L3_BIT, 1)) {
		row->type = ODI_SW_L2_IPMC;
		row->group = l2_bits(raw, 0, L2_GIP_LEN);
		row->ports = l2_bits(raw, L2_MBR_LSB, L2_MBR_LEN);
		row->ext_ports = l2_bits(raw, L2_EXTMBR_LSB, L2_EXTMBR_LEN);
		return;
	}

	row->mac[0] = (uint8_t)(hi >> 8);
	row->mac[1] = (uint8_t)hi;
	row->mac[2] = (uint8_t)(lo >> 24);
	row->mac[3] = (uint8_t)(lo >> 16);
	row->mac[4] = (uint8_t)(lo >> 8);
	row->mac[5] = (uint8_t)lo;

	if (row->mac[0] & 1U) {
		row->type = ODI_SW_L2_MCAST;
		row->ports = l2_bits(raw, L2_MBR_LSB, L2_MBR_LEN);
		row->ext_ports = l2_bits(raw, L2_EXTMBR_LSB, L2_EXTMBR_LEN);
		return;
	}

	row->type = ODI_SW_L2_UCAST;
	row->fid = l2_bits(raw, L2_FID_BIT, 1);
	row->port = (uint8_t)l2_bits(raw, L2_SPA_LSB, L2_SPA_LEN);
	row->age = (uint8_t)l2_bits(raw, L2_AGE_LSB, L2_AGE_LEN);
	row->ext_port = (uint8_t)l2_bits(raw, L2_EXTSPA_LSB, L2_EXTSPA_LEN);
	if (l2_bits(raw, L2_CTAG_BIT, 1))
		row->flags |= ODI_SW_L2_F_CTAG;
	if (l2_bits(raw, L2_AUTH_BIT, 1))
		row->flags |= ODI_SW_L2_F_AUTH;
	if (l2_bits(raw, L2_SABLK_BIT, 1))
		row->flags |= ODI_SW_L2_F_SA_BLOCK;
	if (l2_bits(raw, L2_DABLK_BIT, 1))
		row->flags |= ODI_SW_L2_F_DA_BLOCK;
	if (l2_bits(raw, L2_ARP_BIT, 1))
		row->flags |= ODI_SW_L2_F_ARP;
}

int odi_switch_l2_mcast_mac_ok(const uint8_t mac[6])
{
	static const uint8_t reserved[5] = { 0x01, 0x80, 0xc2, 0x00, 0x00 };
	unsigned int i, all_ff = 1, in_reserved = 1;

	if (!(mac[0] & 1U))
		return 0;
	for (i = 0; i < 6; i++)
		if (mac[i] != 0xff)
			all_ff = 0;
	for (i = 0; i < 5; i++)
		if (mac[i] != reserved[i])
			in_reserved = 0;
	if (in_reserved && mac[5] > 0x0f)
		in_reserved = 0;
	return !all_ff && !in_reserved;
}

/* An L2 multicast row: static (the table must never age a group out on
 * its own; igmpd owns the lifetime), not an L3 route, the given key and
 * member masks, valid set. A delete sends the key alone -- MAC, VID or
 * filtering id, IVL -- with valid and everything else clear: the static
 * bit and the member masks are not part of what the hash matches on.
 */
int odi_switch_l2_mcast_encode(const struct odi_sw_l2_mcast_req *req, int valid,
			       uint32_t raw[ODI_SW_L2_WORDS])
{
	uint32_t lo, hi;
	unsigned int i;

	if (!odi_switch_l2_mcast_mac_ok(req->mac))
		return ODI_SW_L2_EINVAL;
	if (req->key >= (1U << L2_KEY_LEN) || req->ivl > 1U ||
	    (req->ports & ~L2_PORTS_MASK) || (req->ext_ports & ~L2_EXT_PORTS_MASK))
		return ODI_SW_L2_EINVAL;

	for (i = 0; i < ODI_SW_L2_WORDS; i++)
		raw[i] = 0;
	lo = ((uint32_t)req->mac[2] << 24) | ((uint32_t)req->mac[3] << 16) |
	     ((uint32_t)req->mac[4] << 8) | req->mac[5];
	hi = ((uint32_t)req->mac[0] << 8) | req->mac[1];
	l2_set_bits(raw, L2_MAC_LSB, 32, lo);
	l2_set_bits(raw, L2_MAC_LSB + 32U, 16, hi);
	l2_set_bits(raw, L2_KEY_LSB, L2_KEY_LEN, req->key);
	l2_set_bits(raw, L2_IVL_BIT, 1, req->ivl);
	if (valid) {
		l2_set_bits(raw, L2_STATIC_BIT, 1, 1);
		l2_set_bits(raw, L2_MBR_LSB, L2_MBR_LEN, req->ports);
		l2_set_bits(raw, L2_EXTMBR_LSB, L2_EXTMBR_LEN, req->ext_ports);
		l2_set_bits(raw, L2_VALID_BIT, 1, 1);
	}
	return 0;
}

/* ---- The engine ----------------------------------------------------- */

uint32_t odi_switch_l2_rows(void)
{
	if (odi_reg_read(ODI_SW_L2_LOOKUP_SETUP_OFF) & ODI_SW_L2_LOOKUP_SETUP_CAM_OFF)
		return ODI_SW_L2_HASH_ROWS;
	return ODI_SW_L2_HASH_ROWS + ODI_SW_L2_CAM_ROWS;
}

uint32_t odi_switch_l2_ipmc_mode(void)
{
	return (odi_reg_read(ODI_SW_L2_LOOKUP_SETUP_OFF) & ODI_SW_L2_LOOKUP_SETUP_IPMC_ON_GROUP) ? 1U : 0U;
}

/* Waits for TABLE_STATUS.BUSY to clear and returns the status word, or
 * sets *timed_out. A real bounded spin on both builds: the host model in
 * test/odi_switch_l2_test.c answers the status register itself.
 */
static uint32_t l2_wait(int *timed_out)
{
	uint32_t sts = 0;
	unsigned int i;

	for (i = 0; i < ODI_SWITCH_TBL_MAX_SPINS; i++) {
		sts = odi_reg_read(ODI_SW_TABLE_STATUS_OFF);
		if (!(sts & ODI_SW_TABLE_STATUS_IN_PROGRESS)) {
			*timed_out = 0;
			return sts;
		}
	}
	*timed_out = 1;
	return sts;
}

/* One access: the data words when given (a write, or a hash lookup,
 * which reads its key from them), TABLE_CMD, the wait. Returns the status
 * word through *sts.
 */
static int l2_access(uint32_t method, uint32_t row, int is_write,
		     const uint32_t raw[ODI_SW_L2_WORDS], uint32_t *sts)
{
	uint32_t ctrl;
	int timed_out;
	unsigned int i;

	lockdep_assert_held(&odi_switch_lock);

	(void)l2_wait(&timed_out);
	if (timed_out)
		return ODI_SW_L2_EBUSY;

	if (raw)
		for (i = 0; i < ODI_SW_L2_WORDS; i++)
			odi_reg_write(ODI_SW_TABLE_WRITE_WORD(i), raw[i]);

	ctrl = odi_reg_read(ODI_SW_TABLE_CMD_OFF);
	ctrl &= ~ODI_SW_TABLE_CMD_WALK_PORT_MASK;
	ctrl = ODI_SW_TABLE_CMD_ROW_SET(ctrl, row);
	ctrl = ODI_SW_TABLE_CMD_METHOD_SET(ctrl, method);
	ctrl = ODI_SW_TABLE_CMD_IS_WRITE_SET(ctrl, is_write ? 1U : 0U);
	ctrl = ODI_SW_TABLE_CMD_TABLE_KIND_SET(ctrl, ODI_SW_L2_TABLE_KIND);
	ctrl = ODI_SW_TABLE_CMD_START_SET(ctrl, 1);
	odi_reg_write(ODI_SW_TABLE_CMD_OFF, ctrl);

	*sts = l2_wait(&timed_out);
	return timed_out ? ODI_SW_L2_EBUSY : 0;
}

static uint32_t l2_sts_index(uint32_t sts)
{
	return ((sts & ODI_SW_TABLE_STATUS_IN_CAM) ? ODI_SW_L2_HASH_ROWS : 0U) +
	       (sts & ODI_SW_TABLE_STATUS_ROW_MASK);
}

int odi_switch_l2_read(uint32_t index, struct odi_sw_l2_row *row)
{
	uint32_t sts;
	unsigned int i;
	int rc;

	/* A CAM row is only read while the CAM is on: the stock driver never
	 * addresses one with it off, so neither does this.
	 */
	if (index >= odi_switch_l2_rows())
		return ODI_SW_L2_EINVAL;
	rc = l2_access(ODI_SW_L2_METHOD_ROW, index, 0, NULL, &sts);
	if (rc)
		return rc;
	row->index = index;
	for (i = 0; i < ODI_SW_L2_WORDS; i++)
		row->raw[i] = odi_reg_read(ODI_SW_TABLE_READ_WORD(i));
	odi_switch_l2_decode(row);
	/* In use or empty is the engine answer, not a bit of the row. */
	if (sts & ODI_SW_TABLE_STATUS_HIT)
		row->flags |= ODI_SW_L2_F_VALID;
	return 0;
}

int odi_switch_l2_next(uint32_t *index, struct odi_sw_l2_row *row)
{
	uint32_t rows = odi_switch_l2_rows();
	uint32_t i;

	for (i = *index; i < rows; i++) {
		int rc = odi_switch_l2_read(i, row);

		if (rc)
			return rc;
		if (row->flags & ODI_SW_L2_F_VALID) {
			*index = i;
			return 0;
		}
	}
	*index = rows;
	return ODI_SW_L2_ENOENT;
}

int odi_switch_l2_mcast_add(const struct odi_sw_l2_mcast_req *req, uint32_t *index)
{
	uint32_t raw[ODI_SW_L2_WORDS], sts;
	int rc = odi_switch_l2_mcast_encode(req, 1, raw);

	if (rc)
		return rc;
	rc = l2_access(ODI_SW_L2_METHOD_HASH, 0, 1, raw, &sts);
	if (rc)
		return rc;
	if (!(sts & ODI_SW_TABLE_STATUS_HIT))
		return ODI_SW_L2_ENOSPC;
	*index = l2_sts_index(sts);
	return 0;
}

int odi_switch_l2_mcast_del(const struct odi_sw_l2_mcast_req *req, int *found)
{
	uint32_t raw[ODI_SW_L2_WORDS], sts;
	int rc = odi_switch_l2_mcast_encode(req, 0, raw);

	if (rc)
		return rc;
	/* Look the key up first, and write only when it is there. HIT after
	 * the delete write itself says nothing: on the stick it comes back
	 * set for a key that was never in the table too (the engine reports
	 * the row the key hashes to either way), while a hash lookup read
	 * has HIT clear for an absent key and set, with the row, for a
	 * present one.
	 */
	rc = l2_access(ODI_SW_L2_METHOD_HASH, 0, 0, raw, &sts);
	if (rc)
		return rc;
	if (found)
		*found = (sts & ODI_SW_TABLE_STATUS_HIT) ? 1 : 0;
	if (!(sts & ODI_SW_TABLE_STATUS_HIT))
		return 0;
	return l2_access(ODI_SW_L2_METHOD_HASH, 0, 1, raw, &sts);
}
