#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# Every writable section of an omcid object built for the host is one that
# state.c puts back between two runs.
#
#   check-sections.sh <object>...
#
# The Makefile renames .data and .bss of each omcid object to omcid_data and
# omcid_bss. A compiler that starts to put variables anywhere else (a new
# section name, thread-local data) would leave state behind from one fuzz
# input to the next, and nothing would say so: inputs would only stop
# reproducing. This fails the build instead. The sections allowed besides the
# two are bookkeeping of the compiler and the sanitizers, written once at
# start or holding nothing omcid reads back.
set -eu

bad=0
for obj in "$@"; do
	# A common symbol (a tentative definition built with -fcommon) is in
	# no section at all: the rename does not move it and nothing here
	# would list it. The Makefile passes -fno-common; this is for the day
	# a flag from outside turns it back on.
	common=$(nm "$obj" | awk '$2 == "C" { print $3 }')
	if [ -n "$common" ]; then
		echo "check-sections: $obj has common symbols, which state.c cannot put back (build with -fno-common):" >&2
		echo "$common" | sed 's/^/    /' >&2
		bad=1
	fi
	left=$(readelf -SW "$obj" | sed -n 's/^ *\[ *[0-9]*\] *//p' |
		awk '$2 != "NULL" && $7 ~ /W/ && $7 ~ /A/ { print $1 }' |
		grep -vE '^(omcid_data|omcid_bss|asan_globals|__sancov_[a-z_]*|__llvm_prf_[a-z]*|\.init_array(\..*)?|\.fini_array(\..*)?|\.data\.rel\.ro(\..*)?|\.ctors|\.dtors)$' || true)
	if [ -n "$left" ]; then
		echo "check-sections: $obj keeps writable data outside omcid_data and omcid_bss:" >&2
		echo "$left" | sed 's/^/    /' >&2
		bad=1
	fi
done
exit $bad
