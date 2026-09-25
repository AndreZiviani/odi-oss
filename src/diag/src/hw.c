#include "hw.h"
#include "sys.h"
#include "gpon_status.h"
#include "odi_sw_ioctl.h"
#include "nl.h"
#include "omci_flows.h"

#define ODI_SW_DEV_PATH   "/dev/odi_sw"
#define ODI_GPON_PROC     "/proc/odi_gpon"
#define ODI_EOPNOTSUPP    (-95)

/* Netlink reply wait: the kernel answers in well under a millisecond, so
 * this only bounds how long a missing handler can stall one command. */
#define FLOWS_TIMEOUT_US  200000u
#define FLOWS_RETRIES     5

/* One descriptor for the whole run. The ioctls are stateless, and a batch
 * of a few thousand `register set` lines (regreplay) would otherwise pay an
 * open and a close for each. A failed open is retried on the next call. */
static long sw_fd = -1;

static int sw_ioctl(unsigned long req, void *arg)
{
	if (sw_fd < 0) {
		sw_fd = sys_open(ODI_SW_DEV_PATH, O_RDWR);
		if (sw_fd < 0)
			return (int)sw_fd;
	}
	return (int)sys_ioctl((int)sw_fd, req, arg);
}

static int swcore_ok(uint32_t addr)
{
	return !(addr & 3) && addr < SWCORE_SIZE;
}

int hw_addr_get(uint32_t addr, uint32_t *value)
{
	struct odi_sw_reg r;
	int rc;

	if (!swcore_ok(addr))
		return -1;
	r.addr = addr;
	r.value = 0;
	rc = sw_ioctl(ODI_SW_IOC_REG_GET, &r);
	if (rc != 0)
		return rc;
	*value = r.value;
	return 0;
}

int hw_addr_set(uint32_t addr, uint32_t value)
{
	struct odi_sw_reg r;

	if (!swcore_ok(addr))
		return -1;
	r.addr = addr;
	r.value = value;
	return sw_ioctl(ODI_SW_IOC_REG_SET, &r);
}

int hw_transceiver_get(int sel, uint8_t out[DDM_RAW_LEN])
{
	struct odi_sw_ddm d;
	int i, rc;

	if (sel < 0 || sel >= DDM_SEL_COUNT)
		return -1;
	d.type = (uint32_t)sel;
	for (i = 0; i < DDM_RAW_LEN; i++)
		d.raw[i] = 0;
	rc = sw_ioctl(ODI_SW_IOC_DDM_GET, &d);
	if (rc != 0)
		return rc;
	for (i = 0; i < DDM_RAW_LEN; i++)
		out[i] = d.raw[i];
	return 0;
}

int hw_stat_port_get(uint32_t port, uint32_t counter, uint64_t *value)
{
	struct odi_sw_mib m;
	int rc;

	m.port = port;
	m.counter = counter;
	m.value = 0;
	rc = sw_ioctl(ODI_SW_IOC_MIB_GET, &m);
	if (rc != 0)
		return rc;
	*value = m.value;
	return 0;
}

#define ODI_ENOENT (-2)

int hw_l2_get(uint32_t index, struct odi_sw_l2_row *row)
{
	row->index = index;
	return sw_ioctl(ODI_SW_IOC_L2_GET, row);
}

int hw_l2_next(uint32_t *index, struct odi_sw_l2_row *row)
{
	int rc;

	row->index = *index;
	rc = sw_ioctl(ODI_SW_IOC_L2_NEXT, row);
	if (rc == ODI_ENOENT)
		return 1;
	if (rc != 0)
		return rc;
	*index = row->index;
	return 0;
}

int hw_l2_mode(uint32_t *rows, uint32_t *ipmc_on_group)
{
	struct odi_sw_l2_mode m;
	int rc;

	m.rows = 0;
	m.ipmc_on_group = 0;
	rc = sw_ioctl(ODI_SW_IOC_L2_MODE, &m);
	if (rc != 0)
		return rc;
	*rows = m.rows;
	*ipmc_on_group = m.ipmc_on_group;
	return 0;
}

/* Reads the whole of /proc/odi_gpon into buf. Returns the byte count, or a
 * negative value when the file cannot be opened or is empty. */
static long gpon_proc_read(char *buf, unsigned long max)
{
	long fd = sys_open(ODI_GPON_PROC, O_RDONLY), n, got = 0;

	if (fd < 0)
		return fd;
	while (got < (long)max &&
	       (n = sys_read((int)fd, buf + got, max - (unsigned long)got)) > 0)
		got += n;
	sys_close((int)fd);
	return got > 0 ? got : -1;
}

int hw_gpon_status_get(uint32_t *state)
{
	char buf[32];
	long fd = sys_open(ODI_GPON_PROC, O_RDONLY), n;

	/* The state is on the first line, so one short read is enough. */
	if (fd < 0)
		return (int)fd;
	n = sys_read((int)fd, buf, sizeof buf);
	sys_close((int)fd);
	if (n <= 0)
		return -1;
	return gpon_proc_parse_state(buf, (unsigned)n, state);
}

/* The live register first: it answers all three alarms and needs no
 * interrupt to have happened. The /proc LOS sample is what is left when
 * /dev/odi_sw cannot be read. */
int hw_gpon_alarms_get(uint32_t *alarms, uint32_t *known)
{
	static char buf[2048];
	uint32_t sts = 0, los = 0;
	long got;

	if (hw_addr_get(GPON_DS_INTR_STS, &sts) == 0) {
		*alarms = gpon_alarms_from_sts(sts);
		*known = GPON_ALARM_KNOWN;
		return 0;
	}
	got = gpon_proc_read(buf, sizeof buf);
	if (got <= 0 || gpon_proc_parse_los(buf, (unsigned)got, &los) != 0)
		return -1;
	*alarms = los ? GPON_ALARM_LOS : 0;
	*known = GPON_ALARM_LOS;
	return 0;
}

/* Autobinds (pid 0) and never registers for a redirect type, so omcid keeps
 * the OMCI channel while this runs. */
int hw_gpon_flows_get(struct omci_flows *f)
{
	static uint8_t scratch[NLMSG_HDR + NL_CMD_REQ_HDR + NL_CMD_MAX_LEN];
	uint32_t tid = 0;
	int status = -1;
	long fd = nl_open_pid(0, &tid, FLOWS_TIMEOUT_US), rc;

	if (fd < 0)
		return 1;
	for (unsigned i = 0; i < sizeof *f; i++)
		((uint8_t *)f)[i] = 0;
	rc = nl_cmd_call((int)fd, tid, scratch, OMCI_FLOWS_CMD, f, OMCI_FLOWS_LEN,
			 &status, FLOWS_RETRIES, 0);
	sys_close((int)fd);
	if (rc != 0)
		return 1;
	if (status == ODI_EOPNOTSUPP)
		return 2;
	return status == 0 ? 0 : 1;
}
