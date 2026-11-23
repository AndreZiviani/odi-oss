#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# Write an uploaded image tarball to the slot this stick is NOT running.
#
#     fwu_starter.sh <slot> <image.tar>              write in the background
#     fwu_starter.sh --foreground <slot> <image.tar> write, and wait for it
#
# The path and the two arguments are what confd execve()s for its Firmware
# tab (odi-ui confd.h FWU_STARTER), which is why this is a script of its own
# rather than a line in confd: fwu.sh travels INSIDE the tarball, not in the
# rootfs, so the flasher that runs is always the one built with the image it
# writes.
#
# What it does, in order, all before anything is erased:
#   1. refuses a second write while one is running, in either mode: a
#      lock every writer inherits, taken before anything shared is touched;
#   2. refuses any slot but the inactive one -- the running slot is read the
#      way fwu.sh reads it, root=31:N in /proc/cmdline, then sw_active --
#      and the slot sw_commit names, in the active environment copy or in
#      the fallback one; refuses either slot when sw_commit or the fallback
#      copy cannot be read;
#   3. extracts fwu.sh and md5.txt into a scratch directory in /tmp and checks
#      fwu.sh against its own md5.txt line;
#   4. runs fwu.sh <slot> <tarball> there, detached (setsid, stdin from
#      /dev/null) so neither confd nor an ssh session ending can stop it
#      halfway, with its output in FWU_LOG and its outcome in FWU_STATE,
#      and then runs slot-state.sh, so the check of the other slot that the
#      banner and the exporter show describes what was just written.
#
# fwu.sh itself checks both members md5 and size before erasing, reads the
# write back, and never touches the U-Boot environment unless asked
# (FWU_RECORD_VERSION, which this does not set). Neither does this script:
# no sw_commit, no sw_tryactive. Trying the written slot is a separate step.
#
# The background default is for confd, which is single-threaded: the write
# takes about eighty seconds, and blocking for it meant the whole UI stopped
# answering. confd reports FWU_STATE and the tail of FWU_LOG in /api/firmware
# instead, and the page polls it.
set -u

FWU_DIR=${FWU_DIR:-/tmp/fwu.d}
FWU_LOG=${FWU_LOG:-/tmp/fwu.log}
FWU_STATE=${FWU_STATE:-/tmp/fwu.state}
# How long the background job may take to claim the state file.
FWU_START_S=${FWU_START_S:-10}
PROC_MTD=${PROC_MTD:-/proc/mtd}
PROC_CMDLINE=${PROC_CMDLINE:-/proc/cmdline}
NV=${NV:-/bin/nv}
SLOT_STATE=${SLOT_STATE:-/etc/scripts/slot-state.sh}

fg=0
if [ "${1:-}" = --foreground ]; then
	fg=1
	shift
fi
slot=${1:-}
tarball=${2:-}

say() { echo "fwu_starter: $*"; }
die() { echo "fwu_starter: $*" >&2; exit 1; }

case "$slot" in
0|1) ;;
*) die "usage: $0 [--foreground] <0|1> <image.tar>" ;;
esac
[ -f "$tarball" ] || die "no such image: $tarball"
case "$tarball" in
/*) ;;
*) tarball=$(pwd)/$tarball ;;
esac

# One writer at a time, in both modes. The lock is on a file of its own,
# beside the state file and never removed, and fd 9 is inherited by
# everything started below, fwu.sh included: it is held until the write
# ends, even if this shell is killed first. The subshell probe is there
# for the message: a redirection that fails on exec ends the shell before
# any "|| die" could run.
( : >> "$FWU_STATE.lock" ) 2>/dev/null || die "cannot open the writer lock $FWU_STATE.lock"
exec 9>>"$FWU_STATE.lock"
flock -n 9 || die "a write is already running; see $FWU_LOG"

# The state file says running while fwu.sh is alive;
# a state left saying running by a process that is gone (a reboot mid-write
# cannot leave one, /tmp is RAM, but a kill can) does not block forever.
st=''; pid=''
if [ -f "$FWU_STATE" ]; then
	read -r st pid _ < "$FWU_STATE"
	if [ "$st" = running ] && [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
		die "a write is already running (pid $pid); see $FWU_LOG"
	fi
fi

# The inactive slot, derived the way fwu.sh derives it. fwu.sh refuses the
# running slot too; this refuses before anything is extracted, and also
# refuses when the running slot cannot be told at all.
mtd_idx() {
	sed -n "s/^mtd\([0-9]*\):.*\"$1\"\$/\1/p" "$PROC_MTD" | sed -n '1p'
}
running=
root=$(sed -n "s/.*root=31:\([0-9]*\).*/\1/p" "$PROC_CMDLINE" 2>/dev/null)
if [ -n "$root" ]; then
	[ "$root" = "$(mtd_idx r0)" ] && running=0
	[ "$root" = "$(mtd_idx r1)" ] && running=1
