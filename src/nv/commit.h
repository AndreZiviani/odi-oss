/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

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
 * The same writes also set mupgrade_en=0. The stock bootloader ships it as 1,
 * which makes U-Boot listen on the host link after the boot countdown for a
 * firmware image sent by multicast and flash it, unauthenticated; anyone on
 * the LAN side during a reboot could replace the image. Our kernel resets
 * the chip through the watchdog and quiesces the NIC itself, so nothing
 * relies on that listener. It rides in the copies' existing sw_commit
 * writes: a copy that lacks the variable gets it appended, one that already
 * says 0 is left as it is, and no extra erase cycle is spent. A copy needs
 * a write when any of them is off, so a stick committed before this
 * existed is fixed by running `nv commit` again.
 *
 * sw_tryactive=2 rides along the same way. The copy U-Boot saved as it
 * started the trial says 2, but the other one still holds the value that
 * armed the trial; if U-Boot ever had to fall back to that copy, bootcmd
 * would start a one-shot trial nobody asked for. The fallback's value is
 * never a refusal, only the primary's (below). The crash argument above
 * is unchanged: each copy is still replaced whole, and a half-applied
 * state is only ever "old, valid copy" or "new, verified copy".
 *
 * What it refuses, before anything is written:
 *
 *   - a slot other than 0 or 1;
 *   - a slot that is not the one running (the caller reads /proc/cmdline);
 *   - a slot the primary copy does not name as sw_active -- U-Boot writes
 *     sw_active as it boots a slot, so this is its own record of what it
 *     started. sw_active is NEVER written here;
 *   - a primary copy holding a sw_tryactive other than 2 (NVC_TRIAL_PENDING):
 *     U-Boot set it to 2 in the copy it saved before starting this slot, so
 *     another value there is a trial someone queued since, and committing
 *     would drop it silently;
 *   - a copy that could not be read completely (NVC_READ): it may be
 *     perfectly good, so it is not repaired over, and nothing is written;
 *   - both copies failing their CRC: there is nothing to commit from.
 *
 * One copy that was read in full and fails its CRC is not a refusal. The
 * valid copy is the primary and the invalid one is REPAIRED first: it is written with the whole new
 * environment (sw_commit, mupgrade_en and sw_tryactive applied), read back and compared,
 * and only then is the primary written. The only good copy is never erased
 * while it is the only one, so a second power cut during the commit still
 * leaves a valid copy. The repaired copy gets the flags byte that keeps the
 * primary winning (env_repair_flags, env.h); the one case where it cannot
 * (env2 holding flags 0) makes the repaired copy, which holds the complete
 * verified new environment, the winner, and the commit still ends with both
 * copies agreeing.
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
	 * bytes. Returns the partition size read, 0 when it could not be read
	 * completely (open, a failed or short read, a partition that is not
	 * there). 0 is NOT "invalid": a copy read in full that fails its CRC
	 * is the invalid one the sequences below repair; one not read in full
	 * says nothing about its content, and nothing is written on it. */
	uint32_t (*read)(void *ctx, int copy, uint8_t *buf);
	/* Erase copy and write len bytes of buf over it. 1 on success. */
	int (*write)(void *ctx, int copy, const uint8_t *buf, uint32_t len);
};

enum env_commit_rc {
	NVC_DONE = 0,          /* written and verified in both copies */
	NVC_ALREADY,           /* both copies already said so (sw_commit, mupgrade_en=0); nothing written */
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
	NVC_CONFIRM_ARP,       /* the confirm-arp development file exists and no force was given; nothing written */
	NVC_WRITE_REPAIR,      /* the invalid copy could not be written; the valid one was not touched */
	NVC_VERIFY_REPAIR,     /* the invalid copy did not read back as written; the valid one was not touched */
	NVC_READ,              /* a copy could not be read completely (not the same as failing its CRC); nothing written */
	NVC_TRIAL_PENDING,     /* the primary copy holds a sw_tryactive other than 2; nothing written */
};

