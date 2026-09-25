/* The packet-redirect transport, shared by the capture and the responder.
 *
 * OMCI frames do not arrive on a netdev. The driver hands them to whichever
 * userland app has registered for redirect type 1, over NETLINK_USERSOCK, and
 * takes frames back the same way. libpr.so is the vendor's wrapper; this is the
 * same three messages, checked against the kernel's own handler at 0x8017f968
 * in the decompressed uImage:
 *
 *     nlh  = skb->data;  body = nlh + 16;  op = *(u16 *)body;
 *     op 1: body[8] == 1 register, == 2 deregister
 *     op 2: send, type at +2, port at +4, len as a u16 at +14, data at nlh + 32
 *
 * `len` being read as a u16 at +14 is why libpr's u32 at +12 works: on a
 * big-endian machine its low half lands exactly there.
 *
 * There is one receiver per type. Registering for a type that is already
 * registered overwrites that entry's pid rather than adding a receiver, so
 * running either tool takes the OMCI channel from omci_app until it registers
 * again.
 */
#ifndef ODI_OMCI_NL_H
#define ODI_OMCI_NL_H

#include "sys.h"

#define AF_NETLINK        16
#define NETLINK_USERSOCK  2
#define NL_SO_RCVTIMEO    0x1006      /* MIPS value, from asm/socket.h */
#define NL_SOL_SOCKET     0xffff

#define REDIRECT_TYPE     1           /* what omci_wrapper_createMsgDev uses */
#define REDIRECT_MTU      1500

#define NLMSG_HDR         16
/* The driver's own header, ahead of the payload. Both lengths were
 * recovered by tracing the netlink traffic of the stock firmware:
 *
 *     data header      u16 op, u16 uid, u32 is-user, u32 flag, u32 length
 *     register body    u16 op, u16 own uid, u32 own pid, u8 type,
 *                      u16 mtu                            (12 with padding)
 */
#define NL_BODY           16          /* op, uid, isUser, flag, dataLen */
#define NL_REG_BODY       12

/* PKT_REDIRECT_OPTYPE_* and PKT_REDIRECT_REGTYPE_* in the driver's header. */
#define NL_OP_REG         1
#define NL_OP_SEND        2
#define NL_ACT_REG        1
#define NL_ACT_DEREG      2

/* NL_OP_CMD -- ours, kept byte-identical by hand with
 * kernel/extra/drivers/net/ethernet/odi/odi_omci_wire.h
 * ODI_OMCI_OP_CMD (that file's own comment has the full wire layout).
 * Carries one odi_switch_cmd() call to odi_switch.ko over the same
 * NETLINK_USERSOCK socket this header already uses for OMCI frame RX/TX.
 */
#define NL_OP_CMD          3
#define NL_CMD_REQ_HDR     12   /* op@0 u16, rsv@2 u16, cmd@4 u32, len@8 u32 */
#define NL_CMD_REPLY_HDR   16   /* op@0, rsv@2, cmd@4, status@8 i32, len@12 */
#define NL_CMD_MAX_LEN     256  /* matches apply.c's own existing sockopt-path cap */

/* nlmsg_type is deliberately left zero.
 *
 * The driver takes the raw netlink input and casts skb->data to an nlmsghdr
 * itself; it never calls netlink_rcv_skb, and it dispatches on the op type --
 * the first field of the PAYLOAD. nlmsg_type is not examined anywhere on that path, so its value
 * cannot matter. See the note in capture/main.c, which used to claim
 * otherwise. */

/* The driver's buffer is handed over verbatim: four bytes of its own -- the
 * redirect port, zero on this device -- and then the OMCI frame. */
#define NL_FRAME_OFF      4

struct sockaddr_nl {
	uint16_t family;
	uint16_t pad;
	uint32_t pid;
	uint32_t groups;
};

struct iovec {
	void *base;
	uint32_t len;
};

