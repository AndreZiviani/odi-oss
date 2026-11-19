#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

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
#     host-autoneg.sh boot          rcS, once, after network.sh: apply off
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
#     host-autoneg.sh status        the state as key=value lines; while
#                                   host-serdes.sh owns the port, running=
#                                   is the live bit and owner=host-serdes
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
# The safe mode (safe-mode.sh): three short boots in a row with
# HOST_AUTONEG=off (five otherwise) make the next boot a recovery boot,
# which clears HOST_AUTONEG and runs auto-negotiation; on that boot this
# script writes nothing and refuses a trial.
#
# The host SerDes mode (HOST_SERDES, host-serdes.sh). HOST_AUTONEG is the
# auto-negotiation of Fiber 1G. While the port runs 2500basex or hisgmii,
# host-serdes.sh sets the bit itself, so this script applies nothing at boot
# (it logs event=serdes_mode when the key is off) and refuses a trial; it
# also refuses one while host-serdes.sh runs a trial or its boot watch. Its
# state, /var/run/host-serdes, is read under the same lock.
#
# Every wait is bounded (sysfs writes and reads, flash) and every state
# change is one rename of the state file, under a mkdir lock, the host-port
# lock host-serdes.sh takes too.
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

FLASH=${FLASH:-/etc/scripts/flash}
SELF=${SELF:-/etc/scripts/host-autoneg.sh}
PARAM_DIR=${PARAM_DIR:-/sys/module/odi_switch/parameters}
STATE=${STATE:-/var/run/host-autoneg}
SERDES_STATE=${SERDES_STATE:-/var/run/host-serdes}
LOCKDIR=${LOCKDIR:-/var/run/host-autoneg.lock}
SAFE_FLAG=${SAFE_FLAG:-/var/run/safe-mode}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
LOG=${LOG:-/var/log/services.log}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}

TRIAL_S=${TRIAL_S:-120}         # a trial reverts after this long unconfirmed
TRIAL_MAX_S=${TRIAL_MAX_S:-600} # the longest trial a caller may ask for
WATCH_S=${WATCH_S:-30}          # each phase of the boot watch
WATCH_MAX_S=${WATCH_MAX_S:-600} # the whole boot watch
IO_S=${IO_S:-5}                 # one sysfs or flash call
LOCK_S=${LOCK_S:-10}            # waiting for the lock

say() { echo "host-autoneg: $*"; }
die() { echo "host-autoneg: $*" >&2; exit 1; }

# The lock, the state file, the event line and the trial run
# (trial-lib.sh).
TAG=host-autoneg
LOCK_OWNER=host-autoneg.sh
STATE_KEYS="setting running phase fallback trial_mode trial_prev trial_id
	trial_deadline watch_until event:ev event_uptime:ev_up message:ev_msg"
