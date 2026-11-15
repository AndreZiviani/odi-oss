/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "guard.h"

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* Each guarded key with the reason, and what to run instead where there is
 * something. */
static const struct {
	const char *key;
	const char *why;
} guarded[] = {
	{ "sw_commit",
	  "U-Boot boots this slot from now on; `nv commit <slot>` checks the slot and writes both copies" },
	{ "sw_active",
	  "U-Boot boots this slot with no trial; `nv setenv sw_tryactive <slot>` boots it once, then `nv commit <slot>`" },
	{ "mupgrade_en",
	  "1 lets U-Boot take an unauthenticated firmware image from the LAN at every boot" },
	{ "bootcmd",           "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "boot_by_commit",    "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "boot_by_tryactive", "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "b0",                "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "b1",                "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "set_act0",          "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "set_act1",          "U-Boot runs it at every boot; a broken value stops the stick in U-Boot" },
	{ "en_wdt",            "U-Boot runs it on a trial boot; without it a hung image is never reset" },
};

const char *nv_guard(const char *key, const char *value)
{
	unsigned int i;

	if (same(key, "sw_tryactive")) {
		if (!value)
			return "without sw_tryactive U-Boot trial-boots slot 1 at the next boot; `nv setenv sw_tryactive 2` cancels a trial";
		if (same(value, "0") || same(value, "1") || same(value, "2"))
			return 0;
		return "U-Boot trial-boots slot 1 for any value but 0 and 2; use 0 or 1 to boot that slot once, 2 for none";
	}
	for (i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
		if (same(key, guarded[i].key))
			return guarded[i].why;
	return 0;
}

int nv_setenv_parse(int argc, char **argv, struct nv_setenv_args *out)
{
	int a = 0;
	int i;

	out->force = 0;
	out->copy = 0;
	out->key = 0;
	out->value = 0;
	while (a < argc && argv[a][0] == '-') {
		if (same(argv[a], "--force") || same(argv[a], "-f")) {
			out->force = 1;
			a++;
		} else if (same(argv[a], "--")) {
			a++;
			break;
		} else if (same(argv[a], "-c")) {
			if (a + 1 >= argc)
				return NV_ARGS_USAGE;
			if (same(argv[a + 1], "1"))
				out->copy = 1;
			else if (same(argv[a + 1], "2"))
				out->copy = 2;
			else
				return NV_ARGS_COPY;
			a += 2;
		} else {
			return NV_ARGS_USAGE;
		}
	}
	if (a >= argc)
		return NV_ARGS_USAGE;
	/* Words after the value are ignored, as before and as the stock nv
	 * does. */
	out->key = argv[a];
	out->value = argc - a >= 2 ? argv[a + 1] : 0;
	if (!out->key[0])
		return NV_ARGS_NAME;
	for (i = 0; out->key[i]; i++)
		if (out->key[i] == '=')
			return NV_ARGS_NAME;
	return NV_ARGS_OK;
}
