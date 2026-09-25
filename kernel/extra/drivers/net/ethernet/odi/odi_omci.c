// SPDX-License-Identifier: GPL-2.0
/*
 * odi_omci.c -- the OMCI frame transport, ours.
 *
 * Replaces the stock firmware's OMCI packet-redirect module: a
 * NETLINK_USERSOCK kernel socket speaking the
 * same wire protocol omcid already implements (src/omci/nl.h, restated as
 * pack/unpack functions in odi_omci_wire.h so this file and its host test
 * share one reading of it), sourcing and sinking OMCI frames through our
 * own odi_nic driver instead of the stock GPON/OMCC stack.
 *
 * RX (PON -> omcid): registers an odi_nic_rxhook_register() callback at
 * a priority above any other hook. Frames with reason 246 (the OMCI
 * trap code on this chip) are consumed here and never reach another hook
 * at all.
 *
 * TX (omcid -> PON): the netlink input callback, on an NL_OP_SEND for
 * REDIRECT_TYPE, calls odi_nic_tx_words() (odi_nic.c, declared in
 * odi_nic.h) directly, building the descriptor words a captured stock OMCI
 * reply carries: opts1 bit 25 | NO_LEARN, opts2 TAGGED,
 * opts3 a port mask and GEM stream id -- both taken from the most recent RX
 * frame rather than a hardcoded port number, since a reply belongs on
 * whichever port/stream the request it answers arrived on.
 *
 * Every constant either comes from our own nl.h, from observed behaviour
 * of the stock firmware, or from odi_nic.c's own interface comments.
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

#ifdef CONFIG_ODI_SWITCH
#include "odi_switch_cmd.h"
/*
 * odi_switch_dal.h: ODI_SWITCH_INIT_PLATFORM_ITEM_ALL for the bare
 * "init_platform" /proc write below, and the
 * direct odi_switch_parity_add()/_clear()/
 * _active_count()/_is_loaded() leaves the
 * parity commands below call
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h" /* odi_reg_read()/odi_reg_write(), for "peek"/"poke" below */
/*
 * odi_switch_api.h: the lazy "did it fire" triggers and their _rc_get()
 * getters (odi_switch.c/odi_switch_dal.c)
 */
#include "odi_switch_api.h"
#include "odi_ddm.h" /* odi_ddm_get(), for the "ddm <type>" /proc write below */
/*
 * odi_i2c.h: odi_i2c_read_bytes(), for the "i2c <dev> <addr> <len>"
 * /proc write below
 */
#include "odi_i2c.h"

/* odi_switch_platform_init_trigger(mask) is the only way to run
 * odi_switch_init_platform() -- never from module init, see odi_switch.c
 * ("platform init made safe"); odi_switch_platform_init_rc_get() is -1
 * until a trigger with a nonzero mask has fired at least once. The two
 * call sites below (a /proc/odi_omci write, and the first ODI_OMCI_OP_CMD
 * as a fallback) are the only two triggers in this codebase.
 *
 * odi_switch_parity_init_trigger()/_trigger_all() follow the same -1/0
 * convention for the loadable parity mechanism (odi_switch_dal.c); the
 * other four parity commands below (parity_add/parity_clear/
 * parity_active_count/parity_is_loaded) call the plain dal leaves
 * directly (odi_switch_dal.h) -- their trigger-wrapper equivalents were
 * pure pass-through with no bookkeeping of their own, so nothing is lost
 * calling straight through.
 *
 * odi_switch_modload_init_trigger()/_rc_get() -- "module-load replay":
 * `init_modload <mask>` below, plus rcS's own write of the same command
 * line.
 *
 * odi_switch_ds_encrypt_trigger()/_rc_get() -- "DS GEM encryption flag":
 * `ds_encrypt <mask>` below, a manual fallback for when the GPON
 * FSM/PLOAM core's own automatic Encrypted_Port-ID handling
 * (odi_switch_gpon_encrypt_port(), odi_switch_dal.c) has not run yet or
 * an operator wants to force a mask by hand.
 */

#ifdef CONFIG_ODI_SDKINIT
/*
 * odi_switch_sdkinit.h: odi_switch_sdkinit_mask_get()/_set(): `sdkinit_mask
 * <hex>` below sets the mask the /proc/rtk_init
 * dispatch consults, bit per verb, before every
 * rtk_init verb this boot still has to run -- rcS's
 * own /var/config/sdkinit.mask read is the only
 * other caller.
 */
