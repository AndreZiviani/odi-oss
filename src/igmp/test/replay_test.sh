#!/bin/sh
# igmpd -r against its golden transcripts (test/replay/*.in, expected output
# *.out), and the refusals around it. Runs under qemu-user in the diag
# toolchain container, from src/igmp, after the binary is built:
#   make test-igmp
# The goldens are what the daemon decides, so a change to them is a change of
# behaviour and wants a reason in the commit. To rewrite them after a change
# that is meant (read the diff before committing it), from src/igmp in the
# container:
#   sh test/replay_test.sh --update
set -u
Q=qemu-mips-static
BIN=build/igmpd
fail=0
update=0
[ "${1:-}" = "--update" ] && update=1

ok() { echo "ok    $1"; }
no() { echo "FAIL  $1"; fail=$((fail + 1)); }

for in in test/replay/*.in; do
	name=${in%.in}
	got=$($Q $BIN -r "$in" 2>&1; echo "status $?")
	if [ "$update" = 1 ]; then
		printf '%s\n' "$got" > "$name.out"
		echo "wrote $name.out"
	elif [ "$got" = "$(cat "$name.out")" ]; then
		ok "replay $(basename "$name")"
	else
		no "replay $(basename "$name")"
		printf '%s\n' "$got" > /tmp/replay-got.txt
		diff "$name.out" /tmp/replay-got.txt | head -30
	fi
done
[ "$update" = 1 ] && exit 0

# Refusals: each is a status 2 and a line saying why, never a half run.
t=/tmp/replay-bad.in
printf '5 tick\n4 tick\n' > $t
out=$($Q $BIN -r $t 2>&1); rc=$?
[ "$rc" = 2 ] && [ "$out" != "${out#*clock does not go backwards}" ] \
	&& ok "a clock that goes backwards stops the run" \
	|| no "a clock that goes backwards stops the run ($rc)"
printf '1 rx 0g\n' > $t
out=$($Q $BIN -r $t 2>&1); rc=$?
[ "$rc" = 2 ] && [ "$out" != "${out#*bad hex}" ] \
	&& ok "bad hex stops the run" || no "bad hex stops the run ($rc)"
printf '1 wave\n' > $t
$Q $BIN -r $t >/dev/null 2>&1; rc=$?
[ "$rc" = 2 ] && ok "an unknown event stops the run" || no "an unknown event stops the run ($rc)"
$Q $BIN -r /nonexistent >/dev/null 2>&1; rc=$?
[ "$rc" = 2 ] && ok "a missing transcript is status 2" || no "a missing transcript ($rc)"
out=$($Q $BIN -w -r test/replay/join-leave.in 2>&1); rc=$?
[ "$rc" = 2 ] && [ "$out" != "${out#*never writes}" ] \
	&& ok "-r with -w is refused before anything is read" \
	|| no "-r with -w is refused ($rc)"
rm -f $t /tmp/replay-got.txt

[ "$fail" -eq 0 ] && echo "replay_test.sh: all checks passed" || echo "FAILED ($fail)"
[ "$fail" -eq 0 ]
