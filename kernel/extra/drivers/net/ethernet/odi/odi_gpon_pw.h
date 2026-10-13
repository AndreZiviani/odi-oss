/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_pw.h -- the argument of the gponpw verb: how the stored PLOAM
 * password text becomes the 10 bytes the ONU sends. No kernel dependency, so
 * test/odi_gpon_pw_test.c runs it with the host cc.
 */
#ifndef ODI_GPON_PW_H
#define ODI_GPON_PW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define ODI_GPON_PW_LEN 10U

/*
 * What every value that registers today keeps meaning, so nothing that works
 * changes; the one addition is an explicit short-hex form. First match wins:
 *
 *   - empty or NULL: no password, ten zero bytes;
 *   - "0x" or "0X" and then an even number (2..20) of hex digits: those
 *     bytes (new; the vendor omci_app convention);
 *   - exactly 20 characters: 20 hex digits, 10 bytes; -EINVAL if one is not
 *     a hex digit (as before);
 *   - 1..10 characters: the characters themselves, EVEN WHEN all are hex
 *     digits ("123456" is six ASCII bytes, as before).
 *
 * Every form is zero-padded on the right to ten bytes. Anything else
 * (11..19 or more than 20 characters, 20 with a non-hex digit) is -EINVAL
 * and pw is left all zero. Short hex without the prefix is deliberately not
 * guessed at: it cannot be told from text.
 */
int odi_gpon_parse_pw(const char *arg, uint8_t pw[ODI_GPON_PW_LEN]);

#endif
