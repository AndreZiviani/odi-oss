/* Host-side parser harness: feeds each line of stdin through parse_line()
 * and prints what it resolved, for the Makefile to diff against
 * test/cases.expected. A line starting with "? " is a help query instead:
 * the rest goes to parse_matches() and the matching syntax lines print.
 * Links libc, unlike the target build; parse.c and table.c need none. */
#include <stdio.h>
#include <string.h>
#include "parse.h"

/* parse.c sizes its working set at 64 commands. */
#define TABLE_LIMIT 64

static void show_parse(const char *line)
{
	struct parsed p;
	int err = -1;
	enum parse_status st = parse_line(line, &p, &err);

	printf("%s => ", line);
	if (st != PARSE_OK) {
		printf("%s", st == PARSE_EMPTY ? "(empty)" : parse_status_text(st));
		if (err >= 0)
			printf(" at word %d", err);
		printf("\n");
		return;
	}
	printf("[%s]", p.cmd->syntax);
	for (int i = 0; i < p.nargs; i++)
		printf(" u%d=0x%x", i, p.u[i]);
	if (strstr(p.cmd->syntax, "<ports>")) {
		printf(" ports=%u-%u%s {", p.ports.lo, p.ports.hi,
		       p.ports.all ? " all" : "");
		for (unsigned k = 0; k <= 31; k++)
			if (ports_has(&p.ports, k) && !p.ports.all)
				printf(" %u", k);
		printf(" }");
	}
	printf("\n");
}

static void show_help(const char *line)
{
	const struct cmd_def *m[TABLE_LIMIT];
	int n = parse_matches(line, m, TABLE_LIMIT);

	printf("? %s => %d:", line, n);
	for (int i = 0; i < n; i++)
		printf(" [%s]", m[i]->syntax);
	printf("\n");
}

int main(void)
{
	char line[512];

	if (cmd_table_len > TABLE_LIMIT) {
		printf("table has %d commands, parse.c holds %d\n",
		       cmd_table_len, TABLE_LIMIT);
		return 1;
	}
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '?' && line[1] == ' ')
			show_help(line + 2);
		else
			show_parse(line);
	}
	return 0;
}