fi
if [ -z "$running" ]; then
	# Only from a read that exited 0: what a failed one printed is no
	# answer, as for sw_commit below.
	running=$(timeout 5 "$NV" getenv sw_active 2>/dev/null) || running=
	running=$(printf '%s\n' "$running" | sed -n "s/^sw_active=//p")
	case "$running" in 0|1) ;; *) running= ;; esac
fi
[ -n "$running" ] || die "cannot tell which slot is running; refusing to write either"
[ "$running" != "$slot" ] || die "slot $slot is the one this stick is running; write slot $((1 - slot))"

# A line before the next nv read: confd ends a child that stays silent for
# its timeout, and older confds allow 5 s.
say "running slot $running; reading sw_commit"

# The revert target. fwu.sh only warns when it cannot read sw_commit, because
# the vendor nv that runs the first flash may not answer; this script ships
# with our nv, so an empty answer is a failed read, not an old tool. On an
# uncommitted trial the slot not running IS the committed one, and writing
# it on a guess would erase the only image a failed trial can go back to.
commit=$(timeout 5 "$NV" getenv sw_commit 2>/dev/null) || commit=
commit=$(printf '%s\n' "$commit" | sed -n "s/^sw_commit=//p")
case "$commit" in
0|1) ;;
*) die "cannot read sw_commit, the slot a failed trial goes back to; refusing to write either slot. Check with: nv getenv sw_commit; nv fallback sw_commit" ;;
esac
say "sw_commit is $commit"
# The refusals fwu.sh makes, made here before anything is extracted: the
# starter runs whatever fwu.sh the upload brought, and does not rely on it.
[ "$commit" != "$slot" ] || die "sw_commit is $slot, the slot this would write: a failed trial would revert into the half-written image. Commit the running slot first: nv commit $running"

# The other environment copy, the one U-Boot boots by once the active copy
# is lost (an interrupted nv setenv). Exit 0 is its value; exit 1 with its
# line is no valid second copy, which is fine; anything else (3, a copy not
# read in full; 124 or more, no answer) leaves it unknown.
alt_out=$(timeout 5 "$NV" fallback sw_commit 2>/dev/null)
alt_rc=$?
# Each outcome that lets the write go on is said, so the page log shows what
# was checked. A copy that sets no sw_commit is not refused, as in fwu.sh,
# but warned about, since what U-Boot does with it is not checked.
if [ "$alt_rc" = 0 ]; then
	alt=$(printf '%s\n' "$alt_out" | sed -n "s/^sw_commit=//p")
	[ "$alt" != "$slot" ] || die "the fallback environment copy says sw_commit=$slot, the slot this would write. Make the two copies agree first: nv commit $running"
	if [ -n "$alt" ]; then
		say "the fallback environment copy says sw_commit=$alt"
	else
		say "warning: the fallback environment copy sets no sw_commit; where U-Boot goes after an interrupted nv setenv is not checked"
	fi
elif [ "$alt_rc" = 1 ] && [ "$alt_out" = "nv: no valid second copy of the environment" ]; then
	say "no valid second environment copy"
else
	die "cannot read the fallback environment copy (nv fallback exit $alt_rc); refusing to write either slot. Check with: nv fallback sw_commit"
fi

