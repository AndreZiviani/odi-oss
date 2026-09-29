/* Software download from the OLT: Start software download, Download section,
 * End software download, Activate image and Commit image (G.988 message types
 * 19 to 23, on the software image entity, class 7).
 *
 * This image never runs an ISP firmware, so there is nothing to install. What
 * the OLT needs is a conforming exchange: an ONU that refuses a download is,
 * to some OLTs, an ONU to retry forever or to flag. So by default the whole
 * exchange is ACCEPTED and the image DISCARDED:
 *
 *   - every section is acknowledged the G.988 way (only the last section of a
 *     window asks for an answer; a window with a missing section is answered
 *     with a processing error, so the OLT sends it again), counted, run
 *     through the CRC and dropped -- nothing is kept past its window;
 *   - End software download is answered from the CRC-32 the OLT sends, over
 *     exactly the image size it announced (ITU-T I.363.5, the same CRC as the
 *     OMCI trailer): success when it matches, a processing error when not;
 *   - Activate and Commit move the flags this ONU REPORTS (is_active,
 *     is_committed, is_valid; attr_value in mibstore.c answers from
 *     sw_flag()), and nothing else.
 *
 * Never, in either mode: a write to flash (/dev/mtd*), to the U-Boot
 * environment (nv), or a reboot. The real slots, and which one boots, stay
 * exactly as they were. `make test-omci` (swdl-test.sh) runs a whole
 * download under qemu -strace and asserts none of those syscalls happens.
 *
 * OLT_SW_DOWNLOAD=reject in /etc/config/odi.conf keeps the old answers: each
 * message "not supported". It is read at every Start, Activate and Commit, so
 * a change applies to the next download without a restart.
 *
 * The reported flags live in RAM and in SWIMAGE_PATH, on tmpfs: they hold
 * for the rest of this boot, across an omcid respawn (the OLT view of the ONU
 * does not change when omcid does), and are gone at the next reboot, which
 * comes back reporting what the stick really runs. They are not MIB state --
 * a MIB reset does not touch them, which is why they are not in the resume
 * snapshot, which a MIB reset deletes. A download in progress is not saved:
 * after a respawn the OLT starts it again.
 *
 * What an OLT is used to from the stock stack, and what this matches: it
 * refuses a download to the active or committed image, answers Start with its
 * window size, checks the whole-image CRC at End, and refuses Activate or
 * Commit of an image that is not valid.
 */
#include "omcid.h"

#define SW_SECTION      31               /* baseline: 32 bytes minus the number */
#define SW_WIN_MAX      256              /* the window size is one byte, minus one */
#define SW_KEY          "OLT_SW_DOWNLOAD"

/* ------------------------------------------------------------ the flags */

struct sw_flags { uint8_t committed, active, valid; };

/* What omcid has always reported: image 0 committed, active and valid,
 * image 1 valid. */
static struct sw_flags swf[2] = { { 1, 1, 1 }, { 0, 0, 1 } };
static int swf_loaded;

static void sw_flags_load(void)
{
	uint8_t b[10];
	long fd, n;

	if (swf_loaded)
		return;
	swf_loaded = 1;
	fd = sys_open(SWIMAGE_PATH, O_RDONLY);
	if (fd < 0)
		return;
	n = sys_read((int)fd, b, sizeof b);
	sys_close((int)fd);
	if (n != (long)sizeof b || b[0] != 'S' || b[1] != 'W' || b[2] != 'I' ||
	    b[3] != '1')
		return;
	for (int i = 0; i < 2; i++) {
		swf[i].committed = b[4 + 3 * i] ? 1 : 0;
		swf[i].active    = b[5 + 3 * i] ? 1 : 0;
		swf[i].valid     = b[6 + 3 * i] ? 1 : 0;
	}
	out("software image: flags from this boot restored\n");
}

static void sw_flags_save(void)
{
	uint8_t b[10] = { 'S', 'W', 'I', '1' };

	for (int i = 0; i < 2; i++) {
		b[4 + 3 * i] = swf[i].committed;
		b[5 + 3 * i] = swf[i].active;
		b[6 + 3 * i] = swf[i].valid;
	}
	/* tmpfs, never the config partition: a failure costs the flags
	 * surviving a respawn, nothing else. */
	(void)atomic_write(SWIMAGE_PATH, SWIMAGE_TMP_PATH, b, sizeof b);
}

