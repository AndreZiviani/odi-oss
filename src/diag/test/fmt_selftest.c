/* Target side: the same cases through out_fmt, run under qemu. Its output is
 * diffed against fmt_ref's, so any disagreement with a real printf on a format
 * the vendor uses fails the build rather than showing up as a misaligned
 * column on the stick. */
#include "io.h"
#include "fmt_cases.h"

int main(void)
{
#define X(...) do { out_fmt(__VA_ARGS__); out_char('\n'); } while (0);
	FMT_CASES(X)
#undef X
	out_flush();
	return 0;
}
