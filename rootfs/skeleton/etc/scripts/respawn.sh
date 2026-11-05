#!/bin/sh
# respawn.sh -- exec a foreground daemon under a busybox init `respawn`
# entry, after a bounded backoff when it keeps dying, a state file and a log
# line (docs/SETTINGS.md, "Restart budget").
#
#     respawn.sh <name> <oom_score_adj> <logfile> <cmd...>
#
# busybox init restarts a respawn entry the instant it exits and has no rate
# limit of its own ("does not stop processes from respawning out of
# control", its own usage text). The native alternatives were checked and do
# not fit: runsv (already built in) restarts after a fixed one second and
# keeps no count, so a budget would still be a hand-written finish script,
# on top of a supervision tree (runsvdir, one runsv per daemon, service
# directories in RAM) that changes how the daemons are started, found and
# stopped; start-stop-daemon starts a daemon once and does not supervise.
# So this stays what it has been, the process init tracks: it decides how
# long to wait before the exec, from state it keeps between runs, and then
# execs in place. Nothing runs beside the daemon, so there is no pid to
# lose and no signal to forward: init signals the daemon itself, and a
# SIGTERM during the wait ends this shell (and the sleep, which init also
# signals at shutdown) like any other.
#
# How it knows how long the daemon lived without a post-exit hook: it
# records the uptime at every exec, and the next start (init ran this
# script again, so the daemon is gone) subtracts. A run shorter than
# STABLE_S is a short run; STABLE_S or longer resets the count.
#
#   0 short runs in a row: start at once, as before
#   n short runs in a row: wait 1, 2, 4, ... seconds, up to the daemon cap
#   DEGRADED_AFTER or more: the same wait, and the state says degraded
#
# Nothing here disables a daemon, enters safe mode or reboots: the wait is
# only ever as long as the cap, so the daemon keeps being retried, and what
# escalates is not decided here (omcid and userland hangs belong to the
# watchdog, docs/KERNEL.md; `services stop <name>` is the way to disable).
#
# The state file, /var/run/respawn/<name> (RAM), key=value lines, written
# at every start by rename so a reader never sees half of it:
#   state=ok|backoff|degraded    as of the latest start
#   starts=N          starts since boot, the first included
#   fast=N            consecutive short runs before this start
#   last_run_s=N      how long the previous run lived (absent on the first)
#   delay_s=N         the wait before this start
#   cap_s=N           the longest wait this daemon gets
#   stable_s=N        a run this long counts as healthy
#   last_start_s=N    uptime of this start, in seconds
# The state is not rewritten when the daemon stops dying, so a reader takes
# a daemon as healthy again once uptime - last_start_s >= stable_s.
#
# A log line goes to the kernel log (dmesg, the ramlog, syslog through
# klogd) at every restart, except that beyond the first DEGRADED_AFTER short
# runs only every tenth is written, so a crash loop cannot cycle the ring:
#   respawn: event=respawn service=NAME state=... starts=N fast=N ...
#
# ash/dash-compatible: every respawn entry runs under busybox ash, not bash
# (AGENTS.md, make lint checks this file in POSIX sh mode too). Every value
# below can be overridden by the environment for the host test only.
name=$1
oom=$2
log=$3
shift 3
{ echo "$oom" > "${RESPAWN_OOM_FILE:-/proc/self/oom_score_adj}"; } 2>/dev/null || true

DIR=${RESPAWN_DIR:-/var/run/respawn}
UPTIME=${RESPAWN_UPTIME:-/proc/uptime}
KMSG=${RESPAWN_KMSG:-/dev/kmsg}
SLEEP=${RESPAWN_SLEEP:-sleep}
STABLE_S=${RESPAWN_STABLE_S:-30}
DEGRADED_AFTER=${RESPAWN_DEGRADED_AFTER:-5}

# The longest wait per daemon. omcid is critical and covered by the
# watchdog (a 60 s ping deadline, docs/SETTINGS.md): its wait stays far
# below that, so the budget can only stop a spin, never delay a recovery
# the watchdog would otherwise get. dropbear is the way in, so it is kept
# short too. The rest are not on the way to the OLT or to a login.
case $name in
omcid)       cap=2 ;;
dropbear)    cap=10 ;;
syslogd|confd) cap=30 ;;
*)           cap=60 ;;
esac

uptime_s() {
	_up=0
	read -r _up _ < "$UPTIME" 2>/dev/null || _up=0
	_up=${_up%%.*}
	case $_up in
	'' | *[!0-9]*) _up=0 ;;
	esac
	echo "$_up"
}

num() { # a stored value is a number or it is 0
	case $1 in
	'' | *[!0-9]*) echo 0 ;;
	*) echo "$1" ;;
	esac
}

starts=0
fast=0
last=-1
if [ -r "$DIR/$name" ]; then
	while IFS='=' read -r k v; do
		case $k in
		starts) starts=$(num "$v") ;;
		fast) fast=$(num "$v") ;;
		last_start_s) last=$(num "$v") ;;
		esac
	done < "$DIR/$name"
fi

now=$(uptime_s)
starts=$((starts + 1))
ran=-1
if [ "$last" -ge 0 ]; then
	ran=$((now - last))
	if [ "$ran" -ge "$STABLE_S" ]; then
		fast=0
	else
		fast=$((fast + 1))
	fi
fi

delay=0
if [ "$fast" -ge 1 ]; then
	delay=1
	n=1
	while [ "$n" -lt "$fast" ] && [ "$delay" -lt "$cap" ]; do
		delay=$((delay * 2))
		n=$((n + 1))
	done
	[ "$delay" -gt "$cap" ] && delay=$cap
fi

state=ok
[ "$fast" -ge 1 ] && state=backoff
[ "$fast" -ge "$DEGRADED_AFTER" ] && state=degraded

# Logged on every restart and every short run up to the budget, then one in
# ten. The first start of the boot (ran=-1) is not a restart.
if [ "$ran" -ge 0 ] && { [ "$fast" -le "$DEGRADED_AFTER" ] || [ $((fast % 10)) -eq 0 ]; }; then
	lvl=5
	[ "$fast" -ge 1 ] && lvl=4
	printf '<%s>respawn: event=respawn service=%s state=%s starts=%s fast=%s last_run_s=%s delay_s=%s cap_s=%s\n' \
		"$lvl" "$name" "$state" "$starts" "$fast" "$ran" "$delay" "$cap" >> "$KMSG" 2>/dev/null || true
fi

# Bounded by the cap, and by construction: the waiting shell is the pid init
# tracks, so a shutdown signal reaches it as it would reach the daemon.
[ "$delay" -gt 0 ] && "$SLEEP" "$delay"

# The uptime recorded is the one at the exec, after the wait, so the next
# start measures the life of the daemon itself.
start=$(uptime_s)
if mkdir -p "$DIR" 2>/dev/null; then
	{
		echo "state=$state"
		echo "starts=$starts"
		echo "fast=$fast"
		[ "$ran" -ge 0 ] && echo "last_run_s=$ran"
		echo "delay_s=$delay"
		echo "cap_s=$cap"
		echo "stable_s=$STABLE_S"
		echo "last_start_s=$start"
	} > "$DIR/.$name.tmp" 2>/dev/null && mv -f "$DIR/.$name.tmp" "$DIR/$name" 2>/dev/null
fi

exec "$@" >> "$log" 2>&1
