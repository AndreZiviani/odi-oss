/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Every run starts from the same omcid: the one a fresh process holds.
 *
 * omcid keeps its state in statics, in every translation unit and inside
 * functions, and has no way to reset most of it, because on a stick a new
 * state is a new process. A fuzzer calls it many thousand times in one
 * process. Left alone, the MIB of one input would still be there for the
 * next, a crash would depend on everything that ran before it, and no saved
 * input would reproduce on its own.
 *
 * So the writable data of omcid is put back before each run. The Makefile
 * renames .data and .bss of every omcid object to omcid_data and omcid_bss
 * (objcopy), the linker gives a section with such a name __start_ and
 * __stop_ symbols, and this file copies the first back from the image taken
 * before the first run and zeroes the second. Nothing in omcid is changed
 * for it, and nothing needs keeping in step when omcid grows a variable.
 * The Makefile checks that no omcid object has writable data left under
 * another name.
 *
 * The loops are spelled out, a word at a time with an empty asm statement
 * between, and this file is built without the sanitizers: the copy covers
 * the red zones AddressSanitizer puts between globals, which memcpy() and
 * an instrumented loop would both report, and the asm keeps a compiler from
 * turning the loop back into a memcpy() call.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "harness.h"

extern unsigned char __start_omcid_data[], __stop_omcid_data[];
extern unsigned char __start_omcid_bss[], __stop_omcid_bss[];

/* A word wherever the section happens to start. */
typedef uint64_t __attribute__((aligned(1), may_alias)) word;

static void bytes_copy(unsigned char *dst, const unsigned char *src, size_t n)
{
	size_t i = 0;

	for (; i + sizeof(word) <= n; i += sizeof(word)) {
		*(word *)(dst + i) = *(const word *)(src + i);
		__asm__ volatile ("");
	}
	for (; i < n; i++) {
		dst[i] = src[i];
		__asm__ volatile ("");
	}
}

static void bytes_zero(unsigned char *dst, size_t n)
{
	size_t i = 0;

	for (; i + sizeof(word) <= n; i += sizeof(word)) {
		*(word *)(dst + i) = 0;
		__asm__ volatile ("");
	}
	for (; i < n; i++) {
		dst[i] = 0;
		__asm__ volatile ("");
	}
}

void state_restore(void)
{
	static unsigned char *pristine;
	/* The linker puts __start_ and __stop_ at the two ends of one section,
	 * so the difference is its size, whatever cppcheck sees in two
	 * separate declarations. */
	/* cppcheck-suppress comparePointers */
	size_t dn = (size_t)(__stop_omcid_data - __start_omcid_data);
	/* cppcheck-suppress comparePointers */
	size_t bn = (size_t)(__stop_omcid_bss - __start_omcid_bss);

	if (!pristine) {
		/* The first run: omcid has not run yet, so what is there now is
		 * the image, and .bss is still zero. Kept for the life of the
		 * process. */
		pristine = malloc(dn ? dn : 1);
		if (!pristine)
			abort();
		bytes_copy(pristine, __start_omcid_data, dn);
		return;
	}
	bytes_copy(__start_omcid_data, pristine, dn);
	bytes_zero(__start_omcid_bss, bn);
}
