/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Parsers for the two /proc files our 6.18 drivers publish, shared by the
 * OMCI tools. Pure functions over a buffer the caller already read: no
 * syscalls, no libc, so test/procparse_test.c builds them natively.
 *
 *   /proc/odi_omci   first line "registered: type=1 pid=593 type=..." --
 *                    who holds each packet-redirect type
 *                    (kernel/extra/drivers/net/ethernet/odi/odi_omci.c,
 *                    odi_omci_proc_show).
 *   /proc/odi_gpon/status
 *                    a "sn <16 hex digits>" line -- the serial number the
 *                    GPON block ranges with -- and "alloc_ids <n> <id>...",
 *                    the Alloc-IDs the OLT assigned by PLOAM (odi_gpon.c,
 *                    odi_gpon_proc_status_show), and "enc_ports <n> <id>...",
 *                    the GEM ports the downstream path decrypts.
 *   /proc/odi_gpon/cam
 *                    "alloc_cam <row> hw <id|-|timeout> ..." and
 *                    "gem_cam <row> hw <port|-|timeout> ...", the Alloc-ID
 *                    and downstream GEM CAMs read from the chip
 *                    (odi_gpon_proc_cam_show).
 *   /proc/odi_gpon/ponq
 *                    "tcont", "queue" and "flow" lines, the upstream PON
 *                    queue block read from the chip
 *                    (odi_switch_cmd.c, odi_switch_ponq_show).
 */
#ifndef ODI_OMCI_PROCPARSE_H
#define ODI_OMCI_PROCPARSE_H

#include <stdint.h>

/* Start of the line holding `key` followed by a space or colon, or -1. Only
 * line starts count, so "sn" never matches inside another word. */
static inline int pp_find_line(const char *buf, unsigned len, const char *key)
{
	unsigned i = 0;

	while (i < len) {
		unsigned k = 0;

		while (key[k] && i + k < len && buf[i + k] == key[k])
			k++;
		if (!key[k] && i + k < len && (buf[i + k] == ' ' || buf[i + k] == ':'))
			return (int)i;
		while (i < len && buf[i] != '\n')
			i++;
		i++;
	}
	return -1;
}

static inline int pp_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Who holds redirect `type`, from /proc/odi_omci.
 *
 * Returns 1 and sets *pid when the "registered:" line names that type, 0 when
 * the line is there but the type is not on it (nobody holds it), and -1 when
 * there is no such line at all -- not our driver, or a truncated read -- which
 * callers must treat as "cannot tell", never as "free". */
static inline int pp_redirect_holder(const char *buf, unsigned len, unsigned type,
				     uint32_t *pid)
{
	int at = pp_find_line(buf, len, "registered");
	unsigned i;

	if (at < 0)
		return -1;
	i = (unsigned)at + 11;          /* past "registered:" */
	while (i < len && buf[i] != '\n') {
		uint32_t t = 0, p = 0;
		int nt = 0, np = 0;

		while (i < len && buf[i] == ' ')
			i++;
		if (i + 5 > len || buf[i] != 't' || buf[i + 1] != 'y' ||
		    buf[i + 2] != 'p' || buf[i + 3] != 'e' || buf[i + 4] != '=') {
			while (i < len && buf[i] != ' ' && buf[i] != '\n')
				i++;
			continue;
		}
		for (i += 5; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, nt++)
			t = t * 10 + (uint32_t)(buf[i] - '0');
		if (i + 5 > len || buf[i] != ' ' || buf[i + 1] != 'p' ||
		    buf[i + 2] != 'i' || buf[i + 3] != 'd' || buf[i + 4] != '=')
			continue;
		for (i += 5; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, np++)
			p = p * 10 + (uint32_t)(buf[i] - '0');
		if (nt && np && t == type) {
			*pid = p;
			return 1;
		}
	}
	return 0;
}

/* The eight serial-number bytes from the "sn" line of /proc/odi_gpon/status:
 * four ASCII vendor bytes and four binary ones, printed as sixteen hex digits. Returns 0
 * and fills sn[] on a well-formed line; -1 otherwise. An all-zero serial is
 * returned as read -- deciding that it means "not set yet" is the caller's
 * business (pp_sn_is_zero). */
