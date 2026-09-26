/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_omci_wire.h -- the pkt_redirect wire layout, restated as pack/unpack
 * functions instead of two independent readings (one in the kernel module,
 * one in a host test) that could silently drift apart.
 *
 * Source of truth: src/omci/nl.h (omcid's own view of the
 * transport: NLMSG_HDR, NL_BODY, NL_REG_BODY, NL_OP_REG/SEND, the autobind
 * pid convention) plus one thing nl.h does not state because it is the
 * kernel's own delivery format, not something omcid builds: the 4-byte pad
 * ahead of an RX frame. That pad is confirmed from src/omci/respond/main.c's
 * own receive loop (`rxbuf + NLMSG_HDR + NL_FRAME_OFF` is where it reads the
 * frame from). Every constant is either already in nl.h (ours) or derived
 * by reading our own omcid.
 *
 * Plain C, fixed-width types, no kernel dependency -- test/odi_omci_test.c
 * compiles and runs this with the host cc, the same pattern odi_nic_hw.h and
 * test/odi_nic_hw_test.c already use for the NIC driver.
 */
#ifndef ODI_OMCI_WIRE_H
#define ODI_OMCI_WIRE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <string.h>
#endif

/* nl.h: NETLINK_ODI -- our own protocol number, not one the kernel names in
 * include/uapi/linux/netlink.h (0-16 and 18-22 are all taken as of 6.18,
 * MAX_LINKS 32; 17 is a reserved gap, never assigned). Any process can open
 * either kind of netlink socket -- picking our own number is about not
 * sharing a family with unrelated NETLINK_USERSOCK users, not privilege.
 * odi_omci.c used NETLINK_USERSOCK (2) before this.
 */
#define ODI_OMCI_NETLINK	23

/* nl.h: NLMSG_HDR total-length@0 (u32) ... pid/tid@12 (u32). */
#define ODI_OMCI_NLMSG_HDR	16u
/* nl.h: NL_BODY -- op@0, type/uid@2, isUser@4, flag@8, dataLen@12 (u32,
 * only the low 16 bits of which the original driver read -- our own kernel
 * side reads the full 32 bits, correctly, since REDIRECT_MTU (1500) never
 * sets the high half anyway).
 */
#define ODI_OMCI_NL_BODY	16u
/* nl.h: NL_REG_BODY -- op@0, type@2, tid@4, action@8 (u8), mtu@10. */
#define ODI_OMCI_NL_REG_BODY	12u
/* nl.h: NL_FRAME_OFF -- the driver's own 4-byte prefix (redirect port, zero
 * on this device) ahead of a frame the kernel hands up to a registered pid.
 * Confirmed from src/omci/respond/main.c's receive loop, see file header.
 */
#define ODI_OMCI_FRAME_OFF	4u

/* nl.h: NL_OP_REG / NL_OP_SEND, NL_ACT_REG / NL_ACT_DEREG. */
#define ODI_OMCI_OP_REG		1u
#define ODI_OMCI_OP_SEND	2u
#define ODI_OMCI_ACT_REG	1u
#define ODI_OMCI_ACT_DEREG	2u

/* ODI_OMCI_OP_CMD -- ours: not a stock op number, both ends of
 * this one are our own code (this file, the kernel side; src/omci/nl.h,
 * omcid's own side -- kept byte-identical by hand, there is no shared source
 * to generate either from). Carries an odi_switch_cmd() call
 * (odi_switch_cmd.h) over the same ODI_OMCI_NETLINK socket omcid already
 * holds for OMCI
 * frame RX/TX, in place of the getsockopt path of the stock driver; the
 * sender is omci_drv_call() (src/omci/respond/drv.c).
 *
 * Request body (after NLMSG_HDR, ODI_OMCI_CMD_REQ_HDR bytes then the
 * argument):
 *   op   u16 @0   ODI_OMCI_OP_CMD
 *   rsv  u16 @2   0, reserved
 *   cmd  u32 @4   the OMCI driver command number, same numbering odi_switch_cmd()
 *                 dispatches on (odi-oss's own numbering, see odi_switch_cmd.c's own
 *                 header comment)
 *   len  u32 @8   argument byte count
 *   arg  len bytes @12
 *
 * Reply body (ODI_OMCI_CMD_REPLY_HDR bytes then the argument, updated in
 * place for commands that read hardware state, which write their result
 * back into the same struct the caller sent):
 *   op     u16 @0   ODI_OMCI_OP_CMD
 *   rsv    u16 @2   0
 *   cmd    u32 @4   echoed
 *   status i32 @8   0 on success, a negative errno otherwise (-EOPNOTSUPP,
 *                   -95, is what a CONFIG_ODI_SWITCH=n kernel always
 *                   answers, so the caller can fall back)
 *   len    u32 @12  argument byte count (equals the request's own len)
 *   arg    len bytes @16
 */
#define ODI_OMCI_OP_CMD		3u
#define ODI_OMCI_CMD_REQ_HDR	12u
#define ODI_OMCI_CMD_REPLY_HDR	16u
/* Matches apply.c's own existing getsockopt-path cap (omci_drv_call `req[264]`,
 * 8 bytes of its own header leaving 256) -- no generated command struct
 * is larger than that today.
 */
#define ODI_OMCI_CMD_MAX_LEN	256u

/* nl.h: REDIRECT_TYPE -- the redirect uid the stock OMCI daemon registers
 * for OMCI traffic (1, observed on the wire).
 */
#define ODI_OMCI_REDIRECT_TYPE	1u

