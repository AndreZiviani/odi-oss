/* Parsers for the two /proc files our 6.18 drivers publish, shared by the
 * OMCI tools. Pure functions over a buffer the caller already read: no
 * syscalls, no libc, so test/procparse_test.c builds them natively.
 *
 *   /proc/odi_omci   first line "registered: type=1 pid=593 type=..." --
 *                    who holds each packet-redirect type
 *                    (kernel/extra/drivers/net/ethernet/odi/odi_omci.c,
 *                    odi_omci_proc_show).
 *   /proc/odi_gpon   a "sn <16 hex digits>" line -- the serial number the
 *                    GPON block ranges with -- and "alloc_ids <n> <id>...",
 *                    the Alloc-IDs the OLT assigned by PLOAM (odi_gpon.c,
 *                    odi_gpon_proc_show).
 */
#ifndef ODI_OMCI_PROCPARSE_H
#define ODI_OMCI_PROCPARSE_H

#include <stdint.h>

/* Start of the line holding `key` followed by a space or colon, or -1. Only
 * line starts count, so "sn" never matches inside another word. */
static inline int pp_find_line(const char *buf, unsigned len, const char *key)
{
	unsigned i = 0;

	while (i < len) {
		unsigned k = 0;

		while (key[k] && i + k < len && buf[i + k] == key[k])
			k++;
		if (!key[k] && i + k < len && (buf[i + k] == ' ' || buf[i + k] == ':'))
			return (int)i;
		while (i < len && buf[i] != '\n')
			i++;
		i++;
	}
	return -1;
}

static inline int pp_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Who holds redirect `type`, from /proc/odi_omci.
 *
 * Returns 1 and sets *pid when the "registered:" line names that type, 0 when
 * the line is there but the type is not on it (nobody holds it), and -1 when
 * there is no such line at all -- not our driver, or a truncated read -- which
 * callers must treat as "cannot tell", never as "free". */
static inline int pp_redirect_holder(const char *buf, unsigned len, unsigned type,
				     uint32_t *pid)
{
	int at = pp_find_line(buf, len, "registered");
	unsigned i;

	if (at < 0)
		return -1;
	i = (unsigned)at + 11;          /* past "registered:" */
	while (i < len && buf[i] != '\n') {
		uint32_t t = 0, p = 0;
		int nt = 0, np = 0;

		while (i < len && buf[i] == ' ')
			i++;
		if (i + 5 > len || buf[i] != 't' || buf[i + 1] != 'y' ||
		    buf[i + 2] != 'p' || buf[i + 3] != 'e' || buf[i + 4] != '=') {
			while (i < len && buf[i] != ' ' && buf[i] != '\n')
				i++;
			continue;
		}
		for (i += 5; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, nt++)
			t = t * 10 + (uint32_t)(buf[i] - '0');
		if (i + 5 > len || buf[i] != ' ' || buf[i + 1] != 'p' ||
		    buf[i + 2] != 'i' || buf[i + 3] != 'd' || buf[i + 4] != '=')
			continue;
		for (i += 5; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, np++)
			p = p * 10 + (uint32_t)(buf[i] - '0');
		if (nt && np && t == type) {
			*pid = p;
			return 1;
		}
	}
	return 0;
}

/* The eight serial-number bytes from /proc/odi_gpon's "sn" line: four ASCII
 * vendor bytes and four binary ones, printed as sixteen hex digits. Returns 0
 * and fills sn[] on a well-formed line; -1 otherwise. An all-zero serial is
 * returned as read -- deciding that it means "not set yet" is the caller's
 * business (pp_sn_is_zero). */
