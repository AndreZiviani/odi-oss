/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Run a fuzz target over saved inputs, with no fuzzer.
 *
 *   replay [-v] <file or directory>...
 *
 * The target is whatever LLVMFuzzerTestOneInput() this is linked with. Each
 * file is one input; a directory is every file in it, in name order, so two
 * runs do the same thing. -v shows what omcid prints and sends for each.
 * This is how the corpus runs as a test under any host compiler (`make
 * replay`), and how a crash the fuzzer saved is looked at again.
 *
 * An input that breaks omcid stops the program where it breaks, under a
 * sanitizer or by abort(); the name of the input is printed first, so the
 * last name in the output is the one to look at.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "harness.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define INPUT_MAX (1u << 20)

static unsigned ran;
static int verbose;

/* What an input has to make omcid print: the lines of the file of the same
 * name in <directory>.expect, each to be found somewhere in the output of
 * the run. seeds.py writes that file for a corpus input with "# expect:"
 * lines (beside the seeds and not among them: libFuzzer reads every file
 * of a corpus directory as an input). A sanitizer says an input did
 * harm; this says the input was answered the way its fix answers it, with
 * no sanitizer in the build, and that the input still gets as far as the
 * code it was written for. */
static int expected(const char *path)
{
	char name[4096], line[512];
	const char *out;
	size_t len;
	FILE *f;
	int rc = 0;

	const char *base = strrchr(path, '/');

	if (!base)
		return 0;
	snprintf(name, sizeof name, "%.*s.expect%s", (int)(base - path), path,
		 base);
	f = fopen(name, "r");
	if (!f)
		return 0;
	out = fk_output(&len);
	while (fgets(line, sizeof line, f)) {
		size_t n = strcspn(line, "\r\n"), at = 0;

		if (!n)
			continue;
		while (at + n <= len && memcmp(out + at, line, n) != 0)
			at++;
		if (at + n > len) {
			line[n] = 0;
			fprintf(stderr, "replay: %s: omcid did not print \"%s\"\n",
				path, line);
			rc = 1;
		}
	}
	fclose(f);
	return rc;
}

static int run_file(const char *path)
{
	static uint8_t buf[INPUT_MAX];
	FILE *f = fopen(path, "rb");
	uint64_t first = 0;
	uint8_t *copy;
	size_t n;
	int rc;

	if (!f) {
		perror(path);
		return 1;
	}
	n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (verbose)
		fprintf(stderr, "== %s (%zu bytes)\n", path, n);
	else
		fprintf(stderr, "%s\n", path);
	/* A heap copy of exactly the input, so a read past its end is seen by
	 * AddressSanitizer as it would be under the fuzzer. */
	copy = malloc(n ? n : 1);
	if (!copy)
		return 1;
	memcpy(copy, buf, n);
	/* Twice, and omcid has to let out the same both times: what it
	 * printed, the frames it sent, the driver commands. A difference is
	 * state that crossed from one run into the next, past state.c, and
	 * with it no saved input reproduces on its own. Not when the run is
	 * shown: once is enough to read. */
	for (int pass = 0; pass < (verbose ? 1 : 2); pass++) {
		LLVMFuzzerTestOneInput(copy, n);
		if (pass == 0)
			first = fk_digest();
	}
	free(copy);
	rc = expected(path);
	if (fk_digest() != first) {
		fprintf(stderr, "replay: %s: a second run of this input did not "
			"go as the first\n", path);
		rc = 1;
	}
	ran++;
	return rc;
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static int run_path(const char *path)
{
	struct stat st;
	struct dirent **list;
	int n, rc = 0;

	if (stat(path, &st) != 0) {
		perror(path);
		return 1;
	}
	if (!S_ISDIR(st.st_mode))
		return run_file(path);
	n = scandir(path, &list, 0, by_name);
	if (n < 0) {
		perror(path);
		return 1;
	}
	for (int i = 0; i < n; i++) {
		char sub[4096];

		if (list[i]->d_name[0] != '.') {
			snprintf(sub, sizeof sub, "%s/%s", path, list[i]->d_name);
			rc |= run_path(sub);
		}
		free(list[i]);
	}
	free(list);
	return rc;
}

int main(int argc, char **argv)
{
	int rc = 0, first = 1;

	stick_capture = 1;
	if (argc > 1 && strcmp(argv[1], "-v") == 0) {
		verbose = 1;
		setenv("FUZZ_ECHO", "1", 1);
		first = 2;
	}
	if (first >= argc) {
		fprintf(stderr, "usage: %s [-v] <file or directory>...\n", argv[0]);
		return 2;
	}
	for (int i = first; i < argc; i++)
		rc |= run_path(argv[i]);
	fprintf(stderr, "replay: %u input(s)%s\n", ran, rc ? ", with errors" : "");
	return rc || !ran;
}
