/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_init_test.c -- host-side unit test for odi_init.c own
 * verb-bucket dispatch (odi_init_apply()), against STUB
 * odi_switch_sdkinit_verb()/odi_gpon_verb() definitions -- the real ones
 * pull in the whole switch/GPON driver (odi_switch_sdkinit_test.c and
 * odi_gpon_test.c already cover those directly); this test only checks
 * that odi_init.c routes each verb name to the right one of the
 * three buckets its own file header documents, and that a verb neither
 * bucket recognises AND that odi_switch_sdkinit_verb() refuses becomes
 * -ENOSYS rather than silently succeeding.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int stub_sdkinit_calls;
static char stub_sdkinit_last[64];
static int stub_sdkinit_ret = -1; /* -ENOENT-shaped default: "no data" */

static int stub_gpon_calls;
static char stub_gpon_last_verb[64];
static char stub_gpon_last_arg[64];
static int stub_gpon_ret;

/* odi_init.c's own two #includes declare these; defining them here
 * before including the .c file (unity build, same posture as
 * odi_gpon_irq_test.c) is what makes this a link, not a redeclaration
 * conflict -- odi_init.c never defines them itself.
 */
int odi_switch_sdkinit_verb(const char *verb)
{
	stub_sdkinit_calls++;
	strncpy(stub_sdkinit_last, verb, sizeof(stub_sdkinit_last) - 1);
	return stub_sdkinit_ret;
}

int odi_gpon_verb(const char *verb, const char *arg)
{
	stub_gpon_calls++;
	strncpy(stub_gpon_last_verb, verb, sizeof(stub_gpon_last_verb) - 1);
	stub_gpon_last_arg[0] = 0;
	if (arg)
		strncpy(stub_gpon_last_arg, arg, sizeof(stub_gpon_last_arg) - 1);
	return stub_gpon_ret;
}

static int stub_optics_calls;

int odi_board_optics(void)
{
	stub_optics_calls++;
	return 0;
}

#include "../kernel/extra/drivers/net/ethernet/odi/odi_init.c"

static int failures;
#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static void reset(void)
{
	stub_sdkinit_calls = 0;
	stub_sdkinit_last[0] = 0;
	stub_sdkinit_ret = -1;
	stub_gpon_calls = 0;
	stub_gpon_last_verb[0] = 0;
	stub_gpon_last_arg[0] = 0;
	stub_gpon_ret = 0;
}

int main(void)
{
	int rc;

	/* intr/irq: documented no-op, neither stub called. */
	reset();
	rc = odi_init_apply("intr", NULL);
	CHECK(rc == 0, "intr returns 0");
	CHECK(stub_sdkinit_calls == 0 && stub_gpon_calls == 0, "intr calls neither stub");

	reset();
	rc = odi_init_apply("irq", NULL);
	CHECK(rc == 0, "irq returns 0");
	CHECK(stub_sdkinit_calls == 0 && stub_gpon_calls == 0, "irq calls neither stub");

	/* optics: the board function, neither replay. */
	reset();
	stub_optics_calls = 0;
	rc = odi_init_apply("optics", NULL);
	CHECK(rc == 0 && stub_optics_calls == 1, "optics calls odi_board_optics() once");
	CHECK(stub_sdkinit_calls == 0 && stub_gpon_calls == 0, "optics calls neither replay");

	/* Every PON-step verb routes to odi_gpon_verb(), arg passed through. */
	{
		static const char *const gpon_verbs[] = {
			"gpondrv", "gpondev", "gponsn", "gponpw",
			"gponact", "gpondeact", "gponstat",
		};
		unsigned int i;

		for (i = 0; i < sizeof(gpon_verbs) / sizeof(gpon_verbs[0]); i++) {
			reset();
			stub_gpon_ret = 0;
			rc = odi_init_apply(gpon_verbs[i], "argtext");
			CHECK(rc == 0, "gpon verb returns the stub's ret");
			CHECK(stub_gpon_calls == 1, "gpon verb calls odi_gpon_verb once");
			CHECK(stub_sdkinit_calls == 0, "gpon verb never calls odi_switch_sdkinit_verb");
			CHECK(!strcmp(stub_gpon_last_verb, gpon_verbs[i]), "gpon verb name passed through");
			CHECK(!strcmp(stub_gpon_last_arg, "argtext"), "gpon verb arg passed through");
		}
	}

	/* A negative odi_gpon_verb() return is passed straight back. */
	reset();
	stub_gpon_ret = -EIO;
	rc = odi_init_apply("gponact", NULL);
	CHECK(rc == -EIO, "gpon verb failure code passed through");

	/* Every switch-table / PON-step-sharing-the-node verb routes to
	 * odi_switch_sdkinit_verb(); a 0 return there becomes 0 here. */
	{
		static const char *const replay_verbs[] = {
			"switch", "svlan", "stp", "oam", "acl", "qos", "sec",
			"rate", "classify", "stat", "trunk", "l2", "vlan",
			"port", "mirror", "cpu", "rldp", "trap", "gpio",
			"time", "ponmac", "i2c", "i2cen", "gpon", "rxsd",
		};
		unsigned int i;

		for (i = 0; i < sizeof(replay_verbs) / sizeof(replay_verbs[0]); i++) {
			reset();
			stub_sdkinit_ret = 0;
			rc = odi_init_apply(replay_verbs[i], NULL);
			CHECK(rc == 0, "replay verb returns 0 on replayed data");
			CHECK(stub_sdkinit_calls == 1, "replay verb calls odi_switch_sdkinit_verb once");
			CHECK(stub_gpon_calls == 0, "replay verb never calls odi_gpon_verb");
			CHECK(!strcmp(stub_sdkinit_last, replay_verbs[i]), "replay verb name passed through");
		}
	}

	/* No vendor fallback: a verb odi_switch_sdkinit_verb() refuses (no
	 * data this boot, or an outright unknown name) is -ENOSYS, never a
	 * silent success. */
	reset();
	stub_sdkinit_ret = -1;
	rc = odi_init_apply("switch", NULL);
	CHECK(rc == -ENOSYS, "no replay data -> -ENOSYS, no vendor fallback exists");

	reset();
	stub_sdkinit_ret = -1;
	rc = odi_init_apply("nonsense_verb", NULL);
	CHECK(rc == -ENOSYS, "unknown verb name -> -ENOSYS the same way");

	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	else
		printf("ok\n");
	return failures ? 1 : 0;
}
