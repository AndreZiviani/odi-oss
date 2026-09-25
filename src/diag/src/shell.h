#ifndef ODI_SHELL_H
#define ODI_SHELL_H

/* The longest line the shell takes, from argv or stdin. */
#define SHELL_LINE_MAX 512

/* Parse, report and run one command line. Returns non-zero only when the
 * shell should stop. */
int shell_run_line(const char *line);

/* List the commands `line` is the start of: the '?' path. */
void shell_help(const char *line);

/* Printed before each line read from stdin. The exporter splits the output
 * of a batch on it, so it is part of the contract (README.md). */
#define PROMPT "RTK.0> "

#endif
