/* The omcicli transport: one System V message queue, one 260-byte message.
 *
 * `omci_app` owns queue key 0x800 and creates it; a client opens it without
 * IPC_CREAT and would rather fail than invent one. A command that answers does
 * so on a *second* queue, whose key the request carries, so the client makes
 * one of its own and removes it afterwards.
 *
 *     header, 20 bytes (omci_CreateMsg)
 *       +0  mtype     2 for a CLI command
 *       +4  reserved  0
 *       +8  msgType   4 = CLI, 0 = a raw OMCI frame from the OLT
 *       +12 replyKey  0 for fire and forget
 *       +16 len       20 + 240
 *     payload, exactly 240 bytes, command id in word 0
 *
 * `msgsnd` is given len - 4: the leading `long` is the type, not data. A reply
 * arrives with the same shape, the command id echoed in word 0, and is read
 * with msgtyp -2 -- "the lowest type at most 2" -- which is what the vendor
 * does and what lets mtype 1 be used for anything urgent later.
 *
 * The vendor hardcodes reply key 0x6868, so two concurrent runs read each
 * other's answers. This derives the key from the pid instead.
 */
#ifndef OMCI_MSGQ_H
#define OMCI_MSGQ_H

#include <stdint.h>
#include "sys.h"

#define OMCI_MQ_KEY        0x800    /* omci_app's command queue */
#define OMCI_MQ_MTYPE      2        /* what a CLI command carries */
#define OMCI_MQ_TYPE_CLI   4
#define OMCI_MQ_TYPE_FRAME 0        /* a raw OMCI frame, as from the line */
#define OMCI_MQ_PAYLOAD    240
#define OMCI_MQ_HDR        20
#define OMCI_MQ_LEN        (OMCI_MQ_HDR + OMCI_MQ_PAYLOAD)

/* msgrcv flag. Without it an oversize message is NOT consumed: msgrcv returns
 * -E2BIG and leaves it in the queue, so every later receive retrieves the same
 * unreadable message and everything behind it is stuck. A server that reads a
 * queue anyone can write must set this. */
#define MSG_NOERROR       010000

#define IPC_CREAT          01000
#define IPC_NOWAIT         04000
#define IPC_EXCL           02000
#define IPC_RMID           0

/* The errno values these calls actually return, as MIPS numbers. EAGAIN,
 * EINVAL and E2BIG are the asm-generic ones and are the same everywhere, but
 * EIDRM is NOT: MIPS keeps the original System V numbering, so it is 36 where
 * x86 and arm say 43. This trap is silent, because a comparison against 43 simply never
 * matches and the recovery path never runs.
 *
 *     ENOMSG 35, EIDRM 36, ECHRNG 37 ... ENOCSI 43
 *
 * These are the MIPS values in the Linux uapi asm/errno.h. */
#define MQ_EAGAIN          11
#define MQ_E2BIG            7
#define MQ_EINVAL          22
#define MQ_EIDRM           36
#define MQ_EEXIST          17   /* below the MIPS-divergent range: same as x86/arm */

struct omci_msg {
	uint32_t mtype;                       /* the SysV message type */
	uint32_t reserved;
	uint32_t msgType;
	uint32_t replyKey;
	uint32_t len;
	uint8_t  p[OMCI_MQ_PAYLOAD];
};

static inline void mq_put32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
}

static inline uint32_t mq_get32(const uint8_t *b)
{
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | b[3];
}

/* Payload field offsets, as omci_app's dispatch reads them. Recovered by
 * tools/omci-cli-table.py from the 45-entry jump table; the entity id is a
 * *halfword* at 24, which is easy to miss and makes every handler that takes
 * one look as though it took the word at 4. */
#define OMCI_P_ID       0       /* u32, the command id */
#define OMCI_P_ARG      4       /* u32, the third number: entity id for a dump,
				 * attribute index for getattr, key for avltree */
#define OMCI_P_CLASS    20      /* u32, class id */
#define OMCI_P_ENTITY   24      /* u16, entity id */
#define OMCI_P_NAME     35      /* string: table name, attribute name */
#define OMCI_P_VALUE    99      /* string: value, password, serial number */
#define OMCI_P_BYTE0    227     /* bytes: iotvlan config, simavc attr index */

