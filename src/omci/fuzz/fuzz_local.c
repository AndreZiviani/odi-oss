/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Fuzz target: what the stick itself can hand omcid.
 *
 * Besides the OLT, omcid reads the config stores and a handful of files
 * under /var/run and /proc, and serves two System V message queues: the
 * omcicli protocol on key 0x800 (respond/vqsrv.c) and its own omcli one
 * (respond/clisrv.c). All of that is written by root on the stick, so a
 * defect here is a robustness problem and not a way in; it is fuzzed for
 * the same reason the parsers have unit tests. The OLT is fuzz_olt.c.
 *
 * An input is a list of records: a kind, a length (two bytes, low first)
 * and that many bytes.
 *
 *   0  a file, there before omcid starts: byte 0 picks the path
 *      (stick_paths[] in stick.c), the rest is the contents
 *   1  an omcicli message: byte 0 says whether it asks for a reply (1) and
 *      whether the reply queue it names exists (2), the rest is the payload
 *      (240 bytes, zero filled)
 *   2  an omcli request: byte 0 says whether the reply queue exists (1) and
 *      whether the magic is right (2 spoils it), byte 1 is the argument
 *      count, the rest the argument bytes
 *   3  an OMCI frame injected on the omcicli queue, as `omcli --inject`
 *      does; a 48-byte one with device id 0x0a gets its CRC here
 *   4  byte 0 seconds pass
 *   5  SIGHUP
 *   6  the stick: three bytes as in fuzz_olt.c, then one more: 1 no
 *      netlink (as under qemu), 2 the reply queues are full
 *   7  omcid is killed and started again (the first RESPAWN_MAX times);
 *      four bytes or more patch the snapshot in between (snap_patch() in
 *      stick.c)
 *
 * The message headers are built here in host byte order with the fields a
 * real client sets, for the same reason fuzz_olt.c computes the CRC: the
 * fuzzer should spend its time on what is behind the check of a constant.
 */
#include <string.h>

#include "sys_host.h"
#include "harness.h"
#include "../omci_msgq.h"
#include "../omci_cli_proto.h"

#define EV_MAX      384                /* a whole session and the questions after it */
#define RESPAWN_MAX 3
#define FILES_MAX   16

union msg {
	struct omci_msg vq;
	struct omcli_req cli;
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static union msg msgs[EV_MAX];
	static struct fk_event ev[EV_MAX];
	struct stick_file files[FILES_MAX];
	struct stick s;
	unsigned n = 0, nfiles = 0, respawns = 0;
	size_t off = 0;

	memset(&s, 0, sizeof s);
	while (off + 3 <= size && n < EV_MAX) {
		uint8_t kind = data[off];
		size_t len = data[off + 1] | ((size_t)data[off + 2] << 8);
		const uint8_t *d = data + off + 3;
		union msg *m = &msgs[n];

		if (len > size - off - 3)
			len = size - off - 3;
		off += 3 + len;
		memset(&ev[n], 0, sizeof ev[n]);
		switch (kind) {
		case 0:
			if (len >= 1 && nfiles < FILES_MAX) {
				files[nfiles].path = d[0];
				files[nfiles].data = d + 1;
				files[nfiles].len = len - 1;
				nfiles++;
			}
			continue;
		case 1:
			if (len < 1)
				continue;
			memset(&m->vq, 0, sizeof m->vq);
			m->vq.mtype = OMCI_MQ_MTYPE;
			m->vq.msgType = OMCI_MQ_TYPE_CLI;
			m->vq.replyKey = !(d[0] & 1) ? 0 :
					 (d[0] & 2) ? STICK_REPLY_KEY : STICK_REPLY_KEY + 1;
			m->vq.len = OMCI_MQ_LEN;
			memcpy(m->vq.p, d + 1, len - 1 > OMCI_MQ_PAYLOAD ?
			       OMCI_MQ_PAYLOAD : len - 1);
			ev[n].kind = FK_VQ_MSG;
			ev[n].data = (const uint8_t *)&m->vq;
			ev[n].len = sizeof m->vq;
			break;
		case 2: {
			size_t alen = len < 2 ? 0 : len - 2;

			if (len < 2)
				continue;
			if (alen > OMCLI_ARGS)
				alen = OMCLI_ARGS;
			memset(&m->cli, 0, sizeof m->cli);
			m->cli.mtype = OMCLI_MTYPE;
			m->cli.magic = (d[0] & 2) ? OMCLI_MAGIC ^ 1 : OMCLI_MAGIC;
			m->cli.version = OMCLI_VERSION;
			/* The private queue of stick_run(), or an id nobody has. */
			m->cli.replyQid = (d[0] & 1) ? STICK_REPLY_QID : 77;
			m->cli.nargs = d[1];
			memcpy(m->cli.args, d + 2, alen);
			ev[n].kind = FK_CLI_MSG;
			ev[n].data = (const uint8_t *)&m->cli;
			/* As long as the client made it: a short request is
			 * the case cli_poll() clears the tail for. */
			ev[n].len = (uint32_t)(4 + OMCLI_REQ_LEN(alen));
			break;
		}
		case 3:
			memset(&m->vq, 0, sizeof m->vq);
			m->vq.mtype = OMCI_MQ_MTYPE;
			m->vq.msgType = OMCI_MQ_TYPE_FRAME;
			m->vq.len = (uint32_t)(len > OMCI_MQ_PAYLOAD ?
					       OMCI_MQ_PAYLOAD : len);
			memcpy(m->vq.p, d, m->vq.len);
			if (m->vq.len == 48 && m->vq.p[3] == 0x0a) {
				uint32_t c = fk_crc32(m->vq.p, 44);

				m->vq.p[44] = (uint8_t)(c >> 24);
				m->vq.p[45] = (uint8_t)(c >> 16);
				m->vq.p[46] = (uint8_t)(c >> 8);
				m->vq.p[47] = (uint8_t)c;
			}
			ev[n].kind = FK_VQ_MSG;
			ev[n].data = (const uint8_t *)&m->vq;
			ev[n].len = sizeof m->vq;
			break;
		case 4:
			ev[n].kind = FK_PAUSE;
			ev[n].arg = len ? d[0] : 1;
			break;
		case 5:
			ev[n].kind = FK_SIGNAL;
			ev[n].arg = 1;
			break;
		case 7:
			if (respawns++ >= RESPAWN_MAX)
				continue;
			ev[n].kind = FK_RESPAWN;
			ev[n].data = d;
			ev[n].len = (uint32_t)len;
			break;
		case 6:
			if (len >= 3)
				stick_from_bytes(d, &s);
			if (len >= 4) {
				s.os.no_netlink = d[3] & 1;
				s.os.reply_full = !!(d[3] & 2);
			}
			continue;
		default:
			continue;
		}
		n++;
	}
	s.client_queues = 1;
	stick_run(&s, files, nfiles, ev, n);
	return 0;
}
