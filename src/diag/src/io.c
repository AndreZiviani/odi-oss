#include "io.h"
#include "sys.h"
#include <stdarg.h>

#define OUTBUF 2048

static char buf[OUTBUF];
static int used;

/* Where flushed bytes go, so a program that answers over something other than
 * stdout (omcid replies on a message queue) redirects once. A sink is expected
 * to buffer: out_char flushes on every newline, which would be one message per
 * line for a queue. */
static void (*sink)(const char *, int);

void out_set_sink(void (*fn)(const char *, int))
{
	out_flush();
	sink = fn;
}

void out_flush(void)
{
	int off = 0;

	if (sink) {
		if (used)
			sink(buf, used);
		used = 0;
		return;
	}
	while (off < used) {
		long n = sys_write(STDOUT_FILENO, buf + off, (unsigned long)(used - off));

		if (n <= 0)
			break;   /* the caller has no recovery for a dead stdout */
		off += (int)n;
	}
	used = 0;
}

void out_char(char c)
{
	if (used == OUTBUF)
		out_flush();
	buf[used++] = c;
	if (c == '\n')
		out_flush();
}

void out_n(const char *s, int n)
{
	while (n-- > 0)
		out_char(*s++);
}

void out(const char *s)
{
	while (*s)
		out_char(*s++);
}

void out_udec(unsigned long v)
{
	char tmp[24];
	int i = 0;

	if (!v) {
		out_char('0');
		return;
	}
	while (v) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i--)
		out_char(tmp[i]);
}

void out_dec(long v)
{
	if (v < 0) {
		out_char('-');
		out_udec((unsigned long)-v);
	} else {
		out_udec((unsigned long)v);
	}
}

void out_hex(unsigned long v, int width)
{
	static const char digits[] = "0123456789abcdef";
	char tmp[16];
	int i = 0;

	if (!v)
		tmp[i++] = '0';
	while (v) {
		tmp[i++] = digits[v & 0xf];
		v >>= 4;
	}
	while (i < width)
		tmp[i++] = '0';
	while (i--)
		out_char(tmp[i]);
}

/* n / 10 and n % 10 without a 64-bit divide.
 *
 * q approximates n * 0.8 by shifts, then folds the error down; the remainder
 * is recovered as n - 10q and corrected once. Every shift is by a constant, so
 * gcc emits them inline rather than calling __lshrdi3, and there is no
 * __udivdi3 -- neither exists in a -nostdlib link. */
static uint64_t divmod10(uint64_t n, unsigned *rem)
{
	uint64_t q = (n >> 1) + (n >> 2);
	unsigned r;

	q += q >> 4;
	q += q >> 8;
	q += q >> 16;
	q += q >> 32;
	q >>= 3;
	r = (unsigned)(n - ((q << 3) + (q << 1)));
	if (r > 9) {
		q += 1;
		r -= 10;
	}
	*rem = r;
	return q;
}

void out_u64(uint64_t v)
{
	char tmp[24];
	int i = 0;

	if (!v) {
		out_char('0');
		return;
	}
	while (v) {
		unsigned r;

		v = divmod10(v, &r);
		tmp[i++] = (char)('0' + r);
	}
	while (i--)
		out_char(tmp[i]);
}

static int u64_digits(uint64_t v)
{
	int n = 1;

	while (v > 9) {
		unsigned r;

		v = divmod10(v, &r);
		n++;
	}
	return n;
}

void out_str_pad(const char *s, int width)
{
	int n = 0;

	while (s[n]) {
		out_char(s[n]);
		n++;
	}
	while (n++ < width)
		out_char(' ');
}

void out_u64_pad(uint64_t v, int width)
{
	int n = u64_digits(v);

	while (n++ < width)
		out_char(' ');
	out_u64(v);
}

/* Render the digits of one numeric conversion into `buf`, returning their
 * count. Sign, precision zeros and width padding are the job of the caller, so
 * a precision wider than the scratch buffer is never silently capped by it.
 * Only the digits are bounded here, and a u64 has at most twenty. */
static int fmt_digits(char *buf, char conv, uint64_t u, int64_t d, int prec,
                      int *neg)
{
	char tmp[24];
	int t = 0, n = 0;

	*neg = 0;
	if (conv == 'd') {
		if (d < 0) {
			*neg = 1;
			u = (uint64_t)-(uint64_t)d;
		} else {
			u = (uint64_t)d;
		}
		conv = 'u';
	}
	/* An explicit precision of zero prints nothing for a zero value. */
	if (!u && prec == 0)
		return 0;
	do {
		if (conv == 'u') {
			unsigned r;

			u = divmod10(u, &r);
			tmp[t++] = (char)('0' + r);
		} else {
			/* Neither base may use / or %% on a uint64_t: -nostdlib
			 * means no libgcc, so __udivdi3 would not link. Base ten
			 * goes through divmod10(), base sixteen through a shift. */
			const char *digits = conv == 'X' ? "0123456789ABCDEF"
			                                 : "0123456789abcdef";

			tmp[t++] = digits[u & 0xf];
			u >>= 4;
		}
	} while (u);
	while (t--)
		buf[n++] = tmp[t];
	return n;
}

