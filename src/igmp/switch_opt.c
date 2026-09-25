#include "switch_opt.h"
#include "entry.h"
#include "sys.h"

/* The raw-socket protocol the stock driver hooks its options on. */
#define SWITCH_OPT_PROTO   0xff
#define SWITCH_OPT_BUF     516

#define OPT_IPMC_MODE_GET  0x2a5c
#define OPT_MCAST_ADD      0x2a58
#define OPT_MCAST_DEL      0x2a59

/* Offsets into the exchange buffer. */
#define OFF_IPMC_MODE      0x19c
#define OFF_MCAST_ENTRY    0x178
#define MCAST_ENTRY_WORDS  IGMP_MAC_ENTRY_WORDS

static void put32(uint8_t *b, unsigned off, uint32_t v)
{
	b[off] = (uint8_t)(v >> 24);
	b[off + 1] = (uint8_t)(v >> 16);
	b[off + 2] = (uint8_t)(v >> 8);
	b[off + 3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *b, unsigned off)
{
	return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16)
	     | ((uint32_t)b[off + 2] << 8) | b[off + 3];
}

static void zero(uint8_t *b, unsigned n)
{
	while (n--)
		*b++ = 0;
}

/* One socket per exchange, opened and closed around it. */
static int opt_get(int opt, void *buf, unsigned len)
{
	unsigned l = len;
	long fd = sys_socket(AF_INET, SOCK_RAW, SWITCH_OPT_PROTO), rc;

	if (fd < 0)
		return -1;
	rc = sys_getsockopt((int)fd, 0, opt, buf, &l);
	sys_close((int)fd);
	return (int)rc;
}

static int opt_set(int opt, const void *buf, unsigned len)
{
	long fd = sys_socket(AF_INET, SOCK_RAW, SWITCH_OPT_PROTO), rc;

	if (fd < 0)
		return -1;
	rc = sys_setsockopt((int)fd, 0, opt, buf, len);
	sys_close((int)fd);
	return (int)rc;
}

int rtk_l2_ipmcMode_get(uint32_t *mode)
{
	uint8_t buf[SWITCH_OPT_BUF];
	int rc;

	zero(buf, sizeof buf);
	put32(buf, OFF_IPMC_MODE, *mode);
	rc = opt_get(OPT_IPMC_MODE_GET, buf, sizeof buf);
	if (rc)
		return rc;
	*mode = get32(buf, OFF_IPMC_MODE);
	return 0;
}

int rtk_l2_mcastAddr_add(uint32_t *entry)
{
	uint8_t buf[SWITCH_OPT_BUF];
	int rc;

	zero(buf, sizeof buf);
	for (unsigned i = 0; i < MCAST_ENTRY_WORDS; i++)
		put32(buf, OFF_MCAST_ENTRY + 4 * i, entry[i]);
	rc = opt_set(OPT_MCAST_ADD, buf, sizeof buf);
	if (rc)
		return rc;
	for (unsigned i = 0; i < MCAST_ENTRY_WORDS; i++)
		entry[i] = get32(buf, OFF_MCAST_ENTRY + 4 * i);
	return 0;
}

int rtk_l2_mcastAddr_del(const uint32_t *entry)
{
	uint8_t buf[SWITCH_OPT_BUF];

	zero(buf, sizeof buf);
	for (unsigned i = 0; i < MCAST_ENTRY_WORDS; i++)
		put32(buf, OFF_MCAST_ENTRY + 4 * i, entry[i]);
	return opt_set(OPT_MCAST_DEL, buf, sizeof buf);
}
