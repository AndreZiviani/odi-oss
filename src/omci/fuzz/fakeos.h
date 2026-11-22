/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* What a harness tells the kernel model (fakeos.c) before it starts omcid,
 * and what it reads back afterwards. See fakeos.c for the model itself.
 */
#ifndef ODI_FUZZ_FAKEOS_H
#define ODI_FUZZ_FAKEOS_H

#include <stdint.h>
#include <stddef.h>

/* One thing that happens to the running daemon. The script of a run is an
 * array of these, taken in order: each is delivered when omcid next asks the
 * source it comes from, and the one after it waits until then. */
enum fk_kind {
	FK_LINE_FRAME,   /* an OMCI frame off the line: data, len */
	FK_VQ_MSG,       /* a message on the omcicli queue (key 0x800): a whole
			  * struct omci_msg in data, len bytes of it */
	FK_CLI_MSG,      /* a message on the omcli queue: a struct omcli_req */
	FK_PAUSE,        /* arg seconds pass with nothing arriving */
	FK_SIGNAL,       /* signal arg is delivered to the handler omcid set */
	FK_RESPAWN,      /* omcid dies here as if by SIGKILL; fk_run() returns
			  * with stats.respawn set and the rest of the script
			  * waits for the next fk_run() */
};

struct fk_event {
	uint8_t kind;
	uint32_t arg;
	const uint8_t *data;
	uint32_t len;
};

/* How the model behaves for one run. All zero is a healthy stick. */
struct fk_opts {
	/* The driver: answer command number drv_fail_at (counted from 1) with
	 * -EIO, or never answer anything (drv_dead), as with no odi_switch. */
	unsigned drv_fail_at;
	int drv_dead;
	/* The netlink protocol does not exist, as under qemu-user: omcid runs
	 * with no line side and serves only its queues. */
	int no_netlink;
	/* A client whose reply queue is full for good. */
	int reply_full;
	/* Print what omcid prints, and every frame it sends, on stderr. */
	int echo;
	/* Keep what omcid prints, for fk_output(). The replay asks for it; the
	 * fuzzer does not, and saves the copying. */
	int capture;
};

/* What the run did, for the checks a harness makes after it. */
struct fk_stats {
	unsigned frames_sent;    /* answers and notifications put on the line */
	unsigned drv_calls;      /* driver commands sent */
	unsigned replies;        /* messages sent to a client reply queue */
	unsigned kills;          /* sys_kill() calls; none reaches a process */
	long kill_pid;           /* the last one */
	int kill_sig;
	unsigned files_open;     /* file descriptors still open at the end */
	unsigned skipped;        /* script events omcid never took */
	/* FNV-1a over everything omcid let out so far: what it printed, the
	 * frames it put on the line, the number and length of each driver
	 * command. The
	 * same input gives the same value, or state crossed from one run
	 * into the next (replay.c runs every input twice to see). */
	uint64_t digest;
	int respawn;             /* ended by FK_RESPAWN, with script left */
	const uint8_t *respawn_data;  /* data and len of that event */
	uint32_t respawn_len;
	int exit_status;
};

/* Forget everything: files, queues, sockets, the clock, the script. A run
 * starts from the state of a stick that has just booted. */
void fk_reset(const struct fk_opts *opts);

/* A file omcid will find. Call after fk_reset(), before fk_run(). */
void fk_file_put(const char *path, const void *data, size_t len);

/* A client reply queue that exists before omcid starts. Returns its id;
 * key 0 is a private queue, found by id only. */
long fk_client_queue(uint32_t key);

/* The script. The array and what it points to stay the caller's and must
 * outlive fk_run(). */
void fk_script(const struct fk_event *ev, unsigned n);

/* What a process leaves behind when it is killed, and nothing else: the
 * files and the message queues stay, the descriptors, the handlers and a
 * pending alarm go. The clock runs on. Call between two fk_run() of one
 * script, when the first ended with stats.respawn. */
void fk_respawn(void);

/* mv -f, for the part of a respawn the service script does. */
void fk_file_move(const char *from, const char *to);

/* What omcid printed since fk_reset(), when opts.capture was set; not
 * terminated. Valid until the next fk_reset(). */
const char *fk_output(size_t *len);

/* stats.digest of the run since the last fk_reset(). */
uint64_t fk_digest(void);

/* The contents of a file, to look at or to change in place; 0 if there is
 * no such file. */
uint8_t *fk_file_data(const char *path, size_t *len);

/* Run omcid's main() until it returns, calls sys_exit(), the script kills
 * it (FK_RESPAWN), or the script is done and the model has sent it
 * SIGTERM. */
const struct fk_stats *fk_run(int (*entry)(int, char **), int argc, char **argv);

/* The CRC of a baseline frame, written separately from omcid's own so that
 * the check on what omcid sends does not rest on the code it checks. */
uint32_t fk_crc32(const uint8_t *d, size_t n);

#endif
