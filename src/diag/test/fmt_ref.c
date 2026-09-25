/* Host side of the out_fmt test: the same cases through glibc's printf. */
#include <stdio.h>
#include "fmt_cases.h"

int main(void)
{
#define X(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0);
	FMT_CASES(X)
#undef X
	return 0;
}
