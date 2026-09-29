/* `nv commit <slot>` -- make the running slot the committed one, in BOTH
 * copies of the U-Boot environment, verified.
 *
 * Committing is two writes, not one. `env` and `env2` are a redundant pair,
 * and U-Boot boots from whichever wins (env_pick); the other is what it falls
 * back to if the winner is ever left invalid. A commit that writes only the
 * winner leaves the fallback still naming the previous slot, and a later
 * interrupted write sends the next boot there. So this writes the winner
 * (the primary) first, reads it back and compares it byte for byte, and only
 * then writes the fallback copy the same way, and finally re-reads both.
 *
 * The order is what makes an interruption safe at every point:
 *
 *   - before the primary is verified, the fallback still holds the old,
 *     valid environment, so a power cut mid-erase lands on the previous
 *     committed slot -- the same place the trial would have reverted to;
 *   - once the primary is verified, it holds the new commit and wins, so a
 *     power cut while the fallback is written loses nothing.
 *
 * What it refuses, before anything is written:
 *
 *   - a slot other than 0 or 1;
 *   - a slot that is not the one running (the caller reads /proc/cmdline);
 *   - a slot the primary copy does not name as sw_active -- U-Boot writes
 *     sw_active as it boots a slot, so this is its own record of what it
 *     started. sw_active is NEVER written here;
 *   - either copy unreadable or failing its CRC: with one good copy, the
 *     erase leaves the board with no good environment for as long as the
 *     write takes, and there is no second copy to make agree.
 *
 * Nothing here makes a syscall: the partition I/O comes in through
 * struct env_io, so the whole sequence, failures included, is tested on the
 * host against a fake flash (test/test_commit.c). */
#ifndef ODI_COMMIT_H
#define ODI_COMMIT_H

#include <stdint.h>

struct env_io {
	void *ctx;
	/* Read copy 1 (`env`) or 2 (`env2`) raw into buf, at most ENV_MAX
	 * bytes. Returns the partition size read, 0 on any failure. */
	uint32_t (*read)(void *ctx, int copy, uint8_t *buf);
	/* Erase copy and write len bytes of buf over it. 1 on success. */
	int (*write)(void *ctx, int copy, const uint8_t *buf, uint32_t len);
};

enum env_commit_rc {
	NVC_DONE = 0,          /* written and verified in both copies */
	NVC_ALREADY,           /* both copies already said so; nothing written */
	NVC_BAD_SLOT,
	NVC_NOT_RUNNING,
	NVC_NO_PAIR,
	NVC_NOT_ACTIVE,
	NVC_NO_ROOM,
	NVC_WRITE_PRIMARY,
	NVC_VERIFY_PRIMARY,
	NVC_WRITE_FALLBACK,
	NVC_VERIFY_FALLBACK,
	NVC_FINAL,
};

struct env_commit_report {
	int primary;           /* 1 or 2 once both copies were read, else 0 */
	int fallback;
	int wrote_primary;     /* 1 when that copy was rewritten (and verified) */
	int wrote_fallback;
};

/* `slot` is what the caller asks to commit, `running` the slot the kernel
 * was booted from (-1 when it cannot tell). Returns an env_commit_rc; only
 * NVC_DONE and NVC_ALREADY mean both copies now say sw_commit=<slot>. */
int env_commit(const struct env_io *io, int slot, int running,
	       struct env_commit_report *r);

/* One line, no newline, for the rc. */
const char *env_commit_msg(int rc);

#endif
