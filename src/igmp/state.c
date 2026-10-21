#include "state.h"

static uint32_t put_s(char *b, uint32_t at, uint32_t max, const char *s)
{
	while (*s && at < max)
		b[at++] = *s++;
	return at;
}

static uint32_t put_u(char *b, uint32_t at, uint32_t max, uint32_t v)
{
	char d[10];
	int n = 0;

	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && at < max)
		b[at++] = d[--n];
	return at;
}

static uint32_t put_kv(char *b, uint32_t at, uint32_t max, const char *k, uint32_t v)
{
	at = put_s(b, at, max, k);
	at = put_s(b, at, max, "=");
	at = put_u(b, at, max, v);
	return put_s(b, at, max, "\n");
}

uint32_t igmp_state_format(const struct igmp_db *db, char *buf, uint32_t max)
{
	uint32_t at = 0;

	at = put_kv(buf, at, max, "joins", db->joins);
	at = put_kv(buf, at, max, "denied", db->denied);
	at = put_kv(buf, at, max, "groups", (uint32_t)igmp_db_dynamic_count(db));
	/* One line a group, not a line a (vid, group): two VLANs of the same
	 * group are one group to the OLT. */
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		const struct igmp_entry *e = &db->g[i];
		int seen = 0;

		if (!e->in_use || e->fixed)
			continue;
		for (int j = 0; j < i && !seen; j++)
			if (db->g[j].in_use && !db->g[j].fixed && db->g[j].group == e->group)
				seen = 1;
		if (seen)
			continue;
		at = put_s(buf, at, max, "group=");
		for (int k = 3; k >= 0; k--) {
			at = put_u(buf, at, max, (e->group >> (8 * k)) & 0xff);
			if (k)
				at = put_s(buf, at, max, ".");
		}
		at = put_s(buf, at, max, "\n");
	}
	return at >= max ? 0 : at;
}