#include "odi_switch_sdkinit.h"
#endif

/* "peek <off>" result, shown on the next /proc/odi_omci READ (the write
 * itself only triggers the read and logs it, same posture as every other
 * command here) -- odi_omci_peek_valid is false until the first peek.
 */
static bool odi_omci_peek_valid;
static uint32_t odi_omci_peek_off;
static uint32_t odi_omci_peek_val;

/* "ddm <type>"/"i2c <dev> <addr> <len>" results: same "write triggers,
 * next read shows" shape as peek above --
 * lets a live session drive odi_ddm.c/odi_i2c.c directly from the shell,
 * without going through /dev/odi_sw or src/diag, to tell a bad selector
 * from an I2C failure from a working read. ODI_OMCI_I2C_MAX_LEN bounds
 * the raw-read buffer; odi_ddm_get() always fills 24 bytes regardless of
 * selector, so the ddm buffer is fixed at that size.
 */
#define ODI_OMCI_I2C_MAX_LEN 32U

static bool odi_omci_ddm_valid;
static int odi_omci_ddm_type;
static int odi_omci_ddm_rc;
static uint8_t odi_omci_ddm_raw[ODI_DDM_BUF_LEN];

static bool odi_omci_i2c_valid;
static uint32_t odi_omci_i2c_sel;
static uint32_t odi_omci_i2c_addr;
static unsigned int odi_omci_i2c_len;
static int odi_omci_i2c_rc;
static uint8_t odi_omci_i2c_raw[ODI_OMCI_I2C_MAX_LEN];

/* Sticky flags: each lazy first-OP_CMD trigger fires at most once per
 * boot, even if odi_switch_platform_init_mask is later changed via sysfs
 * -- repeat triggers past the first are for a deliberate /proc/odi_omci
 * write to ask for, not something that happens again silently just
 * because another OP_CMD came in.
 */
static bool odi_omci_platform_init_lazy_done;
#endif

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
 * Only REDIRECT_TYPE(1) is ever populated on our image (igmp_drv, the only
 * other pkt_redirect consumer, is dropped); a small
 * table rather than one global is kept because the wire protocol itself is
 * per-type.
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

/* ---- /proc/odi_omci status counters (s2 fixes): s1's own "nobody registered for type 1, dropped" storm had no way
 * to say whether omcid had ever registered, deregistered itself, or was
 * simply never reached -- these seven counters plus the current
 * registration make that visible from a live stick without a kernel
 * debugger.
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

/* Returns 0 and *pid on a hit, -1 if nothing is registered for this type --
 * the same "drop, and say so once" shape igmp_drv used for its own unicast,
 * here rate-limited since
 * OMCI frames arrive far more often than IGMP control frames did.
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

/* The words a stock OMCI reply carries (trial o1): opts1 bit 25,
 * NO_LEARN and PORT_SEL (without the port
 * select bit the switch does not honour tx_portmask and the reply never
 * reaches the PON port: o1 saw every OLT request three times), opts2 TAGGED
 * and bit 19, opts3 the PON port mask and the OMCC GEM stream id.
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

/* ---- ODI_OMCI_OP_CMD: ours -- carries one
 * odi_switch_cmd() call (odi_switch_cmd.h) over this same
 * netlink socket, replacing apply.c's own stock getsockopt path
 * (src/omci/respond/apply.c omci_drv_call(), which still falls back to
 * it on -EOPNOTSUPP or no reply -- see that function's own comment).
 *
 * Under CONFIG_ODI_SWITCH: copies the request argument into a kernel
 * buffer (never trusts the skb's own buffer past the length the request
 * itself declares, odi_omci_parse() already bounds-checked), calls
 * odi_switch_cmd(cmd, buf, len) and replies with its status and the
 * (possibly modified, for the `read` commands) argument bytes. Without
 * CONFIG_ODI_SWITCH: always replies -EOPNOTSUPP so a NIC=oss OMCI=oss
 * image without SWITCH=oss still answers the op (apply.c fallback
 * needs an actual reply, not a timeout, to know the transport itself is
 * fine and only this kernel lacks the switch core) -- the same posture
 * this design calls for.
 */
