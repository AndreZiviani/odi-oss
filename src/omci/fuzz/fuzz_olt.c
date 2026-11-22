/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Fuzz target: what the OLT can send.
 *
 * The OMCI channel is the one input of omcid that comes from outside the
 * stick, so this is the target that matters most. An input is a session:
 * 48-byte records, each a baseline OMCI frame, delivered in order on the
 * line side of one freshly started omcid (stick.c). That is also the format
 * of the session fixtures in test/fixtures once their hex is turned to
 * bytes, which is where the seeds come from (Makefile, `seeds`).
 *
 * Byte 3 of a frame is the device identifier, 0x0a in every frame omcid
 * accepts. The target uses the other values of that byte to say what a
 * record is, so a session stays a plain run of 48-byte frames:
 *
 *   0x0a  a frame. The CRC in bytes 44-47 is computed here before the frame
 *         goes in: a fuzzer does not find a CRC-32 on its own, and without
 *         this nothing would get past rx_accept().
 *   0x1a  a frame sent with device identifier 0x0a and the CRC as it stands
 *         in the record, so the CRC check itself is reached too.
 *   0x00  no frame: the seconds in bytes 0-1 pass (at most a little over an
 *         hour, so the 15-minute intervals of the PM entities turn over).
 *   0xc0  as the first record only, the stick: bytes 0-2 choose the
 *         configuration omcid starts with (stick_from_bytes()).
 *   0x51  SIGHUP, the reload apply.sh asks for.
 *   0x52  omcid is killed and init starts it again, with the files the old
 *         one left: the snapshot, and what the session wrote under
 *         /var/run. The first RESPAWN_MAX of an input count.
 *   else  a frame as it stands; rx_accept() drops it for its device
 *         identifier.
 *
 * A last record shorter than 48 bytes is delivered at its length, which is
 * the short-frame case of rx_accept().
 *
 * Under libFuzzer the mutations work on whole records (the custom mutator
 * below): the stock ones insert and remove single bytes, which shifts every
 * later frame of a session out of place and turns it into noise.
 */
#include <string.h>

#include "harness.h"

#define REC        48
#define REC_MAX    320                 /* the longest fixture is 269 frames */
#define PAUSE_MAX  4000u               /* seconds */

#define DEV_FRAME   0x0a
#define DEV_RAWCRC  0x1a
#define DEV_PAUSE   0x00
#define DEV_STICK   0xc0
#define DEV_SIGHUP  0x51
#define DEV_RESPAWN 0x52
#define RESPAWN_MAX 3                  /* each is a start and a stop of omcid */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static uint8_t frames[REC_MAX][REC];
	static struct fk_event ev[REC_MAX];
	struct stick s;
	unsigned n = 0, respawns = 0;

	memset(&s, 0, sizeof s);
	for (size_t off = 0; off < size && n < REC_MAX; off += REC) {
		size_t len = size - off < REC ? size - off : REC;
		uint8_t *f = frames[n];
		uint8_t dev;

		memcpy(f, data + off, len);
		if (len < REC) {
			ev[n].kind = FK_LINE_FRAME;
			ev[n].data = f;
			ev[n].len = (uint32_t)len;
			n++;
			break;
		}
		dev = f[3];
		if (dev == DEV_STICK) {
			if (off == 0)
				stick_from_bytes(f, &s);
			continue;
		}
		if (dev == DEV_PAUSE) {
			uint32_t secs = ((uint32_t)f[0] << 8) | f[1];

			ev[n].kind = FK_PAUSE;
			ev[n].arg = secs > PAUSE_MAX ? PAUSE_MAX : secs;
		} else if (dev == DEV_SIGHUP) {
			ev[n].kind = FK_SIGNAL;
			ev[n].arg = 1;
		} else if (dev == DEV_RESPAWN) {
			if (respawns++ >= RESPAWN_MAX)
				continue;
			ev[n].kind = FK_RESPAWN;
		} else {
			if (dev == DEV_RAWCRC)
				f[3] = DEV_FRAME;
			else if (dev == DEV_FRAME)
				put32(f + 44, fk_crc32(f, 44));
			ev[n].kind = FK_LINE_FRAME;
			ev[n].data = f;
			ev[n].len = REC;
		}
		n++;
	}
	stick_run(&s, 0, 0, ev, n);
	return 0;
}

#ifdef FUZZ_LIBFUZZER
size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

/* A small generator of its own, so a mutation is a function of the seed
 * libFuzzer hands over and nothing else. */
static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

/* The message types omcid handles, and a few it does not. */
static const uint8_t msg_types[] = {
	4, 6, 8, 9, 11, 12, 13, 14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 28,
	0, 1, 16, 17, 27, 29, 31,
};

/* Class ids worth trying when a frame gets a new one: those omcid names
 * (../omci.h) and has handlers for. The others are found by mutation. */
