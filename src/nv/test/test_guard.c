/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Host-side tests for the keys `nv setenv` writes only with --force, and
 * for its argument parsing (guard.h).
 *
 *     cc -o t test_guard.c ../guard.c && ./t
 */
#include <stdio.h>
#include "../guard.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static int parse(struct nv_setenv_args *a, int argc, ...)
{
	char *argv[8];
	__builtin_va_list ap;
	int i;

	__builtin_va_start(ap, argc);
	for (i = 0; i < argc; i++)
		argv[i] = __builtin_va_arg(ap, char *);
	__builtin_va_end(ap);
	return nv_setenv_parse(argc, argv, a);
}

int main(void)
{
	static const char *const boot_keys[] = {
		"sw_commit", "sw_active", "mupgrade_en", "bootcmd",
		"boot_by_commit", "boot_by_tryactive", "b0", "b1",
		"set_act0", "set_act1", "en_wdt",
	};
	struct nv_setenv_args a;
	unsigned int i;
	int all = 1;

	/* Every key the boot script reads is refused, set or unset. */
	for (i = 0; i < sizeof boot_keys / sizeof boot_keys[0]; i++)
		if (!nv_guard(boot_keys[i], "0") || !nv_guard(boot_keys[i], 0))
			all = 0;
	ck(all, "every boot key is refused, set and unset");

	ck(!nv_guard("sw_tryactive", "0"), "sw_tryactive 0 is open");
	ck(!nv_guard("sw_tryactive", "1"), "sw_tryactive 1 is open");
	ck(!nv_guard("sw_tryactive", "2"), "sw_tryactive 2 is open");
	ck(nv_guard("sw_tryactive", "3") != 0, "sw_tryactive 3 is refused (U-Boot boots slot 1)");
	ck(nv_guard("sw_tryactive", "") != 0, "an empty sw_tryactive is refused");
	ck(nv_guard("sw_tryactive", "00") != 0, "sw_tryactive 00 is refused (the script compares a string)");
	ck(nv_guard("sw_tryactive", 0) != 0, "unsetting sw_tryactive is refused");

	ck(!nv_guard("sw_version0", "v1.16.0") && !nv_guard("sw_version1", 0),
	   "the version strings are open");
	ck(!nv_guard("ipaddr", "192.168.1.1") && !nv_guard("foo", 0), "any other key is open");
	ck(!nv_guard("sw_commi", "0") && !nv_guard("sw_commit2", "0") && !nv_guard("b", "0"),
	   "only whole key names match");
	ck(nv_guard("sw_commit", "0") != 0 && nv_guard("sw_active", "1") != 0,
	   "the refusal says why");

	ck(parse(&a, 2, "sw_tryactive", "1") == NV_ARGS_OK && !a.force && !a.copy &&
	   a.key && a.value && a.value[0] == '1', "key value");
	ck(parse(&a, 1, "foo") == NV_ARGS_OK && a.value == 0, "a key alone unsets it");
	ck(parse(&a, 4, "-c", "2", "sw_commit", "0") == NV_ARGS_OK && a.copy == 2 && !a.force,
	   "-c 2 key value");
	ck(parse(&a, 5, "--force", "-c", "1", "sw_commit", "0") == NV_ARGS_OK &&
	   a.force && a.copy == 1, "--force -c 1 key value");
	ck(parse(&a, 5, "-c", "1", "-f", "sw_commit", "0") == NV_ARGS_OK &&
	   a.force && a.copy == 1, "-c 1 -f key value");
	ck(parse(&a, 4, "sw_version0", "a", "b", "c") == NV_ARGS_OK && a.value[0] == 'a',
	   "words after the value are ignored, as before");
	ck(parse(&a, 3, "-c", "3", "x") == NV_ARGS_COPY, "-c 3 is refused");
	ck(parse(&a, 1, "-c") == NV_ARGS_USAGE, "-c with nothing after it is usage");
	ck(parse(&a, 2, "-c", "1") == NV_ARGS_USAGE, "no key after -c 1 is usage");
	ck(parse(&a, 1, "--force") == NV_ARGS_USAGE, "--force alone is usage");
	ck(parse(&a, 0) == NV_ARGS_USAGE, "nothing is usage");
	ck(parse(&a, 2, "--frce", "x") == NV_ARGS_USAGE, "an unknown option is usage");

	printf("%s: %d failed\n", fails ? "FAIL" : "ok", fails);
	return fails ? 1 : 0;
}