static DEFINE_RATELIMIT_STATE(odi_omci_cmd_reply_drop_rl, 5 * HZ, 3);

static void odi_omci_cmd(struct sk_buff *skb, const struct odi_omci_msg *msg, u32 sender_portid)
{
	/* __aligned(4): odi_switch_cmd() (odi_switch_cmd.c) casts this buffer
	 * to struct omci_* pointers (cmd_tcont(), cmd_gem_flow(), ...) and
	 * dereferences uint32_t fields through them -- an unaligned word load
	 * traps on this core (L4, MIPS o32).
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

#ifdef CONFIG_ODI_SWITCH
	/* Lazy trigger (odi_switch.c "odi_switch platform init"): the first
	 * OP_CMD to arrive fires odi_switch_init_platform() with the
	 * odi_switch_platform_init_mask module parameter, once per boot,
	 * if nothing has triggered it already (a /proc/odi_omci write, or an
	 * earlier OP_CMD). mask 0 (the default) makes
	 * odi_switch_platform_init_trigger() itself a no-op, so this line is
	 * safe to leave unconditional rather than checking the mask here too.
	 */
	/* odi_switch_lock (odi_switch.c): this callback runs in the
	 * sendmsg() of whichever process sent the message, so two senders
	 * reach this point concurrently. The lazy flag, the trigger and the
	 * command are one critical section.
	 */
	mutex_lock(&odi_switch_lock);
	if (!odi_omci_platform_init_lazy_done) {
		odi_omci_platform_init_lazy_done = true;
		odi_switch_platform_init_trigger(odi_switch_platform_init_mask);
	}
	/* No GPON PLOAM-hook registration here: no stock GPON stack exists
	 * anywhere in this tree for it to register with. The FSM/PLOAM core
	 * (odi_gpon, CONFIG_ODI_GPON) calls odi_switch_gpon_encrypt_port()
	 * (odi_switch_dal.c) directly on the OLT's own Encrypted_Port-ID
	 * PLOAM instead of going through a callback -- the manual
	 * `ds_encrypt <mask>` /proc/odi_omci write remains the only fallback
	 * for a build without CONFIG_ODI_GPON.
	 */
	status = odi_switch_cmd(msg->cmd_num, argbuf, msg->cmd_len);
	mutex_unlock(&odi_switch_lock);
#else
	status = -95; /* -EOPNOTSUPP; no kernel header dependency needed for one constant */
#endif

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

/* ---- Netlink input: the driver's own custom framing, the one omcid
 * speaks (nl.h file header: nlmsg_type is never
 * examined, dispatch is entirely on the payload's own op field). One skb is
 * one message -- nl.h's nl_send() is one sendmsg() per call and this
 * transport never reassembles more than one at a time.
 */
static void odi_omci_nl_input(struct sk_buff *skb)
{
	struct odi_omci_msg msg;

	/* NETLINK_USERSOCK accepts senders without privileges, and every
	 * op here either drives the switch or speaks to the OLT.
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
		/* NETLINK_CB(skb).portid is the kernel's own record of which
		 * socket sent this skb -- authoritative, unlike trusting a
		 * pid the sender put in its own payload (nl.h's own nl_cmd_call()
		 * does put the right one in the NLMSG_HDR pid field too, the
		 * same convention nl_redirect()/nl_send_payload() already
		 * use, but there is no reason to prefer the payload's own copy
		 * when the kernel's own is right there).
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

/* ---- /proc/odi_omci: the status s1 was missing (s2 fixes).
 * The /proc/rtk_init dispatch uses the same
 * single_open/seq_read shape this codebase already uses for a read-only
 * /proc status line.
 */
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static void odi_omci_seq_hex(struct seq_file *seq, const uint8_t *buf, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		seq_printf(seq, "%02x", buf[i]);
}

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
#ifdef CONFIG_ODI_SWITCH
	/* The same lock the writes hold, so a peek/ddm/i2c result is never
	 * shown half updated (odi_switch.c).
	 */
	mutex_lock(&odi_switch_lock);
	seq_printf(seq, "platform_init_rc %d\n", odi_switch_platform_init_rc_get());
	seq_printf(seq, "parity_init_rc %d\n", odi_switch_parity_init_rc_get());
	seq_printf(seq, "parity_active_count %u\n", odi_switch_parity_active_count());
	seq_printf(seq, "parity_using_loaded_table %d\n", odi_switch_parity_is_loaded());
	seq_printf(seq, "modload_init_rc %d\n", odi_switch_modload_init_rc_get());
	seq_printf(seq, "ds_encrypt_rc %d\n", odi_switch_ds_encrypt_rc_get());
#ifdef CONFIG_ODI_SDKINIT
	seq_printf(seq, "sdkinit_mask 0x%08x\n", odi_switch_sdkinit_mask_get());
#endif
	if (odi_omci_peek_valid)
		seq_printf(seq, "peek 0x%08x 0x%08x\n", odi_omci_peek_off, odi_omci_peek_val);
	if (odi_omci_ddm_valid) {
		seq_printf(seq, "ddm %d rc %d ", odi_omci_ddm_type, odi_omci_ddm_rc);
		odi_omci_seq_hex(seq, odi_omci_ddm_raw, ODI_DDM_BUF_LEN);
		seq_printf(seq, "\n");
	}
	if (odi_omci_i2c_valid) {
		seq_printf(seq, "i2c 0x%08x 0x%08x %u rc %d ",
			   odi_omci_i2c_sel, odi_omci_i2c_addr, odi_omci_i2c_len, odi_omci_i2c_rc);
		odi_omci_seq_hex(seq, odi_omci_i2c_raw, odi_omci_i2c_len);
		seq_printf(seq, "\n");
	}
	mutex_unlock(&odi_switch_lock);
#else
	seq_printf(seq, "platform_init_rc n/a (CONFIG_ODI_SWITCH not set)\n");
	seq_printf(seq, "parity_init_rc n/a (CONFIG_ODI_SWITCH not set)\n");
	seq_printf(seq, "parity_active_count n/a (CONFIG_ODI_SWITCH not set)\n");
	seq_printf(seq, "parity_using_loaded_table n/a (CONFIG_ODI_SWITCH not set)\n");
	seq_printf(seq, "modload_init_rc n/a (CONFIG_ODI_SWITCH not set)\n");
	seq_printf(seq, "ds_encrypt_rc n/a (CONFIG_ODI_SWITCH not set)\n");
#endif
	return 0;
}

static int odi_omci_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_omci_proc_show, NULL);
}

