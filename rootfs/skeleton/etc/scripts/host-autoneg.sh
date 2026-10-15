#!/bin/sh
#
# host-autoneg.sh -- the 1000BASE-X auto-negotiation of the host port (the
# SFP edge, switch port 0), from the odi-only key HOST_AUTONEG:
#
#     on, empty or absent   auto-negotiation on, the default and what the
#                           stock image runs
#     off                   forced 1000BASE-X full duplex, for a host whose
#                           SFP port has auto-negotiation off
#
# Any other value runs on, and says so. The kernel does the register write
# (odi_switch.host_autoneg, docs/SWITCH.md "Host port auto-negotiation");
# this script decides what to write and when, and keeps the stick reachable
# when the choice is wrong. A wrong mode costs the only way in, so nothing
# here persists a mode that has not been seen to work:
#
#     host-autoneg.sh boot          rcS, once, after network.sh: count the
#                                   boot, fire the safe mode, or apply off
#                                   and start the boot watch (below)
#     host-autoneg.sh try on|off [SECONDS]
#                                   a trial: one second later the mode is
#                                   written (so this answer leaves first),
#                                   in RAM only, and it reverts by itself
#                                   after SECONDS (default 120) unless
#                                   confirmed
#     host-autoneg.sh confirm       keep the trial mode: stores HOST_AUTONEG.
#                                   Only reaches the stick if the link works
#                                   in that mode, which is the point
#     host-autoneg.sh revert        end the trial now, back to the mode before
#     host-autoneg.sh ack           acknowledge a safe mode (removes its record)
#     host-autoneg.sh status        the state as key=value lines
#
# The boot watch. With HOST_AUTONEG=off the boot writes off and watches the
# host link (odi_switch.host_link) for WATCH_S seconds. No link: it switches
# to auto-negotiation for WATCH_S seconds (the fallback, logged and shown in
# the web UI), then back to off, and so on, until a link comes up in either
# mode, for at most WATCH_MAX_S; past that it stays on auto-negotiation. The
# setting itself is never changed by the watch. Alternating, rather than
# falling back once for the whole boot, is for a host that boots slower than
# the stick (both powered up together after an outage): a forced host that
# comes up late would otherwise find the stick on auto-negotiation for the
# rest of the boot.
#
# The safe mode. HOST_AUTONEG=off and SAFE_BOOTS (3) boots in a row that each
# ended before BOOT_OK_S (60) seconds of uptime: the boot after them clears
# the key, runs auto-negotiation, and leaves a record on the config
# partition until someone acknowledges it (the web UI banner, or `ack`).
# Power the stick for about 20 to 40 seconds and pull it, three times, and
# the fourth boot runs auto-negotiation. U-Boot here has no boot counter, and its environment
# must not be written on every boot, so the counter is a file on the config
# partition, written by temp file and rename, only while HOST_AUTONEG=off:
# a stick that never sets the key never writes it.
#
# Every wait is bounded (sysfs writes and reads, flash) and every state
# change is one rename of the state file, under a mkdir lock.
set -u

FLASH=${FLASH:-/etc/scripts/flash}
SELF=${SELF:-/etc/scripts/host-autoneg.sh}
PARAM_DIR=${PARAM_DIR:-/sys/module/odi_switch/parameters}
STATE=${STATE:-/var/run/host-autoneg}
LOCKDIR=${LOCKDIR:-/var/run/host-autoneg.lock}
BOOTS=${BOOTS:-/etc/config/host-autoneg.boots}
SAFEMODE=${SAFEMODE:-/etc/config/host-autoneg.safemode}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
BUILD=${BUILD:-/etc/odi-build}
LOG=${LOG:-/var/log/services.log}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}

