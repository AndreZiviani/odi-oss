/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* One run of omcid, start to stop, on the kernel model.
 *
 * This is the whole daemon: main() of respond/main.c (renamed omcid_main by
 * the Makefile, the one name that had to give way) with its start-up, its
 * main loop and its signal handler, not a call into one function of it. A
 * frame goes in where the kernel would hand it over and comes out where
 * omcid would hand the answer back, so the receive path, the retransmission
 * cache, the rebuild a second after the last frame and the once-a-second
 * work all run as they do on a stick.
 *
 * The files below are what omcid reads from /proc, sysfs and the config
 * partition. Every value is made up; the serial is the one the suites of
 * src/omci use.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

int omcid_main(int argc, char **argv);

const char *const stick_paths[] = {
	"/var/config/lastgood.xml",
	"/var/config/lastgood_hs.xml",
	"/var/config/odi.conf",
	"/proc/odi_gpon/status",
	"/proc/odi_gpon/counters",
	"/var/run/omcid-mib.snap",
	"/var/run/identity-trial.conf",
	"/var/config/odi-reregister",
	"/var/run/omcid-swimage",
	"/var/run/voip-line-status",
	"/var/run/odi-slot",
	"/var/config/omci-identity.on",
	"/var/run/omcid-resume-decision",
	"/sys/module/odi_switch/parameters/host_link",
	"/sys/class/net/eth0/address",
	"/proc/odi_omci",
};
const unsigned stick_npaths = sizeof stick_paths / sizeof stick_paths[0];

static const char gpon_status[] =
	"state 5 (O5)\n"
	"onu_id 26\n"
	"sn 4f44495400000001\n"
	"eqd multiframe 0 inframe 0\n"
	"ploam ds_rx 0 us_tx 0\n"
	"alloc_ids 5 282 794 1050 1306 538\n";

static const char gpon_counters[] =
	"ctr_samples 11\n"
	"ctr_ds_superframe 305491896\n"
	"ctr_fec_now ds 1\n"
	"ctr ds_bip_err_blocks 4\n"
	"ctr ds_bip_err_bits 9\n"
	"ctr ds_fec_corrected_bits 2560000\n"
	"ctr ds_fec_corrected_bytes 2500000\n"
	"ctr ds_fec_corrected_cw 2530000\n"
	"ctr ds_fec_uncorrectable_cw 2\n";

static const char store_vlan[] =
	"<Config Name=\"ROOT\">\n"
	"\t<Dir Name=\"MIB_TABLE\">\n"
	"\t\t<Value Name=\"VLAN_CFG_TYPE\" Value=\"1\"/>\n"
	"\t\t<Value Name=\"VLAN_MANU_MODE\" Value=\"1\"/>\n"
	"\t\t<Value Name=\"VLAN_MANU_TAG_VID\" Value=\"11\"/>\n"
	"\t\t<Value Name=\"VLAN_MANU_TAG_PRI\" Value=\"0\"/>\n"
	"\t</Dir>\n"
	"</Config>\n";

/* Byte 0: 1 dry run, 2 a phone port, 4 unknown entities answered ok, 8 a
 * manual VLAN in the store, 16 a driver that never answers, 32 no /proc,
 * and in the top two bits OMCI_ACCEPT_REBOOT (absent, 0, 1, 2). Byte 1: the
 * driver command that fails, counted from 1. Byte 2: 1 no VEIP, 2 the UNI
 * lock, 4 started as a restart (-r). */
void stick_from_bytes(const uint8_t b[3], struct stick *s)
{
	s->dry_run = b[0] & 1;
	s->pots = !!(b[0] & 2);
	s->unknown_me_ok = !!(b[0] & 4);
	s->manual_vlan = !!(b[0] & 8);
	s->os.drv_dead = !!(b[0] & 16);
	s->no_proc = !!(b[0] & 32);
	s->accept_reboot = b[0] >> 6;
	s->os.drv_fail_at = b[1];
	s->no_veip = b[2] & 1;
	s->uni_lock = !!(b[2] & 2);
	s->respawn = !!(b[2] & 4);
}

static void put(const char *path, const char *text)
{
	fk_file_put(path, text, strlen(text));
}

int stick_capture;

/* The CRC respond/snapshot.c closes its file with. */
static uint32_t snap_crc(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
	}
	return ~c;
}

/* A respawn may carry a patch for the snapshot the killed omcid left: three
 * bytes of offset, then the bytes to put there. The CRC at the end is made
 * right again afterwards, so the patch gets to the code that reads the
 * snapshot and not only to the check in front of it. Nothing on a stick
 * does this; it is how the local target asks what omcid makes of a
 * snapshot an omcid of another build wrote. */