#ifdef CONFIG_ODI_SWITCH
/* Pulls one whitespace-delimited token out of *pp, parses it with
 * kstrtouint base 16 explicitly (not base 0, see below), and advances *pp
 * past it. Returns 0 and *out on success, -1 if no token remains or it
 * does not parse as a number -- used by the parity_add three-number line
 * below instead of sscanf, the same "kstrtouint over a fragile format
 * string" posture the rest of this parser already uses.
 *
 * Base 16, not base 0 (bug found in s9): every table file this feeds
 * (tools/regdump/mkparity.py's own
 * output, parity-v6.table and friends) writes offset/value/mask as
 * zero-padded 8-digit hex with no "0x" prefix, e.g. "0080c101" -- fed to
 * kstrtouint base 0, a leading '0' makes it parse as octal, and '8' is
 * not a valid octal digit, so the whole parity_add write failed with
 * -EINVAL (parity-load.sh: "write error: Invalid argument") until the
 * table was hand-edited to add "0x" to every field. kstrtouint base 16
 * still strips one leading "0x"/"0X" prefix if present (the kernel's own
 * _parse_integer_fixup_radix does that unconditionally for base 16, not
 * only for base 0), so both the plain zero-padded form the table files
 * actually use and an explicit "0x..." form (typed by hand, or from an
 * older table) parse the same way -- init_platform's and init_parity's
 * own mask arguments are untouched by this, they still take kstrtouint
 * base 0 directly, a few lines below.
 */
static int odi_omci_next_uint_token(char **pp, unsigned int *out)
{
	char *p = *pp;
	char *end;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0')
		return -1;
	end = p;
	while (*end != '\0' && *end != ' ' && *end != '\t')
		end++;
	if (*end != '\0')
		*end++ = '\0';
	if (kstrtouint(p, 16, out))
		return -1;
	*pp = end;
	return 0;
}

