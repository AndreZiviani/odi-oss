/* Target side of the out_fmt property test: the generated cases through
 * out_fmt, under qemu. Its output is diffed against fmt_prop_ref's, so any
 * disagreement with a real printf fails the build rather than showing up as a
 * truncated value on the stick.
 *
 * Freestanding -- no libc -- so the seed is parsed by hand. */
#include "io.h"
#include "fmt_prop.h"

static struct fp_case c;

int main(int argc, char **argv)
{
	uint32_t seed = 1;
	int i;

	if (argc > 1) {
		const char *p = argv[1];

		seed = 0;
		while (*p >= '0' && *p <= '9')
			seed = seed * 10 + (uint32_t)(*p++ - '0');
	}
	fp_seed(seed);
	for (i = 0; i < FP_CASES; i++) {
		fp_build(&c);
		FP_EMIT(out_fmt, &c);
		out_char('\n');
	}
	out_flush();
	return 0;
}
