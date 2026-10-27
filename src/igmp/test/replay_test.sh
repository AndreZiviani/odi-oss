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
	# The goldens run with the PON drop opted in (-d); nodrop-* run without
	# it, the default, and must never show a drop. mld-* run with MLD on
	# (-6) as well; every other one runs without it, the default, and
	# nomld-* checks that MLD is then left alone.
	flag=-d
	case $(basename "$name") in
	nodrop-*) flag= ;;
	mld-*) flag="-6 -d" ;;
	esac
	# shellcheck disable=SC2086
	got=$($Q $BIN $flag -r "$in" 2>&1; echo "status $?")
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

# igmpd -s: the same report from files, offline. The state file is what the
# daemon writes (the refused-ring golden prints it), cut off mid-line as a
# reader with a short buffer would see it.
sf=/tmp/status-state.txt
printf 'joins=3\ndenied=2\ngroups=1\nrefused_acl=2\ngroup=225.2.2.2\nrefused=acl 239.1.1.1 vid=0 at=5 n=2\nrefused=acl 239.1.1.2 vid=0 at=9 n=1\n' > $sf
pf=/tmp/status-policy.txt
printf 'snooping=1\nallow=225.0.0.0-225.255.255.255\n' > $pf
out=$($Q $BIN -s $sf $pf 2>&1); rc=$?
want='policy: snooping, function 0, timed leave, 2/125/1 s, limit 0, 1 allow + 0 static ranges
  upstream: reports taken and sent by igmpd; tag as the switch tags it; unauthorized joins withheld
  allow 225.0.0.0 - 225.255.255.255
joins 3, refused by the profile 2 (lists 2, group limit 0), table full 0
groups held: 1
  225.2.2.2
recent refusals, newest first:
  239.1.1.2  vid 0  outside the access control lists (acl)  at 9 s
  239.1.1.1  vid 0  outside the access control lists (acl)  x2  at 5 s'
[ "$rc" = 0 ] && [ "$out" = "$want" ] \
	&& ok "igmpd -s reads a state file and a policy file" \
	|| { no "igmpd -s reads a state file and a policy file ($rc)"; printf '%s\n' "$out"; }
head -c 120 $sf > $t.cut
out=$($Q $BIN -s $t.cut $pf 2>&1)
[ "$out" != "${out#*joins 3}" ] && [ "$out" = "${out%239.1.1.2*}" ] \
	&& ok "igmpd -s on a cut-off state file drops the partial last line" \
	|| { no "igmpd -s on a cut-off state file"; printf '%s\n' "$out"; }
# The IPv6 half: a third file, the IPv6 state, and the IPv6 lists of the
# policy file. Without either, nothing of it is printed (above).
sf6=/tmp/status-state6.txt
printf 'joins=2\ndenied=1\ngroups=1\nrefused_acl=1\ngroup=ff3e::8000:5\nrefused=acl ff3e::9 vid=0 at=4 n=2\n' > $sf6
printf 'snooping=1\nallow=225.0.0.0-225.255.255.255\nallow6=ff3e::8000:0-ff3e::8000:ffff\n' > $pf
out=$($Q $BIN -s $sf $pf $sf6 2>&1); rc=$?
want6='MLD: 1 allow + 0 static IPv6 ranges
  allow6 ff3e::8000:0 - ff3e::8000:ffff
MLD joins 2, refused by the profile 1 (lists 1, group limit 0), table full 0
IPv6 groups held: 1
  ff3e::8000:5
recent IPv6 refusals, newest first:
  ff3e::9  vid 0  outside the access control lists (acl)  x2  at 4 s'
[ "$rc" = 0 ] && [ "$out" = "$want
$want6" ] \
	&& ok "igmpd -s reads the IPv6 state file and lists" \
	|| { no "igmpd -s reads the IPv6 state file and lists ($rc)"; printf '%s\n' "$out"; }
printf 'snooping=1\nallow=225.0.0.0-225.255.255.255\n' > $pf
rm -f $sf6
out=$($Q $BIN -s /nonexistent /nonexistent 2>&1); rc=$?
[ "$rc" = 0 ] && [ "$out" != "${out#*policy: none}" ] && [ "$out" != "${out#*state: none}" ] \
	&& ok "igmpd -s with no files says so" || no "igmpd -s with no files ($rc)"
rm -f $t $t.cut $sf $pf /tmp/replay-got.txt

[ "$fail" -eq 0 ] && echo "replay_test.sh: all checks passed" || echo "FAILED ($fail)"
[ "$fail" -eq 0 ]
