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
 * The config store keeps GPON_PLOAM_PASSWD as the hex of the password (the
 * web UI shows the decoded text beside it and asks for the hex back), and
 * the vendor tools also hand over plain text, so the argument is read in
 * this order, first match wins:
 *
 *   - empty or NULL: no password, ten zero bytes;
 *   - "0x" or "0X" and then an even number (2..20) of hex digits: those
 *     bytes, the vendor omci_app convention;
 *   - an even number (2..20) of hex digits: those bytes;
 *   - anything else of 1..10 characters: the characters themselves.
 *
 * Every form is zero-padded on the right to ten bytes. Anything longer, or an
 * odd run of 11..19 characters, or an unusable "0x" form, is -EINVAL and pw
 * is left all zero.
 *
 * The one ambiguity is a plain-text password of 2..10 characters that is
 * also valid hex ("1234", "deadbeef"): it is read as hex, as the store
 * holds it. A text password that is not all hex digits is always read as text.
 */
int odi_gpon_parse_pw(const char *arg, uint8_t pw[ODI_GPON_PW_LEN]);

#endif