/* /proc/odi_omci write: six commands, all synchronous from the write()
 * syscall -- if one hangs the kernel the way s4's unconditional module-init
 * call did, that is this one write's problem, not the boot's: the stick
 * stays reachable up to the moment of the write, and whoever is watching
 * the RAM log knows exactly which write to blame.
 *
 * "init_platform[ mask]" -- bare runs
 * every item (ODI_SWITCH_INIT_PLATFORM_ITEM_ALL), "init_platform 0x3f"
 * runs only the items 0x3f names.
 *
 * "init_parity[ mask]" -- bare calls odi_switch_parity_init_trigger_all() (every entry
 * of whichever table -- compiled default or loaded -- is currently active,
 * any size); "init_parity 0x3fff" calls odi_switch_parity_init_trigger()
 * with that bitmask instead, which can only reach entries 0-31 (a uint32_t
 * mask has no bit for entry 32 and beyond -- use the bare form for a
 * loaded table bigger than that, such as tools/regdump/parity-v6.table).
 *
 * "parity_add <offset> <value> <mask>" (three numbers, hex or decimal) --
 * appends one entry to the loaded table via odi_switch_parity_add();
 * the first successful parity_add after boot (or after parity_clear)
 * switches the active table from the 14 compiled defaults to the loaded
 * one. offset is validated against the mapped MMIO window on the kernel
 * side (odi_switch_mmio_offset_in_bounds(), odi_switch_dal.c) -- refused,
 * not silently accepted, if it falls outside it.
 *
 * "parity_clear" -- empties the loaded table and switches back to the
 * compiled default, via odi_switch_parity_clear().
 *
 * rootfs/skeleton/etc/scripts/parity-load.sh is the intended caller for
 * parity_add/parity_clear/bare-init_parity, reading a table file (rcS:
 * /var/config/parity.table) and issuing one parity_add write per entry
 * followed by one bare "init_parity" write -- but any of the four commands
 * can be typed here directly for a one-off trial.
 *
 * "init_modload[ mask]" -- same shape as init_platform: bare runs every
 * category (ODI_SWITCH_INIT_MODLOAD_ITEM_ALL, bits 0-5, category 0's own
 * bit 0 still means "every register, unsplit"), "init_modload 0x7" runs
 * only the categories 0x7 names (registers, CF sweeps and CF specific
 * rules here). Category 0's own registers are further bit-split when bit
 * 0 is clear: bit 6 the LUT flood masks, bits 7 and up each a register
 * family, bit 30 forcing the flood masks to include the CPU port --
 * odi_switch_dal.h has the exact bit table. rcS's own /var/config/
 * modload.mask read triggers this the same way, right alongside
 * init_platform's own trigger.
 *
 * "ds_encrypt <mask>" -- sets bit 4
 * (AES enable) of DSF_GEM_FLOW_TYPE for every GEM port slot mask
 * names (bit n selects slot n, 0-31), via odi_switch_ds_encrypt(). A
 * manual stand-in for the OLT's own Encrypted_Port-ID PLOAM message,
 * whose stock handler never fired for odi_switch's
 * own GEM ports until running the GPON stack open by default added the
 * automatic hook below -- this /proc write is now the fallback for when
 * that hook is not registered or the operator wants to force a mask by
 * hand. No rcS gate, unlike modload/parity/init_platform: GEM ports do
 * not exist yet at rcS's own trigger point, before OLT provisioning.
 *
 * "peek <off>" -- reads one switch-core register through the odi_switch
 * MMIO primitive (odi_reg_read(), the same bounds guard against
 * ODI_SWITCH_MMIO_SIZE every other caller gets) and stores the result for
 * the NEXT /proc/odi_omci read to show, as a "peek 0x<off> 0x<val>" line
 * (odi_omci_proc_show() above) -- the write itself only triggers the read
 * and logs it, same posture as every other command here.
 *
 * "poke <off> <val>" -- writes one switch-core register the same way
 * (odi_reg_write()), logged with pr_info, no further trigger or state.
 *
 * Both take offset/value as odi_omci_next_uint_token() parses them (base
 * 16, one leading "0x"/"0X" stripped if present -- same helper parity_add
 * already uses above), and both are switch-core offsets like every other
 * register this codebase names, not physical addresses.
 */
