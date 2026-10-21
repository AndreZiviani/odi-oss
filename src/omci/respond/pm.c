/* Performance monitoring history data: the 15-minute interval framework, and
 * ME 312 (FEC PMHD) on it.
 *
 * G.988 9.2.9 and the PM rules of clause I.4 in short: a history entity
 * counts in an interval of 15 minutes. At the end of one the counts are kept
 * as the "completed interval" (what a Get returns, together with the Interval
 * end time) and counting starts again from zero; Get current data returns
 * the interval in progress. The OLT places the interval boundaries with
 * Synchronize time (ME 256): they fall on multiples of 15 minutes of the time
 * of day it sends, and the message restarts the intervals, so every count is
 * cleared and Interval end time starts again from 0. Without one the
 * intervals run from the moment omcid started. Interval end time is an 8-bit
 * counter of completed intervals since the entity was created.
 *
 * The values do not come from a register read here. The GPON MAC counters are
 * clear-on-read, so the driver is their only reader and keeps 64-bit running
 * totals (odi_gpon_ctr.c, listed in /proc/odi_gpon/counters). An entity keeps
 * the totals at the start of its interval, and a count is the difference.
 * Nothing in the hardware is cleared, so another reader of the totals (the
 * metrics exporter) is never disturbed by an interval ending.
 *
 * The attributes, and where each comes from:
 *
 *   Corrected bytes        ds_fec_corrected_bytes
 *   Corrected code words   ds_fec_corrected_cw
 *   Uncorrectable code w.  ds_fec_uncorrectable_cw
 *   FEC seconds            seconds in which the uncorrectable total grew,
 *                          counted here from a once-a-second look: G.988
 *                          defines it as seconds with a FEC anomaly, and an
 *                          uncorrectable code word is the anomaly that
 *                          matters; a second that only had corrections is a
 *                          working FEC, not an anomaly.
 *   Total code words       NOT SOURCED, stays 0. The MAC has no count of the
 *                          code words it received, only of the bad ones, and
 *                          deriving it from the line rate would be a guess
 *                          (the shortened last code word of a frame, FEC
 *                          switched off, time spent outside O5). A wrong
 *                          total would turn the OLT's error ratio into a
 *                          wrong number; zero says "not reported".
 *   Threshold data 1/2 id  the OLT's own pointer, kept by the MIB store. No
 *                          threshold crossing alert is raised: the
 *                          thresholds (ME 273/274) are held and never
 *                          compared, because no alarm is built on the FEC
 *                          counters and an alarm that never fires is a
 *                          claim the line cannot back.
 *
 * Counts stop at the most their attribute holds (4 294 967 295, 65 535 for
 * FEC seconds) rather than wrap.
 *
 * Not kept across a respawn of omcid: an entity the MIB snapshot brings back
 * starts counting at the first look, with Interval end time 0, as one just
 * created would. The OLT sees a PM entity whose history began again, which
 * is true of it.
 *
 * Time comes from CLOCK_MONOTONIC, or from the -t file in tests, so a test
 * moves the 15 minutes by writing a number.
 */
#include "omcid.h"
#include "../procparse.h"

#define PM_MAX        8
#define PM_INTERVAL_S 900
#define PM_CTRS       4                  /* the order of pm_ctr_name below */
#define PM_CLS_FEC    312

enum { C_COR_BYTES, C_COR_CW, C_UNCOR_CW, C_FEC_SECS };

static const char *const pm_ctr_name[3] = {
	"ds_fec_corrected_bytes", "ds_fec_corrected_cw", "ds_fec_uncorrectable_cw",
};

struct pm_ent {
	uint16_t cls, inst;
	uint8_t used;
	uint8_t end_time;                /* Interval end time */
	uint64_t base[PM_CTRS];          /* the totals when this interval began */
	uint32_t prev[PM_CTRS];          /* the completed interval */
};

static struct pm_ent ent[PM_MAX];

/* The latest totals, and whether there has ever been a reading: with none
 * (no driver, as under a test with no -n file) every value is 0 and nothing
 * advances. */
static uint64_t tot[PM_CTRS];
static int tot_ok;