TRIAL_S=${TRIAL_S:-120}         # a trial reverts after this long unconfirmed
TRIAL_MAX_S=${TRIAL_MAX_S:-600} # the longest trial a caller may ask for
WATCH_S=${WATCH_S:-30}          # each phase of the boot watch
WATCH_MAX_S=${WATCH_MAX_S:-600} # the whole boot watch
BOOT_OK_S=${BOOT_OK_S:-60}      # uptime that makes a boot not short
SAFE_BOOTS=${SAFE_BOOTS:-3}     # short boots in a row before the one that fires the safe mode
IO_S=${IO_S:-5}                 # one sysfs or flash call
LOCK_S=${LOCK_S:-10}            # waiting for the lock

say() { echo "host-autoneg: $*"; }
die() { echo "host-autoneg: $*" >&2; exit 1; }

uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

# event NAME TEXT: one line to the kernel log (dmesg, the ramlog, and syslog
# through klogd), at notice, and to stdout.
event() {
	name=$1
	shift
	line="event=$name setting=$setting running=$running $*"
	printf '<5>host-autoneg: %s\n' "$line" 2>/dev/null >> "$KMSG"
	say "$line"
	ev=$name
	ev_up=$(uptime_s)
	ev_msg=$*
}

# ---- the key, the kernel, the files

# parse VALUE: the mode HOST_AUTONEG means; anything but off is on.
parse() {
	case $1 in
	off) echo off ;;
	*)   echo on ;;
	esac
}

key_raw() {
	"$TIMEOUT" "$IO_S" "$FLASH" get HOST_AUTONEG 2>/dev/null | sed -n 's/^HOST_AUTONEG=//p' | head -n 1
}

param_ok() { [ -w "$PARAM_DIR/host_autoneg" ]; }

# hw_write on|off: the register write, through the kernel parameter.
hw_write() {
	v=1
	[ "$1" = off ] && v=0
	# shellcheck disable=SC2016 # the inner shell expands them
	"$TIMEOUT" "$IO_S" sh -c 'echo "$1" > "$2"' _ "$v" "$PARAM_DIR/host_autoneg" 2>/dev/null
}

# link: 1, 0, or nothing when the kernel cannot say.
link() {
	"$TIMEOUT" "$IO_S" cat "$PARAM_DIR/host_link" 2>/dev/null | sed -n '1s/^\([01]\)$/\1/p'
}

# put_in_place FILE: puts FILE.new.$$ in place of FILE in one rename on the same
# filesystem, so a reader or a power cut sees the old file or the new one.
put_in_place() {
	mv "$1.new.$$" "$1" 2>/dev/null || { rm -f "$1.new.$$"; return 1; }
}

# ---- the state file, /var/run/host-autoneg

clear_trial() { trial_mode='' trial_prev='' trial_id='' trial_deadline=''; }

load() {
	setting=on running=on phase=steady fallback=0 safe_mode=0
	clear_trial
	watch_until='' ev='' ev_up='' ev_msg=''
	[ -f "$STATE" ] || return 0
	while IFS='=' read -r k v; do
		case $k in
		setting) setting=$v ;;
		running) running=$v ;;
		phase) phase=$v ;;
		fallback) fallback=$v ;;
		safe_mode) safe_mode=$v ;;
		trial_mode) trial_mode=$v ;;
		trial_prev) trial_prev=$v ;;
		trial_id) trial_id=$v ;;
		trial_deadline) trial_deadline=$v ;;
		watch_until) watch_until=$v ;;
		event) ev=$v ;;
		event_uptime) ev_up=$v ;;
		message) ev_msg=$v ;;
		esac
	done < "$STATE"
}

save() {
	{
		echo "setting=$setting"
		echo "running=$running"
		echo "phase=$phase"
		echo "fallback=$fallback"
		echo "safe_mode=$safe_mode"
		echo "trial_mode=$trial_mode"
		echo "trial_prev=$trial_prev"
		echo "trial_id=$trial_id"
		echo "trial_deadline=$trial_deadline"
		echo "watch_until=$watch_until"
		echo "event=$ev"
		echo "event_uptime=$ev_up"
		echo "message=$ev_msg"
	} > "$STATE.new.$$" || { rm -f "$STATE.new.$$"; say "could not write $STATE" >&2; return 1; }
	put_in_place "$STATE" || say "could not install $STATE" >&2
}

