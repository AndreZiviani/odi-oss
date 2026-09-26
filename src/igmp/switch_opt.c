#include "switch_opt.h"
#include "sys.h"

#define ODI_SW_DEV_PATH "/dev/odi_sw"

/* One descriptor for the life of the daemon, opened on first use: every
 * membership change is one ioctl, and reopening the device for each would
 * only add two syscalls to each. A failed open is retried on the next call. */
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

int igmp_sw_mode(uint32_t *ipmc_on_group)
{
	struct odi_sw_l2_mode m;
	int rc;

	m.ipmc_on_group = 0;
	m.rows = 0;
	rc = sw_ioctl(ODI_SW_IOC_L2_MODE, &m);
	if (rc)
		return rc;
	*ipmc_on_group = m.ipmc_on_group;
	return 0;
}

int igmp_sw_mcast_add(struct odi_sw_l2_mcast *m)
{
	return sw_ioctl(ODI_SW_IOC_L2_MC_ADD, m);
}

int igmp_sw_mcast_del(struct odi_sw_l2_mcast *m)
{
	return sw_ioctl(ODI_SW_IOC_L2_MC_DEL, m);
}