static int clock_started;
static long next_end_s;                  /* the clock second the interval ends */
static long last_sample_s = -1;
static uint64_t last_uncor;
static int uncor_seen;

const char *pm_counters_path = "/proc/odi_gpon/counters";
const char *pm_clock_path;               /* -t, test only */
static int cur_view;                     /* Get current data in progress */

static long pm_now(void)
{
	long s = 0, ns = 0;

	if (pm_clock_path) {
		char b[24];
		long fd = sys_open(pm_clock_path, O_RDONLY), n;

		s = 0;
		if (fd >= 0) {
			n = sys_read((int)fd, b, sizeof b - 1);
			sys_close((int)fd);
			for (long i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++)
				s = s * 10 + (b[i] - '0');
		}
		return s;
	}
	sys_clock_gettime(CLOCK_MONOTONIC, &s, &ns);
	return s;
}

/* A count since `base`, held at what a 32-bit attribute holds. A total below
 * its base means the driver's counters restarted (a reload of the module):
 * that interval's count is 0, not 2^32. */
static uint32_t sat32(uint64_t tot_now, uint64_t base)
{
	uint64_t d = tot_now >= base ? tot_now - base : 0;

	return d > 0xffffffffULL ? 0xffffffffU : (uint32_t)d;
}

/* Read the driver's totals into tot[]. FEC seconds is ours, kept in tot[]
 * across reads. */
static void read_totals(void)
{
	static char buf[6144];
	long fd = sys_open(pm_counters_path, O_RDONLY);
	unsigned len = 0;
	uint64_t v[3];

	if (fd < 0)
		return;
	for (;;) {
		long n = sys_read((int)fd, buf + len, sizeof buf - len);

		if (n <= 0)
			break;
		len += (unsigned)n;
		if (len >= sizeof buf)
			break;
	}
	sys_close((int)fd);
	for (int i = 0; i < 3; i++)
		if (pp_ctr(buf, len, pm_ctr_name[i], &v[i]) != 0)
			return;              /* cannot tell: keep what we had */
	tot[C_COR_BYTES] = v[0];
	tot[C_COR_CW] = v[1];
	tot[C_UNCOR_CW] = v[2];
	tot_ok = 1;
}

static void start_clock(long now)
{
	if (clock_started)
		return;
	clock_started = 1;
	next_end_s = now + PM_INTERVAL_S;
}

static void ent_restart(struct pm_ent *e)
{
	e->end_time = 0;
	for (int i = 0; i < PM_CTRS; i++) {
		e->base[i] = tot[i];
		e->prev[i] = 0;
	}
}

/* Count the seconds with new uncorrectable code words. At most once per clock
 * second however often it is called, so a Get and the main loop's tick in the
 * same second do not count it twice; growth that falls between two looks in
 * one second is picked up by the next. */
static void sample(long now)
{
	if (now == last_sample_s)
		return;
	last_sample_s = now;
	if (!tot_ok)
		return;
	if (uncor_seen && tot[C_UNCOR_CW] > last_uncor)
		tot[C_FEC_SECS]++;
	last_uncor = tot[C_UNCOR_CW];
	uncor_seen = 1;
}

/* End every interval that has passed. When more than one has passed with
 * nobody looking (a stopped clock, in practice only a test jumping it) the
 * counts of the older ones cannot be told apart, so the completed interval
 * is the last, empty, one and they are dropped. */
static void advance(long now)
{
	long passed;

	if (!clock_started || now < next_end_s)
		return;
	passed = (now - next_end_s) / PM_INTERVAL_S + 1;
	for (unsigned i = 0; i < PM_MAX; i++) {
		struct pm_ent *e = &ent[i];

		if (!e->used)
			continue;
		for (int k = 0; k < PM_CTRS; k++) {
			e->prev[k] = passed == 1 ? sat32(tot[k], e->base[k]) : 0;
			e->base[k] = tot[k];
		}
		e->end_time = (uint8_t)(e->end_time + passed);
	}
	next_end_s += passed * PM_INTERVAL_S;
}

static void look(void)
{
	long now = pm_now();

	start_clock(now);
	read_totals();
	sample(now);
	advance(now);
}