struct env_commit_report {
	int primary;           /* 1 or 2 once both copies were read, else 0 */
	int fallback;
	int wrote_primary;     /* 1 when that copy was rewritten (and verified) */
	int wrote_fallback;
	int repaired;          /* 1 or 2: the copy that was invalid and was written first, else 0 */
};

/* `slot` is what the caller asks to commit, `running` the slot the kernel
 * was booted from (-1 when it cannot tell). Returns an env_commit_rc; only
 * NVC_DONE and NVC_ALREADY mean both copies now say sw_commit=<slot>. */
int env_commit(const struct env_io *io, int slot, int running,
	       struct env_commit_report *r);

/* `nv setenv`, behind the same io. `copy` is 0 for the copy U-Boot reads, or
 * 1|2 for a named one. The sequence, which is what keeps one good copy on
 * flash at every instant:
 *
 *   - both copies valid: the target alone is rewritten and read back (the
 *     other copy is the fallback if that write is cut);
 *   - one copy invalid: the new block is built and checked in memory, then
 *     written to the INVALID copy and read back, and only then is the valid
 *     copy rewritten and read back. With -c naming the invalid copy only that
 *     one is written. The valid copy is never erased while it is the only
 *     one;
 *   - neither valid: nothing is written;
 *   - a copy that could not be read completely (NVS_READ): not taken for an
 *     invalid one, nothing is written, whichever copy -c names.
 *
 * Nothing is erased before the new block has fit, carried the value asked
 * for and had its CRC set. */
enum env_set_rc {
	NVS_DONE = 0,
	NVS_NONE_VALID,
	NVS_NO_ROOM,
	NVS_NOT_SET,
	NVS_WRITE_REPAIR,
	NVS_VERIFY_REPAIR,
	NVS_WRITE,
	NVS_VERIFY,
	NVS_READ,              /* a copy could not be read completely; nothing written */
};

struct env_set_report {
	int target;            /* the copy the change was meant for: 1 or 2 */
	int repaired;          /* 1 or 2 when an invalid copy was written first, else 0 */
	int wrote_target;      /* 1 when the target was rewritten and verified */
};

int env_setenv(const struct env_io *io, int copy, const char *key,
	       const char *value, struct env_set_report *r);
const char *env_setenv_msg(int rc);

/* The development toggles a commit looks at, as files on the config
 * partition. The paths are fields, so a test points them at fixtures. */
#define NV_CONFIRM_ARP_PATH "/etc/config/confirm-arp"
#define NV_BREADCRUMBS_PATH "/etc/config/breadcrumbs.on"

struct commit_aids {
	void *ctx;
	/* 1 when the file at path exists. */
	int (*exists)(void *ctx, const char *path);
	const char *confirm_arp;
	const char *breadcrumbs;
	/* Out: which of the two exist. */
	int has_confirm_arp;
	int has_breadcrumbs;
};

/* env_commit behind the confirm-arp guard. While the confirm-arp file
 * exists every boot waits for an ARP reply from the address in it, and a
 * committed slot has no trial to fall back to, so an unreachable host
 * means a reset every 120 s until safe mode: without `force` that is
 * NVC_CONFIRM_ARP and nothing is read or written. breadcrumbs.on never
 * blocks; the caller only reports it. Both flags are set in `a` first. */
int env_commit_guarded(const struct env_io *io, struct commit_aids *a,
		       int force, int slot, int running,
		       struct env_commit_report *r);

/* One line, no newline, for the rc. */
const char *env_commit_msg(int rc);

/* The N of the root=31:N the kernel mounted, from the text of
 * /proc/cmdline; -1 when there is none. The kernel takes the LAST root=, and
 * the command line holds two: CONFIG_CMDLINE (slot 0, root=31:5) comes first
 * and U-Boot's per-slot arguments after it (MIPS_CMDLINE_BUILTIN_EXTEND), so
 * on a slot-1 boot the first one names the wrong slot. A last root= that is
 * not 31:N is -1 too: that is what the kernel mounted. */
int cmdline_root_index(const char *cmd);

#endif
