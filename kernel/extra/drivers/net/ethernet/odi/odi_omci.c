// SPDX-License-Identifier: GPL-2.0
/*
 * odi_omci.c -- the OMCI frame transport between omcid and the PON: a
 * ODI_OMCI_NETLINK kernel socket speaking the protocol of src/omci/nl.h
 * (restated as pack/unpack functions in odi_omci_wire.h, which the host
 * test shares), with frames going in and out through odi_nic.
 *
 * RX (PON -> omcid): an rxhook above every other takes the frames with RX
 * reason 246, the OMCI trap code, and delivers them to the registered pid.
 *
 * TX (omcid -> PON): an OP_SEND is sent with odi_nic_tx_words(), with the
 * descriptor words a stock OMCI reply carries and the port and GEM stream
 * of the last request, since a reply goes where its request came from.
 *
 * OP_CMD carries one odi_switch_cmd() call; /proc/odi_omci shows the
 * registrations and counters and takes the one switch_init write.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/ratelimit.h>
#include <linux/capability.h>

#include "odi_nic_hw.h"
#include "odi_nic.h"
#include "odi_omci_wire.h"

#include "odi_switch_cmd.h"
#include "odi_switch_reg.h" /* odi_switch_lock */
#include "odi_switch_api.h" /* odi_switch_boot_init() */

#define DRV_NAME "odi_omci"

/* A priority high enough to run ahead of any other rxhook registration
 * without needing to know its exact numeric priority --
 * odi_dispatch_rxhooks() in odi_nic.c walks
 * hooks from the highest priority down, so this simply wins.
 */
#define ODI_OMCI_RXHOOK_PRIO	(INT_MAX - 1)
/* All ports: an OMCI frame is identified by its RX reason alone
 * (opts3.reason is 246 for OMCI on the RTL9602C) -- matched here rather
 * than also gating on a specific source port number, which never needed
 * pinning down.
 */
#define ODI_OMCI_RXHOOK_PORTMASK 0xFFU

#define ODI_OMCI_REASON		246U

/* ---- Registration table: redirect type -> the pid that registered it.
 * Only type 1 (omcid) is used on this image; the table is per type because
 * the protocol is.
 */
#define ODI_OMCI_MAX_REGS 4

struct odi_omci_reg {
	int used;
	unsigned int type;
	u32 pid;
};

static struct odi_omci_reg odi_omci_regs[ODI_OMCI_MAX_REGS];
static DEFINE_SPINLOCK(odi_omci_reg_lock);

/* The PON port/GEM stream id a reply belongs on: remembered from the most
 * recent RX frame rather than assumed, since OMCI is strictly request/reply
 * on the OLT side and a reply is only ever built after a request came in.
 */
static u32 odi_omci_last_src_port;
static u32 odi_omci_last_stream;
static int odi_omci_have_last;
static DEFINE_SPINLOCK(odi_omci_last_lock);

static struct sock *odi_omci_nlsk;

/* ---- /proc/odi_omci counters: with the registration, they tell whether
 * omcid registered, left, or never got a frame.
 *
 * The three RX counters are plain u32: their one writer is
 * odi_omci_rxhook(), which odi_nic calls from its NAPI poll, one instance
 * at a time. The four netlink-side counters are atomic_t, because netlink
 * input is NOT serialised: the input callback of a kernel netlink socket
 * runs in the sendmsg() of each sender, so two senders increment them
 * concurrently.
 */
static u32 odi_omci_cnt_frames_delivered;
static u32 odi_omci_cnt_dropped_unregistered;
static u32 odi_omci_cnt_unicast_fail;
static atomic_t odi_omci_cnt_replies_sent = ATOMIC_INIT(0);
static atomic_t odi_omci_cnt_tx_fail = ATOMIC_INIT(0);
static atomic_t odi_omci_cnt_cmd_reqs = ATOMIC_INIT(0);
static atomic_t odi_omci_cnt_cmd_replies = ATOMIC_INIT(0);

