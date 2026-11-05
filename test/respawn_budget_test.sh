#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The restart budget of every respawned daemon (rootfs/skeleton/etc/scripts/
# respawn.sh, docs/SETTINGS.md "Restart budget"), off the device, with a fake
# clock. busybox init runs the entry again whenever the daemon exits; the
# harness plays init by running respawn.sh again after every stub run, and
# the stub sleep advances the fake uptime by the wait it was asked for, so
# the schedule, the state file and the log lines are the real ones.
#
# Covered per daemon, for every ::respawn: entry of /etc/inittab (the list is
# read from the file, so a new entry without a budget fails here):
#   - the svc script hands its own name to respawn.sh
#   - a crash loop (each run lives 1 s): no wait on the first start, then
#     1, 2, 4 ... seconds, never above the daemon cap, state degraded from
#     the fifth short run, the daemon exec-ed with its own arguments each time
#   - the loop does not flood the log: lines stop at the budget, then every
#     tenth
#   - a run that lived STABLE_S ends the loop: the next start has no wait
#   - a state file that is garbage, and a state directory that cannot be
#     written, never stop the daemon from starting
set -u
cd "$(dirname "$0")/.." || exit 1
D=$PWD/rootfs/skeleton/etc/scripts
R=$D/respawn.sh
INITTAB=rootfs/skeleton/etc/inittab
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
bad() { echo "FAIL  $1"; shift; [ $# -gt 0 ] && printf '        %s\n' "$@"; fail=$((fail + 1)); }
check() { # label wanted got
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "wanted: $2" "got:    $3"; fi
}

# The stub daemon: records that it ran and with what.
cat > "$T/daemon" <<'DAEMON'
#!/bin/sh
echo "$*" >> "$STUB_DIR/ran"
DAEMON
# The stub sleep: moves the fake clock by what it was asked to wait.
cat > "$T/sleep" <<'SLEEP'
#!/bin/sh
echo "$1" >> "$STUB_DIR/waits"
read -r u _ < "$STUB_DIR/uptime"
echo "$((${u%.*} + $1)).00 0.00" > "$STUB_DIR/uptime"
SLEEP
chmod +x "$T/daemon" "$T/sleep"

# init: one start of NAME, with the daemon living LIFE seconds afterwards.
# The lines go to the file named in WAITS, one wait each.
run_start() { # name life
	env STUB_DIR="$S" RESPAWN_DIR="$S/state" RESPAWN_UPTIME="$S/uptime" \
	    RESPAWN_KMSG="$S/kmsg" RESPAWN_SLEEP="$T/sleep" RESPAWN_OOM_FILE="$T/oom" sh "$R" "$1" 0 "$S/log" "$T/daemon" --arg "$1" || return 1
	read -r u _ < "$S/uptime"
	echo "$((${u%.*} + $2)).00 0.00" > "$S/uptime"
}
fresh() { S=$T/$1; rm -rf "$S"; mkdir -p "$S"; echo "100.00 0.00" > "$S/uptime"; : > "$S/waits"; : > "$S/kmsg"; : > "$S/ran"; }
field() { sed -n "s/^$2=//p" "$S/state/$1"; }
cap_of() {
	case $1 in
	omcid) echo 2 ;;
	dropbear) echo 10 ;;
	syslogd | confd) echo 30 ;;
	*) echo 60 ;;
	esac
}

names=$(sed -n 's#^::respawn:/etc/scripts/svc-\([a-z]*\)\.sh$#\1#p' "$INITTAB")
[ -n "$names" ] || { echo "FAIL  no respawn entries read from $INITTAB"; exit 1; }

