/* The L2 listing line, natively: every row type, the column alignment
 * against the header, and the fields a reader zips by header word.
 *
 * The unicast row is the one a public stock listing shows (78:54:2E:07:64:63,
 * Spa 2, Fid 0, Age 6, Vid 1, Auto, SVL): our line must carry the same values
 * under the same header words, since odi-ui reads either listing that way. */
#include <stdio.h>
#include <string.h>
#include "l2.h"

static int failures;

static void check(int cond, const char *label)
{
	printf("  %-60s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

/* The value under header word `word` in `line`, by column position of the
 * word in the header: what a positional reader would see. */
static void field_at(const char *line, const char *word, char *out, int max)
{
	const char *h = strstr(l2_header, word);
	int col = h ? (int)(h - l2_header) : 0, n = 0;

	out[0] = '\0';
	if (!h || col >= (int)strlen(line))
		return;
	while (line[col + n] && line[col + n] != ' ' && n < max - 1) {
		out[n] = line[col + n];
		n++;
	}
	out[n] = '\0';
}

static int has(const char *line, const char *word, const char *want)
{
	char got[32];

	field_at(line, word, got, sizeof got);
	if (strcmp(got, want) != 0)
		printf("    %s: got \"%s\", want \"%s\"\n", word, got, want);
	return strcmp(got, want) == 0;
}

int main(void)
{
	struct odi_sw_l2_row r;
	char line[L2_LINE_MAX];

	puts("l2 listing lines:");

	memset(&r, 0, sizeof r);
	r.index = 8;
	r.type = ODI_SW_L2_UCAST;
	memcpy(r.mac, "\x78\x54\x2e\x07\x64\x63", 6);
	r.key = 1;
	r.port = 2;
	r.age = 6;
	r.flags = ODI_SW_L2_F_VALID;
	l2_format_row(&r, line);
	printf("    %s\n", line);
	check(strcmp(line, "78:54:2E:07:64:63 2   0   6   1    Auto   0   SVL  uc   -     0x008") == 0,
	      "unicast line, byte for byte");
	check(has(line, "Spa", "2") && has(line, "Fid", "0") && has(line, "Age", "6") &&
	      has(line, "Vid", "1") && has(line, "State", "Auto") && has(line, "Hash", "SVL"),
	      "unicast fields sit under their header words");

	r.flags |= ODI_SW_L2_F_STATIC | ODI_SW_L2_F_IVL | ODI_SW_L2_F_ARP | ODI_SW_L2_F_CTAG;
	r.port = 0;
	r.key = 4095;
	r.index = 1023;
	l2_format_row(&r, line);
	printf("    %s\n", line);
	check(has(line, "Age", "-") && has(line, "State", "Static") && has(line, "Hash", "IVL") &&
	      has(line, "Vid", "4095") && has(line, "Index", "0x3ff"),
	      "a static row has no age; the widest VID and row still align");
	check(strstr(line, " ctag arp") != NULL, "set unicast flags are named at the end");

	memset(&r, 0, sizeof r);
	r.index = 0xa1;
	r.type = ODI_SW_L2_MCAST;
	memcpy(r.mac, "\x01\x00\x5e\x01\x02\x03", 6);
	r.key = 10;
	r.ports = 0x1;
	r.flags = ODI_SW_L2_F_VALID | ODI_SW_L2_F_STATIC | ODI_SW_L2_F_IVL;
	l2_format_row(&r, line);
	printf("    %s\n", line);
	check(strcmp(line, "01:00:5E:01:02:03 -   -   -   10   Static -   IVL  mc   0x1   0x0a1") == 0,
	      "multicast line, byte for byte");

	r.ext_ports = 0x41;
	l2_format_row(&r, line);
	check(strstr(line, "0x0a1 ext 0x41") != NULL, "extension members are named when set");

	memset(&r, 0, sizeof r);
	r.index = 0x2f0;
	r.type = ODI_SW_L2_IPMC;
	r.group = 0x0010203;
	r.ports = 0x5;
	r.flags = ODI_SW_L2_F_VALID | ODI_SW_L2_F_STATIC;
	l2_format_row(&r, line);
	printf("    %s\n", line);
	check(strncmp(line, "224.1.2.3         -", 19) == 0 && has(line, "Type", "ipmc") &&
	      has(line, "Ports", "0x5"),
	      "an IPv4 route prints its group where the MAC goes");

	check((int)strlen(l2_header) < L2_LINE_MAX, "the header fits a line");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