struct msghdr {
	void *name;
	uint32_t namelen;
	struct iovec *iov;
	uint32_t iovlen;
	void *control;
	uint32_t controllen;
	int flags;
};

static inline void nl_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void nl_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline uint32_t nl_get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t nl_get16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Open, bind and set a receive timeout. The port is the thread id, as libpr
 * uses; the vendor never binds and relies on netlink autobind giving the same
 * number, which only holds because it registers from its main thread. */
/* `timeout_us` is microseconds, not seconds: a daemon that also serves the
 * stock omcicli has to look at its message queue inside that client's 12 ms
 * sleep, and the netlink receive timeout is what paces the loop. */
/* s3 fix: a second socket in the same thread cannot reuse this same
 * explicit pid -- sys_gettid() returns the
 * same value every time in a single-threaded process, so apply.c's own
 * ODI_OMCI_OP_CMD socket (opened after main.c's own, same thread) tried to
 * bind() the identical nl_pid a second time and got -EADDRINUSE back, which
 * nl_open() correctly reported as a failure but nothing above it logged --
 * cmd_nl_ensure() just saw a negative fd and every command silently fell
 * through to the (absent) sockopt path, never sending a single ODI_OMCI_OP_CMD
 * datagram (s2 trial: /proc/odi_omci cmd_requests stayed 0 the whole run).
 * nl_open_pid() below takes the pid to request explicitly instead of always
 * deriving it from the calling thread; 0 asks the kernel to autobind a free
 * one instead (standard netlink behaviour, kernel bind() with nl_pid == 0).
 * nl_open() is now a thin wrapper keeping every existing caller (main.c)
 * byte-identical -- tid, from the calling thread, same as before.
 */
static inline long nl_open_pid(uint32_t want_pid, uint32_t *tid_out, unsigned timeout_us)
{
	struct sockaddr_nl me;
	uint32_t tv[2];
	long fd, rc;

	fd = sys_socket(AF_NETLINK, SOCK_RAW, NETLINK_USERSOCK);
	if (fd < 0)
		return fd;
	me.family = AF_NETLINK;
	me.pad = 0;
	me.pid = want_pid;
	me.groups = 0;
	rc = sys_bind((int)fd, &me, sizeof me);
	if (rc != 0) {
		sys_close((int)fd);
		return rc;
	}
	tv[0] = timeout_us / 1000000u;
	tv[1] = timeout_us % 1000000u;
	sys_setsockopt((int)fd, NL_SOL_SOCKET, NL_SO_RCVTIMEO, tv, sizeof tv);
	/* want_pid == 0 (autobind): the kernel picked the actual pid, and this
	 * freestanding syscall layer has no sys_getsockname() to read it back
	 * with -- not needed here, since every caller that autobinds
	 * (apply.c's own cmd_nl_ensure()) only ever talks to the kernel itself,
	 * which replies to the netlink layer's own record of the sender
	 * (NETLINK_CB(skb).portid, odi_omci.c), not to any pid value carried
	 * in a payload field. *tid_out is set to want_pid either way -- 0 in
	 * that case -- and callers that embed it in a request payload
	 * (nl_cmd_call()) are documented as not depending on the kernel
	 * reading it.
	 */
	*tid_out = want_pid;
	return fd;
}

static inline long nl_open(uint32_t *tid_out, unsigned timeout_us)
{
	return nl_open_pid((uint32_t)sys_gettid(), tid_out, timeout_us);
}

static inline long nl_send(int fd, uint8_t *buf, uint32_t len)
{
	struct sockaddr_nl to;
	struct iovec iov;
	struct msghdr msg;

	to.family = AF_NETLINK;
	to.pad = 0;
	to.pid = 0;
	to.groups = 0;
	iov.base = buf;
	iov.len = len;
	msg.name = &to;
	msg.namelen = sizeof to;
	msg.iov = &iov;
	msg.iovlen = 1;
	msg.control = 0;
	msg.controllen = 0;
	msg.flags = 0;
	return sys_sendmsg(fd, &msg, 0);
}