static void odi_omci_reg_set(unsigned int type, u32 pid)
{
	unsigned long flags;
	int i, free = -1;

	pr_info(DRV_NAME ": OP_REG type %u pid %u\n", type, pid);
	spin_lock_irqsave(&odi_omci_reg_lock, flags);
	for (i = 0; i < ODI_OMCI_MAX_REGS; i++) {
		if (odi_omci_regs[i].used && odi_omci_regs[i].type == type) {
			odi_omci_regs[i].pid = pid;
			spin_unlock_irqrestore(&odi_omci_reg_lock, flags);
			return;
		}
		if (free < 0 && !odi_omci_regs[i].used)
			free = i;
	}
	if (free >= 0) {
		odi_omci_regs[free].used = 1;
		odi_omci_regs[free].type = type;
		odi_omci_regs[free].pid = pid;
	}
	spin_unlock_irqrestore(&odi_omci_reg_lock, flags);
	if (free < 0)
		pr_warn(DRV_NAME ": registration table full, type %u pid %u dropped\n", type, pid);
}

static void odi_omci_reg_clear(unsigned int type)
{
	unsigned long flags;
	int i;

	pr_info(DRV_NAME ": OP_DEREG type %u\n", type);
	spin_lock_irqsave(&odi_omci_reg_lock, flags);
	for (i = 0; i < ODI_OMCI_MAX_REGS; i++)
		if (odi_omci_regs[i].used && odi_omci_regs[i].type == type)
			odi_omci_regs[i].used = 0;
	spin_unlock_irqrestore(&odi_omci_reg_lock, flags);
}

/* Returns 0 and *pid on a hit, -1 if nothing is registered for this type
 * (the caller drops the frame and says so, rate-limited).
 */
