/* Our own CLI protocol, on queue 0x9601.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"

/* ---------------------------------------------------------------- the CLI
 *
 * omcid answers commands on its own message queue. The protocol is in
 * ../omci_cli_proto.h and the whole point of it is that nothing here writes a
 * file, sleeps, or has a length it cannot report: the request is argv, the
 * answer is a framed byte stream, and every dump below is ordinary out_fmt
 * redirected into it.
 *
 * What it can answer is what it actually knows. Several of these have no
 * vendor equivalent -- the flow tables and the capability blob are state the
 * stock CLI cannot reach at all.
 */
long cliq = -1;

/* Where the answer to the request being dispatched goes. Separate from cliq --
 * which is OUR queue, the one with the well-known key -- so that the signal
 * handler always removes the right one. */
long reply_q = -1;

struct omcli_req clireq;

struct omcli_rep clirep;

uint32_t cli_seq;

int cli_failed;

static void cli_chunk(uint32_t len, uint32_t flags, uint32_t status)
{
	clirep.mtype  = OMCLI_MTYPE;
	clirep.magic  = OMCLI_MAGIC;
	clirep.seq    = cli_seq++;
	clirep.status = status;
	clirep.flags  = flags;
	clirep.len    = len;
	/* A dead client must not wedge the daemon: the queue holds two chunks,
	 * so a reader that has gone away would block the writer forever. Give
	 * up on it instead -- there is nobody left to tell.
	 *
	 * Only EAGAIN is worth waiting out. It means the queue is full and the
	 * client is merely slow. EINVAL (a reply qid that was never valid) and
	 * EIDRM (the client removed its queue, or died and something cleaned
	 * up) never recover, and spending CLI_RETRIES x CLI_RETRY_NS on them is
	 * time this single-threaded daemon is not reading netlink. */
	for (int tries = 0; tries < CLI_RETRIES; tries++) {
		long rc = mq_snd_flags(reply_q, &clirep, OMCLI_REP_LEN(len),
				       IPC_NOWAIT);

		if (rc >= 0)
			return;
		if (rc != -MQ_EAGAIN)
			break;
		sys_nanosleep(0, CLI_RETRY_NS);
	}
	cli_failed = 1;
}

/* The sink out_fmt flushes into. It fills a chunk and sends it; the tail goes
 * out with the end marker in cli_end(). */
uint32_t cli_used;

void cli_sink(const char *s, int n)
{
	while (n > 0 && !cli_failed) {
		uint32_t room = OMCLI_CHUNK - cli_used;
		uint32_t take = (uint32_t)n < room ? (uint32_t)n : room;

		for (uint32_t i = 0; i < take; i++)
			clirep.data[cli_used + i] = (uint8_t)s[i];
		cli_used += take;
		s += take;
		n -= (int)take;
		if (cli_used == OMCLI_CHUNK) {
			cli_chunk(cli_used, OMCLI_MORE, OMCLI_OK);
			cli_used = 0;
		}
	}
}

static void cli_end(uint32_t status)
{
	/* out_flush() pushes whatever the command printed through cli_sink and
	 * into cli_used, so the tail has to be SENT before it is cleared.
	 * Clearing first sends an end marker of length 0 and drops the tail --
	 * which is every reply shorter than one chunk, and the last partial
	 * chunk of every longer one. It did exactly that for fourteen commits.
	 */
	out_flush();
	/* cli_sink already gave up on this client; sending the end marker would
	 * spend the retry budget a second time on a queue known to be dead. */
	if (cli_failed) {
		cli_used = 0;
		return;
	}
	cli_chunk(cli_used, 0, status);
	cli_used = 0;
}

/* ------------------------------------------------------------- the commands */

/* The i'th NUL-terminated argument, or NULL.
 *
 * Bounded, because this walks bytes that arrived over an IPC queue: nothing
 * guarantees the sender put a NUL anywhere in the 512. cli_poll zeroes the tail
 * and terminates the last byte, so a well-formed request always ends inside the
 * buffer -- this is the belt to that braces, and it is what stops a malformed
 * one walking into the next static. */
const char *cli_arg(unsigned i)
{
	const uint8_t *p = clireq.args;
	const uint8_t *end = clireq.args + OMCLI_ARGS;

	if (i >= clireq.nargs)
		return 0;
	while (i--) {
		while (p < end && *p)
			p++;
		if (p >= end)
			return 0;
		p++;
	}
	return p < end ? (const char *)p : 0;
}

