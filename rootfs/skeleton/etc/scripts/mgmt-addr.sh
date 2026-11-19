#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# mgmt-addr.sh -- a change of the management addresses (LAN_IP_ADDR,
# LAN_SUBNET, LAN_ENABLE_IP2, LAN_IP_ADDR2, LAN_SUBNET2) and of the static
# routes beside them (LAN_ROUTES) as a trial that reverts by itself. A typo in an address is the one mistake that takes every
# way in to the stick with it, so nothing here stores an address that has not
# been seen to answer:
#
#     mgmt-addr.sh try [-s SECONDS] KEY=VALUE...
#                                   a trial: one second later the new values
#                                   are applied (network.sh addr, so this
#                                   answer leaves first), in RAM only, and
#                                   they revert after SECONDS (default 90)
#                                   unless confirmed
#     mgmt-addr.sh confirm [LOCAL [PEER]]
#                                   keep the trial: stores the keys. LOCAL is
#                                   the address the confirm arrived on (and
#                                   PEER the client): it must be a NEW
#                                   address, which is the proof that the new
#                                   address works. Over ssh both come from
#                                   SSH_CONNECTION; confd passes them
#     mgmt-addr.sh revert           end the trial now, back to the addresses
#                                   before it
#     mgmt-addr.sh status           the state as key=value lines
#
# The old address stays up during the trial. network.sh addr reads the keys
# (the five address keys and LAN_ROUTES) from the trial file (/var/run/mgmt-addr.trial) while it exists, moves
# the primary to the new address and keeps the previous primary on its own
# alias (br0:4); the second address follows the trial. So the way in that
# worked stays there if the new one is wrong, and the page opened on it can
# still revert. What it cannot do is confirm: a confirm that arrives on an
# address the stick already had proves nothing about the new one and is
# refused. When nothing new is reachable (only the netmask changed, or the
# second address was turned off) the confirm must come from a client that is
# on the link of the new netmask.
#
# LAN_ROUTES (up to 8 "DEST via GW" entries, docs/SETTINGS.md) is the sixth
# key of the trial, the others kept as they are in the store: network.sh addr
# installs the routes of the trial file in RAM (and flushes the routes of
# proto static first), and the end of the trial puts the stored ones back.
# A trial that changes ONLY LAN_ROUTES is confirmed from ANY client: no
# address is new, and the confirm itself, arriving, proves that the client
# still reaches the stick with the routes in place, which is the one thing a
# route change risks. A trial that changes an address key too follows the rule
# above. A LAN_ROUTES network.sh would turn off (malformed, a duplicate DEST, a
# gateway outside the management subnets) is refused by try, so that
# no such value is ever stored.
#
# A reboot during the trial undoes it. The keys are written only by confirm,
# and the state and the trial file are in /var/run, so the boot reads the
# store, which still holds the old addresses, and never the trial.
#
# The safe mode (safe-mode.sh): a recovery boot is entered through the rescue
# address (network.sh, br0:3) and the stored addresses are the thing being
# repaired, from a client that is usually not on their network. There is no
# trial in it: try refuses, and the web UI stores the keys directly then. The
# stored LAN_ROUTES are not applied in that boot either.
#
# Every wait is bounded (flash, network.sh) and every state change is one
# rename of the state file, under a mkdir lock.
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

FLASH=${FLASH:-/etc/scripts/flash}
NETWORK=${NETWORK:-/etc/scripts/network.sh}
SELF=${SELF:-/etc/scripts/mgmt-addr.sh}
STATE=${STATE:-/var/run/mgmt-addr}
TRIAL=${TRIAL:-/var/run/mgmt-addr.trial}
LOCKDIR=${LOCKDIR:-/var/run/mgmt-addr.lock}
SAFE_FLAG=${SAFE_FLAG:-/var/run/safe-mode}
OVERRIDE=${OVERRIDE:-/etc/config/lan-ip}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
LOG=${LOG:-/var/log/services.log}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}
export TRIAL

TRIAL_S=${TRIAL_S:-90}          # a trial reverts after this long unconfirmed
TRIAL_MIN_S=${TRIAL_MIN_S:-10}  # the shortest trial a caller may ask for
TRIAL_MAX_S=${TRIAL_MAX_S:-600} # the longest
IO_S=${IO_S:-5}                 # one flash call
NET_S=${NET_S:-15}              # one network.sh run
LOCK_S=${LOCK_S:-10}            # waiting for the lock