static int odi_omci_reg_get(unsigned int type, u32 *pid)
{
	unsigned long flags;
	int i, found = -1;

	spin_lock_irqsave(&odi_omci_reg_lock, flags);
	for (i = 0; i < ODI_OMCI_MAX_REGS; i++) {
		if (odi_omci_regs[i].used && odi_omci_regs[i].type == type) {
			*pid = odi_omci_regs[i].pid;
			found = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&odi_omci_reg_lock, flags);
	return found;
}

/* ---- RX: PON -> omcid ---------------------------------------------------- */

static DEFINE_RATELIMIT_STATE(odi_omci_rx_drop_rl, 5 * HZ, 3);
static DEFINE_RATELIMIT_STATE(odi_omci_tx_drop_rl, 5 * HZ, 3);

static int odi_omci_rxhook(struct sk_buff *skb, const struct odi_rx_words *info)
{
	unsigned int reason = (info->opts3 >> ODI_RXD_REASON_SHIFT) & ODI_RXD_REASON_MASK;
	unsigned int src_port = (info->opts3 >> ODI_RXD_SRC_PORT_SHIFT) & ODI_RXD_SRC_PORT_MASK;
	unsigned int stream = (info->opts2 >> ODI_RXD_GEM_SHIFT) & ODI_RXD_GEM_MASK;
	struct sk_buff *nskb;
	unsigned long flags;
	u32 dst_pid, total;
	u8 *buf;

	if (reason != ODI_OMCI_REASON)
		return ODI_RXHOOK_CONTINUE;

	spin_lock_irqsave(&odi_omci_last_lock, flags);
	odi_omci_last_src_port = src_port;
	odi_omci_last_stream = stream;
	odi_omci_have_last = 1;
	spin_unlock_irqrestore(&odi_omci_last_lock, flags);

	if (odi_omci_reg_get(ODI_OMCI_REDIRECT_TYPE, &dst_pid)) {
		odi_omci_cnt_dropped_unregistered++;
		if (__ratelimit(&odi_omci_rx_drop_rl))
			pr_info(DRV_NAME ": OMCI frame, nobody registered for type %u, dropped\n",
				ODI_OMCI_REDIRECT_TYPE);
		return ODI_RXHOOK_STOP;
	}

	total = odi_omci_rx_total(skb->len);
	nskb = alloc_skb(total, GFP_ATOMIC);
	if (!nskb) {
		if (__ratelimit(&odi_omci_rx_drop_rl))
			pr_info(DRV_NAME ": OMCI frame, no memory for the netlink copy, dropped\n");
		return ODI_RXHOOK_STOP;
	}
	buf = skb_put(nskb, total);
	odi_omci_build_rx(buf, skb->data, skb->len);

	if (netlink_unicast(odi_omci_nlsk, nskb, dst_pid, MSG_DONTWAIT) < 0) {
		odi_omci_cnt_unicast_fail++;
		if (__ratelimit(&odi_omci_rx_drop_rl))
			pr_info(DRV_NAME ": OMCI frame, netlink_unicast to pid %u failed (omcid gone?)\n",
				dst_pid);
		/* netlink_unicast() frees nskb on failure. */
	} else {
		odi_omci_cnt_frames_delivered++;
	}

	return ODI_RXHOOK_STOP;
}

/* ---- TX: omcid -> PON ----------------------------------------------------- */

/* The words a stock OMCI reply carries: opts1 bit 25, NO_LEARN and
 * PORT_SEL (without PORT_SEL the switch ignores the port mask and the reply
 * never reaches the PON port), opts2 TAGGED and bit 19, opts3 the PON port
 * mask and the OMCC GEM stream id.
 */
#define ODI_OMCI_TXINFO_OPTS1	(ODI_TXD_BIT25 | ODI_TXD_NO_LEARN | ODI_TXD_PORT_SEL)
#define ODI_OMCI_TXINFO_OPTS2	(ODI_TXD_TAGGED | ODI_TXD_BIT19)

static void odi_omci_send_to_pon(const u8 *frame, u32 len)
{
	struct odi_tx_words ti = { 0 };
	unsigned long flags;
	u32 src_port, stream;
	int have;

	spin_lock_irqsave(&odi_omci_last_lock, flags);
	have = odi_omci_have_last;
	src_port = odi_omci_last_src_port;
	stream = odi_omci_last_stream;
	spin_unlock_irqrestore(&odi_omci_last_lock, flags);

	if (!have) {
		if (__ratelimit(&odi_omci_tx_drop_rl))
			pr_info(DRV_NAME ": OMCI reply with no prior RX frame to answer, dropped\n");
		return;
	}
	if (len > 0xFFFFU)
		len = 0xFFFFU;

	ti.opts1 = ODI_OMCI_TXINFO_OPTS1;
	ti.opts2 = ODI_OMCI_TXINFO_OPTS2;
	ti.opts3 = ((1U << src_port) << ODI_TXD_PORTS_SHIFT) |
		   ((stream & ODI_TXD_GEM_MASK) << ODI_TXD_GEM_SHIFT);

	if (odi_nic_tx_words(frame, (unsigned short)len, &ti)) {
		atomic_inc(&odi_omci_cnt_tx_fail);
		if (__ratelimit(&odi_omci_tx_drop_rl))
			pr_info(DRV_NAME ": OMCI reply, odi_nic_tx_words failed\n");
	} else {
		atomic_inc(&odi_omci_cnt_replies_sent);
	}
}

/* ---- ODI_OMCI_OP_CMD: one odi_switch_cmd() call (odi_switch_cmd.h) over
 * the same socket, sent by omci_drv_call() (src/omci/respond/drv.c).
 *
 * Copies the request argument into a kernel buffer (never trusts the skb
 * past the length the request declares, odi_omci_parse() already
 * bounds-checked), calls odi_switch_cmd(cmd, buf, len) and replies with
 * its status and the argument bytes, which the getters fill in.
 */
static DEFINE_RATELIMIT_STATE(odi_omci_cmd_reply_drop_rl, 5 * HZ, 3);

static void odi_omci_cmd(struct sk_buff *skb, const struct odi_omci_msg *msg, u32 sender_portid)
{
	/* __aligned(4): odi_switch_cmd() (odi_switch_cmd.c) casts this buffer
	 * to struct omci_* pointers (cmd_tcont(), cmd_gem_flow(), ...) and
	 * dereferences uint32_t fields through them -- an unaligned word load
	 * traps on this core.
	 */
	u8 argbuf[ODI_OMCI_CMD_MAX_LEN] __aligned(4);
	struct sk_buff *nskb;
	u32 total;
	int status;
	u8 *rbuf;

	if (msg->cmd_len > ODI_OMCI_CMD_MAX_LEN)
		return; /* odi_omci_parse() already refuses this; belt and braces */
	memcpy(argbuf, skb->data + msg->cmd_off, msg->cmd_len);
	atomic_inc(&odi_omci_cnt_cmd_reqs);

	/* odi_switch_lock (odi_switch.c): this callback runs in the
	 * sendmsg() of whichever process sent the message, so two senders
	 * reach this point concurrently.
	 */
	mutex_lock(&odi_switch_lock);
	status = odi_switch_cmd(msg->cmd_num, argbuf, msg->cmd_len);
	mutex_unlock(&odi_switch_lock);

	total = odi_omci_cmd_reply_total(msg->cmd_len);
	nskb = alloc_skb(total, GFP_ATOMIC);
	if (!nskb) {
		if (__ratelimit(&odi_omci_cmd_reply_drop_rl))
			pr_info(DRV_NAME ": ODI_OMCI_OP_CMD reply, no memory, dropped (cmd %u)\n",
				msg->cmd_num);
		return;
	}
	rbuf = skb_put(nskb, total);
	odi_omci_build_cmd_reply(rbuf, msg->cmd_num, status, argbuf, msg->cmd_len);

	if (netlink_unicast(odi_omci_nlsk, nskb, sender_portid, MSG_DONTWAIT) < 0) {
		if (__ratelimit(&odi_omci_cmd_reply_drop_rl))
			pr_info(DRV_NAME ": ODI_OMCI_OP_CMD reply, netlink_unicast to pid %u failed (cmd %u)\n",
				sender_portid, msg->cmd_num);
	} else {
		atomic_inc(&odi_omci_cnt_cmd_replies);
	}
}

/* ---- Netlink input: dispatch is on the op field of the payload, never on
 * nlmsg_type (nl.h). One skb is one message: omcid sends each with one
 * sendmsg(), and nothing is reassembled here.
 */
static void odi_omci_nl_input(struct sk_buff *skb)
{
	struct odi_omci_msg msg;

	/* Opening a netlink socket of any protocol number, ours included,
	 * takes no privilege, and every op here either drives the switch or
	 * speaks to the OLT.
	 */
	if (!netlink_capable(skb, CAP_NET_ADMIN)) {
		pr_warn_ratelimited(DRV_NAME ": dropped message from unprivileged portid %u\n",
				    NETLINK_CB(skb).portid);
		return;
	}

	if (odi_omci_parse(skb->data, skb->len, &msg))
		return;

	if (msg.op == ODI_OMCI_OP_REG) {
		if (msg.reg_action == ODI_OMCI_ACT_REG)
			odi_omci_reg_set(msg.type, msg.reg_pid);
		else if (msg.reg_action == ODI_OMCI_ACT_DEREG)
			odi_omci_reg_clear(msg.type);
		return;
	}

	if (msg.op == ODI_OMCI_OP_CMD) {
		/* Reply to the socket the kernel says sent this, not to a pid
		 * taken from the payload.
		 */
		odi_omci_cmd(skb, &msg, NETLINK_CB(skb).portid);
		return;
	}

	/* ODI_OMCI_OP_SEND: only REDIRECT_TYPE(1) is ours -- no other
	 * redirect user runs on this image.
	 */
	if (msg.type == ODI_OMCI_REDIRECT_TYPE)
		odi_omci_send_to_pon(skb->data + msg.send_off, msg.send_len);
}

/* ---- /proc/odi_omci: who holds each redirect type (the first line, which
 * apply.sh and the omci tools read) and the frame and command counters.
 */
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static int odi_omci_proc_show(struct seq_file *seq, void *v)
{
	unsigned long flags;
	int i;

	seq_printf(seq, "registered:");
	spin_lock_irqsave(&odi_omci_reg_lock, flags);
	for (i = 0; i < ODI_OMCI_MAX_REGS; i++)
		if (odi_omci_regs[i].used)
			seq_printf(seq, " type=%u pid=%u", odi_omci_regs[i].type, odi_omci_regs[i].pid);
	spin_unlock_irqrestore(&odi_omci_reg_lock, flags);
	seq_printf(seq, "\n");
	seq_printf(seq, "frames_delivered %u\n", odi_omci_cnt_frames_delivered);
	seq_printf(seq, "dropped_unregistered %u\n", odi_omci_cnt_dropped_unregistered);
	seq_printf(seq, "unicast_fail %u\n", odi_omci_cnt_unicast_fail);
	seq_printf(seq, "replies_sent_to_pon %u\n", (u32)atomic_read(&odi_omci_cnt_replies_sent));
	seq_printf(seq, "tx_fail %u\n", (u32)atomic_read(&odi_omci_cnt_tx_fail));
	seq_printf(seq, "cmd_requests %u\n", (u32)atomic_read(&odi_omci_cnt_cmd_reqs));
	seq_printf(seq, "cmd_replies %u\n", (u32)atomic_read(&odi_omci_cnt_cmd_replies));
	return 0;
}

static int odi_omci_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_omci_proc_show, NULL);
}

