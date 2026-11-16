/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* The environment keys `nv setenv` writes only with --force.
 *
 * U-Boot reads them on every boot, and a wrong value costs more than a
 * failed command: a sw_tryactive other than 0, 1 or 2 trial-boots slot 1,
 * since the boot script tests for 2 and then for 0 only; sw_commit written by hand skips the
 * checks of `nv commit` and reaches one copy only; a broken bootcmd,
 * boot_by_* or b0/b1 stops the stick in U-Boot; mupgrade_en=1 turns the
 * unauthenticated multicast upgrade back on (docs/FLASHING.md).
 *
 * A valid sw_tryactive (0, 1, 2) and the sw_version keys are not guarded:
 * the web UI and fwu.sh write them.
 *
 * No syscalls here, so it is tested on the host (test/test_guard.c).
 */
#ifndef NV_GUARD_H
#define NV_GUARD_H

/* 0 when `nv setenv key value` may go ahead without --force, else one line
 * that says why not and, where there is one, what to run instead. A null
 * value is an unset. */
const char *nv_guard(const char *key, const char *value);

/* The arguments of `nv setenv`, after the verb:
 *
 *     [--force|-f] [-c 1|2] [--] key [value]
 *
 * the two options in either order; -- ends them, for a key that starts
 * with '-'. A key with '=' in it is refused: the environment matches a
 * name up to its own end, so `bootcmd=x` would reach the bootcmd entry
 * past the guard. */
struct nv_setenv_args {
	int force;
	int copy;          /* 0, or the copy -c named */
	const char *key;
	const char *value; /* 0 to unset */
};

#define NV_ARGS_OK     0
#define NV_ARGS_USAGE  1  /* no key, or an unknown option */
#define NV_ARGS_COPY   2  /* -c without 1 or 2 */
#define NV_ARGS_NAME   3  /* an empty name, or one with '=' in it */

/* argv[0] is the first word after "setenv". */
int nv_setenv_parse(int argc, char **argv, struct nv_setenv_args *out);

#endif
