// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_pw.c -- implementation for odi_gpon_pw.h. Plain C, no kernel
 * dependency (see the header).
 */
#include "odi_gpon_pw.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#endif

static int pw_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* n hex digits (n even, 2..20) into n/2 bytes; false when one is not a digit. */
static bool pw_hex(const char *s, size_t n, uint8_t pw[ODI_GPON_PW_LEN])
{
	size_t i;

	if (n < 2U || n > 2U * ODI_GPON_PW_LEN || (n & 1U))
		return false;
	for (i = 0; i < n; i++)
		if (pw_nibble(s[i]) < 0)
			return false;
	for (i = 0; i < n / 2U; i++)
		pw[i] = (uint8_t)((pw_nibble(s[2U * i]) << 4) | pw_nibble(s[2U * i + 1U]));
	return true;
}

int odi_gpon_parse_pw(const char *arg, uint8_t pw[ODI_GPON_PW_LEN])
{
	size_t len;

	memset(pw, 0, ODI_GPON_PW_LEN);
	if (!arg)
		return 0;
	len = strlen(arg);
	if (len == 0U)
		return 0;
	if (len > 2U && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
		if (pw_hex(arg + 2, len - 2U, pw))
			return 0;
		memset(pw, 0, ODI_GPON_PW_LEN);
		/* "0x" and then something else is still text if it fits. */
	}
	if (pw_hex(arg, len, pw))
		return 0;
	if (len <= ODI_GPON_PW_LEN) {
		memset(pw, 0, ODI_GPON_PW_LEN);
		memcpy(pw, arg, len);
		return 0;
	}
	memset(pw, 0, ODI_GPON_PW_LEN);
	return -EINVAL;
}
