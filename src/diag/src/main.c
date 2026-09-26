/*
 * diag -- the switch, GPON and optics CLI for our own kernel.
 *
 * Two entry paths:
 *
 *   argc == 1   commands on stdin, one per line, each after a "RTK.0> "
 *               prompt, until `exit` or end of input
 *   argc >= 2   argv joined with spaces and run as one command, then exit
 *
 * The stdin path is how a caller amortises startup across a whole batch;
 * the exporter sends its whole scrape that way.
 */
#include "shell.h"
#include "io.h"
#include "sys.h"

#define LINE_MAX SHELL_LINE_MAX

static int run_argv(int argc, char **argv)
{
	char line[LINE_MAX];
	int len = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (i > 1 && len < LINE_MAX - 1)
			line[len++] = ' ';
		while (*a) {
			if (len >= LINE_MAX - 1) {
				out("% Command too long\n");
				out_flush();
				return 1;
			}
			line[len++] = *a++;
		}
	}
	line[len] = '\0';
	shell_run_line(line);
	out_flush();
	return 0;
}

/* A caller piping commands in sees "RTK.0> <command>" before each result,
 * and the exporter splits the output on exactly that. Lines are read whole,
 * so a terminal echoes for us and we echo only when it will not: hence the
 * tty probe. Getting it backwards on a terminal costs a doubled echo;
 * getting it backwards on a pipe would cost the exporter its separator. */
static int run_stream(void)
{
	char line[LINE_MAX];
	int tty = sys_isatty(STDIN_FILENO);

	for (;;) {
		out(PROMPT);
		out_flush();
		if (read_line(STDIN_FILENO, line, LINE_MAX) < 0)
			break;
		if (!tty) {
			out(line);
			out_char('\n');
		}
		if (shell_run_line(line))
			break;
	}
	out_char('\n');
	out_flush();
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2)
		return run_argv(argc, argv);

	return run_stream();
}