/* Register or deregister for a redirect type; both are opcode 1. */
static inline long nl_redirect(int fd, uint32_t tid, int type, int action, int mtu)
{
	uint8_t buf[NLMSG_HDR + NL_REG_BODY];
	uint8_t *b = buf + NLMSG_HDR;

	for (unsigned i = 0; i < sizeof buf; i++)
		buf[i] = 0;
	nl_put32(buf + 0, sizeof buf);
	nl_put32(buf + 12, tid);
	nl_put16(b + 0, NL_OP_REG);
	nl_put16(b + 2, (uint16_t)type);
	nl_put32(b + 4, tid);
	b[8] = (uint8_t)action;
	nl_put16(b + 10, (uint16_t)mtu);
	return nl_send(fd, buf, sizeof buf);
}

/* Hand a payload to the driver, addressed to one redirect uid.
 *
 * What the payload has to contain is per-uid and not this header business:
 * OMCI sends the frame alone, IGMP sends an eight-byte portMask and sid ahead
 * of it. Everything from the netlink header down to `dataLen` is the same for
 * both, which is why this takes the uid rather than each caller repeating it.
 */
static inline long nl_send_payload(int fd, uint32_t tid, uint8_t *scratch,
				   int uid, const uint8_t *payload, uint32_t len)
{
	uint8_t *b = scratch + NLMSG_HDR;
	uint32_t total = NLMSG_HDR + NL_BODY + len;

	for (uint32_t i = 0; i < NLMSG_HDR + NL_BODY; i++)
		scratch[i] = 0;
	nl_put32(scratch + 0, total);
	nl_put32(scratch + 12, tid);
	nl_put16(b + 0, NL_OP_SEND);
	nl_put16(b + 2, (uint16_t)uid);
	nl_put32(b + 4, 0);                   /* isUser, zero for kernel-bound */
	nl_put32(b + 8, 0);
	nl_put32(b + 12, len);                /* low half lands at +14 */
	for (uint32_t i = 0; i < len; i++)
		b[NL_BODY + i] = payload[i];
	return nl_send(fd, scratch, total);
}

/* Hand a frame back to the driver. The kernel pads nothing, so the frame is
 * sent at its natural length; the vendor always sends 1504 and lets the driver
 * trim, which is the same thing with more bytes. */
static inline long nl_send_frame(int fd, uint32_t tid, uint8_t *scratch,
				 const uint8_t *frame, uint32_t len)
{
	return nl_send_payload(fd, tid, scratch, REDIRECT_TYPE, frame, len);
}

static inline long nl_recv(int fd, uint8_t *buf, uint32_t max, uint32_t *plen)
{
	struct sockaddr_nl from;
	struct iovec iov;
	struct msghdr msg;
	long rc;

	iov.base = buf;
	iov.len = max;
	msg.name = &from;
	msg.namelen = sizeof from;
	msg.iov = &iov;
	msg.iovlen = 1;
	msg.control = 0;
	msg.controllen = 0;
	msg.flags = 0;
	rc = sys_recvmsg(fd, &msg, 0);
	if (rc <= 0)
		return rc;
	{
		uint32_t n = (uint32_t)rc, nlen = nl_get32(buf);

		if (nlen >= NLMSG_HDR && nlen <= n)
			n = nlen;
		*plen = n > NLMSG_HDR ? n - NLMSG_HDR : 0;
	}
	return rc;
}

