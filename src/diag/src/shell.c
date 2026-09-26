#include "shell.h"
#include "parse.h"
#include "commands.h"
#include "io.h"

/* A caret under the offending word, then the reason. Nothing downstream
 * parses these: the exporter splits diag's output on the prompt string. */
static void report(const char *line, enum parse_status st, int err_tok)
{
	if (err_tok >= 0) {
		int col = 0, tok = 0, i = 0;

		while (line[i]) {
			while (line[i] == ' ' || line[i] == '\t')
				i++, col++;
			if (!line[i] || tok == err_tok)
				break;
			while (line[i] && line[i] != ' ' && line[i] != '\t')
				i++, col++;
			tok++;
		}
		for (int s = 0; s < col; s++)
			out_char(' ');
		out("^\n");
	}
	out_fmt("%% %s\n", parse_status_text(st));
}

void shell_help(const char *line)
{
	const struct cmd_def *list[CMD_LIST_MAX];
	int n = parse_matches(line, list, CMD_LIST_MAX);

	if (n <= 0) {
		out("% No matching command\n");
		return;
	}
	cmd_list(list, n);
}

int shell_run_line(const char *line)
{
	struct parsed p;
	int err_tok = -1, i, last = -1;
	enum parse_status st;

	/* A trailing '?' asks for help with what came before it. */
	for (i = 0; line[i]; i++)
		if (line[i] != ' ' && line[i] != '\t')
			last = i;
	if (last >= 0 && line[last] == '?') {
		char trimmed[SHELL_LINE_MAX];
		int n = last < SHELL_LINE_MAX - 1 ? last : SHELL_LINE_MAX - 1;

		for (i = 0; i < n; i++)
			trimmed[i] = line[i];
		trimmed[n] = '\0';
		shell_help(trimmed);
		return 0;
	}

	st = parse_line(line, &p, &err_tok);
	if (st == PARSE_EMPTY)
		return 0;
	if (st != PARSE_OK) {
		report(line, st, err_tok);
		return 0;
	}
	cmd_run(&p);
	return cmd_should_quit();
}
