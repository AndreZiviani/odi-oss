#!/bin/sh
# trial-guard.sh -- a trial boot that nobody can reach goes back by itself
# (docs/FLASHING.md, "The trial guard"). Run by /etc/inittab as a `once`
# entry, next to slot-state.sh, whose state file it reads.
#
# U-Boot reverts a trial that fails to boot. A trial that boots and then
# cannot be reached over the management path (the kernel and userland up,
# so every watchdog rule is satisfied) stays up until someone pulls the
# power. This closes that gap for a trial only:
#
#   - armed only when /var/run/odi-slot says the running slot is not
#     committed and the next boot goes to another slot (uncommitted=1,
#     next_boot != running): a reboot then returns to the committed slot,
#     which is the whole point. Never on a committed slot, never when the
#     state cannot be told, never with /etc/config/trial-guard.off;
#   - it registers an odi_wdt client, "trial", with a deadline of
#     DEADLINE_S, and never pings it: unless released, the kernel stops
#     kicking the watchdog at the deadline and the board resets, logged as a
#     missed client with its name. A guard that dies therefore still
#     reverts;
#   - it releases the client (deadline 0) the first time it sees the
#     management path work in BOTH directions, or once the slot is
#     committed (slot-state.sh rewrites the state after nv commit).
#
# Two-way evidence, because one-way says nothing about the way back: a
# frame from the host proves receive only, and a stick whose transmit is
# dead still receives every ARP request the host sends. Either of
#   - a TCP connection with a peer that is not the stick itself, past the
#     handshake (any state but LISTEN, SYN_SENT and SYN_RECV in
#     /proc/net/tcp or tcp6): the peer received what the stick sent;
#   - a neighbour in REACHABLE (ip neigh): it answered the stick.
# An ssh login, the web UI and a metrics scrape each make the first. A host
# that never talks to the stick looks the same as a dead path from in here,
# so the trial reverts: nobody can have checked it, and every way to commit
# it is a TCP session that disarms the guard first.
#
# DEADLINE_S, 300: boot to the services takes about a minute, a metrics
# scrape comes every 60 s, and the trial check is an ssh login in the first
# minutes. Five scrape intervals is room enough; a stranded trial is back on
# the committed slot about six minutes after power-on.
#
# Every wait is bounded; the script always exits 0.
set -u

PATH=${TRIAL_GUARD_PATH:-/sbin:/usr/sbin:/bin:/usr/bin}${PATH:+:$PATH}
export PATH

STATE=${STATE:-/var/run/odi-slot}
REGISTER=${REGISTER:-/proc/odi_wdt/register}
OFF_FLAG=${OFF_FLAG:-/etc/config/trial-guard.off}
PROC_TCP=${PROC_TCP:-/proc/net/tcp /proc/net/tcp6}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
IP=${IP:-ip}
LOGGER=${LOGGER:-logger}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}

DEADLINE_S=${DEADLINE_S:-300}	# the trial reverts this long after arming, unless proven
POLL_S=${POLL_S:-5}		# one look at the evidence
STATE_WAIT_S=${STATE_WAIT_S:-60}	# slot-state.sh starts in the same breath
CMD_S=${CMD_S:-5}		# one ip, logger or /proc write

CLIENT=trial

# To the kernel log, so the line is in the ramlog the next boot reads after
# a revert, and to syslog.
log() {
	printf '<4>trial-guard: %s\n' "$*" 2>/dev/null >> "$KMSG"
	"$TIMEOUT" "$CMD_S" "$LOGGER" -t trial-guard -p daemon.warning "$*" 2>/dev/null || true
}

uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

state_val() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | head -n 1; }

wdt_write() {
	"$TIMEOUT" "$CMD_S" sh -c 'echo "$1" > "$2"' _ "$1" "$REGISTER" 2>/dev/null
}

# The hex address of /proc/net/tcp: IPv4 as one little-endian word, IPv6 as
# four. 127/8, ::1 and ::ffff:127/8 are the stick talking to itself.
is_loopback() {
	case "$1" in
	??????7F) return 0 ;;
	00000000000000000000000001000000) return 0 ;;
	0000000000000000FFFF0000??????7F) return 0 ;;
	esac
	return 1
}

# Prints what proved it, and returns 0, for a TCP connection past the
# handshake with a peer other than the stick itself (same address on both
# ends is the stick connecting to its own management address).
tcp_proven() {
	for f in $PROC_TCP; do
		[ -r "$f" ] || continue
		while read -r _sl laddr rem st _rest; do
			case "$st" in
			01 | 04 | 05 | 06 | 07 | 08 | 09 | 0B) ;;
			*) continue ;;
			esac
			raddr=${rem%%:*}
			is_loopback "$raddr" && continue
			[ "$raddr" = "${laddr%%:*}" ] && continue
			echo "tcp peer $raddr state $st"
			return 0
		done < "$f"
	done
	return 1
}

neigh_proven() {
	n=$("$TIMEOUT" "$CMD_S" "$IP" neigh show nud reachable 2>/dev/null | head -n 1)
	[ -n "$n" ] || return 1
	echo "reachable neighbour ${n%% *}"
	return 0
}

release() {
	if wdt_write "$CLIENT 0"; then
		log "released: $1"
	else
		log "could not release the $CLIENT client ($1); the watchdog reverts this trial"
	fi
}

[ -e "$OFF_FLAG" ] && exit 0

i=0
while [ ! -s "$STATE" ] && [ "$i" -lt "$STATE_WAIT_S" ]; do
	"$SLEEP" 1
	i=$((i + 1))
done

running=$(state_val running)
next=$(state_val next_boot)
case "$running" in 0 | 1) ;; *) exit 0 ;; esac
case "$next" in 0 | 1) ;; *) exit 0 ;; esac
[ "$(state_val uncommitted)" = 1 ] && [ "$next" != "$running" ] || exit 0

if [ ! -w "$REGISTER" ]; then
	log "trial of slot $running, but no $REGISTER: not armed"
	exit 0
fi
if ! wdt_write "$CLIENT $DEADLINE_S"; then
	log "trial of slot $running, but the $CLIENT client did not register: not armed"
	exit 0
fi
log "trial of slot $running: reverts to slot $next in ${DEADLINE_S} s unless the management path is seen working both ways (a TCP session or a reachable neighbour)"

t0=$(uptime_s)
case "$t0" in '' | *[!0-9]*) t0=0 ;; esac
end=$((t0 + DEADLINE_S))
while :; do
	if why=$(tcp_proven) || why=$(neigh_proven); then
		release "management path proven, $why"
		exit 0
	fi
	if [ "$(state_val uncommitted)" != 1 ]; then
		release "slot $running is committed now"
		exit 0
	fi
	now=$(uptime_s)
	case "$now" in '' | *[!0-9]*) now=$end ;; esac
	[ "$now" -lt "$end" ] || break
	"$SLEEP" "$POLL_S"
done
log "no two-way management traffic in ${DEADLINE_S} s: the watchdog reverts this trial to slot $next"
exit 0