static inline void odi_omci_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void odi_omci_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint16_t odi_omci_get16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t odi_omci_get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* Decoded view of one incoming message, filled by odi_omci_parse(). One
 * message is one skb: nl.h's nl_send() is one sendmsg() per call, and the
 * driver never reassembles more than one at a time (see odi_omci.c's input
 * callback comment).
 */
struct odi_omci_msg {
	unsigned int op;		/* ODI_OMCI_OP_REG or ODI_OMCI_OP_SEND */
	unsigned int type;		/* redirect type/uid; only REDIRECT_TYPE(1) is ours */
	uint32_t reg_pid;	/* op == REG: the tid the sender bound to (nl_open) */
	unsigned int reg_action;	/* op == REG: ODI_OMCI_ACT_REG or ODI_OMCI_ACT_DEREG */
	uint32_t send_len;	/* op == SEND: payload length */
	uint32_t send_off;	/* op == SEND: payload offset within the buffer */
	uint32_t cmd_num;	/* op == CMD: the OMCI driver command number */
	uint32_t cmd_len;	/* op == CMD: argument byte count */
	uint32_t cmd_off;	/* op == CMD: argument offset within the buffer */
};

/* Parses buf[0..len) as one message. Returns 0 and fills *out on success;
 * -1 if the buffer is too short for the op it declares, or for an op this
 * transport does not know (op 0, never sent by nl.h, falls here too).
 */
static inline int odi_omci_parse(const uint8_t *buf, uint32_t len, struct odi_omci_msg *out)
{
	const uint8_t *body;
	unsigned int op;

	if (len < ODI_OMCI_NLMSG_HDR + 2u)
		return -1;
	body = buf + ODI_OMCI_NLMSG_HDR;
	op = odi_omci_get16(body + 0);

	if (op == ODI_OMCI_OP_REG) {
		if (len < ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_REG_BODY)
			return -1;
		out->op = op;
		out->type = odi_omci_get16(body + 2);
		out->reg_pid = odi_omci_get32(body + 4);
		out->reg_action = body[8];
		return 0;
	}
	if (op == ODI_OMCI_OP_SEND) {
		uint32_t dlen;

		if (len < ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY)
			return -1;
		dlen = odi_omci_get32(body + 12);
		if (dlen > len - (ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY))
			return -1;
		out->op = op;
		out->type = odi_omci_get16(body + 2);
		out->send_len = dlen;
		out->send_off = ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY;
		return 0;
	}
	if (op == ODI_OMCI_OP_CMD) {
		uint32_t dlen;

		if (len < ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR)
			return -1;
		dlen = odi_omci_get32(body + 8);
		if (dlen > ODI_OMCI_CMD_MAX_LEN ||
		    dlen > len - (ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR))
			return -1;
		out->op = op;
		out->cmd_num = odi_omci_get32(body + 4);
		out->cmd_len = dlen;
		out->cmd_off = ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR;
		return 0;
	}
	return -1;
}

/* Bytes odi_omci_build_rx() needs for a frame of this length. */
static inline uint32_t odi_omci_rx_total(uint32_t framelen)
{
	return ODI_OMCI_NLMSG_HDR + ODI_OMCI_FRAME_OFF + framelen;
}

/* Builds the kernel-to-user delivery omcid's receive loop expects (file
 * header: NLMSG_HDR, then the 4-byte redirect-port pad, then the frame).
 * buf must be at least odi_omci_rx_total(framelen) bytes; returns the total
 * length written. The pid field (NLMSG_HDR offset 12) is left zero: main.c's
 * receive loop never reads it, only the length at offset 0.
 */
static inline uint32_t odi_omci_build_rx(uint8_t *buf, const uint8_t *frame, uint32_t framelen)
{
	uint32_t total = odi_omci_rx_total(framelen);

	odi_omci_put32(buf + 0, total);
	odi_omci_put32(buf + 4, 0);
	odi_omci_put32(buf + 8, 0);
	odi_omci_put32(buf + 12, 0);
	odi_omci_put32(buf + ODI_OMCI_NLMSG_HDR, 0);
	memcpy(buf + ODI_OMCI_NLMSG_HDR + ODI_OMCI_FRAME_OFF, frame, framelen);
	return total;
}

/* Bytes odi_omci_build_cmd_reply() needs for an argument of this length. */
static inline uint32_t odi_omci_cmd_reply_total(uint32_t arglen)
{
	return ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REPLY_HDR + arglen;
}

/* Builds an ODI_OMCI_OP_CMD reply. buf must be at least
 * odi_omci_cmd_reply_total(arglen) bytes; returns the total length
 * written. status is a plain int here (not int32_t) so the kernel side
 * can pass an errno straight through without a cast at every call site.
 */
static inline uint32_t odi_omci_build_cmd_reply(uint8_t *buf, uint32_t cmd, int status,
						  const uint8_t *arg, uint32_t arglen)
{
	uint32_t total = odi_omci_cmd_reply_total(arglen);
	uint8_t *body = buf + ODI_OMCI_NLMSG_HDR;

	odi_omci_put32(buf + 0, total);
	odi_omci_put32(buf + 4, 0);
	odi_omci_put32(buf + 8, 0);
	odi_omci_put32(buf + 12, 0);
	odi_omci_put16(body + 0, (uint16_t)ODI_OMCI_OP_CMD);
	odi_omci_put16(body + 2, 0);
	odi_omci_put32(body + 4, cmd);
	odi_omci_put32(body + 8, (uint32_t)status);
	odi_omci_put32(body + 12, arglen);
	if (arglen)
		memcpy(body + ODI_OMCI_CMD_REPLY_HDR, arg, arglen);
	return total;
}

#endif /* ODI_OMCI_WIRE_H */
