/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* What the fuzz targets share: one run of omcid on the kernel model. */
#ifndef ODI_FUZZ_HARNESS_H
#define ODI_FUZZ_HARNESS_H

#include <stdint.h>
#include <stddef.h>

#include "fakeos.h"

/* state.c: put omcid's own data back to what a new process holds. */
void state_restore(void);

/* The stick omcid finds itself on. All zero is a provisioned stick in O5
 * with an empty odi.conf, started as `omcid -a -d`. */
struct stick {
	struct fk_opts os;
	int dry_run;             /* no -a: nothing is sent to the driver */
	int no_proc;             /* no /proc/odi_gpon, no sysfs: a bare host */
	int manual_vlan;         /* the store holds a manual VLAN (VID 11) */
	int pots;                /* OMCI_POTS_PORTS=1 */
	int unknown_me_ok;       /* OMCI_UNKNOWN_ME_OK=1 */
	int no_veip;             /* OMCI_VEIP=0 */
	int uni_lock;            /* OMCI_UNI_LOCK=1 */
	int accept_reboot;       /* OMCI_ACCEPT_REBOOT: 0 absent, else value + 1 */
	int respawn;             /* an omcid ran before this one: -r */
	int client_queues;       /* the two reply queues below exist */
};

/* The reply queues of the clients a harness plays: one found by key, as
 * omcicli makes it, and a private one found by id, as omcli does. */
#define STICK_REPLY_KEY 0x6868u
#define STICK_REPLY_QID 1

/* Files of the target a harness may give contents of its own; index into
 * this table. stick_run() writes its defaults first, so a file given here
 * replaces one. */
extern const char *const stick_paths[];
extern const unsigned stick_npaths;

struct stick_file {
	unsigned path;           /* index into stick_paths */
	const uint8_t *data;
	size_t len;
};

/* A stick from three bytes of fuzz input; all zero is the default one.
 * stick.c has the bits. */
void stick_from_bytes(const uint8_t b[3], struct stick *s);

/* One whole life of the daemon: state back to new, the files of `s` and
 * then `files` written, the script run to its end. Aborts when the run
 * broke something no sanitizer sees (a descriptor left open, a signal sent
 * that the configuration did not allow). */
/* Set by the replay: keep what omcid prints (fk_output()), so an input can
 * be held to what it is expected to print. */
extern int stick_capture;

void stick_run(const struct stick *s, const struct stick_file *files,
	       unsigned nfiles, const struct fk_event *ev, unsigned nev);

/* The entry of a target; libFuzzer and replay.c both call it. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif
