/* The omcid CLI protocol: argv in, a framed byte stream out.
 *
 * The vendor's arrangement is a 240-byte payload on one queue and, for
 * anything longer than that, a temp file in /tmp that the client races a
 * 12 ms sleep against. That is not a design so much as a consequence: its
 * dump functions printf, so the only way to capture them was to dup2 a file
 * over stdout. We own both ends here, so none of it is necessary.
 *
 *   * **A separate queue.** omcid keeps the vendor's key 0x800 for
 *     compatibility and owns OMCLI_KEY for this. A client finds out whether
 *     the good protocol is available by asking whether that queue exists --
 *     no probe message, no guessing at what an unknown msgType does to a
 *     daemon that is not ours. Keys 0x801 and 0x802 are taken: omci_app has
 *     three queues, not one.
 *   * **The reply queue is IPC_PRIVATE** and the request carries its id
 *     rather than a key, so two clients can never pick the same one and there
 *     is no key space to manage. The client also ignores SIGPIPE, because a
 *     client killed mid-stream -- `omcli mib | head` -- would otherwise leave
 *     its queue behind, and there are only 53.
 *   * **The request is argv.** The client does not need a command table at
 *     all; it forwards what it was given and prints what comes back, so a new
 *     command on the daemon needs no new client.
 *   * **The reply is a length-delimited stream** with an explicit end and a
 *     status, so a long dump cannot be truncated, a truncated one cannot go
 *     unnoticed, and there is nothing to wait a fixed time for.
 *
 * Sizing: this kernel reports msgmax 8192 and msgmnb 16384, so a chunk is kept
 * to 4 KB -- two fit in a queue, which is enough to keep the writer moving
 * while the reader drains without letting it run far ahead.
 */
#ifndef OMCI_CLI_PROTO_H
#define OMCI_CLI_PROTO_H

#include <stdint.h>

#define OMCLI_KEY        0x9601     /* ours; 0x800..0x802 are the vendor's */
#define OMCLI_MAGIC      0x4f4d434cu        /* "OMCL" */
#define OMCLI_VERSION    1
#define OMCLI_MTYPE      2

#define OMCLI_CHUNK      4096
#define OMCLI_ARGS       512        /* the whole command line, NUL-separated */
#define OMCLI_MAXARGS    16

/* flags */
#define OMCLI_MORE       1          /* another chunk follows this one */

/* status, valid on the chunk that clears OMCLI_MORE */
#define OMCLI_OK         0
#define OMCLI_ENOCMD     1          /* no such command */
#define OMCLI_EARGS      2          /* wrong arguments */
#define OMCLI_EFAIL      3          /* the command ran and failed */

struct omcli_req {
	uint32_t mtype;                 /* SysV type, always OMCLI_MTYPE */
	uint32_t magic;
	uint32_t version;
	uint32_t replyQid;      /* the queue id itself: the reply queue is
				 * IPC_PRIVATE, so it has no key to look up */
	uint32_t nargs;
	uint8_t  args[OMCLI_ARGS];      /* nargs NUL-terminated strings */
};

struct omcli_rep {
	uint32_t mtype;
	uint32_t magic;
	uint32_t seq;                   /* 0, 1, 2, ... within one answer */
	uint32_t status;
	uint32_t flags;
	uint32_t len;
	uint8_t  data[OMCLI_CHUNK];
};

/* What msgsnd is given: everything but the leading type word. */
#define OMCLI_REQ_LEN(arglen)  ((uint32_t)(sizeof(struct omcli_req) - OMCLI_ARGS \
					   - 4 + (arglen)))
#define OMCLI_REP_LEN(n)       ((uint32_t)(sizeof(struct omcli_rep) - OMCLI_CHUNK \
					   - 4 + (n)))

#endif