static inline int pp_gpon_sn(const char *buf, unsigned len, uint8_t sn[8])
{
	int at = pp_find_line(buf, len, "sn");
	unsigned i;
	uint8_t tmp[8];

	if (at < 0)
		return -1;
	i = (unsigned)at + 3;
	if (i + 16 > len)
		return -1;
	for (unsigned k = 0; k < 8; k++) {
		int hi = pp_hexval(buf[i + 2 * k]), lo = pp_hexval(buf[i + 2 * k + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		tmp[k] = (uint8_t)((hi << 4) | lo);
	}
	if (i + 16 < len && buf[i + 16] != '\n')
		return -1;
	for (unsigned k = 0; k < 8; k++)
		sn[k] = tmp[k];
	return 0;
}

/* The ONU state number from /proc/odi_gpon's first line, "state 5 (O5)":
 * 1 to 7 for O1 to O7, or -1 when there is no such line or the number is
 * not a digit. "cannot tell" is -1, never a state. */
static inline int pp_gpon_state(const char *buf, unsigned len)
{
	int at = pp_find_line(buf, len, "state");
	unsigned i;

	if (at < 0)
		return -1;
	i = (unsigned)at + 6;
	if (i >= len || buf[i] < '0' || buf[i] > '9')
		return -1;
	if (i + 1 < len && buf[i + 1] >= '0' && buf[i + 1] <= '9')
		return -1;
	return buf[i] - '0';
}

/* The Alloc-IDs from /proc/odi_gpon's "alloc_ids" line: a count, then
 * that many decimal ids, in CAM row order (the order the OLT assigned
 * them, a released row reused first). Returns the number stored in out[]
 * (at most max), or -1 when there is no such line or it is malformed:
 * short of its count, an id past 4095 (a 12-bit G.984.3 Alloc-ID), or
 * anything else on the line. "cannot tell" and "none assigned" differ,
 * and a caller treats -1 as the former. */
static inline int pp_gpon_alloc_ids(const char *buf, unsigned len, uint16_t *out,
				    unsigned max)
{
	int at = pp_find_line(buf, len, "alloc_ids");
	unsigned i, count = 0, got = 0;
	int digits = 0;

	if (at < 0)
		return -1;
	i = (unsigned)at + 9;                   /* past "alloc_ids" */
	if (i >= len || buf[i] != ' ')
		return -1;
	for (i++; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, digits++)
		if ((count = count * 10 + (unsigned)(buf[i] - '0')) > 4096)
			return -1;
	if (!digits)
		return -1;
	for (unsigned k = 0; k < count; k++) {
		uint32_t v = 0;

		if (i >= len || buf[i] != ' ')
			return -1;
		digits = 0;
		for (i++; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, digits++)
			if ((v = v * 10 + (uint32_t)(buf[i] - '0')) > 4095)
				return -1;
		if (!digits)
			return -1;
		if (got < max)
			out[got++] = (uint16_t)v;
	}
	if (i < len && buf[i] != '\n')
		return -1;
	return (int)got;
}

/* The config store keeps GPON_SN as it is printed on the label: four vendor
 * characters then eight hex digits ("ABCD0011AAFF"). Same result shape as
 * pp_gpon_sn. */
static inline int pp_label_sn(const char *s, unsigned len, uint8_t sn[8])
{
	uint8_t tmp[8];

	if (len != 12)
		return -1;
	for (unsigned k = 0; k < 4; k++) {
		if (s[k] < 0x20 || s[k] > 0x7e)
			return -1;
		tmp[k] = (uint8_t)s[k];
	}
	for (unsigned k = 0; k < 4; k++) {
		int hi = pp_hexval(s[4 + 2 * k]), lo = pp_hexval(s[5 + 2 * k]);

		if (hi < 0 || lo < 0)
			return -1;
		tmp[4 + k] = (uint8_t)((hi << 4) | lo);
	}
	for (unsigned k = 0; k < 8; k++)
		sn[k] = tmp[k];
	return 0;
}

static inline int pp_sn_is_zero(const uint8_t sn[8])
{
	for (unsigned k = 0; k < 8; k++)
		if (sn[k])
			return 0;
	return 1;
}

#endif