static struct pm_ent *ent_find(uint16_t cls, uint16_t inst)
{
	for (unsigned i = 0; i < PM_MAX; i++)
		if (ent[i].used && ent[i].cls == cls && ent[i].inst == inst)
			return &ent[i];
	return 0;
}

/* An entity that exists in the MIB but not here is one the snapshot brought
 * back, or one created when all slots were taken. */
static struct pm_ent *ent_get(uint16_t cls, uint16_t inst)
{
	struct pm_ent *e = ent_find(cls, inst);

	if (e)
		return e;
	for (unsigned i = 0; i < PM_MAX; i++) {
		if (ent[i].used)
			continue;
		ent[i].used = 1;
		ent[i].cls = cls;
		ent[i].inst = inst;
		ent_restart(&ent[i]);
		return &ent[i];
	}
	return 0;
}

int pm_class(uint16_t cls)
{
	return cls == PM_CLS_FEC;
}

void pm_create(uint16_t cls, uint16_t inst)
{
	struct pm_ent *e;

	if (!pm_class(cls))
		return;
	look();
	e = ent_get(cls, inst);
	if (e)
		ent_restart(e);
}

void pm_delete(uint16_t cls, uint16_t inst)
{
	struct pm_ent *e = ent_find(cls, inst);

	if (e)
		e->used = 0;
}

void pm_mib_reset(void)
{
	for (unsigned i = 0; i < PM_MAX; i++)
		ent[i].used = 0;
}

/* Synchronize time: hour, minute, second of the OLT's clock. The next
 * interval ends at the next multiple of 15 minutes of it, and every entity
 * starts again. A time that is not a time is ignored, so a garbled message
 * cannot wipe the history. */
void pm_sync_time(unsigned hh, unsigned mm, unsigned ss)
{
	long now;
	unsigned tod;

	if (hh > 23 || mm > 59 || ss > 59)
		return;
	look();
	now = pm_now();
	tod = hh * 3600 + mm * 60 + ss;
	clock_started = 1;
	next_end_s = now + (long)(PM_INTERVAL_S - tod % PM_INTERVAL_S);
	for (unsigned i = 0; i < PM_MAX; i++)
		if (ent[i].used)
			ent_restart(&ent[i]);
}

void pm_tick(void)
{
	look();
}

/* Which view the next pm_attr calls answer: 1 for Get current data. */
void pm_view(int current)
{
	cur_view = current;
	if (current)
		look();
}

/* An attribute of a PM entity, big-endian, `n` bytes wide. 1 when the
 * attribute is one pm.c answers (the caller keeps what it had otherwise). */
int pm_attr(uint16_t cls, uint16_t inst, unsigned k, uint8_t *out, uint16_t n)
{
	struct pm_ent *e;
	uint32_t v;

	if (cls != PM_CLS_FEC)
		return 0;
	/* Attributes 1 (interval end time) and 3-7. 2, the threshold pointer,
	 * is the OLT's own. */
	if (k != 1 && (k < 3 || k > 7))
		return 0;
	if (!cur_view)
		look();
	/* Only an entity the OLT created: asking for one that is not in the MIB
	 * must not take a slot. */
	if (!mib_find(cls, inst))
		return 0;
	e = ent_get(cls, inst);
	if (!e)
		return 0;
	switch (k) {
	case 1: v = e->end_time; break;
	case 3: v = cur_view ? sat32(tot[C_COR_BYTES], e->base[C_COR_BYTES]) : e->prev[C_COR_BYTES]; break;
	case 4: v = cur_view ? sat32(tot[C_COR_CW], e->base[C_COR_CW]) : e->prev[C_COR_CW]; break;
	case 5: v = cur_view ? sat32(tot[C_UNCOR_CW], e->base[C_UNCOR_CW]) : e->prev[C_UNCOR_CW]; break;
	case 6: v = 0; break;            /* total code words: not sourced, see above */
	default:
		v = cur_view ? sat32(tot[C_FEC_SECS], e->base[C_FEC_SECS]) : e->prev[C_FEC_SECS];
		if (v > 0xffff)
			v = 0xffff;
		break;
	}
	for (uint16_t i = 0; i < n; i++)
		out[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
	return 1;
}