/* Sends one ODI_OMCI_OP_CMD request (cmd number, buf/len as the argument)
 * and waits for its reply, retrying recvmsg() up to retries times (the
 * netlink receive timeout nl_open() set, NL_POLL_US, paces each try).
 * fd/tid are a socket dedicated to this call -- apply.c opens its own,
 * separate from main.c's own OMCI-frame socket (nl_fd/nl_tid) -- so any
 * datagram this recvmsg() reads is either this call's own reply or noise
 * on a socket nothing else uses; there is no spillover/lost-frame case
 * to handle any more (s1 shared one socket for both jobs and needed a
 * one-slot buffer for exactly this reason -- s2 fixes removed it along
 * with the sharing).
 *
 * On success (return 0), buf holds the reply argument (in place -- some
 * commands write their result back into the caller's own struct, per
 * odi_omci_wire.h's own wire-layout comment) and *status is the driver's own
 * status word (0 or a negative errno, -EOPNOTSUPP in particular when the
 * kernel has no CONFIG_ODI_SWITCH). Returns -1 on a transport failure
 * (send failed, no reply arrived within the retry budget, or a reply
 * arrived malformed) without touching buf or *status -- the caller falls
 * back to the sockopt path either way.
 *
 * send_rc (may be NULL): set to the raw nl_send() return value on a send
 * failure (a negative errno) and left untouched otherwise -- s3 fix, so a
 * caller can log a send failure with its actual errno instead of the same
 * bare "-1" a timed-out reply also produces.
 */
static inline long nl_cmd_call(int fd, uint32_t tid, uint8_t *scratch,
				uint32_t cmd, void *buf, uint32_t len, int *status,
				unsigned retries, long *send_rc)
{
	uint8_t *b = scratch + NLMSG_HDR;
	uint32_t total = NLMSG_HDR + NL_CMD_REQ_HDR + len;
	long rc;
	unsigned try;

	if (len > NL_CMD_MAX_LEN)
		return -1;
	for (uint32_t i = 0; i < NLMSG_HDR + NL_CMD_REQ_HDR; i++)
		scratch[i] = 0;
	nl_put32(scratch + 0, total);
	nl_put32(scratch + 12, tid);
	nl_put16(b + 0, (uint16_t)NL_OP_CMD);
	nl_put16(b + 2, 0);
	nl_put32(b + 4, cmd);
	nl_put32(b + 8, len);
	for (uint32_t i = 0; i < len; i++)
		b[NL_CMD_REQ_HDR + i] = ((uint8_t *)buf)[i];
	rc = nl_send(fd, scratch, total);
	if (rc < 0) {
		if (send_rc)
			*send_rc = rc;
		return -1;
	}

	for (try = 0; try < retries; try++) {
		uint8_t rbuf[NLMSG_HDR + NL_CMD_REPLY_HDR + NL_CMD_MAX_LEN];
		struct sockaddr_nl from;
		struct iovec iov;
		struct msghdr msg;
		uint8_t *rbody;
		uint32_t rn, rcmd, rarglen;

		iov.base = rbuf;
		iov.len = sizeof rbuf;
		msg.name = &from;
		msg.namelen = sizeof from;
		msg.iov = &iov;
		msg.iovlen = 1;
		msg.control = 0;
		msg.controllen = 0;
		msg.flags = 0;
		rc = sys_recvmsg(fd, &msg, 0);
		if (rc <= 0)
			continue;
		rn = (uint32_t)rc;
		if (rn < NLMSG_HDR + NL_CMD_REPLY_HDR)
			continue; /* too short to be any message this transport sends -- drop */
		rbody = rbuf + NLMSG_HDR;
		if (nl_get16(rbody + 0) != NL_OP_CMD)
			continue; /* not one of ours -- ignore and keep waiting */
		rcmd = nl_get32(rbody + 4);
		if (rcmd != cmd)
			continue; /* a stale reply to an earlier, already-timed-out call */
		rarglen = nl_get32(rbody + 12);
		if (rarglen != len || rn < NLMSG_HDR + NL_CMD_REPLY_HDR + rarglen)
			return -1; /* our own reply, but malformed */
		*status = (int32_t)nl_get32(rbody + 8);
		for (uint32_t i = 0; i < len; i++)
			((uint8_t *)buf)[i] = rbody[NL_CMD_REPLY_HDR + i];
		return 0;
	}
	return -1;
}

#endif