static int odi_omci_proc_cmd(char *buf)
{
	char *p;
	unsigned int mask, offset, value;

	lockdep_assert_held(&odi_switch_lock);

	if (strncmp(buf, "init_platform", 13) == 0) {
		p = buf + 13;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0') {
			mask = ODI_SWITCH_INIT_PLATFORM_ITEM_ALL;
		} else if (kstrtouint(p, 0, &mask)) {
			return -EINVAL;
		}
		pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_init_platform(0x%02x)\n", mask);
		odi_switch_platform_init_trigger(mask);
		return 0;
	}

	if (strncmp(buf, "init_parity", 11) == 0) {
		p = buf + 11;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0') {
			pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_init_parity_all()\n");
			odi_switch_parity_init_trigger_all();
			return 0;
		}
		if (kstrtouint(p, 0, &mask))
			return -EINVAL;
		pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_init_parity(0x%08x)\n", mask);
		odi_switch_parity_init_trigger(mask);
		return 0;
	}

	if (strncmp(buf, "parity_add", 10) == 0) {
		p = buf + 10;
		if (odi_omci_next_uint_token(&p, &offset) ||
		    odi_omci_next_uint_token(&p, &value) ||
		    odi_omci_next_uint_token(&p, &mask))
			return -EINVAL;
		pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_parity_add(0x%08x, 0x%08x, 0x%08x)\n", offset, value, mask);
		if (odi_switch_parity_add(offset, value, mask))
			return -EINVAL; /* rejected -- odi_switch_dal.c already logged why */
		return 0;
	}

	if (strncmp(buf, "parity_clear", 12) == 0) {
		pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_parity_clear()\n");
		odi_switch_parity_clear();
		return 0;
	}

	if (strncmp(buf, "init_modload", 12) == 0) {
		p = buf + 12;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0') {
			mask = ODI_SWITCH_INIT_MODLOAD_ITEM_ALL;
		} else if (kstrtouint(p, 0, &mask)) {
			return -EINVAL;
		}
		pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_init_modload(0x%08x)\n", mask);
		odi_switch_modload_init_trigger(mask);
		return 0;
	}

#ifdef CONFIG_ODI_SDKINIT
	if (strncmp(buf, "sdkinit_mask", 12) == 0) {
		p = buf + 12;
		while (*p == ' ' || *p == '\t')
			p++;
		if (kstrtouint(p, 0, &mask))
			return -EINVAL;
		pr_info("odi_omci: /proc/odi_omci write sets sdkinit_mask 0x%08x\n", mask);
		odi_switch_sdkinit_mask_set(mask);
		return 0;
	}