for n in $names; do
	f=$D/svc-$n.sh
	# every exec of respawn.sh in the script names the daemon first
	bad_lines=$(grep -E 'exec /etc/scripts/respawn\.sh ' "$f" | grep -v -E "respawn\.sh $n -?[0-9]+ /var/log/" || true)
	calls=$(grep -c -E 'exec /etc/scripts/respawn\.sh ' "$f")
	if [ "$calls" -ge 1 ] && [ -z "$bad_lines" ]; then ok "svc-$n.sh hands respawn.sh its own name"
	else bad "svc-$n.sh hands respawn.sh its own name" "$bad_lines"; fi

	cap=$(cap_of "$n")
	fresh "loop-$n"
	i=0
	while [ "$i" -lt 12 ]; do run_start "$n" 1 || bad "$n: respawn.sh failed on start $i"; i=$((i + 1)); done

	# waits: start 1 none; start k (k>=2) waits min(2^(k-2), cap)
	want=""
	k=2; w=1
	while [ "$k" -le 12 ]; do
		want="$want$w "
		w=$((w * 2)); [ "$w" -gt "$cap" ] && w=$cap
		k=$((k + 1))
	done
	check "$n: crash loop waits 1,2,4... up to ${cap}s" "$want" "$(tr '\n' ' ' < "$S/waits")"
	check "$n: the daemon ran on every start, with its arguments" "12 12" "$(wc -l < "$S/ran" | tr -d ' ') $(grep -c -- "^--arg $n\$" "$S/ran")"
	check "$n: state degraded, counted" "degraded 12 11 $cap" "$(field "$n" state) $(field "$n" starts) $(field "$n" fast) $(field "$n" cap_s)"
	check "$n: the first state was ok" "ok" "$(fresh "first-$n"; run_start "$n" 1; field "$n" state)"

	# the log: one line per restart up to the budget (fast 1..5), then one in ten
	fresh "log-$n"
	i=0; while [ "$i" -lt 25 ]; do run_start "$n" 1; i=$((i + 1)); done
	lines=$(grep -c "event=respawn service=$n " "$S/kmsg")
	check "$n: 24 restarts log 5 lines (budget) + 2 (fast 10, 20)" 7 "$lines"
	check "$n: the log line carries the state" 1 "$(grep -c "service=$n state=degraded starts=6 fast=5 last_run_s=1 " "$S/kmsg")"

	# a stable run ends the loop
	fresh "stable-$n"
	i=0; while [ "$i" -lt 8 ]; do run_start "$n" 1; i=$((i + 1)); done
	run_start "$n" 30   # this start (the 9th) gets a wait; the run then lives 30 s
	: > "$S/waits"
	run_start "$n" 1
	check "$n: after a 30 s run the next start has no wait" "" "$(cat "$S/waits")"
	check "$n: and the state is ok again" "ok 0" "$(field "$n" state) $(field "$n" fast)"
done

# the first ordinary restart (a run that lived long) logs at notice and waits nothing
fresh single
run_start confd 500; : > "$S/waits"; run_start confd 1
check "a restart after a long run starts at once" "" "$(cat "$S/waits")"
check "a restart is logged at notice, with how long the run lived" "1" "$(grep -c '^<5>respawn: event=respawn service=confd state=ok starts=2 fast=0 last_run_s=500 ' "$S/kmsg")"
check "the very first start logs nothing" "0" "$(fresh first; run_start confd 1; wc -l < "$S/kmsg" | tr -d ' ')"

# garbage state, and a state directory that cannot be created
fresh junk
mkdir -p "$S/state"; printf 'starts=abc\nfast=-3\nlast_start_s=x y\n' > "$S/state/confd"
check "a garbage state file is read as a first start" "0" "$(run_start confd 1; echo "$?" >/dev/null; wc -c < "$S/waits" | tr -d ' ')"
fresh nodir
: > "$S/blocker"
env STUB_DIR="$S" RESPAWN_DIR="$S/blocker/x" RESPAWN_UPTIME="$S/uptime" RESPAWN_KMSG="$S/kmsg" \
    RESPAWN_SLEEP="$T/sleep" sh "$R" confd 0 "$S/log" "$T/daemon" a
check "an unwritable state directory still starts the daemon" "a" "$(cat "$S/ran")"

# oom_score_adj still lands before the exec
fresh oom
env STUB_DIR="$S" RESPAWN_DIR="$S/state" RESPAWN_UPTIME="$S/uptime" RESPAWN_KMSG="$S/kmsg" RESPAWN_OOM_FILE="$S/oom" \
    sh "$R" omcid -1000 "$S/log" "$T/daemon" x
check "oom_score_adj is written before the exec" "-1000" "$(cat "$S/oom")"

# the exec stays in place: the pid init tracks is the daemon
fresh pid
cat > "$T/pid-daemon" <<'PIDD'
#!/bin/sh
echo $$ > "$STUB_DIR/daemon.pid"
PIDD
chmod +x "$T/pid-daemon"
env STUB_DIR="$S" RESPAWN_DIR="$S/state" RESPAWN_UPTIME="$S/uptime" RESPAWN_KMSG="$S/kmsg" RESPAWN_SLEEP="$T/sleep" \
    sh -c 'echo $$ > "$STUB_DIR/wrapper.pid"; exec sh "$0" confd 0 "$STUB_DIR/log" "$1"' "$R" "$T/pid-daemon"
check "the wrapper execs in place, same pid as the daemon" "$(cat "$S/wrapper.pid")" "$(cat "$S/daemon.pid")"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
