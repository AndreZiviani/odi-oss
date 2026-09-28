#!/bin/sh
# respawn.sh -- exec a foreground daemon under a busybox init `respawn`
# entry, after setting its own oom_score_adj (docs/SETTINGS.md).
#
# Replaces supervise.sh (native over hand-rolled, docs/SETTINGS.md: a
# hardware trial killed a supervised omcid with kill -9 and it stayed
# dead): busybox init already restarts a respawn entry the instant its
# process exits, so nothing here loops, backs off, or rate-limits -- init IS
# the supervisor now, and it never loses track of the daemon the way the old
# supervise() could (a shell background job whose own accounting can drift
# from what actually holds the pid). Runs as the entrys own process, exec-ed
# in place by the shell that /etc/inittab starts, so oom_score_adj lands on
# the right pid before it is replaced by the daemon.
#
# respawn.sh <oom_score_adj> <logfile> <cmd...>
#
# ash/dash-compatible: every respawn entry runs under busybox ash, not bash
# (AGENTS.md, make lint checks this file in POSIX sh mode too).
oom=$1
log=$2
shift 2
echo "$oom" > /proc/self/oom_score_adj 2>/dev/null || true
exec "$@" >> "$log" 2>&1