static const uint16_t classes[] = {
	2, 5, 6, 7, 11, 24, 45, 46, 47, 48, 49, 51, 52, 53, 58, 84, 89, 130, 131,
	133, 134, 136, 137, 138, 139, 141, 142, 143, 148, 150, 153, 157, 158,
	171, 256, 257, 262, 263, 264, 266, 267, 268, 272, 273, 276, 277, 278,
	280, 281, 287, 290, 296, 298, 309, 310, 311, 312, 321, 322, 329, 340,
	341, 65529, 65530, 65535,
};

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
			       unsigned int seed)
{
	uint32_t s = seed;
	size_t n = size / REC, i, j;
	uint8_t tmp[REC];

	/* A ragged tail stays as it is in the corpus; a mutated input is whole
	 * records. */
	size = n * REC;
	if (n == 0) {
		if (max_size < REC)
			return 0;
		memset(data, 0, REC);
		data[2] = 0x49;                  /* Get, AR */
		data[3] = DEV_FRAME;
		data[4] = 1;                     /* ONU-G */
		data[8] = 0x80;
		return REC;
	}
	i = rnd(&s) % n;
	switch (rnd(&s) % 16) {
	case 0:                                  /* repeat a record */
		if (size + REC > max_size)
			break;
		j = rnd(&s) % (n + 1);
		memcpy(tmp, data + i * REC, REC);
		memmove(data + (j + 1) * REC, data + j * REC, (n - j) * REC);
		memcpy(data + j * REC, tmp, REC);
		return size + REC;
	case 1:                                  /* drop one */
		if (n < 2)
			break;
		memmove(data + i * REC, data + (i + 1) * REC, (n - i - 1) * REC);
		return size - REC;
	case 2:                                  /* swap two */
		j = rnd(&s) % n;
		memcpy(tmp, data + i * REC, REC);
		memcpy(data + i * REC, data + j * REC, REC);
		memcpy(data + j * REC, tmp, REC);
		return size;
	case 3:                                  /* another class */
		j = rnd(&s) % (sizeof classes / sizeof classes[0]);
		data[i * REC + 4] = (uint8_t)(classes[j] >> 8);
		data[i * REC + 5] = (uint8_t)classes[j];
		return size;
	case 4:                                  /* another message type */
		j = rnd(&s) % sizeof msg_types;
		data[i * REC + 2] = (uint8_t)(msg_types[j] |
					      (rnd(&s) % 8 ? 0x40 : rnd(&s) & 0xe0));
		return size;
	case 5:                                  /* the entity of another record */
		j = rnd(&s) % n;
		memcpy(data + i * REC + 4, data + j * REC + 4, 4);
		return size;
	case 6:                                  /* a pause, or a reload */
		if (size + REC > max_size)
			break;
		memmove(data + (i + 1) * REC, data + i * REC, (n - i) * REC);
		memset(data + i * REC, 0, REC);
		if (rnd(&s) % 4)
			data[i * REC + 1] = (uint8_t)(1 + rnd(&s) % 8);
		else if (rnd(&s) % 2)
			data[i * REC] = 4;       /* 1024 s: past a PM interval */
		else
			data[i * REC + 3] = rnd(&s) % 3 ? DEV_SIGHUP : DEV_RESPAWN;
		return size + REC;
	case 7:                                  /* another stick */
		if (data[3] != DEV_STICK) {
			if (size + REC > max_size)
				break;
			memmove(data + REC, data, size);
			memset(data, 0, REC);
			data[3] = DEV_STICK;
			size += REC;
		}
		data[rnd(&s) % 3] ^= (uint8_t)(1u << (rnd(&s) % 8));
		return size;
	default:
		break;
	}
	/* The usual case: the stock mutations on the 44 bytes of one record
	 * the CRC covers, at its length. Most of the time the record stays a
	 * frame omcid will read. */
	memcpy(tmp, data + i * REC, REC);
	j = LLVMFuzzerMutate(tmp, 44, 44);
	if (j < 44)
		memset(tmp + j, 0, 44 - j);
	if (data[i * REC + 3] == DEV_FRAME && rnd(&s) % 16)
		tmp[3] = DEV_FRAME;
	memcpy(data + i * REC, tmp, REC);
	return size;
}

/* Two sessions joined at a record boundary: the start of one, the rest of
 * the other. */
size_t LLVMFuzzerCustomCrossOver(const uint8_t *a, size_t an, const uint8_t *b,
				 size_t bn, uint8_t *out, size_t max_out,
				 unsigned int seed)
{
	uint32_t s = seed;
	size_t na = an / REC, nb = bn / REC, ka, kb, n;

	ka = na ? rnd(&s) % (na + 1) : 0;
	kb = nb ? rnd(&s) % (nb + 1) : 0;
	if (ka * REC > max_out)
		ka = max_out / REC;
	n = nb - kb;
	if ((ka + n) * REC > max_out)
		n = max_out / REC - ka;
	memcpy(out, a, ka * REC);
	memcpy(out + ka * REC, b + kb * REC, n * REC);
	return (ka + n) * REC;
}
#endif