# Next to this script; ${0%/*} rather than dirname, which would be one more
# process on the boot path (and in the rcS trace goldens).
case $0 in */*) TRIAL_LIB=${TRIAL_LIB:-${0%/*}/trial-lib.sh} ;; *) TRIAL_LIB=${TRIAL_LIB:-./trial-lib.sh} ;; esac
# shellcheck source=rootfs/skeleton/etc/scripts/trial-lib.sh
. "$TRIAL_LIB"

event_fields() { echo "setting=$setting running=$running"; }

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

# hw_read: on or off as the kernel holds it, nothing when it cannot say.
hw_read() {
	case $("$TIMEOUT" "$IO_S" cat "$PARAM_DIR/host_autoneg" 2>/dev/null) in
	N|0) echo off ;;
	Y|1) echo on ;;
	esac
}

# link: 1, 0, or nothing when the kernel cannot say.
link() {
	"$TIMEOUT" "$IO_S" cat "$PARAM_DIR/host_link" 2>/dev/null | sed -n '1s/^\([01]\)$/\1/p'
}

# serdes_owns: 0 when host-serdes.sh owns the port now: a mode other than
# fiber1g runs, or its trial or boot watch does. Sets sd_running and
# sd_phase. Read with the shell alone, under the lock; no state file is
# fiber1g, steady.
serdes_owns() {
	sd_running=fiber1g sd_phase=steady
	if [ -f "$SERDES_STATE" ]; then
		while IFS='=' read -r _k _v; do
			case $_k in
			running) sd_running=$_v ;;
			phase) sd_phase=$_v ;;
			esac
		done < "$SERDES_STATE"
	fi
	case $sd_running in 2500basex|hisgmii) return 0 ;; esac
	case $sd_phase in watch|trial|reapply) return 0 ;; esac
	return 1
}

# ---- the state file, /var/run/host-autoneg

clear_trial() { trial_mode='' trial_prev='' trial_id='' trial_deadline=''; }

load() {
	setting=on running=on phase=steady fallback=0
	clear_trial
	watch_until='' ev='' ev_up='' ev_msg=''
	state_load
}
save() { state_save; }

# ---- boot

boot() {
	lock
	load
	raw=$(key_raw)
	setting=$(parse "$raw")
	running=on phase=steady fallback=0
	clear_trial
	watch_until=''
	if [ -n "$raw" ] && [ "$raw" != on ] && [ "$raw" != off ]; then
		event bad_value "HOST_AUTONEG=$raw is neither on nor off; running auto-negotiation"
	fi
	if [ -f "$SAFE_FLAG" ]; then
		event safe_mode_boot "a safe-mode boot: auto-negotiation on, nothing applied (HOST_AUTONEG=${raw:-unset})"
		save
		unlock
		return 0
	fi
	if [ "$setting" = on ]; then
		# The default is not an event: nothing is written, nothing logged.
		ev=boot ev_up=$(uptime_s) ev_msg="auto-negotiation on"
		save
		unlock
		return 0
	fi
	if serdes_owns; then
		event serdes_mode "HOST_AUTONEG=off is not applied: the host port runs HOST_SERDES $sd_running ($sd_phase), which sets its own auto-negotiation"
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
	[ ! -f "$SAFE_FLAG" ] || die "this is a safe-mode boot: reboot to the saved configuration first"
	lock
	load
	case $phase in
	watch) unlock; die "the boot watch is still deciding the mode (until uptime $watch_until s); try again after it" ;;
	trial) unlock; die "a trial of $trial_mode is already running; confirm or revert it first" ;;
	esac
	if serdes_owns; then
		unlock
		die "the host port runs HOST_SERDES $sd_running ($sd_phase), which sets its own auto-negotiation; HOST_AUTONEG applies to fiber1g (host-serdes.sh)"
	fi
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

# The trial run (trial_run, trial-lib.sh): the write, then the window.
trial_apply() { hw_write "$trial_mode"; }
trial_applied() {
	running=$trial_mode
	# expire and revert go by the file. If it cannot be written it still
	# says the mode before, and they would leave the port in the trial
	# mode: back at once. The expiry gets a second go at the file.
	save || back_to_prev trial_failed "could not write $STATE"
}
trial_apply_failed() {
	running=$trial_prev phase=steady
	event trial_failed "could not write odi_switch.host_autoneg; still $running"
	save
}

# back_to_prev NAME WHY: the mode before the trial, live. Returns 1 when
# the parameter cannot be written back, and the event says so, or when the
# state file cannot be written. After a failed write, running is what the
# kernel reads back, or the trial mode it was left in when it cannot say.
back_to_prev() {
	name=$1 why=$2 rc=0 back="back to auto-negotiation $trial_prev"
	if [ "$running" != "$trial_prev" ] && ! hw_write "$trial_prev"; then
		now=$(hw_read)
		[ -n "$now" ] && running=$now
	else
		running=$trial_prev
	fi
	if [ "$running" != "$trial_prev" ]; then
		back="could not write odi_switch.host_autoneg back to auto-negotiation $trial_prev; it stays $running" rc=1
	fi
	if [ "$setting" = off ] && [ "$running" = on ]; then fallback=1; else fallback=0; fi
	phase=steady
	event "$name" "$why: $back; HOST_AUTONEG unchanged ($setting)"
	clear_trial
	save || rc=1
	return "$rc"
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
	return 0
}

# status: the state file, plus what is live. While host-serdes.sh owns the
# port, the file running= is what this script runs in Fiber 1G, not what the
# port runs; status prints the bit the kernel holds instead, and says who
# owns it.
status() {
	load
	if [ ! -f "$STATE" ]; then
		setting=$(parse "$(key_raw)")
		[ "$(cat "$PARAM_DIR/host_autoneg" 2>/dev/null)" = N ] && running=off
	fi
	owner=host-autoneg
	if serdes_owns; then
		owner=host-serdes
		_hw=$(hw_read)
		[ -n "$_hw" ] && running=$_hw
	fi
	echo "setting=$setting"
	echo "running=$running"
	echo "owner=$owner"
	[ "$owner" = host-autoneg ] ||
		echo "owner_message=the link mode HOST_SERDES=$sd_running ($sd_phase) sets the auto-negotiation; HOST_AUTONEG ($setting) applies in fiber1g"
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
	[ -f "$SAFE_FLAG" ] && echo "safe_mode_boot=1" || echo "safe_mode_boot=0"
	return 0
}

case "${1:-}" in
boot)       boot ;;
watch)      watch ;;
try)        shift; try "$@" ;;
trial-run)  shift; trial_run "$@" ;;
expire)     shift; expire "$@" ;;
confirm)    confirm ;;
revert)     revert ;;
status)     status ;;
parse)      parse "${2:-}" ;;
*)          echo "usage: $0 boot|try on|off [SECONDS]|confirm|revert|status" >&2; exit 1 ;;
esac