rm -rf "$FWU_DIR"
mkdir -p "$FWU_DIR" || die "cannot create $FWU_DIR"
( cd "$FWU_DIR" && tar -xf "$tarball" fwu.sh md5.txt ) 2>/dev/null ||
	die "$tarball has no fwu.sh and md5.txt; is it an image tarball?"
want=$(sed -n 's/^\([0-9a-f]*\) [ *]\{0,1\}fwu\.sh$/\1/p' "$FWU_DIR/md5.txt" | sed -n '1p')
got=$(md5sum "$FWU_DIR/fwu.sh" | cut -d' ' -f1)
[ -n "$want" ] || die "md5.txt has no line for fwu.sh"
[ "$want" = "$got" ] || die "fwu.sh md5 mismatch (md5.txt $want, file $got)"
say "slot $slot (running $running), $(basename "$tarball"), fwu.sh ok ($got)"

if [ "$fg" = 1 ]; then
	# Claimed here too, for the upload guard, which reads only the state
	# file: exec keeps this pid, so it is the flasher that the state
	# names. Nothing is left to record the outcome; a running state whose
	# pid is gone is read as stale.
	echo "running $$ $slot" > "$FWU_STATE" || die "cannot record the writer state in $FWU_STATE"
	if ! cd "$FWU_DIR"; then
		echo "failed $$ $slot start" > "$FWU_STATE"
		die "cannot enter $FWU_DIR"
	fi
	exec sh ./fwu.sh "$slot" "$tarball"
fi

: > "$FWU_LOG"
# Claimed before the job starts, with this shell as the live pid, so a
# second request in the gap cannot start a second writer. Checked: a state
# that cannot be written (a full /tmp) leaves nothing for the job to claim,
# and the wait below would read the empty file as a job that had started.
echo "running $$ $slot" > "$FWU_STATE" || die "cannot record the writer state in $FWU_STATE"
# The job records its own pid and outcome. setsid gives it a session of its
# own: confd, and the shell that started this, can go away without a SIGHUP
# reaching a flash in progress.
# It claims the state file only while that still names this shell: a job
# that starts after this shell gave up on it (below) must not write.
setsid sh -c '
	cd "$1" || exit 1
	read -r st pid _ < "$4"
	[ "$st $pid" = "running $6" ] || exit 1
	echo "running $$ $2" > "$4"
	sh ./fwu.sh "$2" "$3" >> "$5" 2>&1
	rc=$?
	if [ "$rc" = 0 ]; then echo "ok $$ $2" > "$4"; else echo "failed $$ $2 $rc" > "$4"; fi
	# The write is over: let go of the writer lock (fd 9) first, so the
	# reads of the refresh do not hold Try, Upload and Reboot off in confd.
	exec 9>&-
	[ ! -x "$7" ] || "$7" > /dev/null 2>&1
' fwu "$FWU_DIR" "$slot" "$tarball" "$FWU_STATE" "$FWU_LOG" "$$" "$SLOT_STATE" \
	< /dev/null > /dev/null 2>&1 &
# Wait for the job to claim the state file, so a request that arrives right
# after this one returns sees a live writer, and a job that never started is
# reported here rather than as a state that stays "running" forever. Up to
# FWU_START_S seconds: on a loaded stick the job can take longer than one to
# get there, and giving up on it early reported a write that then ran.
n=0
while :; do
	sleep 1
	n=$((n + 1))
	# Every few seconds a line, for the same reason as above.
	[ $((n % 3)) != 0 ] || say "waiting for the write job to start ($n s)"
	st=''; pid=''
	read -r st pid _ < "$FWU_STATE"
	# Started only when the state names another writer; an empty or
	# unreadable state is waited out like a job that has not started.
	case "$st" in
	running | ok | failed) [ "$pid" = "$$" ] || break ;;
	esac
	if [ "$n" -ge "$FWU_START_S" ]; then
		echo "failed $$ $slot start" > "$FWU_STATE"
		die "the write job did not start within $FWU_START_S s"
	fi
done
say "writing slot $slot in the background; progress in $FWU_LOG"
exit 0