# ---- the lock: one state change at a time. mkdir is atomic; a holder that
# died is found by its pid and its lock taken over.

lock() {
	i=0
	while ! mkdir "$LOCKDIR" 2>/dev/null; do
		p=$(cat "$LOCKDIR/pid" 2>/dev/null)
		if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
			rm -rf "$LOCKDIR"
			continue
		fi
		# A holder killed between its mkdir and its pid write leaves no
		# pid: after the wait, that lock is stale too.
		if [ "$i" -ge "$LOCK_S" ] && [ -z "$p" ]; then
			rm -rf "$LOCKDIR"
			i=0
			continue
		fi
		[ "$i" -lt "$LOCK_S" ] || die "busy: another host-autoneg.sh holds $LOCKDIR"
		"$SLEEP" 1
		i=$((i + 1))
	done
	echo $$ > "$LOCKDIR/pid"
}
unlock() { rm -rf "$LOCKDIR"; }

# spawn VERB ARGS...: this script again, detached, so it outlives the caller
# (rcS, confd). Its output goes to the services log.
spawn() {
	setsid sh "$SELF" "$@" < /dev/null >> "$LOG" 2>&1 &
}

# ---- boot

# The short-boot counter: the boots in a row, this one included, that have
# not (yet) reached BOOT_OK_S of uptime; so the boots before this one that
# were short are the counter minus one. Anything unreadable counts as 0.
count_boot() {
	n=$(sed -n '1s/^\([0-9][0-9]*\)$/\1/p' "$BOOTS" 2>/dev/null)
	n=$(( ${n:-0} + 1 ))
	echo "$n" > "$BOOTS.new.$$" && put_in_place "$BOOTS" || return 1
	echo "$n"
}

safe_mode_fire() {
	n=$1
	"$TIMEOUT" "$IO_S" "$FLASH" set HOST_AUTONEG "" > /dev/null 2>&1
	setting=on running=on safe_mode=1 phase=steady
	if [ -n "$(key_raw)" ]; then
		event safe_mode_failed "short_boots=$(( n - 1 )) could not clear HOST_AUTONEG; running auto-negotiation this boot"
	else
		event safe_mode "short_boots=$(( n - 1 )) $(( n - 1 )) short boots in a row: HOST_AUTONEG cleared, auto-negotiation on"
	fi
	{
		echo "short_boots=$(( n - 1 ))"
		echo "uptime=$(uptime_s)"
		echo "previous=off"
		echo "build=$(sed -n 's/^image=//p' "$BUILD" 2>/dev/null | head -n 1)"
	} > "$SAFEMODE.new.$$" && put_in_place "$SAFEMODE"
	rm -f "$BOOTS"
}

boot() {
	lock
	load
	raw=$(key_raw)
	setting=$(parse "$raw")
	running=on phase=steady fallback=0
	clear_trial
	watch_until=''
	[ -f "$SAFEMODE" ] && safe_mode=1 || safe_mode=0
	if [ -n "$raw" ] && [ "$raw" != on ] && [ "$raw" != off ]; then
		event bad_value "HOST_AUTONEG=$raw is neither on nor off; running auto-negotiation"
	fi
	if [ "$setting" = on ]; then
		rm -f "$BOOTS"
		# The default is not an event: nothing is written, nothing logged.
		ev=boot ev_up=$(uptime_s) ev_msg="auto-negotiation on"
		save
		unlock
		return 0
	fi
	if ! param_ok; then
		event no_kernel "this kernel has no odi_switch.host_autoneg; running auto-negotiation"
		save
		unlock
		return 0
	fi
	if n=$(count_boot); then
		if [ "$n" -gt "$SAFE_BOOTS" ]; then
			safe_mode_fire "$n"
			save
			unlock
			return 0
		fi
		spawn boot-ok
	else
		say "could not write $BOOTS: this boot is not counted" >&2
	fi
	if hw_write off; then
		running=off phase=watch
		watch_until=$(( $(uptime_s) + WATCH_MAX_S ))
		event applied "auto-negotiation off (forced 1000BASE-X); watching the host link for ${WATCH_S} s"
		save
		unlock
		spawn watch
	else
		event apply_failed "could not write odi_switch.host_autoneg; running auto-negotiation"
		save
		unlock
	fi
	return 0
}

