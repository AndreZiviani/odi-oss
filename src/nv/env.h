/* The U-Boot environment block, as this device stores it.
 *
 * Read off isp2's /dev/mtd1 rather than assumed:
 *
 *     offset 0..3     CRC32, BIG-ENDIAN
 *     offset 4        flags byte (U-Boot's REDUNDANT environment; 0 here)
 *     offset 5..end   NUL-separated "key=value", a double NUL ends the list
 *     the CRC covers offsets 5..end -- NOT the flags byte
 *
 * `env` and `env2` are a redundant pair, 8 KB each with 4 KB erase blocks.
 * That flags byte is the whole reason a plain "crc32 of everything after the
 * first four bytes" does not verify, and it was tried first.
 *
 * Nothing here makes a syscall, so it builds for the host and is tested there.
 */
#ifndef ODI_ENV_H
#define ODI_ENV_H

#include <stdint.h>

#define ENV_HDR_CRC   4                  /* bytes 0..3  */
#define ENV_HDR_FLAGS 1                  /* byte 4      */
#define ENV_HDR_LEN   (ENV_HDR_CRC + ENV_HDR_FLAGS)
/* Both env partitions on this device are 8 KB. env_set rebuilds the block in
 * a stack buffer of this size, so it is the ceiling on what it will touch. */
#define ENV_MAX       8192

uint32_t env_crc32(const uint8_t *p, uint32_t n);

/* Big-endian, because that is how the stored CRC reads. */
uint32_t env_crc_stored(const uint8_t *buf);
void     env_crc_store(uint8_t *buf, uint32_t crc);

/* 1 when the stored CRC matches the data. */
int env_valid(const uint8_t *buf, uint32_t len);

/* Walk the pairs. `pos` starts at 0 and is advanced; returns a pointer to the
 * "key=value" string and its length, or 0 at the end. The buffer is not
 * modified and the strings are not NUL-terminated copies -- they point into
 * the block, which already NUL-terminates each one. */
const char *env_next(const uint8_t *buf, uint32_t len, uint32_t *pos);

/* Copy the value of `key` into `out` (max `max`, always NUL-terminated).
 * Returns 1 when found. */
int env_get(const uint8_t *buf, uint32_t len, const char *key,
	    char *out, int max);

/* Replace or append `key=value`, rebuilding the block in place and fixing the
 * CRC. A NULL `value` deletes the key. Returns 1 on success, 0 if the result
 * would not fit -- in which case `buf` is left untouched. */
int env_set(uint8_t *buf, uint32_t len, const char *key, const char *value);

/* Which of the redundant pair wins: 1 for `env`, 2 for `env2`, 0 when
 * neither is valid. `ok1`/`ok2` say whether that copy passed env_valid; the
 * blocks are only read for their flags byte. With both valid the HIGHER
 * flags byte wins and a tie goes to `env` -- the rule the stock nv follows
 * (on isp2, flags 0x00 in env and 0x01 in env2 reads "Valid environment:
 * 2"). The one definition, so the reader and `nv commit` cannot disagree
 * about which copy is the primary. */
int env_pick(const uint8_t *b1, int ok1, const uint8_t *b2, int ok2);

#endif
