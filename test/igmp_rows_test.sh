#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror ${ODI_SAN_CFLAGS:-} \
    -Isrc/diag/src test/igmp_rows_test.c src/igmp/rows.c -o "$T/test"
"$T/test"
