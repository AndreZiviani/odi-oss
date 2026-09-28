#!/bin/sh
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
#   1. refuses a second write while one is running;
#   2. refuses any slot but the inactive one -- the running slot is read the
#      way fwu.sh reads it, root=31:N in /proc/cmdline, then sw_active;
#   3. extracts fwu.sh and md5.txt into a scratch directory in /tmp and checks
#      fwu.sh against its own md5.txt line;
#   4. runs fwu.sh <slot> <tarball> there, detached (setsid, stdin from
#      /dev/null) so neither confd nor an ssh session ending can stop it
#      halfway, with its output in FWU_LOG and its outcome in FWU_STATE.
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
PROC_MTD=${PROC_MTD:-/proc/mtd}
PROC_CMDLINE=${PROC_CMDLINE:-/proc/cmdline}
NV=${NV:-/bin/nv}

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

# One write at a time. The state file says running while fwu.sh is alive;
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
	running=$(timeout 5 "$NV" getenv sw_active 2>/dev/null | sed -n "s/^sw_active=//p")
	case "$running" in 0|1) ;; *) running= ;; esac
fi
[ -n "$running" ] || die "cannot tell which slot is running; refusing to write either"
[ "$running" != "$slot" ] || die "slot $slot is the one this stick is running; write slot $((1 - slot))"

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
	cd "$FWU_DIR" && exec sh ./fwu.sh "$slot" "$tarball"
fi

: > "$FWU_LOG"
# Claimed before the job starts, with this shell as the live pid, so a
# second request in the gap cannot start a second writer.
echo "running $$ $slot" > "$FWU_STATE"
# The job records its own pid and outcome. setsid gives it a session of its
# own: confd, and the shell that started this, can go away without a SIGHUP
# reaching a flash in progress.
setsid sh -c '
	cd "$1" || exit 1
	echo "running $$ $2" > "$4"
	sh ./fwu.sh "$2" "$3" >> "$5" 2>&1
	rc=$?
	if [ "$rc" = 0 ]; then echo "ok $$ $2" > "$4"; else echo "failed $$ $2 $rc" > "$4"; fi
' fwu "$FWU_DIR" "$slot" "$tarball" "$FWU_STATE" "$FWU_LOG" \
	< /dev/null > /dev/null 2>&1 &
# Wait for the job to claim the state file, so a request that arrives right
# after this one returns sees a live writer, and a job that never started is
# reported here rather than as a state that stays "running" forever.
sleep 1
read -r st pid _ < "$FWU_STATE"
if [ "$pid" = "$$" ]; then
	echo "failed $$ $slot start" > "$FWU_STATE"
	die "the write job did not start"
fi
say "writing slot $slot in the background; progress in $FWU_LOG"
exit 0
