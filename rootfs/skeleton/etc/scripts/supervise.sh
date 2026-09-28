#!/bin/sh
# supervise.sh -- minimal respawn-with-backoff for a foreground daemon.
# Sourced by /etc/init.d/services and by the omci_start() function in rcS, never run on
# its own. ash/dash-compatible: rcS and services run under busybox ash, not
# bash (AGENTS.md, make lint checks this file in POSIX sh mode too).
#
# supervise <name> <oom_score_adj> <cmd...>
#
# Backgrounds <cmd...>, sets its oom_score_adj, waits for it to exit, and
# restarts it -- up to SUPERVISE_MAX_RESTARTS times inside any
# SUPERVISE_WINDOW_S window. Past that it stops and logs why: a daemon that
# cannot stay up for even a minute is a problem for the watchdog to escalate
# (docs/SETTINGS.md, "Watchdog rules" -- a client whose respawns burn its
# own ping deadline still stops the kicker eventually), not something a
# restart loop should spin on forever.
#
# Assumes <cmd...> runs in the FOREGROUND (none of metricsd, confd,
# dropbear or omcid double-forks to daemonize itself -- the trailing `&` at
# every call site is what backgrounds them today). A self-daemonizing
# command would make the immediate child exit right after forking, and
# `wait` would return long before the real daemon does.
SUPERVISE_MAX_RESTARTS=${SUPERVISE_MAX_RESTARTS:-5}
SUPERVISE_WINDOW_S=${SUPERVISE_WINDOW_S:-60}

supervise() {
	name=$1
	oom=$2
	shift 2
	(
		restarts=0
		window_start=$(cut -d. -f1 /proc/uptime 2>/dev/null)
		[ -n "$window_start" ] || window_start=0
		while :; do
			"$@" &
			pid=$!
			if [ -w "/proc/$pid/oom_score_adj" ]; then
				echo "$oom" > "/proc/$pid/oom_score_adj" 2>/dev/null || true
			fi
			wait "$pid"
			now=$(cut -d. -f1 /proc/uptime 2>/dev/null)
			[ -n "$now" ] || now=0
			if [ $((now - window_start)) -gt "$SUPERVISE_WINDOW_S" ]; then
				restarts=0
				window_start=$now
			fi
			restarts=$((restarts + 1))
			echo "supervise: $name exited, restart $restarts/$SUPERVISE_MAX_RESTARTS" > /dev/kmsg 2>/dev/null || true
			if [ "$restarts" -gt "$SUPERVISE_MAX_RESTARTS" ]; then
				echo "supervise: $name restarted $restarts times in ${SUPERVISE_WINDOW_S}s, giving up" > /dev/kmsg 2>/dev/null || true
				break
			fi
			sleep 1
		done
	) &
}
