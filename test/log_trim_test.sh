#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# log_trim of rcs-lib.sh, the body of the log trim loop in rcS, off the
# device. /var is a tmpfs of 6 MB, and the trim used to write its copy next
# to the log. With /var full the copy came out empty or cut short, busybox
# tail exited 0 all the same, and the log was replaced by that: the trim
# gave the space back by throwing the log away. The copy goes to LOG_CUT_DIR
# (/var/tmp, a tmpfs of its own) now and is judged by its size, so a full
# /var keeps the last LOG_KEEP bytes; only when the copy cannot be made
# whole there either is the log emptied.
#
# Checked: a log under LOG_MAX is left alone; one past it keeps its last
# LOG_KEEP bytes; the same with a log directory that takes no new file (not
# as root, which writes anywhere); a copy cut short by a tail that exits 0,
# and one that cannot be made at all, empty the log; no copy is left behind.
set -u
cd "$(dirname "$0")/.." || exit 1
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'chmod -R u+w "$T"; rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; printf '        wanted: %s\n        got:    %s\n' "$2" "$3"; fail=$((fail + 1)); }
eq() { if [ "$3" = "$2" ]; then ok "$1"; else no "$1" "$2" "$3"; fi; }

mkdir "$T/log" "$T/tmp"
# shellcheck disable=SC2034 # read by the sourced lib
LOG_MAX=1000 LOG_KEEP=400 LOG_CUT_DIR=$T/tmp
# shellcheck source=/dev/null
. "$LIB"

L=$T/log/omcid.log
# fill <lines>: 10 bytes a line, numbered, so the tail is recognisable.
fill() { i=0; while [ "$i" -lt "$1" ]; do printf 'line %04d\n' "$i"; i=$((i + 1)); done > "$L"; }
size() { wc -c < "$L" | tr -d ' '; }
left() { find "$T/log" "$T/tmp" -name '*.cut' | wc -l | tr -d ' '; }

log_trim "$T/log/none.log"
eq "a missing log is no error" 0 $?

fill 100
log_trim "$L"
eq "a log at LOG_MAX is left alone" 1000 "$(size)"

fill 150
tail -c 400 "$L" > "$T/want"
log_trim "$L"
eq "a log past LOG_MAX is cut to LOG_KEEP" 400 "$(size)"
cmp -s "$L" "$T/want" && ok "  and those are its last bytes" || no "  and those are its last bytes" "$(tail -n 1 "$T/want")" "$(tail -n 1 "$L")"
eq "  no copy is left behind" 0 "$(left)"

# No room next to the log: the log itself can be rewritten in place, a new
# file beside it cannot be made.
if [ "$(id -u)" != 0 ]; then
	fill 150
	chmod 500 "$T/log"
	log_trim "$L"
	chmod 700 "$T/log"
	eq "no room next to the log: still cut to LOG_KEEP" 400 "$(size)"
	cmp -s "$L" "$T/want" && ok "  and those are its last bytes" || no "  and those are its last bytes" "$(tail -n 1 "$T/want")" "$(tail -n 1 "$L")"
	eq "  no copy is left behind" 0 "$(left)"
else
	echo "skip  no room next to the log: root can write there"
fi

# No room for the whole copy in LOG_CUT_DIR either. busybox tail writes what
# fits and exits 0; the stub does the same. The log is emptied, not replaced
# by the piece, so /var gets its space back.
mkdir "$T/bin"
cat > "$T/bin/tail" <<'EOS'
#!/bin/sh
printf 'line 0110\nline 0111\n'
exit 0
EOS
chmod +x "$T/bin/tail"
fill 150
PATH="$T/bin:$PATH" log_trim "$L"
eq "a copy cut short, tail exiting 0: the log is emptied" 0 "$(size)"
[ -f "$L" ] && ok "  and still exists for its writers" || no "  and still exists for its writers" "a file" "none"
eq "  no copy is left behind" 0 "$(left)"

# A LOG_CUT_DIR that is not a directory fails for every user.
fill 150
: > "$T/not-a-dir"
LOG_CUT_DIR=$T/not-a-dir log_trim "$L"
eq "no copy can be made at all: the log is emptied" 0 "$(size)"
eq "  no copy is left behind" 0 "$(left)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