KEYS="LAN_IP_ADDR LAN_SUBNET LAN_ENABLE_IP2 LAN_IP_ADDR2 LAN_SUBNET2 LAN_ROUTES"
ADDR_KEYS="LAN_IP_ADDR LAN_SUBNET LAN_ENABLE_IP2 LAN_IP_ADDR2 LAN_SUBNET2"
DEF_IP=192.168.1.1
DEF_MASK=255.255.255.0

say() { echo "mgmt-addr: $*"; }
die() { echo "mgmt-addr: $*" >&2; exit 1; }

# The lock, the state file, the event line and the trial run
# (trial-lib.sh). A revert runs from the address being left: back_to_prev
# defers the move back.
TAG=mgmt-addr
LOCK_OWNER=mgmt-addr.sh
REVERT_HOW=defer
STATE_KEYS="phase applied trial_id trial_deadline prev_ip prev_mask"
for k in $KEYS; do STATE_KEYS="$STATE_KEYS old_$k"; done
for k in $KEYS; do STATE_KEYS="$STATE_KEYS new_$k"; done
STATE_KEYS="$STATE_KEYS event:ev event_uptime:ev_up message:ev_msg"
# Next to this script; ${0%/*} rather than dirname, which would be one more
# process on the boot path (and in the rcS trace goldens).
case $0 in */*) TRIAL_LIB=${TRIAL_LIB:-${0%/*}/trial-lib.sh} ;; *) TRIAL_LIB=${TRIAL_LIB:-./trial-lib.sh} ;; esac
# shellcheck source=rootfs/skeleton/etc/scripts/trial-lib.sh
. "$TRIAL_LIB"

event_fields() { echo "ip=${new_LAN_IP_ADDR:-} prev=${prev_ip:-}"; }

# ---- the keys: six fixed names, held as old_NAME (the store, as the trial
# found it) and new_NAME (what the trial asks for)

setv() { eval "$1=\$2"; }
getv() { eval "printf '%s' \"\${$1:-}\""; }

store_get() {
	"$TIMEOUT" "$IO_S" "$FLASH" get "$1" 2>/dev/null | sed -n "s/^$1=//p" | sed -n 1p
}

# net_addr: network.sh addr, bounded. It reads the trial file when there is one.
net_addr() {
	"$TIMEOUT" "$NET_S" sh "$NETWORK" addr
}

# ---- the state file, /var/run/mgmt-addr

load() {
	phase=steady applied=0 trial_id='' trial_deadline='' prev_ip='' prev_mask=''
	ev='' ev_up='' ev_msg=''
	for k in $KEYS; do setv "old_$k" ''; setv "new_$k" ''; done
	state_load
}
save() { state_save; }

# ---- addresses

# onlink A B MASK: A and B are on one link under MASK.
onlink() {
	_ifs=$IFS
	IFS=.
	# shellcheck disable=SC2086 # twelve numbers, on purpose
	set -- $1 $2 $3
	IFS=$_ifs
	[ $# -eq 12 ] || return 1
	[ $(( $1 & ${9} )) -eq $(( $5 & ${9} )) ] &&
	[ $(( $2 & ${10} )) -eq $(( $6 & ${10} )) ] &&
	[ $(( $3 & ${11} )) -eq $(( $7 & ${11} )) ] &&
	[ $(( $4 & ${12} )) -eq $(( $8 & ${12} )) ]
}

# primary_of PREFIX: the primary address of the key set PREFIX (old_ or new_),
# as network.sh reads it: an empty key is the stock default.
primary_of() {
	_v=$(getv "${1}LAN_IP_ADDR")
	echo "${_v:-$DEF_IP}"
}
mask_of() {
	_v=$(getv "${1}$2")
	echo "${_v:-$DEF_MASK}"
}
# second_of PREFIX: the second address of the set, empty when it is off.
second_of() {
	if [ "$(getv "${1}LAN_ENABLE_IP2")" = 1 ]; then getv "${1}LAN_IP_ADDR2"; fi
}

# addr_changed: one of the five address keys differs from the store.
addr_changed() {
	for _k in $ADDR_KEYS; do
		[ "$(getv "new_$_k")" = "$(getv "old_$_k")" ] || return 0
	done
	return 1
}

# fresh_addrs: the addresses the trial adds, the ones the stick did not answer
# on before it: the new primary and second address that are neither the
# previous primary (what ran, not what was stored) nor the previous second.
fresh_addrs() {
	fresh=''
	_old2=$(second_of old_)
	for _a in "$(primary_of new_)" "$(second_of new_)"; do
		[ -n "$_a" ] || continue
		[ "$_a" != "$prev_ip" ] || continue
		[ "$_a" != "$_old2" ] || continue
		case " $fresh " in *" $_a "*) continue ;; esac
		fresh="$fresh $_a"
	done
}

# ---- the trial

# overlay FILE: the trial file network.sh reads, to FILE.
overlay() {
	{
		for k in $KEYS; do echo "$k=$(getv "new_$k")"; done
		echo "KEEP_IP=$prev_ip"
		echo "KEEP_MASK=$prev_mask"
	} > "$1"
}

try() {
	secs=$TRIAL_S
	if [ "${1:-}" = -s ]; then
		[ $# -ge 2 ] || die "usage: mgmt-addr.sh try [-s SECONDS] KEY=VALUE..."
		secs=$2
		shift 2
	fi
	case $secs in ''|*[!0-9]*) die "SECONDS must be a number" ;; esac
	[ "$secs" -ge "$TRIAL_MIN_S" ] && [ "$secs" -le "$TRIAL_MAX_S" ] || die "SECONDS must be $TRIAL_MIN_S to $TRIAL_MAX_S"
	[ $# -gt 0 ] || die "usage: mgmt-addr.sh try [-s SECONDS] KEY=VALUE..."
	[ ! -f "$SAFE_FLAG" ] || die "this is a safe-mode boot: no trial, the rescue address is the way in. Store the addresses and run apply.sh network"
	for a in "$@"; do
		case $a in *=*) ;; *) die "$a is not KEY=VALUE" ;; esac
		k=${a%%=*} v=${a#*=}
		case " $KEYS " in *" $k "*) ;; *) die "$k is not a management address key ($KEYS)" ;; esac
		case $k:$v in
		LAN_IP_ADDR:|LAN_SUBNET:) die "$k cannot be empty" ;;
		esac
	done

	lock
	load
	if [ "$phase" = trial ]; then
		unlock
		die "a trial is already running (until uptime $trial_deadline s); confirm or revert it first"
	fi
	# What is asked for, over what the store holds now.
	for k in $KEYS; do
		setv "old_$k" "$(store_get "$k")"
		setv "new_$k" "$(getv "old_$k")"
	done
	for a in "$@"; do setv "new_${a%%=*}" "${a#*=}"; done
	changed=''
	for k in $KEYS; do
		[ "$(getv "new_$k")" = "$(getv "old_$k")" ] || changed="$changed $k"
	done
	if [ -z "$changed" ]; then
		unlock
		say "the addresses asked for are the stored ones, nothing to try"
		return 0
	fi
	if [ -s "$OVERRIDE" ]; then
		case "$changed " in
		*" LAN_IP_ADDR "*|*" LAN_SUBNET "*)
			unlock
			die "$OVERRIDE exists and wins over LAN_IP_ADDR and LAN_SUBNET: remove it first, or the trial would change nothing" ;;
		esac
	fi
	# What runs now, which is what the trial keeps reachable. Not the store:
	# a store network.sh refused at the boot runs the stock default.
	prev=$("$TIMEOUT" "$IO_S" sh "$NETWORK" primary 2>/dev/null | sed -n 1p)
	prev_ip=${prev%% *} prev_mask=${prev#* }
	[ -n "$prev" ] || { prev_ip='' prev_mask=''; }

	# network.sh judges the values, as it does at every boot: the dry run
	# says what it would change, and refuses what the kernel would.
	overlay "$TRIAL.new.$$"
	if ! plan=$(TRIAL="$TRIAL.new.$$" "$TIMEOUT" "$NET_S" sh "$NETWORK" addr -n 2>&1); then
		rm -f "$TRIAL.new.$$"
		unlock
		echo "$plan" >&2
		die "network.sh refuses these values; nothing changed"
	fi
	echo "$plan"

	up=$(uptime_s)
	trial_id="$up.$$"
	trial_deadline=$(( up + 1 + secs ))
	phase=trial applied=0
	fresh_addrs
	if addr_changed; then
		how="confirm over${fresh:- a client on the new netmask}"
	else
		how="only the routes change: confirm from any client"
	fi
	event trial_started "trying for $secs s: $how (mgmt-addr.sh confirm) or it reverts to ${prev_ip:-the stored addresses}"
	put_in_place "$TRIAL" || { unlock; die "could not write $TRIAL"; }
	save
	unlock
	spawn trial-run "$trial_id" "$secs"
	return 0
}

# The trial run (trial_run, trial-lib.sh): the apply, then the window.
trial_apply() { net_addr; }
trial_applied() {
	applied=1
	save
}
trial_apply_failed() {
	back_to_prev trial_failed "network.sh could not apply the addresses"
}

# back_to_prev NAME WHY [defer]: ends the trial. The trial file goes, so
# network.sh addr reads the store again, and that moves the primary back and
# takes the kept address down. Run at once, or with defer from a caller that is
# itself on the address being left: its answer has to leave first, and the
# event says the move is under way; settle records how it ended.
back_to_prev() {
	name=$1 why=$2
	back="back to ${prev_ip:-the stored addresses}"
	rm -f "$TRIAL"
	if [ "${3:-}" = defer ]; then
		back="moving back to ${prev_ip:-the stored addresses} in a second"
		spawn settle "$name" "$why"
	elif ! net_addr; then
		back="network.sh could not move back to ${prev_ip:-the stored addresses} (run network.sh addr, or reboot)"
	fi
	phase=steady applied=0 trial_id='' trial_deadline=''
	event "$name" "$why: $back; nothing was stored"
	save
}

# settle [NAME WHY]: the move back, one second after the answer that asked
# for it, and the event of how it went. Under the lock when it can be had;
# without, the move is still made and the event still logged, and the state
# file is left to the holder.
settle() {
	name=${1:-trial_reverted} why=${2:-reverted on request}
	"$SLEEP" 1
	lock_try
	_lk=$?
	if [ -s "$TRIAL" ]; then
		[ "$_lk" = 1 ] || unlock
		return 0
	fi
	_ok=1
	net_addr || _ok=0
	if [ "$_lk" != 1 ]; then
		load
		if [ "$phase" = steady ]; then
			event "$name" "$why: $(settled_how); nothing was stored"
			save
		fi
		unlock
	else
		event "$name" "$why: $(settled_how); nothing was stored (the state file is busy)"
	fi
	[ "$_ok" = 1 ]
}
settled_how() {
	if [ "$_ok" = 1 ]; then
		echo "back to ${prev_ip:-the stored addresses}"
	else
		echo "network.sh could not move back to ${prev_ip:-the stored addresses} (run network.sh addr, or reboot)"
	fi
}

# via_check LOCAL PEER: whether a confirm that arrived on LOCAL from PEER
# proves the new addresses. Sets why when it does not.
via_check() {
	_local=$1 _peer=$2
	# Only LAN_ROUTES changed: no address is new and none can be proven by
	# where the confirm arrived. The confirm arriving at all, from whichever
	# client, is the proof: the routes are in place and the client reached
	# the stick.
	if ! addr_changed; then
		return 0
	fi
	if [ -z "$_local" ]; then
		why="it cannot be told which address it arrived on (over ssh, SSH_CONNECTION says; otherwise pass LOCAL)"
		return 1
	fi
	fresh_addrs
	if [ -n "$fresh" ]; then
		case " $fresh " in
		*" $_local "*) return 0 ;;
		esac
		why="it arrived on $_local, which the stick already had. Confirm on the new address:${fresh}"
		return 1
	fi
	# Nothing new to reach: the address kept, the netmask moved (or the
	# second address went away). The client must be on the new link.
	_p=$(primary_of new_) _m=$(mask_of new_ LAN_SUBNET)
	_s=$(second_of new_)
	if [ -n "$_s" ] && [ "$_local" = "$_s" ]; then
		_m=$(mask_of new_ LAN_SUBNET2)
	elif [ "$_local" != "$_p" ]; then
		why="it arrived on $_local, which is not one of the trial addresses ($_p)"
		return 1
	fi
	if [ -z "$_peer" ] || ! onlink "$_local" "$_peer" "$_m"; then
		why="the new addresses are the same and only the netmask moved: confirm from a client on the new netmask ($_m), not from ${_peer:-an unknown client}"
		return 1
	fi
	return 0
}

confirm() {
	local_=${1:-} peer=${2:-}
	if [ -z "$local_" ] && [ -n "${SSH_CONNECTION:-}" ]; then
		# peer, peer port, local, local port
		# shellcheck disable=SC2086 # four words
		set -- $SSH_CONNECTION
		peer=${1:-} local_=${3:-}
	fi
	lock
	load
	if [ "$phase" != trial ]; then
		unlock
		die "no trial is running"
	fi
	if [ "$applied" != 1 ]; then
		unlock
		die "the trial addresses are not applied yet; confirm again in a second"
	fi
	why=''
	if ! via_check "$local_" "$peer"; then
		unlock
		die "not confirmed: $why. The trial carries on and reverts unless confirmed on a new address"
	fi
	# Store what changed, one key at a time, and read each back: flash set
	# is not trusted for its exit code. A key that does not stick puts the
	# ones written back and leaves the trial running. Those writes are read
	# back too: what stopped the key can stop them, and the store then
	# holds new values next to old ones, a set that was never tried and
	# that the next boot would use. flash set writes one key at a time, so
	# this cannot be one write; the message names what the store holds.
	wrote=''
	for k in $KEYS; do
		n=$(getv "new_$k") o=$(getv "old_$k")
		[ "$n" != "$o" ] || continue
		"$TIMEOUT" "$IO_S" "$FLASH" set "$k" "$n" > /dev/null 2>&1
		if [ "$(store_get "$k")" != "$n" ]; then
			kept=''
			for w in $wrote $k; do
				o=$(getv "old_$w")
				"$TIMEOUT" "$IO_S" "$FLASH" set "$w" "$o" > /dev/null 2>&1
				v=$(store_get "$w")
				[ "$v" = "$o" ] || kept="$kept $w=$v"
			done
			unlock
			[ -n "$kept" ] || die "could not store $k=$n; nothing is kept and the trial carries on and reverts unless confirmed"
			die "could not store $k=$n, and could not put back:$kept. The store now holds a mix of new and old values that the next boot would use: confirm again, or store the old values. The trial carries on and reverts unless confirmed"
		fi
		wrote="$wrote $k"
	done
	# The store now holds the trial values: ending the trial changes nothing
	# on the primary and only takes the kept address down.
	rm -f "$TRIAL"
	net_addr > /dev/null 2>&1 || say "network.sh addr after the confirm failed; the addresses are stored and apply at the next boot" >&2
	phase=steady applied=0 trial_id='' trial_deadline=''
	event trial_confirmed "confirmed on ${local_:-?} and stored:$wrote"
	save
	unlock
	return 0
}

status() {
	load
	echo "phase=$phase"
	echo "applied=$applied"
	echo "trial_deadline=$trial_deadline"
	echo "prev_ip=$prev_ip"
	for k in $KEYS; do echo "new_$k=$(getv "new_$k")"; done
	echo "uptime=$(uptime_s)"
	echo "event=$ev"
	echo "event_uptime=$ev_up"
	echo "message=$ev_msg"
	[ -f "$SAFE_FLAG" ] && echo "safe_mode_boot=1" || echo "safe_mode_boot=0"
	return 0
}

case "${1:-}" in
try)        shift; try "$@" ;;
trial-run)  shift; trial_run "$@" ;;
expire)     shift; expire "$@" ;;
settle)     shift; settle "$@" ;;
confirm)    shift; confirm "$@" ;;
revert)     revert ;;
status)     status ;;
*)          echo "usage: $0 try [-s SECONDS] KEY=VALUE...|confirm [LOCAL [PEER]]|revert|status" >&2; exit 1 ;;
esac