uint8_t sw_flag(uint16_t inst, unsigned attr)
{
	sw_flags_load();
	if (inst > 1)
		return 0;
	return attr == 2 ? swf[inst].committed :
	       attr == 3 ? swf[inst].active :
	       attr == 4 ? swf[inst].valid : 0;
}

/* ------------------------------------------------------------ the mode */

/* 1 accept (the default, also for an absent or empty key), 0 reject. An
 * unrecognised value accepts, and says so once. */
static int sw_accept(void)
{
	static int warned;
	char v[16];
	int n = cfg_odi_get(CFG_ODI_PATH, SW_KEY, v, sizeof v);

	if (n <= 0 || str_eq(v, "accept"))
		return 1;
	if (str_eq(v, "reject"))
		return 0;
	if (!warned) {
		warned = 1;
		out_fmt("%% %s=%s is neither accept nor reject; accepting\n",
			SW_KEY, v);
	}
	return 1;
}

/* ------------------------------------------------------------ the download */

static struct {
	int open;                        /* a Start was accepted, no End yet */
	int started;                     /* a Start was seen, so `accept` holds */
	int accept;                      /* the mode the last Start read */
	uint16_t inst;
	uint8_t window;                  /* sections per window, minus one */
	uint32_t size, bytes, crc;       /* announced; counted and CRC so far */
	unsigned long sections;          /* accepted, windows that closed */
	unsigned long seen;              /* every section frame, for reject */
	uint8_t got[SW_WIN_MAX / 8];     /* this window: which numbers came */
	uint8_t win[SW_WIN_MAX * SW_SECTION];
	int last_ar_ok;                  /* the last window close was accepted */
	uint16_t last_ar_tci;
	int end_ok;                      /* the last End succeeded, for a repeat */
	uint16_t end_inst;
	uint32_t end_crc;
} dl;

/* The image CRC, carried across calls: ITU-T I.363.5 (AAL5), which is the
 * OMCI trailer CRC too (omci.h): poly 0x04C11DB7, not reflected, all ones in
 * and out. Kept complemented between calls, so update(0, whole image) is
 * omci_crc(whole image). Bitwise: about 250 operations a section, a few
 * million for a whole image spread over minutes of download. */