# boot-ok: once the uptime reaches BOOT_OK_S this boot was not short, and
# the counter goes. Sleeps until then.
boot_ok() {
	up=$(uptime_s)
	[ "${up:-0}" -ge "$BOOT_OK_S" ] || "$SLEEP" $(( BOOT_OK_S - ${up:-0} ))
	if [ -f "$BOOTS" ]; then
		rm -f "$BOOTS"
		say "up ${BOOT_OK_S} s: the short-boot counter is cleared"
	fi
}

# watch: the boot watch (above). Each phase polls the link once a second.
watch() {
	t0=$(uptime_s)
	mode=off
	while :; do
		i=0
		while :; do
			if [ "$(link)" = 1 ]; then
				lock
				load
				phase=steady watch_until=
				if [ "$mode" = off ]; then
					fallback=0
					event forced_link "host link up with auto-negotiation off after $(( $(uptime_s) - t0 )) s"
				else
					fallback=1
					event fallback "no host link with auto-negotiation off; the link came up with it on after $(( $(uptime_s) - t0 )) s. HOST_AUTONEG stays off"
				fi
				save
				unlock
				return 0
			fi
			[ "$i" -lt "$WATCH_S" ] || break
			"$SLEEP" 1
			i=$((i + 1))
		done
		lock
		load
		if [ "$phase" != watch ]; then
			unlock
			return 0
		fi
		if [ $(( $(uptime_s) - t0 )) -ge "$WATCH_MAX_S" ]; then
			[ "$running" = on ] || hw_write on
			running=on fallback=1 phase=steady watch_until=
			event fallback_nolink "no host link in either mode for $(( $(uptime_s) - t0 )) s; staying on auto-negotiation. HOST_AUTONEG stays off"
			save
			unlock
			return 0
		fi
		if [ "$mode" = off ]; then mode=on; else mode=off; fi
		if hw_write "$mode"; then
			running=$mode
			if [ "$mode" = on ]; then fallback=1; else fallback=0; fi
			event watch_switch "no host link for ${WATCH_S} s; trying auto-negotiation $mode"
		fi
		save
		unlock
	done
}

# ---- the trial

try() {
	mode=$1 secs=${2:-$TRIAL_S}
	case $mode in on|off) ;; *) die "usage: host-autoneg.sh try on|off [SECONDS]" ;; esac
	case $secs in ''|*[!0-9]*) die "SECONDS must be a number" ;; esac
	[ "$secs" -ge 10 ] && [ "$secs" -le "$TRIAL_MAX_S" ] || die "SECONDS must be 10 to $TRIAL_MAX_S"
	param_ok || die "this kernel has no odi_switch.host_autoneg"
	lock
	load
	case $phase in
	watch) unlock; die "the boot watch is still deciding the mode (until uptime $watch_until s); try again after it" ;;
	trial) unlock; die "a trial of $trial_mode is already running; confirm or revert it first" ;;
	esac
	if [ "$mode" = "$running" ] && [ "$mode" = "$setting" ]; then
		unlock
		say "already $mode, nothing to try"
		return 0
	fi
	up=$(uptime_s)
	trial_id="$up.$$" trial_mode=$mode trial_prev=$running
	trial_deadline=$(( up + 1 + secs ))
	phase=trial
	event trial_started "trying auto-negotiation $mode for $secs s: confirm (host-autoneg.sh confirm) or it reverts to $trial_prev"
	save
	unlock
	spawn trial-run "$trial_id" "$secs"
	return 0
}

