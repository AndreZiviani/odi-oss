/* See env.h for the format and where it was read from. */
#include "env.h"

static int e_len(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

/* The ordinary Ethernet/zlib CRC32, computed a nibble at a time so the table
 * is 16 entries rather than 256 -- this binary is meant to be small and the
 * block is 8 KB, so the speed does not matter. */
uint32_t env_crc32(const uint8_t *p, uint32_t n)
{
	static const uint32_t tbl[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
		0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
		0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
		0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
	};
	uint32_t c = 0xffffffffu;

	for (uint32_t i = 0; i < n; i++) {
		c ^= p[i];
		c = tbl[c & 0xf] ^ (c >> 4);
		c = tbl[c & 0xf] ^ (c >> 4);
	}
	return c ^ 0xffffffffu;
}

uint32_t env_crc_stored(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
	     | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

void env_crc_store(uint8_t *buf, uint32_t crc)
{
	buf[0] = (uint8_t)(crc >> 24);
	buf[1] = (uint8_t)(crc >> 16);
	buf[2] = (uint8_t)(crc >> 8);
	buf[3] = (uint8_t)crc;
}

int env_valid(const uint8_t *buf, uint32_t len)
{
	if (len <= ENV_HDR_LEN)
		return 0;
	return env_crc_stored(buf)
	    == env_crc32(buf + ENV_HDR_LEN, len - ENV_HDR_LEN);
}

const char *env_next(const uint8_t *buf, uint32_t len, uint32_t *pos)
{
	const char *d = (const char *)buf + ENV_HDR_LEN;
	uint32_t n = len - ENV_HDR_LEN, p = *pos;

	if (len <= ENV_HDR_LEN || p >= n || d[p] == '\0')
		return 0;
	const char *s = d + p;
	while (p < n && d[p] != '\0')
		p++;
	*pos = p + 1;                    /* step over the NUL */
	return s;
}

/* "key=value" against "key": true when the name matches up to the '='. */
static int pair_is(const char *pair, const char *key)
{
	int i = 0;

	while (key[i] && pair[i] == key[i])
		i++;
	return key[i] == '\0' && pair[i] == '=';
}

int env_get(const uint8_t *buf, uint32_t len, const char *key,
	    char *out, int max)
{
	uint32_t pos = 0;
	const char *pair;

	while ((pair = env_next(buf, len, &pos)) != 0) {
		if (!pair_is(pair, key))
			continue;
		const char *v = pair + e_len(key) + 1;
		int i = 0;

		while (v[i] && i < max - 1) {
			out[i] = v[i];
			i++;
		}
		out[i] = '\0';
		return 1;
	}
	if (max > 0)
		out[0] = '\0';
	return 0;
}

int env_set(uint8_t *buf, uint32_t len, const char *key, const char *value)
{
	uint8_t tmp[ENV_MAX];
	uint32_t pos = 0, w = ENV_HDR_LEN;
	const char *pair;
	int replaced = 0;
	int klen = e_len(key);
	int vlen = value ? e_len(value) : 0;

	if (len > ENV_MAX || len <= ENV_HDR_LEN)
		return 0;
	for (uint32_t i = 0; i < len; i++)
		tmp[i] = 0;
	tmp[ENV_HDR_CRC] = buf[ENV_HDR_CRC];        /* keep the flags byte */

	/* Copy every pair but the one being replaced, in order. Order is not
	 * load-bearing for U-Boot, but keeping it makes a before/after diff of
	 * the block readable. */
	while ((pair = env_next(buf, len, &pos)) != 0) {
		int n = e_len(pair);

		if (pair_is(pair, key)) {
			replaced = 1;
			continue;
		}
		if (w + (uint32_t)n + 2 > len)
			return 0;
		for (int i = 0; i < n; i++)
			tmp[w++] = (uint8_t)pair[i];
		tmp[w++] = 0;
	}
	if (value) {
		if (w + (uint32_t)(klen + 1 + vlen) + 2 > len)
			return 0;
		for (int i = 0; i < klen; i++)
			tmp[w++] = (uint8_t)key[i];
		tmp[w++] = '=';
		for (int i = 0; i < vlen; i++)
			tmp[w++] = (uint8_t)value[i];
		tmp[w++] = 0;
	} else if (!replaced) {
		return 0;                    /* nothing to delete */
	}
	tmp[w++] = 0;                        /* the list-ending second NUL */

	for (uint32_t i = 0; i < len; i++)
		buf[i] = tmp[i];
	env_crc_store(buf, env_crc32(buf + ENV_HDR_LEN, len - ENV_HDR_LEN));
	return 1;
}

int env_pick(const uint8_t *b1, int ok1, const uint8_t *b2, int ok2)
{
	if (ok1 && ok2)
		return b2[ENV_HDR_CRC] > b1[ENV_HDR_CRC] ? 2 : 1;
	if (ok1)
		return 1;
	if (ok2)
		return 2;
	return 0;
}