/* Supports the conversions the stock diag format strings use, and no more:
 * "%-13s", "%02x", "%25llu", "%2.2x", "%8d". Flags are '-' and '0', with an
 * optional width, precision, and an l/ll length that only decides which
 * va_arg is taken. A full printf is most of a libc. */
void out_fmt(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		int left = 0, zero = 0, width = 0, prec = -1, wide = 0;
		char buf[24];   /* digits only: a u64 has at most twenty */
		int n, pad, zeros, neg = 0;
		char conv;

		if (*fmt != '%') {
			out_char(*fmt);
			continue;
		}
		fmt++;
		for (; *fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#'; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			prec = 0;
			for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++)
				prec = prec * 10 + (*fmt - '0');
		}
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
			if (*fmt == 'l')
				wide++;
			fmt++;
		}
		conv = *fmt;
		if (conv == '\0') {
			va_end(ap);
			return;
		}
		if (conv == '%') {
			out_char('%');
			continue;
		}
		if (conv == 'c') {
			char c = (char)va_arg(ap, int);

			pad = width - 1;
			if (!left)
				while (pad-- > 0)
					out_char(' ');
			out_char(c);
			if (left)
				while (pad-- > 0)
					out_char(' ');
			continue;
		}
		if (conv == 's') {
			/* Emitted directly, never through a scratch buffer: a
			 * U-Boot environment value runs to 196 bytes and more. */
			const char *s = va_arg(ap, const char *);

			if (!s)
				s = "(null)";
			n = 0;
			while (s[n] && (prec < 0 || n < prec))
				n++;
			pad = width - n;
			if (!left)
				while (pad-- > 0)
					out_char(' ');
			out_n(s, n);
			if (left)
				while (pad-- > 0)
					out_char(' ');
			continue;
		}
		if (conv == 'd' || conv == 'i') {
			int64_t d = wide >= 2 ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, long);

			n = fmt_digits(buf, 'd', 0, d, prec, &neg);
		} else if (conv == 'u' || conv == 'x' || conv == 'X') {
			uint64_t u = wide >= 2 ? va_arg(ap, uint64_t)
			                       : (uint64_t)va_arg(ap, unsigned long);

			n = fmt_digits(buf, conv, u, 0, prec, &neg);
		} else {
			out_char('%');
			out_char(conv);
			continue;
		}

		/* The sign precedes zero padding ("-0042", never "00-42");
		 * precision zeros count inside the width; an explicit precision
		 * cancels '0', as in printf; '-' pads on the right with spaces. */
		zeros = prec > n ? prec - n : 0;
		pad = width - (n + zeros + neg);
		if (left) {
			if (neg)
				out_char('-');
			while (zeros-- > 0)
				out_char('0');
			out_n(buf, n);
			while (pad-- > 0)
				out_char(' ');
		} else if (zero && prec < 0) {
			if (neg)
				out_char('-');
			while (pad-- > 0)
				out_char('0');
			out_n(buf, n);
		} else {
			while (pad-- > 0)
				out_char(' ');
			if (neg)
				out_char('-');
			while (zeros-- > 0)
				out_char('0');
			out_n(buf, n);
		}
	}
	va_end(ap);
}

int read_line(int fd, char *dst, int max)
{
	int n = 0;

	for (;;) {
		char c;
		long r = sys_read(fd, &c, 1);

		if (r <= 0)
			return n ? n : -1;   /* partial last line still counts */
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n < max - 1)
			dst[n++] = c;
	}
	dst[n] = '\0';
	return n;
}

int str_len(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

void str_copy(char *dst, const char *src, int max)
{
	int i = 0;

	while (src[i] && i < max - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

int str_has_prefix(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++)
		if (*s != *prefix)
			return 0;
	return 1;
}

/*
 * The compiler is entitled to emit calls to these four regardless of
 * -ffreestanding and -fno-builtin: a plain struct assignment becomes memcpy,
 * and a zeroed array becomes memset. Freestanding means "no libc", not "no
 * compiler-generated calls", so they have to exist somewhere.
 *
 * Built with -fno-tree-loop-distribute-patterns, without which gcc recognises
 * the loop inside memcpy as a memcpy and emits a call to itself.
 */

void *memcpy(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d < s || d >= s + n)
		return memcpy(dst, src, n);
	d += n;
	s += n;
	while (n--)
		*--d = *--s;
	return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *x = a, *y = b;

	for (; n--; x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}