# trial-run ID SECONDS: one second for the caller answer to leave, the
# write, then the window, then the expiry.
trial_run() {
	id=$1 secs=$2
	"$SLEEP" 1
	lock
	load
	if [ "$phase" != trial ] || [ "$trial_id" != "$id" ]; then
		unlock
		return 0
	fi
	if hw_write "$trial_mode"; then
		running=$trial_mode
		save
	else
		running=$trial_prev phase=steady
		event trial_failed "could not write odi_switch.host_autoneg; still $running"
		save
		unlock
		return 1
	fi
	unlock
	"$SLEEP" "$secs"
	expire "$id"
}

expire() {
	id=$1
	lock
	load
	if [ "$phase" = trial ] && [ "$trial_id" = "$id" ]; then
		back_to_prev trial_expired "not confirmed in time"
	fi
	unlock
}

back_to_prev() {
	name=$1 why=$2
	[ "$running" = "$trial_prev" ] || hw_write "$trial_prev"
	running=$trial_prev
	if [ "$setting" = off ] && [ "$running" = on ]; then fallback=1; else fallback=0; fi
	phase=steady
	event "$name" "$why: back to auto-negotiation $running; HOST_AUTONEG unchanged ($setting)"
	clear_trial
	save
}

confirm() {
	lock
	load
	if [ "$phase" != trial ]; then
		unlock
		die "no trial is running"
	fi
	if [ "$running" != "$trial_mode" ]; then
		unlock
		die "the trial mode is not applied yet; confirm again in a second"
	fi
	if [ "$trial_mode" = off ]; then v=off; else v=""; fi
	"$TIMEOUT" "$IO_S" "$FLASH" set HOST_AUTONEG "$v" > /dev/null 2>&1
	if [ "$(key_raw)" != "$v" ]; then
		unlock
		die "could not store HOST_AUTONEG=$v; the trial carries on and reverts unless confirmed"
	fi
	setting=$trial_mode fallback=0 phase=steady
	event trial_confirmed "auto-negotiation $setting confirmed and stored"
	clear_trial
	save
	unlock
	[ "$setting" = on ] && rm -f "$BOOTS"
	return 0
}

revert() {
	lock
	load
	if [ "$phase" != trial ]; then
		unlock
		die "no trial is running"
	fi
	back_to_prev trial_reverted "reverted on request"
	unlock
}

ack() {
	lock
	load
	rm -f "$SAFEMODE"
	if [ "$safe_mode" = 1 ]; then
		safe_mode=0
		event safe_mode_ack "the safe mode was acknowledged"
		save
	fi
	unlock
}

status() {
	load
	if [ ! -f "$STATE" ]; then
		setting=$(parse "$(key_raw)")
		[ "$(cat "$PARAM_DIR/host_autoneg" 2>/dev/null)" = N ] && running=off
	fi
	echo "setting=$setting"
	echo "running=$running"
	echo "phase=$phase"
	echo "fallback=$fallback"
	echo "trial_mode=$trial_mode"
	echo "trial_deadline=$trial_deadline"
	echo "watch_until=$watch_until"
	echo "uptime=$(uptime_s)"
	l=$(link)
	echo "link=${l:-unknown}"
	echo "event=$ev"
	echo "event_uptime=$ev_up"
	echo "message=$ev_msg"
	if [ -f "$SAFEMODE" ]; then
		echo "safe_mode=1"
		sed -n 's/^\([a-z_]*\)=/safe_mode_\1=/p' "$SAFEMODE"
	else
		echo "safe_mode=0"
	fi
	[ -f "$BOOTS" ] && echo "short_boots=$(cat "$BOOTS")"
	return 0
}

case "${1:-}" in
boot)       boot ;;
boot-ok)    boot_ok ;;
watch)      watch ;;
try)        shift; try "$@" ;;
trial-run)  shift; trial_run "$@" ;;
expire)     shift; expire "$@" ;;
confirm)    confirm ;;
revert)     revert ;;
ack)        ack ;;
status)     status ;;
parse)      parse "${2:-}" ;;
*)          echo "usage: $0 boot|try on|off [SECONDS]|confirm|revert|ack|status" >&2; exit 1 ;;
esac
