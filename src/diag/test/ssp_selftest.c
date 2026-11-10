/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The stack protector runtime of start.S, exercised for real under qemu
 * (test/ssp-test.sh):
 *
 *   ssp_selftest guard   prints __stack_chk_guard, so two runs can show it
 *                        was seeded from AT_RANDOM: different each time,
 *                        first byte zero, never the fallback value
 *   ssp_selftest smash   overruns a local array and returns; a working
 *                        protector ends the process in __stack_chk_fail
 *                        with SIGABRT before "returned" is printed
 *
 * Linked with start.o only, not io.o, so that a build of this same file
 * without the protector has no protected function at all, which is what
 * the negative case of check-hardening.sh needs. */
#include "sys.h"

extern unsigned long __stack_chk_guard;

static void say(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	sys_write(1, s, n);
}

static void say_hex(unsigned long v)
{
	static const char digits[] = "0123456789abcdef";
	static char line[12];

	line[0] = '0';
	line[1] = 'x';
	for (int i = 0; i < 8; i++)
		line[2 + i] = digits[(v >> (28 - 4 * i)) & 0xf];
	line[10] = '\n';
	sys_write(1, line, 11);
}

/* Writes n bytes into a 16-byte array. The volatile pointer keeps the
 * compiler from seeing the overrun and folding it away. */
static __attribute__((noinline)) int smash(int n)
{
	char buf[16];
	volatile char *p = buf;

	for (int i = 0; i < n; i++)
		p[i] = 'A';
	return p[0];
}

static int number(const char *s)
{
	int v = 0;

	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		say("usage: ssp_selftest guard|smash [bytes]\n");
		return 2;
	}
	if (argv[1][0] == 'g') {
		say_hex(__stack_chk_guard);
		return 0;
	}
	smash(argc > 2 ? number(argv[2]) : 64);
	say("returned\n");
	return 0;
}