int cli_num(unsigned i, uint32_t *out)
{
	const char *s = cli_arg(i);
	uint32_t v = 0;
	int n = 0;

	if (!s)
		return 0;
	for (; *s; s++, n++) {
		if (*s < '0' || *s > '9')
			return 0;
		v = v * 10 + (uint32_t)(*s - '0');
	}
	*out = v;
	return n > 0;
}

/* An entity id, decimal or 0x-prefixed. The dumps print them in hex and the
 * OLT's own numbering is hex-shaped (0x101, 0x601), so requiring decimal here
 * is a transcription error waiting to happen. */
int cli_id(unsigned i, uint32_t *out)
{
	const char *s = cli_arg(i);
	uint32_t v = 0;
	int n = 0;

	if (!s)
		return 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (s += 2; *s; s++, n++) {
			uint32_t d;

			if (*s >= '0' && *s <= '9')      d = (uint32_t)(*s - '0');
			else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
			else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
			else return 0;
			v = v * 16 + d;
		}
	} else {
		for (; *s; s++, n++) {
			if (*s < '0' || *s > '9')
				return 0;
			v = v * 10 + (uint32_t)(*s - '0');
		}
	}
	*out = v;
	return n > 0;
}

static void cli_dispatch(void)
{
	const char *verb = cli_arg(0);
	uint32_t status;

	cli_seq = 0;
	cli_used = 0;
	cli_failed = 0;
	out_set_sink(cli_sink);
	if (!verb)                       status = OMCLI_EARGS;
	else if (str_eq(verb, "mib"))    status = cli_mib();
	else if (str_eq(verb, "flows"))  status = cli_flows();
	else if (str_eq(verb, "caps"))   status = cli_caps();
	else if (str_eq(verb, "tcont"))  status = cli_tcont();
	else if (str_eq(verb, "conn"))   status = cli_conn();
	else if (str_eq(verb, "bridge")) status = cli_bridge();
	else if (str_eq(verb, "state"))  status = cli_state();
	else if (str_eq(verb, "ident"))  status = cli_ident();
	else if (str_eq(verb, "vlan"))   status = cli_vlan();
	else if (str_eq(verb, "cfgset")) status = cli_cfgset();
	else if (str_eq(verb, "help"))   status = cli_help();
	else {
		out_fmt("no such command: %s\n", verb);
		status = OMCLI_ENOCMD;
	}
	cli_end(status);
	out_set_sink(0);
}

/* One pass of the queue, called from the main loop. Non-blocking: this daemon
 * has one thread and the OMCI channel is the thing it must not be late for. */
int cli_poll(void)
{
	long rc;

	if (cliq < 0)
		return 0;
	/* MSG_NOERROR for the same reason as vq_poll: a request one byte too
	 * long would otherwise stay at the head of the queue forever and wedge
	 * every client after it. */
	rc = mq_rcv_flags(cliq, &clireq, sizeof clireq - 4, -2,
			  IPC_NOWAIT | MSG_NOERROR);
	if (rc < 0)
		return 0;
	if (clireq.magic != OMCLI_MAGIC || clireq.version != OMCLI_VERSION)
		return 0;
	/* Everything past what actually arrived is stale from a previous,
	 * longer request, and cli_arg walks these bytes looking for NULs. Clear
	 * the tail and terminate the buffer so a truncated or malformed request
	 * cannot walk out of the struct. */
	if (rc >= (long)(sizeof clireq - sizeof clireq.args - 4)) {
		unsigned long got = (unsigned long)rc
				  - (sizeof clireq - sizeof clireq.args - 4);
		unsigned long i;

		if (got > OMCLI_ARGS)
			got = OMCLI_ARGS;
		for (i = got; i < OMCLI_ARGS; i++)
			clireq.args[i] = 0;
	} else {
		unsigned long i;

		for (i = 0; i < OMCLI_ARGS; i++)
			clireq.args[i] = 0;
	}
	clireq.args[OMCLI_ARGS - 1] = 0;
	/* The reply target is its own variable rather than a borrowed `cliq`.
	 * It used to be swapped into cliq for the duration of the dispatch,
	 * which meant a signal arriving mid-command removed the CLIENT's
	 * private queue and left ours -- the one with a well-known key, and the
	 * one that outlives the process -- behind. */
	reply_q = (long)clireq.replyQid;
	if (reply_q >= 0) {
		if (clireq.nargs > OMCLI_MAXARGS)
			clireq.nargs = OMCLI_MAXARGS;
		cli_dispatch();
	}
	reply_q = -1;
	return 1;
}