/* /proc/odi_omci write: "switch_init", which rcS writes once, runs the
 * switch-core init of the stock module load (odi_switch_boot_init()). It
 * runs synchronously in that write(), so a hang there stops the write and
 * not the boot, and the ramlog shows which write it was.
 */
static ssize_t odi_omci_proc_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char buf[16];
	size_t len = count;
	int rc;

	(void)file;
	(void)ppos;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0'; /* drop a trailing newline, echo adds one */
	if (strcmp(buf, "switch_init") != 0)
		return -EINVAL;

	pr_info(DRV_NAME ": switch_init\n");
	mutex_lock(&odi_switch_lock);
	rc = odi_switch_boot_init();
	mutex_unlock(&odi_switch_lock);
	return rc ? rc : (ssize_t)count;
}

static const struct proc_ops odi_omci_proc_fops = {
	.proc_open = odi_omci_proc_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
	.proc_write = odi_omci_proc_write,
};

/* ---- Module init/exit ----------------------------------------------------- */

static int __init odi_omci_init(void)
{
	struct netlink_kernel_cfg cfg = { .input = odi_omci_nl_input };
	int rc;

	odi_omci_nlsk = netlink_kernel_create(&init_net, ODI_OMCI_NETLINK, &cfg);
	if (!odi_omci_nlsk) {
		pr_err(DRV_NAME ": netlink_kernel_create failed\n");
		return -ENODEV;
	}

	rc = odi_nic_rxhook_register(ODI_OMCI_RXHOOK_PORTMASK, ODI_OMCI_RXHOOK_PRIO, odi_omci_rxhook);
	if (rc) {
		pr_err(DRV_NAME ": odi_nic_rxhook_register failed, rc %d\n", rc);
		netlink_kernel_release(odi_omci_nlsk);
		odi_omci_nlsk = NULL;
		return -ENODEV;
	}

	if (!proc_create("odi_omci", 0644, NULL, &odi_omci_proc_fops))
		pr_warn(DRV_NAME ": /proc/odi_omci create failed (non-fatal)\n");

	pr_info(DRV_NAME ": ready, netlink %d, rxhook priority %d\n", ODI_OMCI_NETLINK, ODI_OMCI_RXHOOK_PRIO);
	return 0;
}

module_init(odi_omci_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss OMCI frame transport");
