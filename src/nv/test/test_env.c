/* Host-side tests for the pure environment logic.
 *
 * The block format was read off a real device, but no device image is
 * committed: isp2's environment names the stick. These build one instead,
 * with the same layout, and assert the properties that matter.
 *
 *     cc -o t test_env.c ../env.c && ./t
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../env.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

/* A block in the device's layout: CRC, flags, then NUL-separated pairs. */
static void build(uint8_t *b, uint32_t len, const char *const *pairs, int n)
{
	uint32_t w = ENV_HDR_LEN;

	memset(b, 0, len);
	b[ENV_HDR_CRC] = 0;
	for (int i = 0; i < n; i++) {
		size_t l = strlen(pairs[i]);

		memcpy(b + w, pairs[i], l);
		w += l;
		b[w++] = 0;
	}
	b[w++] = 0;
	env_crc_store(b, env_crc32(b + ENV_HDR_LEN, len - ENV_HDR_LEN));
}

int main(void)
{
	static uint8_t b[ENV_MAX];
	const char *pairs[] = {
		"bootdelay=1",
		"baudrate=115200",
		"sw_active=0",
		"device_vendor=Anime-Nijika",
	};
	char v[128];

	build(b, sizeof b, pairs, 4);

	ck(env_valid(b, sizeof b), "a freshly built block verifies");

	b[ENV_HDR_LEN + 1] ^= 0x20;
	ck(!env_valid(b, sizeof b), "one flipped data bit fails the CRC");
	b[ENV_HDR_LEN + 1] ^= 0x20;

	/* The flags byte is deliberately outside the CRC: U-Boot rewrites it
	 * to pick between the redundant copies, and a reader that folded it in
	 * would call a perfectly good block corrupt. */
	b[ENV_HDR_CRC] = 0x01;
	ck(env_valid(b, sizeof b), "the flags byte is NOT covered by the CRC");

	ck(env_get(b, sizeof b, "baudrate", v, sizeof v) && !strcmp(v, "115200"),
	   "get reads a value");
	ck(!env_get(b, sizeof b, "nosuchkey", v, sizeof v) && v[0] == '\0',
	   "a missing key returns 0 and an empty string");
	ck(!env_get(b, sizeof b, "baud", v, sizeof v),
	   "a prefix of a real key does not match it");
	ck(env_get(b, sizeof b, "sw_active", v, sizeof v) && !strcmp(v, "0"),
	   "get reads the last-but-one pair");

	ck(env_set(b, sizeof b, "sw_tryactive", "1"), "set appends a new key");
	ck(env_valid(b, sizeof b), "the CRC is rebuilt after a set");
	ck(env_get(b, sizeof b, "sw_tryactive", v, sizeof v) && !strcmp(v, "1"),
	   "the appended key reads back");
	ck(env_get(b, sizeof b, "bootdelay", v, sizeof v) && !strcmp(v, "1"),
	   "an append leaves the other keys alone");
	ck(b[ENV_HDR_CRC] == 0x01, "an append preserves the flags byte");

	ck(env_set(b, sizeof b, "sw_active", "1"), "set replaces an existing key");
	ck(env_get(b, sizeof b, "sw_active", v, sizeof v) && !strcmp(v, "1"),
	   "the replaced value reads back");
	{
		uint32_t pos = 0, n = 0;
		const char *p;

		while ((p = env_next(b, sizeof b, &pos)) != 0) {
			(void)p;
			n++;
		}
		ck(n == 5, "a replace does not duplicate the key (5 pairs)");
	}

	ck(env_set(b, sizeof b, "sw_tryactive", NULL), "a NULL value deletes");
	ck(!env_get(b, sizeof b, "sw_tryactive", v, sizeof v),
	   "the deleted key is gone");
	ck(!env_set(b, sizeof b, "neverthere", NULL),
	   "deleting a key that is not there fails rather than rewriting");
	ck(env_valid(b, sizeof b), "the block still verifies after a delete");

	/* Overflow must refuse, not truncate: this block is flashed. */
	{
		static uint8_t small[64];
		const char *one[] = { "a=b" };
		char big[64];

		build(small, sizeof small, one, 1);
		memset(big, 'x', sizeof big - 1);
		big[sizeof big - 1] = '\0';
		ck(!env_set(small, sizeof small, "k", big),
		   "a value that would not fit is refused");
		ck(env_valid(small, sizeof small) &&
		   env_get(small, sizeof small, "a", v, sizeof v) &&
		   !strcmp(v, "b"),
		   "and the block is left exactly as it was");
	}

	printf("%s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
