#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/fwu_starter.sh, off the device.
#
# Everything that decides whether a write may start is checked here: the
# inactive-slot rule (from /proc/cmdline, then sw_active, and a refusal when
# neither says), the fwu.sh md5 check against md5.txt, the one-writer lock,
# and that the job it starts records its outcome. fwu.sh is a stub that
# prints its arguments: the real one erases flash, and has its own test
# (fwu_guard_test.sh).
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/fwu_starter.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
t() {
	if printf '%s' "$3" | grep -q -- "$2"; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        wanted /$2/, got:"; printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1)); fi
}

cat > "$T/mtd" <<'MTD'
dev:    size   erasesize  name
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
MTD
mkdir -p "$T/bin" "$T/img"
# setsid is not on every host this runs on; the stub runs the job in place,
# which is what setsid does apart from the new session.
printf '#!/bin/sh\nexec "$@"\n' > "$T/bin/setsid"
cat > "$T/bin/nv" <<'NV'
#!/bin/sh
[ -n "${NV_ACTIVE:-}" ] && echo "sw_active=$NV_ACTIVE"
exit 0
NV
# flock is a busybox applet on the stick. A host without the util-linux
# command gets the same kernel lock through python.
if ! command -v flock >/dev/null 2>&1; then
	cat > "$T/bin/flock" <<'FLOCK'