/* This kernel has no msgget/msgsnd/msgrcv of their own: syscall 4399 and its
 * neighbours return -ENOSYS. MIPS o32 only grew them in Linux 5.x, and before
 * that all of System V IPC went through the `ipc` multiplexer, syscall 4117 --
 * the same arrangement x86 had. Copying the direct numbers out of a modern
 * toolchain header is exactly the kind of ABI trap that
 * costs a failed call rather than a compile error.
 *
 *   ipc(call, first, second, third, ptr, fifth)
 *
 * MSGRCV is the awkward one. With version 0 -- the low form, `call` with no
 * high bits -- the message pointer and the wanted type travel together in a
 * small struct rather than in registers, because sys_ipc ran out of arguments.
 */
#define IPC_MSGSND  11
#define IPC_MSGRCV  12
#define IPC_MSGGET  13
#define IPC_MSGCTL  14

struct ipc_kludge { void *msgp; long msgtyp; };

static inline long mq_open(uint32_t key, int create)
{
	return __syscall6(__NR_ipc, IPC_MSGGET, (long)key,
			  create ? (IPC_CREAT | 0600) : 0, 0, 0, 0);
}

static inline long mq_remove(long qid);

/* Make a queue at `key`, removing whatever is there first.
 *
 * A process that is killed between creating its queue and removing it leaves
 * one behind -- `omcli mib | wc -c` with no `wc` on the device dies on SIGPIPE
 * and does exactly that, and the kernel has 53 queues in total. Since the key
 * is derived from our own pid, anything already at it is a corpse of ours:
 * take it away rather than inherit its messages. */
static inline long mq_open_fresh(uint32_t key)
{
	long old = mq_open(key, 0);

	if (old >= 0)
		mq_remove(old);
	return mq_open(key, 1);
}

static inline long mq_remove(long qid)
{
	return __syscall6(__NR_ipc, IPC_MSGCTL, qid, IPC_RMID, 0, 0, 0);
}

/* The same two calls for a message that is not the vendor's fixed 260 bytes:
 * omcid's own protocol frames are variable length. */
static inline long mq_snd_flags(long qid, const void *m, unsigned long n,
				int flags)
{
	return __syscall6(__NR_ipc, IPC_MSGSND, qid, (long)n, flags, (long)m, 0);
}

/* mq_send() -- a client enqueuing a command onto a queue omcid (or, on the
 * omcli protocol, confd/metricsd) also reads.
 *
 * A plain blocking msgsnd (flags 0) waits in the kernel until there is room,
 * with no bound of its own: every client that shells out to omcicli/omcli
 * now has its OWN timeout around the whole child (metricsd's
 * OMCICLI_TIMEOUT_MS, confd's *_TIMEOUT_MS -- both odi-sfp-exporter and
 * odi-ui's own "Bound every wait" fixes), but that bound is a SIGKILL from
 * outside; it does not stop this call from parking in D state first, and a
 * daemon that is briefly slow (a bursty MIB upload, a respawn) is exactly
 * when several such clients pile in at once. IPC_NOWAIT plus a short bounded
 * retry -- the same shape omcid's own replies use (vqsrv.c, clisrv.c) --
 * means a client backs off and reports failure instead of becoming one more
 * thing silently waiting on another process forever.
 */
#define MQ_SEND_RETRIES   100
#define MQ_SEND_RETRY_NS  (5 * 1000 * 1000)   /* 5 ms; 500 ms total */

static inline long mq_send(long qid, const struct omci_msg *m)
{
	for (int tries = 0; tries < MQ_SEND_RETRIES; tries++) {
		long rc = mq_snd_flags(qid, m, OMCI_MQ_LEN - 4, IPC_NOWAIT);

		if (rc >= 0 || rc != -MQ_EAGAIN)
			return rc;
		sys_nanosleep(0, MQ_SEND_RETRY_NS);
	}
	return -MQ_EAGAIN;
}

static inline long mq_rcv_flags(long qid, void *m, unsigned long n,
				long msgtyp, int flags)
{
	struct ipc_kludge k = { m, msgtyp };

	return __syscall6(__NR_ipc, IPC_MSGRCV, qid, (long)n, flags, (long)&k, 0);
}

static inline long mq_recv_flags(long qid, struct omci_msg *m, long msgtyp,
				 int flags)
{
	struct ipc_kludge k = { m, msgtyp };

	return __syscall6(__NR_ipc, IPC_MSGRCV, qid, OMCI_MQ_LEN - 4, flags,
			  (long)&k, 0);
}

/* msgtyp -2 takes the lowest type <= 2, which is what the vendor asks for. */
static inline long mq_recv(long qid, struct omci_msg *m)
{
	return mq_recv_flags(qid, m, -2, 0);
}

#endif
