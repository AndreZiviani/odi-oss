#include "parse.h"
#include "hw.h"

/* Sized well above the table; parse_test checks the table fits. */
#define PARSE_MAX_CMDS 64

struct token {
	const char *text;
	int len;
};

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Split on runs of whitespace. Returns the word count, or -1 when there are
 * more than `max`. */
static int tokenise(const char *line, struct token *tok, int max)
{
	int n = 0;

	while (*line) {
		while (is_space(*line))
			line++;
		if (!*line)
			break;
		if (n == max)
			return -1;
		tok[n].text = line;
		while (*line && !is_space(*line))
			line++;
		tok[n].len = (int)(line - tok[n].text);
		n++;
	}
	return n;
}

/* The t-th word of a syntax line. Returns its length, 0 when there is none. */
static int word_at(const char *syn, int t, const char **w)
{
	struct token tok[PARSE_MAX_TOKENS];
	int n = tokenise(syn, tok, PARSE_MAX_TOKENS);

	if (t >= n)
		return 0;
	*w = tok[t].text;
	return tok[t].len;
}

static int word_count(const char *syn)
{
	struct token tok[PARSE_MAX_TOKENS];

	return tokenise(syn, tok, PARSE_MAX_TOKENS);
}

static int same(const char *a, int alen, const char *b, int blen)
{
	if (alen != blen)
		return 0;
	for (int i = 0; i < alen; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static int is_param(const char *w)
{
	return w[0] == '<';
}

static int is_ports_param(const char *w, int len)
{
	return same(w, len, "<ports>", 7);
}

/* ------------------------------------------------------------ values */

/* Decimal, or hex after 0x. The whole word must be consumed and the value
 * must fit 32 bits. */
static int parse_u32(const char *s, int len, uint32_t *out)
{
	uint64_t acc = 0;
	int base = 10, i = 0;

	if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		i = 2;
	}
	if (i == len)
		return -1;
	for (; i < len; i++) {
		char c = s[i];
		int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (base == 16 && c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (base == 16 && c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return -1;
		acc = acc * (uint64_t)base + (uint64_t)d;
		if (acc > 0xffffffffu)
			return -1;
	}
	*out = (uint32_t)acc;
	return 0;
}

int ports_has(const struct port_list *p, unsigned port)
{
	return port <= HW_PORT_MAX && ((p->bits >> port) & 1u);
}

/* "all", or a comma-separated list of ports and inclusive ranges. */
static int parse_ports(const char *s, int len, struct port_list *p)
{
	int i = 0, lo = HW_PORT_MAX + 1, hi = -1;

	p->bits = 0;
	p->all = 0;
	if (same(s, len, "all", 3)) {
		p->bits = 0xffffffffu;
		p->lo = 0;
		p->hi = HW_PORT_MAX;
		p->all = 1;
		return 0;
	}
	while (i < len) {
		uint32_t a, b;
		int start = i;

		while (i < len && s[i] != ',' && s[i] != '-')
			i++;
		if (parse_u32(s + start, i - start, &a) != 0 || a > HW_PORT_MAX)
			return -1;
		b = a;
		if (i < len && s[i] == '-') {
			start = ++i;
			while (i < len && s[i] != ',')
				i++;
			if (parse_u32(s + start, i - start, &b) != 0 ||
			    b > HW_PORT_MAX || b < a)
				return -1;
		}
		for (uint32_t n = a; n <= b; n++)
			p->bits |= (uint32_t)1 << n;
		if ((int)a < lo)
			lo = (int)a;
		if ((int)b > hi)
			hi = (int)b;
		if (i < len && s[i] == ',')
			i++;
		else if (i < len)
			return -1;
	}
	if (hi < 0)
		return -1;
	p->lo = (uint8_t)lo;
	p->hi = (uint8_t)hi;
	return 0;
}

static int value_ok(const char *w, int wlen, const struct token *t)
{
	struct port_list p;
	uint32_t u;

	if (is_ports_param(w, wlen))
		return parse_ports(t->text, t->len, &p) == 0;
	return parse_u32(t->text, t->len, &u) == 0;
}

/* ---------------------------------------------------------- matching */

enum { M_NONE, M_PARAM, M_PARTIAL, M_EXACT };

/* Narrow `alive` by word t. `prefix_only` is the help path: the word may be
 * the start of anything, a parameter included, and ambiguity is not an
 * error. Returns PARSE_OK or the reason nothing survived. */
static enum parse_status narrow(uint8_t *alive, int n, int t,
				const struct token *tok, int prefix_only)
{
	uint8_t how[PARSE_MAX_CMDS];
	int best = M_NONE, ended = 0, badvalue = 0;
	const char *partial = 0;       /* the first partial keyword seen */
	int partial_len = 0, partial_differs = 0;

	for (int c = 0; c < n; c++) {
		const char *w;
		int wlen;

		how[c] = M_NONE;
		if (!alive[c])
			continue;
		wlen = word_at(cmd_table[c].syntax, t, &w);
		if (!wlen) {
			ended = 1;
			continue;
		}
		if (is_param(w)) {
			if (prefix_only || value_ok(w, wlen, tok))
				how[c] = M_PARAM;
			else
				badvalue = 1;
		} else if (tok->len <= wlen && same(tok->text, tok->len, w, tok->len)) {
			how[c] = tok->len == wlen ? M_EXACT : M_PARTIAL;
			if (how[c] == M_PARTIAL && !partial) {
				partial = w;
				partial_len = wlen;
			} else if (how[c] == M_PARTIAL &&
				   !same(partial, partial_len, w, wlen)) {
				partial_differs = 1;
			}
		}
		if (how[c] > best)
			best = how[c];
	}

	/* A keyword beats a parameter, and an exact keyword beats a partial
	 * one. Two different partial keywords are only a problem when nothing
	 * typed out in full settles it. */
	if (best == M_PARTIAL && partial_differs && !prefix_only)
		return PARSE_AMBIGUOUS;
	for (int c = 0; c < n; c++)
		if (alive[c] && (how[c] == M_NONE ||
				 (!prefix_only && how[c] != best)))
			alive[c] = 0;
	if (best != M_NONE)
		return PARSE_OK;
	if (ended)
		return PARSE_TRAILING;
	return badvalue ? PARSE_BADVALUE : PARSE_UNRECOGNISED;
}

static int table_size(void)
{
	return cmd_table_len < PARSE_MAX_CMDS ? cmd_table_len : PARSE_MAX_CMDS;
}

enum parse_status parse_line(const char *line, struct parsed *out, int *err_tok)
{
	struct token tok[PARSE_MAX_TOKENS];
	uint8_t alive[PARSE_MAX_CMDS];
	int n = table_size(), ntok;
	const struct cmd_def *cmd = 0;

	out->cmd = 0;
	out->nargs = 0;
	*err_tok = -1;

	ntok = tokenise(line, tok, PARSE_MAX_TOKENS);
	if (ntok < 0)
		return PARSE_TOOLONG;
	if (ntok == 0)
		return PARSE_EMPTY;

	for (int c = 0; c < n; c++)
		alive[c] = 1;
	for (int t = 0; t < ntok; t++) {
		enum parse_status st = narrow(alive, n, t, &tok[t], 0);

		if (st != PARSE_OK) {
			*err_tok = t;
			return st;
		}
	}
	for (int c = 0; c < n && !cmd; c++)
		if (alive[c] && word_count(cmd_table[c].syntax) == ntok)
			cmd = &cmd_table[c];
	if (!cmd)
		return PARSE_INCOMPLETE;

	/* Decode the parameters of the one command left. */
	for (int t = 0; t < ntok; t++) {
		const char *w = "";
		int wlen = word_at(cmd->syntax, t, &w);

		if (!wlen || !is_param(w))
			continue;
		if (is_ports_param(w, wlen)) {
			parse_ports(tok[t].text, tok[t].len, &out->ports);
		} else if (out->nargs < PARSE_MAX_ARGS) {
			parse_u32(tok[t].text, tok[t].len, &out->u[out->nargs]);
			out->nargs++;
		}
	}
	out->cmd = cmd;
	return PARSE_OK;
}

int parse_matches(const char *line, const struct cmd_def *out[], int max)
{
	struct token tok[PARSE_MAX_TOKENS];
	uint8_t alive[PARSE_MAX_CMDS];
	int n = table_size(), ntok, found = 0, trailing_space = 0;
	const char *p = line;

	while (*p)
		p++;
	if (p != line && is_space(p[-1]))
		trailing_space = 1;
	ntok = tokenise(line, tok, PARSE_MAX_TOKENS);
	if (ntok < 0)
		return 0;

	for (int c = 0; c < n; c++)
		alive[c] = 1;
	/* Every word typed out in full narrows as the parser would; the last
	 * one, when the line does not end in a space, is only a prefix. A
	 * command shorter than the line has already dropped out, so what is
	 * left is every command the line is the start of. */
	for (int t = 0; t < ntok; t++) {
		int last = t == ntok - 1 && !trailing_space;

		if (narrow(alive, n, t, &tok[t], last) != PARSE_OK)
			return 0;
	}
	for (int c = 0; c < n; c++)
		if (alive[c] && found < max)
			out[found++] = &cmd_table[c];
	return found;
}

const char *parse_status_text(enum parse_status s)
{
	switch (s) {
	case PARSE_OK:           return "ok";
	case PARSE_EMPTY:        return "";
	case PARSE_UNRECOGNISED: return "Unrecognized command";
	case PARSE_AMBIGUOUS:    return "Ambiguous command";
	case PARSE_INCOMPLETE:   return "Incomplete command";
	case PARSE_TRAILING:     return "Trailing garbage after command";
	case PARSE_BADVALUE:     return "Invalid value";
	case PARSE_TOOLONG:      return "Command too long";
	}
	return "Unknown error";
}
