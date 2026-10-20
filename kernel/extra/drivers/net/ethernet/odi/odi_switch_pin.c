// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_pin.c -- the stick's own MACs pinned to the CPU port as
 * static L2 rows, so a frame from the fibre carrying one of them as its
 * source cannot move the row to the PON port and divert host-to-stick
 * traffic upstream (docs/SWITCH.md#the-sticks-mac).
 *
 * Which MACs: the address of every odi_nic device that is up and sends to
 * a switch port (eth0, eth0.2, eth0.3), and of the bridge it is a port of
 * (br0, which takes eth0.2's). network.sh sets ELAN_MAC_ADDR on eth0 and
 * eth0.2 with the link down and brings them up again, so a netdevice
 * notifier sees every change: up, down, a new address, a new bridge. Each
 * event recomputes the whole set and odi_switch_l2_pins_sync() brings the
 * table in step (odi_switch_l2.c, host-tested). switch_init syncs once
 * more, before omcid starts, in case the first write found the engine
 * busy. The table is emptied once before that (odi_switch_l2_clean(), from
 * odi_switch_boot_init()); nothing we run removes a static row afterwards:
 * no replay writes the L2 table, the flush is set to dynamic rows only,
 * and cmd 51 does not touch it (docs/SWITCH.md).
 *
 * Locking: the notifier runs under RTNL and takes odi_switch_lock, so the
 * order is RTNL -> odi_switch_lock; nothing under odi_switch_lock takes
 * RTNL. The set is read under RCU, which switch_init (holding
 * odi_switch_lock, not RTNL) can do too.
 */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>
#include <linux/init.h>
#include <net/net_namespace.h>

#include "odi_switch_l2.h"
#include "odi_switch_reg.h"
#include "odi_switch_api.h"
#include "odi_nic.h"

#define DRV_NAME "odi_switch"
#define ODI_SW_PIN_PORT	3U	/* the CPU port */

static struct odi_sw_l2_pins odi_switch_pinned;	/* under odi_switch_lock */

static void odi_switch_pin_collect(struct odi_sw_l2_pins *want)
{
	struct net_device *dev, *upper;

	memset(want, 0, sizeof(*want));
	rcu_read_lock();
	for_each_netdev_rcu(&init_net, dev) {
		if (!(dev->flags & IFF_UP) || !odi_nic_dev_transmits(dev))
			continue;
		odi_switch_l2_pins_add(want, dev->dev_addr);
		upper = netdev_master_upper_dev_get_rcu(dev);
		if (upper && (upper->flags & IFF_UP))
			odi_switch_l2_pins_add(want, upper->dev_addr);
	}
	rcu_read_unlock();
}

void odi_switch_pin_sync(void)
{
	struct odi_sw_l2_pins want;
	struct odi_sw_l2_pin_report rep;
	uint32_t i;

	lockdep_assert_held(&odi_switch_lock);
	if (odi_switch_mmio_ensure())
		return;
	odi_switch_pin_collect(&want);
	odi_switch_l2_pins_sync(&odi_switch_pinned, &want, ODI_SW_PIN_PORT, &rep);

	for (i = 0; i < rep.removed_n; i++)
		pr_info(DRV_NAME ": %pM no longer the stick's: static row removed\n", rep.removed[i]);
	for (i = 0; i < rep.stuck_n; i++)
		pr_warn(DRV_NAME ": %pM no longer the stick's, but its static row did not go\n", rep.stuck[i]);
	for (i = 0; i < want.n; i++) {
		if (rep.failed & (1U << i))
			pr_warn(DRV_NAME ": could not pin %pM to the CPU port (%d); tried again at the next address change and at switch_init\n",
				want.mac[i], rep.rc[i]);
		else if (rep.added & (1U << i))
			pr_info(DRV_NAME ": %pM pinned to the CPU port (static row %u)\n",
				want.mac[i], rep.row[i]);
	}
}

static int odi_switch_pin_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGEADDR:
	case NETDEV_CHANGEUPPER:
	case NETDEV_UNREGISTER:
		break;
	default:
		return NOTIFY_DONE;
	}
	mutex_lock(&odi_switch_lock);
	odi_switch_pin_sync();
	mutex_unlock(&odi_switch_lock);
	return NOTIFY_DONE;
}

static struct notifier_block odi_switch_pin_nb = {
	.notifier_call = odi_switch_pin_netdev_event,
};

static int __init odi_switch_pin_init(void)
{
	return register_netdevice_notifier(&odi_switch_pin_nb);
}
device_initcall(odi_switch_pin_init);
