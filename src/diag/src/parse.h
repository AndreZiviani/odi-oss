/* Matching a command line against the command table (table.h).
 *
 * The rules:
 *
 *   - a keyword matches on any prefix, so `pon get transc rx-power` works;
 *   - a fully typed keyword is exact and beats a partial match;
 *   - a position where two different keywords both match only partially is
 *     ambiguous, and the line is refused rather than guessed at.
 *
 * Freestanding and syscall-free, so it builds and is tested on the host.
 */
#ifndef ODI_PARSE_H
#define ODI_PARSE_H

#include <stdint.h>
#include "table.h"

#define PARSE_MAX_TOKENS 16
#define PARSE_MAX_ARGS   4

enum parse_status {
	PARSE_OK = 0,
	PARSE_EMPTY,          /* blank line, nothing to do */
	PARSE_UNRECOGNISED,   /* no command has this word here */
	PARSE_AMBIGUOUS,      /* the prefix matches several keywords */
	PARSE_INCOMPLETE,     /* input ran out before a complete command */
	PARSE_TRAILING,       /* a complete command, then more words */
	PARSE_BADVALUE,       /* a parameter that does not parse */
	PARSE_TOOLONG,        /* more words than any command has */
};

/* "all" is every port 0..HW_PORT_MAX with `all` set, so a handler can skip
 * the ports that do not answer instead of reporting each one. */
struct port_list {
	uint32_t bits;
	uint8_t lo, hi;
	uint8_t all;
};

struct parsed {
	const struct cmd_def *cmd;
	int nargs;                    /* numeric parameters, in order */
	uint32_t u[PARSE_MAX_ARGS];
	struct port_list ports;       /* the <ports> parameter, if any */
};

/* Parse one line. On failure *err_tok is the 0-based index of the offending
 * word, or -1 when the fault is not tied to one. */
enum parse_status parse_line(const char *line, struct parsed *out, int *err_tok);

/* The commands `line` could be the start of, for help. The last word may be
 * a partial keyword. Returns how many were written to `out`. */
int parse_matches(const char *line, const struct cmd_def *out[], int max);

int ports_has(const struct port_list *p, unsigned port);

const char *parse_status_text(enum parse_status s);

#endif