#endif

	if (strncmp(buf, "ds_encrypt", 10) == 0) {
		p = buf + 10;
		while (*p == ' ' || *p == '\t')
			p++;
		if (kstrtouint(p, 0, &mask))
			return -EINVAL;
		pr_info("odi_omci: /proc/odi_omci write triggers odi_switch_ds_encrypt(0x%08x)\n", mask);
		odi_switch_ds_encrypt_trigger(mask);
		return 0;
	}

	if (strncmp(buf, "peek", 4) == 0) {
		p = buf + 4;
		if (odi_omci_next_uint_token(&p, &offset))
			return -EINVAL;
		value = odi_reg_read(offset);
		odi_omci_peek_off = offset;
		odi_omci_peek_val = value;
		odi_omci_peek_valid = true;
		pr_info("odi_omci: /proc/odi_omci peek 0x%08x 0x%08x\n", offset, value);
		return 0;
	}

	if (strncmp(buf, "poke", 4) == 0) {
		p = buf + 4;
		if (odi_omci_next_uint_token(&p, &offset) ||
		    odi_omci_next_uint_token(&p, &value))
			return -EINVAL;
		pr_info("odi_omci: /proc/odi_omci poke 0x%08x 0x%08x\n", offset, value);
		odi_reg_write(offset, value);
		return 0;
	}

	/* "ddm <type>" -- runs odi_ddm_get() for that selector (one of
	 * odi_ddm.h ODI_DDM_* values, 0-6) the same way ODI_SW_IOC_DDM_GET
	 * does, and stores rc plus the 24 raw bytes for the next read to
	 * show. Bypasses /dev/odi_sw and src/diag entirely -- useful to tell
	 * "bad selector" (odi_ddm_get() itself refuses) from "I2C read
	 * failed" (odi_i2c.c) from a working read, right from the shell.
	 */
	if (strncmp(buf, "ddm", 3) == 0) {
		int type;
		unsigned int utype;

		p = buf + 3;
		if (odi_omci_next_uint_token(&p, &utype))
			return -EINVAL;
		type = (int)utype;
		odi_omci_ddm_rc = odi_ddm_get(type, odi_omci_ddm_raw);
		odi_omci_ddm_type = type;
		odi_omci_ddm_valid = true;
		pr_info("odi_omci: /proc/odi_omci ddm %d rc %d\n", type, odi_omci_ddm_rc);
		return 0;
	}

	/* "i2c <dev> <addr> <len>" -- a raw odi_i2c_read_bytes() call: dev is
	 * the I2C_MASTER_SETUP device-select word (odi_i2c.h ODI_I2C_SEL_A0/_A2,
	 * or any other value to try), addr the starting SFF-8472 byte
	 * address, len the byte count (capped at ODI_OMCI_I2C_MAX_LEN --
	 * refused, not silently truncated, past that). Same rc/hex-dump
	 * shape as "ddm" above, one level lower: this skips the odi_ddm.c
	 * selector table entirely.
	 */
	if (strncmp(buf, "i2c", 3) == 0) {
		unsigned int dev, addr, ulen;

		p = buf + 3;
		if (odi_omci_next_uint_token(&p, &dev) ||
		    odi_omci_next_uint_token(&p, &addr) ||
		    odi_omci_next_uint_token(&p, &ulen))
			return -EINVAL;
		if (ulen > ODI_OMCI_I2C_MAX_LEN)
			return -EINVAL;
		odi_omci_i2c_rc = odi_i2c_read_bytes(dev, addr, odi_omci_i2c_raw, ulen);
		odi_omci_i2c_sel = dev;
		odi_omci_i2c_addr = addr;
		odi_omci_i2c_len = ulen;
		odi_omci_i2c_valid = true;
		pr_info("odi_omci: /proc/odi_omci i2c 0x%08x 0x%08x %u rc %d\n",
			dev, addr, ulen, odi_omci_i2c_rc);
		return 0;
	}

	return -EINVAL;
}

static ssize_t odi_omci_proc_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char buf[96];
	size_t len = count;
	int rc;

	(void)file;
	(void)ppos;

	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0'; /* drop a trailing newline, echo adds one */

	/* Every command here drives the switch core or the result buffers
	 * odi_omci_proc_show() reads, so the whole command runs under
	 * odi_switch_lock (odi_switch.c). ddm and i2c take odi_i2c_lock
	 * inside it, in odi_i2c_read_bytes().
	 */
	mutex_lock(&odi_switch_lock);
	rc = odi_omci_proc_cmd(buf);
	mutex_unlock(&odi_switch_lock);
	return rc ? rc : (ssize_t)count;
}
#endif

static const struct proc_ops odi_omci_proc_fops = {
	.proc_open = odi_omci_proc_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
#ifdef CONFIG_ODI_SWITCH
	.proc_write = odi_omci_proc_write,
#endif
};

/* ---- Module init/exit ----------------------------------------------------- */

static int __init odi_omci_init(void)
{
	struct netlink_kernel_cfg cfg = { .input = odi_omci_nl_input };
	int rc;

	odi_omci_nlsk = netlink_kernel_create(&init_net, NETLINK_USERSOCK, &cfg);
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

	pr_info(DRV_NAME ": ready, NETLINK_USERSOCK, rxhook priority %d\n", ODI_OMCI_RXHOOK_PRIO);
	return 0;
}

static void __exit odi_omci_exit(void)
{
	remove_proc_entry("odi_omci", NULL);
	odi_nic_rxhook_unregister(ODI_OMCI_RXHOOK_PORTMASK, ODI_OMCI_RXHOOK_PRIO, odi_omci_rxhook);
	if (odi_omci_nlsk) {
		netlink_kernel_release(odi_omci_nlsk);
		odi_omci_nlsk = NULL;
	}
}

module_init(odi_omci_init);
module_exit(odi_omci_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss OMCI frame transport (pkt_redirect replacement)");