static uint32_t crc_update(uint32_t acc, const uint8_t *p, uint32_t n)
{
	uint32_t c = ~acc;

	for (uint32_t i = 0; i < n; i++) {
		c ^= (uint32_t)p[i] << 24;
		for (int k = 0; k < 8; k++)
			c = (c & 0x80000000u) ? ((c << 1) ^ 0x04c11db7u) : (c << 1);
	}
	return ~c;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void win_clear(void)
{
	for (unsigned i = 0; i < sizeof dl.got; i++)
		dl.got[i] = 0;
}

static void answer(int fd, uint32_t tid, const uint8_t *f, uint8_t result,
		   int extra, uint8_t b1)
{
	uint8_t body[3] = { result, b1, 0 };

	send_resp(fd, tid, f, body, (uint16_t)(1 + extra));
}

static void on_start(int fd, uint32_t tid, const uint8_t *f, uint16_t inst)
{
	uint8_t body[3];

	dl.accept = sw_accept();
	dl.started = 1;
	dl.open = 0;
	dl.seen = 0;
	if (!dl.accept) {
		ev_sw_image("download_start", inst, -1, 0, -1, 0, "not_supported");
		answer(fd, tid, f, OMCI_ERR_CMD, 0, 0);
		out("-> not supported (OLT_SW_DOWNLOAD=reject)\n");
		return;
	}
	sw_flags_load();
	/* G.988 9.1.4: the download goes to the image that is neither active
	 * nor committed. The stock stack refuses the other with a parameter
	 * error, and so does this. */
	if (swf[inst].active || swf[inst].committed) {
		ev_sw_image("download_start", inst, (long)be32(f + 9),
			    (unsigned)f[8] + 1, -1, 0, "refused_active");
		answer(fd, tid, f, OMCI_ERR_BAD_PARAM, 0, 0);
		out("-> refused: that image is active or committed\n");
		return;
	}
	dl.open = 1;
	dl.inst = inst;
	dl.window = f[8];
	dl.size = be32(f + 9);
	dl.bytes = 0;
	dl.crc = 0;
	dl.sections = 0;
	dl.last_ar_ok = 0;
	dl.end_ok = 0;
	win_clear();
	/* Invalid until an End with a good CRC: what G.988 reports for an
	 * image being downloaded. */
	swf[inst].valid = 0;
	sw_flags_save();
	/* Result, the window size granted (all of it), and zero instances
	 * responding -- the shape the stock stack answers. */
	body[0] = OMCI_OK;
	body[1] = dl.window;
	body[2] = 0;
	send_resp(fd, tid, f, body, 3);
	ev_sw_image("download_start", inst, (long)dl.size,
		    (unsigned)dl.window + 1, -1, 0, "ok");
	out_fmt("-> download to image %d accepted: %d bytes, %d sections a "
		"window, discarded\n", (long)inst, (long)dl.size,
		(long)dl.window + 1);
}

static void on_section(int fd, uint32_t tid, const uint8_t *f, uint16_t inst)
{
	int ar = (f[2] & OMCI_AR) != 0;
	uint8_t n = f[8];
	uint16_t tci = nl_get16(f);

	dl.seen++;
	if (!dl.open || !dl.accept || inst != dl.inst) {
		int acc;

		if (!ar)
			return;
		/* Reject mode answers what it always has; a section outside
		 * an accepted download is a processing error, as the stock
		 * stack answers one it has no window for. */
		acc = dl.started ? dl.accept : sw_accept();
		if (acc)
			answer(fd, tid, f, 1, 1, n);
		else
			answer(fd, tid, f, OMCI_ERR_CMD, 0, 0);
		return;
	}
	for (unsigned i = 0; i < SW_SECTION; i++)
		dl.win[(unsigned)n * SW_SECTION + i] = f[9 + i];
	dl.got[n >> 3] |= (uint8_t)(0x80u >> (n & 7));
	if (!ar)
		return;
	/* The close of the window just accepted, again: our answer was lost
	 * and the OLT repeated it. Answer it again, count nothing twice. */
	if (dl.last_ar_ok && tci == dl.last_ar_tci) {
		win_clear();
		answer(fd, tid, f, OMCI_OK, 1, n);
		return;
	}
	for (unsigned s = 0; s <= n; s++)
		if (!(dl.got[s >> 3] & (0x80u >> (s & 7)))) {
			out_fmt("-> window closing at section %d is missing "
				"section %d: processing error, the OLT resends it\n",
				(long)n, (long)s);
			win_clear();
			dl.last_ar_ok = 0;
			answer(fd, tid, f, 1, 1, n);
			return;
		}
	{
		uint32_t len = ((uint32_t)n + 1) * SW_SECTION;

		/* The last window carries padding past the image size. */
		if (dl.bytes >= dl.size)
			len = 0;
		else if (len > dl.size - dl.bytes)
			len = dl.size - dl.bytes;
		dl.crc = crc_update(dl.crc, dl.win, len);
		dl.bytes += len;
	}
	dl.sections += (unsigned long)n + 1;
	dl.last_ar_ok = 1;
	dl.last_ar_tci = tci;
	win_clear();
	answer(fd, tid, f, OMCI_OK, 1, n);
}

static void on_end(int fd, uint32_t tid, const uint8_t *f, uint16_t inst)
{
	uint32_t crc = be32(f + 8), size = be32(f + 12);
	const char *why;
	uint8_t result;

	if (!(dl.started ? dl.accept : sw_accept())) {
		ev_sw_image("download_end", inst, -1, 0, (long)dl.seen, 0,
			    "not_supported");
		answer(fd, tid, f, OMCI_ERR_CMD, 0, 0);
		return;
	}
	/* An End repeated after it succeeded: the answer was lost. */
	if (!dl.open && dl.end_ok && inst == dl.end_inst && crc == dl.end_crc) {
		answer(fd, tid, f, OMCI_OK, 1, 0);
		out("-> end repeated, answered again\n");
		return;
	}
	if (!dl.open || inst != dl.inst) {
		ev_sw_image("download_end", inst, -1, 0, -1, 0, "no_download");
		answer(fd, tid, f, 1, 1, 0);
		return;
	}
	if (size != dl.size) {
		why = "size_mismatch";
		result = OMCI_ERR_BAD_PARAM;
	} else if (dl.bytes != dl.size) {
		why = "short";
		result = 1;
	} else if (crc != dl.crc) {
		why = "crc_error";
		result = 1;
	} else {
		why = "ok";
		result = OMCI_OK;
	}
	ev_sw_image("download_end", inst, -1, 0, (long)dl.sections,
		    dl.bytes == dl.size && crc == dl.crc ? "ok" : "bad", why);
	out_fmt("-> end: %d of %d bytes, %d sections, crc olt %08x ours %08x: "
		"%s; the image is discarded\n", (long)dl.bytes, (long)dl.size,
		(long)dl.sections, (long)crc, (long)dl.crc, why);
	dl.open = 0;
	dl.end_ok = result == OMCI_OK;
	dl.end_inst = inst;
	dl.end_crc = crc;
	if (result == OMCI_OK) {
		swf[inst].valid = 1;
		sw_flags_save();
	}
	/* Result, then zero instances responding. */
	answer(fd, tid, f, result, 1, 0);
}

/* Activate (22) and Commit (23): the reported flags only. Activate never
 * reboots -- the stick keeps running the slot it booted, and a later reboot
 * comes back to the slot sw_commit names, which this never writes. */
static void on_flags(int fd, uint32_t tid, const uint8_t *f, uint16_t inst,
		     int commit)
{
	const char *op = commit ? "commit" : "activate";

	if (!sw_accept()) {
		ev_sw_image(op, inst, -1, 0, -1, 0, "not_supported");
		answer(fd, tid, f, OMCI_ERR_CMD, 0, 0);
		return;
	}
	sw_flags_load();
	if (!swf[inst].valid || (dl.open && dl.inst == inst)) {
		ev_sw_image(op, inst, -1, 0, -1, 0, "refused_invalid");
		answer(fd, tid, f, OMCI_ERR_BAD_PARAM, 0, 0);
		return;
	}
	for (uint16_t i = 0; i < 2; i++) {
		if (commit)
			swf[i].committed = i == inst;
		else
			swf[i].active = i == inst;
	}
	sw_flags_save();
	ev_sw_image(op, inst, -1, 0, -1, 0, "ok");
	answer(fd, tid, f, OMCI_OK, 0, 0);
	out_fmt("-> %s image %d: reported only; no flash, no environment, "
		"no reboot\n", op, (long)inst);
}

void sw_handle(int fd, uint32_t tid, const uint8_t *f, const struct omci_class *c)
{
	uint8_t mt = OMCI_MT(f[2]);
	uint16_t cls = nl_get16(f + 4), inst = nl_get16(f + 6);
	int ar = (f[2] & OMCI_AR) != 0;

	/* Only the software image entity downloads here. Anything else --
	 * ISP2's OLT sends End software download, no AR, to ONU-G three times
	 * at the start of every session -- is not a software image step: no
	 * event line, and the answer, if one is asked, is what it always was. */
	if (cls != OMCI_ME_SOFTWARE_IMAGE) {
		if (!ar) {
			out("   (no ack requested; not a software image)\n");
			return;
		}
		answer(fd, tid, f, c ? OMCI_ERR_CMD : OMCI_ERR_UNKNOWN_ME, 0, 0);
		out("-> not supported on this entity\n");
		return;
	}
	if (inst > 1) {
		if (ar)
			answer(fd, tid, f, OMCI_ERR_UNKNOWN_INST, 0, 0);
		return;
	}
	if (mt == OMCI_MT_DOWNLOAD_SECTION) {
		on_section(fd, tid, f, inst);
		return;
	}
	/* Every other step asks for an answer; one that does not is logged
	 * and otherwise ignored, as omcimsg.c ignores any such frame. */
	if (!ar) {
		out("   (no ack requested)\n");
		return;
	}
	if (mt == OMCI_MT_START_SW_DOWNLOAD)
		on_start(fd, tid, f, inst);
	else if (mt == OMCI_MT_END_SW_DOWNLOAD)
		on_end(fd, tid, f, inst);
	else if (mt == OMCI_MT_ACTIVATE_SW)
		on_flags(fd, tid, f, inst, 0);
	else if (mt == OMCI_MT_COMMIT_SW)
		on_flags(fd, tid, f, inst, 1);
}
