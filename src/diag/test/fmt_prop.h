/* Randomised cases for out_fmt, generated identically on both sides.
 *
 * fmt_cases.h is a hand-written list of the conversions the vendor's format
 * strings actually use. It is necessary and it was not sufficient: its longest
 * %s was 34 characters, and out_fmt truncated %s at 71 for months because the
 * conversion was routed through a buffer sized for numerics. A list of cases
 * only ever finds the bugs someone thought of.
 *
 * So this generates them instead. The host driver (glibc printf) and the
 * target driver (out_fmt, under qemu) both compile THIS header, seed the same
 * PRNG and walk the same sequence, so the two outputs must match line for
 * line. No libc is used here: the target side links -nostdlib.
 *
 * Deliberately one conversion per case. That keeps the argument type knowable
 * at the call site without a varargs trampoline, and it is enough -- the bug
 * that motivated this was a single %s.
 *
 * What is NOT generated, because out_fmt does not claim it and glibc does:
 * the '+', ' ' and '#' flags (parsed and ignored), '0' on %s or %c (undefined
 * in C, and glibc zero-pads both), a precision on %c, and any flag or width
 * on %%.
 */
#ifndef ODI_FMT_PROP_H
#define ODI_FMT_PROP_H

#include <stdint.h>

#define FP_CASES   4000
#define FP_STR_MAX 320

/* Argument kind for the generated case. */
enum { FP_SIGNED, FP_UNSIGNED, FP_STR, FP_CHAR, FP_NONE };

struct fp_case {
	char     fmt[40];
	int      kind;
	int      wide;         /* 0 none, 1 'l', 2 'll' -- selects the arg type */
	int64_t  sv;
	uint64_t uv;
	int      ch;
	char     str[FP_STR_MAX];
};

/* xorshift32: same sequence on every host, and no libc. */
static uint32_t fp_state = 2463534242u;

static uint32_t fp_rand(void)
{
	uint32_t x = fp_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	fp_state = x;
	return x;
}

static void fp_seed(uint32_t s)
{
	fp_state = s ? s : 2463534242u;
}

static uint32_t fp_below(uint32_t n)
{
	return n ? fp_rand() % n : 0;
}

/* Values drawn from the edges as often as from the middle: the interesting
 * inputs to a formatter are zero, the signed boundaries, and all-ones. */
static uint32_t fp_pick32(void)
{
	switch (fp_below(8)) {
	case 0:  return 0;
	case 1:  return 1;
	case 2:  return 0xffffffffu;         /* -1 signed */
	case 3:  return 0x80000000u;         /* INT32_MIN signed */
	case 4:  return 0x7fffffffu;
	default: return fp_rand();
	}
}

static uint64_t fp_pick64(void)
{
	switch (fp_below(8)) {
	case 0:  return 0;
	case 1:  return 1;
	case 2:  return 0xffffffffull;
	case 3:  return 0xffffffffffffffffull;
	case 4:  return 0x8000000000000000ull;
	case 5:  return 0x100000000ull;
	default: return ((uint64_t)fp_rand() << 32) | fp_rand();
	}
}

static char *fp_put_num(char *p, uint32_t v)
{
	if (v >= 100)
		*p++ = (char)('0' + v / 100);
	if (v >= 10)
		*p++ = (char)('0' + (v / 10) % 10);
	*p++ = (char)('0' + v % 10);
	return p;
}

/* Build case `i` into `c`. */
static void fp_build(struct fp_case *c)
{
	static const char conv[] = { 'd', 'i', 'u', 'x', 'X', 's', 'c', '%' };
	char k = conv[fp_below(sizeof conv)];
	char *p = c->fmt;
	int left, zero;

	c->wide = 0;
	c->kind = FP_NONE;

	if (k == '%') {
		*p++ = '%';
		*p++ = '%';
		*p = '\0';
		return;
	}

	left = (int)fp_below(2);
	zero = (int)fp_below(2) && k != 's' && k != 'c';

	*p++ = '%';
	if (left)
		*p++ = '-';
	if (zero)
		*p++ = '0';

	/* Widths and precisions deliberately reach past 71, and past any
	 * plausible scratch buffer: that is the class of bug this exists for. */
	if (fp_below(2))
		p = fp_put_num(p, fp_below(120));
	if (fp_below(2) && k != 'c') {
		*p++ = '.';
		p = fp_put_num(p, fp_below(120));
	}

	if (k != 's' && k != 'c') {
		c->wide = (int)fp_below(3);
		if (c->wide == 2) {
			*p++ = 'l';
			*p++ = 'l';
		} else if (c->wide == 1) {
			*p++ = 'l';
		}
	}

	*p++ = k;
	*p = '\0';

	switch (k) {
	case 'd':
	case 'i':
		c->kind = FP_SIGNED;
		c->sv = c->wide == 2 ? (int64_t)fp_pick64()
		                     : (int64_t)(int32_t)fp_pick32();
		break;
	case 'u':
	case 'x':
	case 'X':
		c->kind = FP_UNSIGNED;
		c->uv = c->wide == 2 ? fp_pick64() : (uint64_t)fp_pick32();
		break;
	case 's': {
		/* Lengths from 0 to well past the old 71-character cliff. */
		uint32_t n = fp_below(FP_STR_MAX - 1);
		uint32_t j;

		for (j = 0; j < n; j++)
			c->str[j] = (char)('!' + fp_below(93));   /* printable */
		c->str[n] = '\0';
		c->kind = FP_STR;
		break;
	}
	case 'c':
		c->ch = (int)('!' + fp_below(93));
		c->kind = FP_CHAR;
		break;
	}
}

/* Both drivers dispatch through this, so the argument types differ only by
 * which printf is substituted. The casts matter: on MIPS o32 `long` is 32 bits
 * and on the host it is 64, so an 'l' case must carry a value that fits in 32
 * either way -- fp_pick32 guarantees it -- and an 'll' case must be passed as
 * an explicit int64_t/uint64_t on both. */
#define FP_EMIT(PRINTF, c)                                                    \
	do {                                                                  \
		switch ((c)->kind) {                                          \
		case FP_SIGNED:                                               \
			if ((c)->wide == 2)                                   \
				PRINTF((c)->fmt, (int64_t)(c)->sv);           \
			else if ((c)->wide == 1)                              \
				PRINTF((c)->fmt, (long)(c)->sv);              \
			else                                                  \
				PRINTF((c)->fmt, (int)(c)->sv);               \
			break;                                                \
		case FP_UNSIGNED:                                             \
			if ((c)->wide == 2)                                   \
				PRINTF((c)->fmt, (uint64_t)(c)->uv);          \
			else if ((c)->wide == 1)                              \
				PRINTF((c)->fmt, (unsigned long)(c)->uv);     \
			else                                                  \
				PRINTF((c)->fmt, (unsigned)(c)->uv);          \
			break;                                                \
		case FP_STR:                                                  \
			PRINTF((c)->fmt, (c)->str);                           \
			break;                                                \
		case FP_CHAR:                                                 \
			PRINTF((c)->fmt, (c)->ch);                            \
			break;                                                \
		default:            /* "%%" -- no argument at all */          \
			PRINTF((c)->fmt);                                     \
			break;                                                \
		}                                                             \
	} while (0)

#endif