static void snap_patch(const uint8_t *d, uint32_t len)
{
	size_t n, at;
	uint8_t *f = fk_file_data("/var/run/omcid-mib.snap", &n);

	if (!f || n < 8 || len < 4)
		return;
	at = (d[0] | ((size_t)d[1] << 8) | ((size_t)d[2] << 16)) % (n - 4);
	d += 3;
	len -= 3;
	if (len > n - 4 - at)
		len = (uint32_t)(n - 4 - at);
	memcpy(f + at, d, len);
	at = snap_crc(f, n - 4);
	f[n - 4] = (uint8_t)(at >> 24);
	f[n - 3] = (uint8_t)(at >> 16);
	f[n - 2] = (uint8_t)(at >> 8);
	f[n - 1] = (uint8_t)at;
}

/* What a run must not have done, sanitizer or not. */
static void stick_check(const struct stick *s, const struct fk_stats *st)
{
	/* omcid opens a file, reads or writes it and closes it, every time;
	 * one still open when the daemon stops was lost on some path, and a
	 * daemon that runs for months runs out of them. */
	if (st->files_open) {
		fprintf(stderr, "stick: %u file descriptor(s) left open\n",
			st->files_open);
		abort();
	}
	/* The one signal omcid ever sends is the Reboot the owner allowed:
	 * SIGTERM to init. The model never sends it; this checks that omcid
	 * asked for no other. */
	if (st->kills && (s->accept_reboot != 2 || st->kill_pid != 1 ||
			  st->kill_sig != 15)) {
		fprintf(stderr, "stick: kill(%ld, %d) with OMCI_ACCEPT_REBOOT %s\n",
			st->kill_pid, st->kill_sig,
			s->accept_reboot == 2 ? "1" : "not 1");
		abort();
	}
}

void stick_run(const struct stick *s, const struct stick_file *files,
	       unsigned nfiles, const struct fk_event *ev, unsigned nev)
{
	char *argv[5] = { "omcid", "-d", 0, 0, 0 };
	int argc = 2, restarted = 0;
	char conf[256];
	const struct fk_stats *st;
	struct fk_opts os = s->os;
	static int echo = -1;

	/* FUZZ_ECHO=1 in the environment: show the run (replay.c -v sets it). */
	if (echo < 0)
		echo = getenv("FUZZ_ECHO") != 0;
	os.echo = echo;
	os.capture = stick_capture;
	state_restore();
	fk_reset(&os);

	if (!s->no_proc) {
		put("/proc/odi_gpon/status", gpon_status);
		put("/proc/odi_gpon/counters", gpon_counters);
		put("/proc/odi_init", "");
		put("/sys/module/odi_switch/parameters/host_link", "1\n");
		put("/sys/class/net/eth0/address", "02:00:00:00:00:01\n");
	}
	if (s->manual_vlan)
		put("/var/config/lastgood.xml", store_vlan);
	snprintf(conf, sizeof conf, "%s%s%s%s",
		 s->pots ? "OMCI_POTS_PORTS=1\n" : "",
		 s->unknown_me_ok ? "OMCI_UNKNOWN_ME_OK=1\n" : "",
		 s->no_veip ? "OMCI_VEIP=0\n" : "",
		 s->uni_lock ? "OMCI_UNI_LOCK=1\n" : "");
	if (s->accept_reboot)
		snprintf(conf + strlen(conf), sizeof conf - strlen(conf),
			 "OMCI_ACCEPT_REBOOT=%d\n", s->accept_reboot - 1);
	put("/var/config/odi.conf", conf);
	for (unsigned i = 0; i < nfiles; i++)
		if (files[i].path < stick_npaths)
			fk_file_put(stick_paths[files[i].path], files[i].data,
				    files[i].len);

	if (s->client_queues) {
		fk_client_queue(STICK_REPLY_KEY);
		if (fk_client_queue(0) != STICK_REPLY_QID)
			abort();
	}

	if (!s->dry_run)
		argv[argc++] = "-a";
	if (s->respawn)
		argv[argc++] = "-r";

	fk_script(ev, nev);
	for (;;) {
		st = fk_run(omcid_main, argc, argv);
		if (echo)
			fprintf(stderr, "stick: %s %d; so far %u frame(s) sent, "
				"%u driver call(s), %u reply message(s), %u of %u "
				"event(s) not taken\n",
				st->respawn ? "killed, exit" : "exit",
				st->exit_status, st->frames_sent, st->drv_calls,
				st->replies, st->skipped, nev);
		stick_check(s, st);
		if (!st->respawn)
			break;
		/* What init and svc-omcid.sh do next: a new process, started
		 * with -r because one ran before, the decision file of the
		 * old one moved aside. */
		snap_patch(st->respawn_data, st->respawn_len);
		state_restore();
		fk_respawn();
		fk_file_move("/var/run/omcid-resume-decision",
			     "/var/run/omcid-resume-decision.prev");
		if (!s->respawn && !restarted) {
			argv[argc++] = "-r";
			restarted = 1;
		}
	}
}