static inline int pp_gpon_sn(const char *buf, unsigned len, uint8_t sn[8])
{
	int at = pp_find_line(buf, len, "sn");
	unsigned i;
	uint8_t tmp[8];

	if (at < 0)
		return -1;
	i = (unsigned)at + 3;
	if (i + 16 > len)
		return -1;
	for (unsigned k = 0; k < 8; k++) {
		int hi = pp_hexval(buf[i + 2 * k]), lo = pp_hexval(buf[i + 2 * k + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		tmp[k] = (uint8_t)((hi << 4) | lo);
	}
	if (i + 16 < len && buf[i + 16] != '\n')
		return -1;
	for (unsigned k = 0; k < 8; k++)
		sn[k] = tmp[k];
	return 0;
}

/* The ONU state number from the first line of /proc/odi_gpon/status,
 * "state 5 (O5)":
 * 1 to 7 for O1 to O7, or -1 when there is no such line or the number is
 * not a digit. "cannot tell" is -1, never a state. */
static inline int pp_gpon_state(const char *buf, unsigned len)
{
	int at = pp_find_line(buf, len, "state");
	unsigned i;

	if (at < 0)
		return -1;
	i = (unsigned)at + 6;
	if (i >= len || buf[i] < '0' || buf[i] > '9')
		return -1;
	if (i + 1 < len && buf[i + 1] >= '0' && buf[i + 1] <= '9')
		return -1;
	return buf[i] - '0';
}

/* The shared reader of an "<key> <n> <id>..." line, ids below 4096. */
static inline int pp_id_list(const char *buf, unsigned len, const char *key,
			     unsigned keylen, uint16_t *out, unsigned max)
{
	int at = pp_find_line(buf, len, key);
	unsigned i, count = 0, got = 0;
	int digits = 0;

	if (at < 0)
		return -1;
	i = (unsigned)at + keylen;              /* past the key */
	if (i >= len || buf[i] != ' ')
		return -1;
	for (i++; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, digits++)
		if ((count = count * 10 + (unsigned)(buf[i] - '0')) > 4096)
			return -1;
	if (!digits)
		return -1;
	for (unsigned k = 0; k < count; k++) {
		uint32_t v = 0;

		if (i >= len || buf[i] != ' ')
			return -1;
		digits = 0;
		for (i++; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, digits++)
			if ((v = v * 10 + (uint32_t)(buf[i] - '0')) > 4095)
				return -1;
		if (!digits)
			return -1;
		if (got < max)
			out[got++] = (uint16_t)v;
	}
	if (i < len && buf[i] != '\n')
		return -1;
	return (int)got;
}

/* The Alloc-IDs from the "alloc_ids" line of /proc/odi_gpon/status: a count, then
 * that many decimal ids, in CAM row order (the order the OLT assigned
 * them, a released row reused first). Returns the number stored in out[]
 * (at most max), or -1 when there is no such line or it is malformed:
 * short of its count, an id past 4095 (a 12-bit G.984.3 Alloc-ID), or
 * anything else on the line. "cannot tell" and "none assigned" differ,
 * and a caller treats -1 as the former. */
static inline int pp_gpon_alloc_ids(const char *buf, unsigned len, uint16_t *out,
				    unsigned max)
{
	return pp_id_list(buf, len, "alloc_ids", 9, out, max);
}

/* The GEM Port-IDs the downstream path decrypts, from the "enc_ports" line of
 * /proc/odi_gpon/status: the same shape as alloc_ids (a count, then each id),
 * each id below 4096. -1 when there is no such line (a kernel that does not
 * publish it) or it is malformed, which a caller reads as "none". */
static inline int pp_gpon_enc_ports(const char *buf, unsigned len, uint16_t *out,
				    unsigned max)
{
	return pp_id_list(buf, len, "enc_ports", 9, out, max);
}

/* The config store keeps GPON_SN as it is printed on the label: four vendor
 * characters then eight hex digits ("ABCD0011AAFF"). Same result shape as
 * pp_gpon_sn. */
static inline int pp_label_sn(const char *s, unsigned len, uint8_t sn[8])
{
	uint8_t tmp[8];

	if (len != 12)
		return -1;
	for (unsigned k = 0; k < 4; k++) {
		if (s[k] < 0x20 || s[k] > 0x7e)
			return -1;
		tmp[k] = (uint8_t)s[k];
	}
	for (unsigned k = 0; k < 4; k++) {
		int hi = pp_hexval(s[4 + 2 * k]), lo = pp_hexval(s[5 + 2 * k]);

		if (hi < 0 || lo < 0)
			return -1;
		tmp[4 + k] = (uint8_t)((hi << 4) | lo);
	}
	for (unsigned k = 0; k < 8; k++)
		sn[k] = tmp[k];
	return 0;
}

static inline int pp_sn_is_zero(const uint8_t sn[8])
{
	for (unsigned k = 0; k < 8; k++)
		if (sn[k])
			return 0;
	return 1;
}

/* One total from /proc/odi_gpon/counters ("ctr <name> <decimal>", the
 * driver's 64-bit running totals). Returns 0 and sets *out, or -1 when the
 * name has no line or the value is not a decimal that fits 64 bits: a
 * caller must treat -1 as "cannot tell", never as 0. */
static inline int pp_ctr(const char *buf, unsigned len, const char *name, uint64_t *out)
{
	char key[48] = "ctr ";
	unsigned k = 4, i;
	uint64_t v = 0;
	int at;

	for (unsigned j = 0; name[j]; j++) {
		if (k + 1 >= sizeof key)
			return -1;
		key[k++] = name[j];
	}
	key[k] = 0;
	at = pp_find_line(buf, len, key);
	if (at < 0)
		return -1;
	i = (unsigned)at + k + 1;
	if (i >= len || buf[i] < '0' || buf[i] > '9')
		return -1;
	for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
		unsigned d = (unsigned)(buf[i] - '0');

		/* 2^64 / 10 spelled out: a 64-bit division would call libgcc,
		 * which the freestanding omcid does not link. */
		if (v > 1844674407370955161ULL || (v == 1844674407370955161ULL && d > 5))
			return -1;
		v = v * 10 + d;
	}
	if (i < len && buf[i] != '\n')
		return -1;
	*out = v;
	return 0;
}

/* The value after the word `key` in one line (len bytes, no newline):
 * decimal, or hex after 0x. Returns 1 and sets *v, 0 when the word is not
 * there or no number follows it. A word counts only as a whole word, so
 * "queue" never matches inside "queues". */
static inline int pp_word_u32(const char *l, unsigned len, const char *key, uint32_t *v)
{
	unsigned i = 0;

	while (i < len) {
		unsigned k = 0, n = 0;
		uint32_t x = 0;

		while (key[k] && i + k < len && l[i + k] == key[k])
			k++;
		if (!key[k] && (i == 0 || l[i - 1] == ' ') && i + k < len && l[i + k] == ' ') {
			unsigned j = i + k + 1;

			if (j + 1 < len && l[j] == '0' && l[j + 1] == 'x') {
				for (j += 2; j < len && pp_hexval(l[j]) >= 0; j++, n++)
					x = x * 16 + (uint32_t)pp_hexval(l[j]);
			} else {
				for (; j < len && l[j] >= '0' && l[j] <= '9'; j++, n++)
					x = x * 10 + (uint32_t)(l[j] - '0');
			}
			if (n && (j == len || l[j] == ' ' || l[j] == '+')) {
				*v = x;
				return 1;
			}
			return 0;
		}
		while (i < len && l[i] != ' ')
			i++;
		i++;
	}
	return 0;
}

/* Whether line l (len bytes) starts with the word `key`. */
static inline int pp_starts(const char *l, unsigned len, const char *key)
{
	unsigned k = 0;

	while (key[k] && k < len && l[k] == key[k])
		k++;
	return !key[k] && k < len && l[k] == ' ';
}

#define PP_PONQ_TCONTS 17u
#define PP_PONQ_QUEUES 65u
#define PP_PONQ_FLOWS  65u

/* /proc/odi_gpon/ponq, row by row. `seen` is 1 for a row the file lists;
 * a T-CONT's `block` is 1 when the driver recorded a queue block for it
 * (first logical queue qbase, qn queues). */
struct pp_ponq {
	struct { uint8_t seen, en, block, qbase, qn; uint32_t qmap; } t[PP_PONQ_TCONTS];
	struct { uint8_t seen, wfq; uint16_t weight; uint32_t cir, pir; } q[PP_PONQ_QUEUES];
	struct { uint8_t seen, valid, rec, queue; uint16_t gem; } f[PP_PONQ_FLOWS];
};

/* Fills *p from the file. Returns the number of rows read, or -1 when the
 * buffer has none at all (not our kernel, or an empty read): "cannot
 * tell", never "nothing programmed". A malformed line is skipped. */
static inline int pp_ponq(const char *buf, unsigned len, struct pp_ponq *p)
{
	unsigned at = 0;
	int rows = 0;
	uint8_t *z = (uint8_t *)p;

	for (unsigned i = 0; i < sizeof *p; i++)
		z[i] = 0;
	while (at < len) {
		const char *l = buf + at;
		unsigned n = 0;
		uint32_t id, a, b, c, d;

		while (at + n < len && l[n] != '\n')
			n++;
		at += n + 1;
		if (pp_starts(l, n, "tcont") && pp_word_u32(l, n, "tcont", &id) &&
		    id < PP_PONQ_TCONTS && pp_word_u32(l, n, "en", &a) &&
		    pp_word_u32(l, n, "qmap", &b)) {
			p->t[id].seen = 1;
			p->t[id].en = (uint8_t)a;
			p->t[id].qmap = b;
			if (pp_word_u32(l, n, "block", &c)) {
				unsigned j = 0;

				while (j < n && l[j] != '+')
					j++;
				d = 0;
				for (j++; j < n && l[j] >= '0' && l[j] <= '9'; j++)
					d = d * 10 + (uint32_t)(l[j] - '0');
				p->t[id].block = 1;
				p->t[id].qbase = (uint8_t)c;
				p->t[id].qn = (uint8_t)d;
			}
			rows++;
		} else if (pp_starts(l, n, "queue") && pp_word_u32(l, n, "queue", &id) &&
			   id < PP_PONQ_QUEUES && pp_word_u32(l, n, "wfq", &a) &&
			   pp_word_u32(l, n, "weight", &b) && pp_word_u32(l, n, "cir", &c) &&
			   pp_word_u32(l, n, "pir", &d)) {
			p->q[id].seen = 1;
			p->q[id].wfq = (uint8_t)a;
			p->q[id].weight = (uint16_t)b;
			p->q[id].cir = c;
			p->q[id].pir = d;
			rows++;
		} else if (pp_starts(l, n, "flow") && pp_word_u32(l, n, "flow", &id) &&
			   id < PP_PONQ_FLOWS && pp_word_u32(l, n, "valid", &a) &&
			   pp_word_u32(l, n, "gem", &b) && pp_word_u32(l, n, "queue", &c) &&
			   pp_word_u32(l, n, "recorded", &d)) {
			p->f[id].seen = 1;
			p->f[id].valid = (uint8_t)a;
			p->f[id].gem = (uint16_t)b;
			p->f[id].queue = (uint8_t)c;
			p->f[id].rec = (uint8_t)d;
			rows++;
		}
	}
	return rows ? rows : -1;
}

#define PP_CAM_ALLOC 32u
#define PP_CAM_GEM   65u
#define PP_CAM_NONE    (-1)      /* "-": the row is not valid */
#define PP_CAM_TIMEOUT (-2)      /* "timeout": the read did not complete */
#define PP_CAM_UNLISTED (-3)     /* no line for the row */

/* /proc/odi_gpon/cam: what the hardware holds per row, or one of the
 * PP_CAM_* values. Returns the number of rows read, or -1 for none. */
struct pp_cam {
	int32_t alloc[PP_CAM_ALLOC];
	int32_t gem[PP_CAM_GEM];
};

static inline int pp_cam(const char *buf, unsigned len, struct pp_cam *c)
{
	unsigned at = 0;
	int rows = 0;

	for (unsigned i = 0; i < PP_CAM_ALLOC; i++)
		c->alloc[i] = PP_CAM_UNLISTED;
	for (unsigned i = 0; i < PP_CAM_GEM; i++)
		c->gem[i] = PP_CAM_UNLISTED;
	while (at < len) {
		const char *l = buf + at;
		unsigned n = 0, j;
		uint32_t row, v;
		int32_t *slot = 0;

		while (at + n < len && l[n] != '\n')
			n++;
		at += n + 1;
		if (pp_starts(l, n, "alloc_cam") && pp_word_u32(l, n, "alloc_cam", &row) &&
		    row < PP_CAM_ALLOC)
			slot = &c->alloc[row];
		else if (pp_starts(l, n, "gem_cam") && pp_word_u32(l, n, "gem_cam", &row) &&
			 row < PP_CAM_GEM)
			slot = &c->gem[row];
		if (!slot)
			continue;
		if (pp_word_u32(l, n, "hw", &v)) {
			*slot = (int32_t)v;
		} else {
			for (j = 0; j + 3 < n && !(l[j] == ' ' && l[j + 1] == 'h' &&
						    l[j + 2] == 'w' && l[j + 3] == ' '); j++)
				;
			if (j + 3 >= n)
				continue;
			*slot = j + 4 < n && l[j + 4] == 't' ? PP_CAM_TIMEOUT : PP_CAM_NONE;
		}
		rows++;
	}
	return rows ? rows : -1;
}

#endif