#!/usr/bin/env python3
import fcntl, sys
try:
    fcntl.flock(int(sys.argv[-1]), fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError:
    sys.exit(1)
FLOCK
	chmod +x "$T/bin/flock"
fi
chmod +x "$T/bin/setsid" "$T/bin/nv"
md5_of() { (md5sum "$1" 2>/dev/null || md5 -r "$1") | cut -d' ' -f1; }

mkimg() {   # mkimg <tar> [bad]
	# The stub names itself in FWU_STARTS and, when FWU_HOLD is set, stays
	# until that file exists (bounded): a writer the tests hold open and
	# let go, with no fixed sleep to race against.
	cat > "$T/img/fwu.sh" <<'STUB'
#!/bin/sh
echo "fwu stub slot=$1 tar=$2"
[ -z "${FWU_STARTS:-}" ] || echo start >> "$FWU_STARTS"
if [ -n "${FWU_HOLD:-}" ]; then
	n=0
	while [ ! -e "$FWU_HOLD" ] && [ "$n" -lt 200 ]; do sleep 0.1; n=$((n + 1)); done
fi
exit ${FWU_STUB_RC:-0}
STUB
	if [ "${2:-}" = bad ]; then
		echo "00000000000000000000000000000000  fwu.sh" > "$T/img/md5.txt"
	else
		echo "$(md5_of "$T/img/fwu.sh")  fwu.sh" > "$T/img/md5.txt"
	fi
	( cd "$T/img" && tar -cf "$1" fwu.sh md5.txt )
}
mkimg "$T/good.tar"
mkimg "$T/bad.tar" bad

run() {   # run <cmdline> [args...]
	# One cmdline file per call: starters run side by side in the race
	# test, and a shared file rewritten by one is read empty by another.
	cl=$(mktemp "$T/cmdline.XXXXXX")
	echo "$1" > "$cl"; shift
	env PATH="$T/bin:$PATH" PROC_MTD="$T/mtd" PROC_CMDLINE="$cl" NV="$T/bin/nv" \
	    FWU_DIR="$T/fwu.d" FWU_LOG="$T/fwu.log" FWU_STATE="$T/fwu.state" \
	    sh "$S" "$@" 2>&1
}

t "a slot that is not 0 or 1 is refused" "usage" "$(run 'root=31:5' 2 "$T/good.tar")"
t "a missing tarball is refused" "no such image" "$(run 'root=31:5' 1 "$T/none.tar")"
t "the running slot is refused (cmdline names r0)" "running; write slot 1" "$(run 'root=31:5' 0 "$T/good.tar")"
t "the running slot is refused (cmdline names r1)" "running; write slot 0" "$(run 'root=31:7' 1 "$T/good.tar")"
t "sw_active answers when the cmdline does not" "running; write slot 0" \
  "$(NV_ACTIVE=1 run 'console=ttyS0' 1 "$T/good.tar")"
t "neither answering refuses both slots" "cannot tell which slot" "$(run 'console=ttyS0' 1 "$T/good.tar")"
t "a fwu.sh that does not match md5.txt is refused" "md5 mismatch" "$(run 'root=31:5' 1 "$T/bad.tar")"
t "a tar without fwu.sh is refused" "no fwu.sh" "$( : > "$T/empty"; ( cd "$T" && tar -cf notimg.tar empty ); run 'root=31:5' 1 "$T/notimg.tar")"

out=$(run 'root=31:5' --foreground 1 "$T/good.tar")
t "foreground runs fwu.sh with the slot and the tarball" "fwu stub slot=1 tar=$T/good.tar" "$out"

rm -f "$T/fwu.state"
out=$(run 'root=31:5' 1 "$T/good.tar")
t "background returns at once and says where the log is" "in the background" "$out"
for _ in 1 2 3 4 5; do grep -q '^ok' "$T/fwu.state" 2>/dev/null && break; sleep 1; done
t "the job records its outcome" "^ok [0-9]* 1" "$(cat "$T/fwu.state")"
t "and its output lands in the log" "fwu stub slot=1" "$(cat "$T/fwu.log")"

rm -f "$T/fwu.state"
FWU_STUB_RC=3 run 'root=31:5' 1 "$T/good.tar" > /dev/null
for _ in 1 2 3 4 5; do grep -q '^failed' "$T/fwu.state" 2>/dev/null && break; sleep 1; done
t "a failing fwu.sh is recorded with its exit code" "^failed [0-9]* 1 3" "$(cat "$T/fwu.state")"

# A live writer holds the lock; a dead one does not.
sleep 30 & live=$!
echo "running $live 1" > "$T/fwu.state"
t "a second write while one runs is refused" "already running" "$(run 'root=31:5' 1 "$T/good.tar")"
kill "$live" 2>/dev/null; wait "$live" 2>/dev/null
t "a stale running state does not block" "in the background" "$(run 'root=31:5' 1 "$T/good.tar")"
sleep 1

# until <seconds> <command...>: poll, bounded, in place of a fixed sleep.
until_ok() {
	_n=$(( $1 * 10 )); shift
	while [ "$_n" -gt 0 ]; do "$@" 2>/dev/null && return 0; sleep 0.1; _n=$((_n - 1)); done
	return 1
}
lines() { _c=$(grep -c "$1" "$2" 2>/dev/null); echo "${_c:-0}"; }
# The lock is free again: no writer, and no detached job, still holds it.
lock_free() { env PATH="$T/bin:$PATH" sh -c 'flock -n 9' 9>>"$T/fwu.state.lock"; }

# A foreground write was recorded nowhere, so a second starter ran beside
# it. Hold one writer of each mode open and send competitors of both.
for mode in --foreground ''; do
	name=${mode:-background}; name=${name#--}
	rm -f "$T/fwu.state" "$T/release" "$T/starts"
	# shellcheck disable=SC2086  # $mode is empty or one word
	FWU_STARTS="$T/starts" FWU_HOLD="$T/release" run 'root=31:5' $mode 1 "$T/good.tar" > "$T/first.log" &
	first=$!
	until_ok 10 test -s "$T/starts"
	t "$name: the writer is running" "^1$" "$(lines start "$T/starts")"
	t "$name: and is in the state file" "^running [0-9]* 1" "$(cat "$T/fwu.state" 2>/dev/null)"
	before=$(md5_of "$T/fwu.d/fwu.sh")
	t "$name: a foreground competitor is refused" "already running" "$(FWU_STARTS="$T/starts" run 'root=31:5' --foreground 1 "$T/good.tar")"
	t "$name: a background competitor is refused" "already running" "$(FWU_STARTS="$T/starts" run 'root=31:5' 1 "$T/good.tar")"
	mv "$T/fwu.state" "$T/fwu.state.away"
	t "$name: refused by the lock alone, with the state file gone" "already running" "$(FWU_STARTS="$T/starts" run 'root=31:5' --foreground 1 "$T/good.tar")"
	mv "$T/fwu.state.away" "$T/fwu.state"
	t "$name: the scratch directory was left alone" "^$before$" "$(md5_of "$T/fwu.d/fwu.sh")"
	t "$name: still one writer" "^1$" "$(lines start "$T/starts")"
	: > "$T/release"
	wait "$first"
	until_ok 10 lock_free
done

# The race itself: four starters at the same moment, two of each mode.
# Exactly one may reach the flasher; the other three are refused.
rm -f "$T/fwu.state" "$T/release" "$T/starts"
until_ok 10 lock_free
pids=
for i in 1 2 3 4; do
	mode=--foreground; [ $((i % 2)) = 0 ] && mode=
	# shellcheck disable=SC2086
	FWU_STARTS="$T/starts" FWU_HOLD="$T/release" run 'root=31:5' $mode 1 "$T/good.tar" > "$T/race.$i" &
	pids="$pids $!"
done
refused() { [ "$(cat "$T"/race.? | grep -c 'already running')" = 3 ]; }
until_ok 10 test -s "$T/starts"
until_ok 10 refused
t "four starters at once: three are refused" "^3$" "$(cat "$T"/race.? | grep -c 'already running')"
t "four starters at once: one writer" "^1$" "$(lines start "$T/starts")"
: > "$T/release"
# shellcheck disable=SC2086
wait $pids
until_ok 10 lock_free
t "and no second writer after the first is done" "^1$" "$(lines start "$T/starts")"

# The job can take longer than a second to claim the state file on a loaded
# stick. The starter waits for it (FWU_START_S) instead of reporting a write
# that then runs; a job that never starts is still reported; and a job that
# starts after the starter gave up does not write. setsid is the stub that
# delays or drops the job.
cp "$T/bin/setsid" "$T/bin/setsid.plain"
printf '#!/bin/sh\nsleep "${SETSID_DELAY:-0}"\n[ -z "${SETSID_DROP:-}" ] || exit 0\nexec "$@"\n' > "$T/bin/setsid"
chmod +x "$T/bin/setsid"
until_ok 10 lock_free
rm -f "$T/fwu.state" "$T/starts"
out=$(SETSID_DELAY=2 FWU_STARTS="$T/starts" run 'root=31:5' 1 "$T/good.tar"); rc=$?
t "a job that takes 2 s to start is waited for" "in the background" "$out"
t "with exit 0" "^0$" "$rc"
until_ok 10 grep -q '^ok' "$T/fwu.state"
t "and it wrote once" "^ok [0-9]* 1 1$" "$(cat "$T/fwu.state" | tr '\n' ' ')$(lines start "$T/starts")"
until_ok 10 lock_free
rm -f "$T/fwu.state" "$T/starts"
out=$(SETSID_DROP=1 FWU_START_S=2 FWU_STARTS="$T/starts" run 'root=31:5' 1 "$T/good.tar"); rc=$?
t "a job that never starts is reported" "did not start within 2 s" "$out"
t "with exit 1 and a failed state" "^1 failed [0-9]* 1 start$" "$rc $(cat "$T/fwu.state")"
until_ok 10 lock_free
rm -f "$T/fwu.state" "$T/starts"
out=$(SETSID_DELAY=4 FWU_START_S=2 FWU_STARTS="$T/starts" run 'root=31:5' 1 "$T/good.tar"); rc=$?
t "a job later than the wait is reported as not started" "did not start within 2 s" "$out"
until_ok 10 lock_free
t "and does not write afterwards" "^0 failed [0-9]* 1 start$" "$(lines start "$T/starts") $(cat "$T/fwu.state")"
cp "$T/bin/setsid.plain" "$T/bin/setsid"

# A foreground writer leaves a running state with a pid that is gone
# (exec keeps nothing behind to record the outcome): read as stale.
rm -f "$T/fwu.state"
run 'root=31:5' --foreground 1 "$T/good.tar" > /dev/null
t "a finished foreground write leaves its claim" "^running [0-9]* 1" "$(cat "$T/fwu.state")"
t "which does not block the next write" "fwu stub slot=1" "$(run 'root=31:5' --foreground 1 "$T/good.tar")"

# A foreground writer that cannot claim the state file does not flash:
# the upload guard reads nothing else. A directory in its place fails
# the write.
rm -f "$T/fwu.state" "$T/starts"; mkdir "$T/fwu.state"
out=$(FWU_STARTS="$T/starts" run 'root=31:5' --foreground 1 "$T/good.tar"); rc=$?
rmdir "$T/fwu.state"
t "an unwritable state file is refused" "cannot record the writer state" "$out"
t "with exit 1" "^1$" "$rc"
t "before the flasher" "^0$" "$(lines start "$T/starts")"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
