#!/bin/sh
#
# mgmt-addr.sh -- a change of the management addresses (LAN_IP_ADDR,
# LAN_SUBNET, LAN_ENABLE_IP2, LAN_IP_ADDR2, LAN_SUBNET2) as a trial that
# reverts by itself. A typo in an address is the one mistake that takes every
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
# The old address stays up during the trial. network.sh addr reads the five
# keys from the trial file (/var/run/mgmt-addr.trial) while it exists, moves
# the primary to the new address and keeps the previous primary on its own
# alias (br0:4); the second address follows the trial. So the way in that
# worked stays there if the new one is wrong, and the page opened on it can
# still revert. What it cannot do is confirm: a confirm that arrives on an
# address the stick already had proves nothing about the new one and is
# refused. When nothing new is reachable (only the netmask changed, or the
# second address was turned off) the confirm must come from a client that is
# on the link of the new netmask.
#
# A reboot during the trial undoes it. The keys are written only by confirm,
# and the state and the trial file are in /var/run, so the boot reads the
# store, which still holds the old addresses, and never the trial.
#
# The safe mode (safe-mode.sh): a recovery boot is entered through the rescue
# address (network.sh, br0:3) and the stored addresses are the thing being
# repaired, from a client that is usually not on their network. There is no
# trial in it: try refuses, and the web UI stores the keys directly then.
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

KEYS="LAN_IP_ADDR LAN_SUBNET LAN_ENABLE_IP2 LAN_IP_ADDR2 LAN_SUBNET2"
DEF_IP=192.168.1.1
DEF_MASK=255.255.255.0

say() { echo "mgmt-addr: $*"; }
die() { echo "mgmt-addr: $*" >&2; exit 1; }

uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

# event NAME TEXT: one line to the kernel log (dmesg, the ramlog, and syslog
# through klogd), at notice, and to stdout.
event() {
	name=$1
	shift
	line="event=$name ip=${new_LAN_IP_ADDR:-} prev=${prev_ip:-} $*"
	printf '<5>mgmt-addr: %s\n' "$line" 2>/dev/null >> "$KMSG"
	say "$line"
	ev=$name
	ev_up=$(uptime_s)
	ev_msg=$*
}

# ---- the keys: five fixed names, held as old_NAME (the store, as the trial
# found it) and new_NAME (what the trial asks for)

setv() { eval "$1=\$2"; }
getv() { eval "printf '%s' \"\${$1:-}\""; }

store_get() {
	"$TIMEOUT" "$IO_S" "$FLASH" get "$1" 2>/dev/null | sed -n "s/^$1=//p" | sed -n 1p
}

# put_in_place FILE: puts FILE.new.$$ in place of FILE in one rename on the same
# filesystem, so a reader or a power cut sees the old file or the new one.
put_in_place() {
	mv "$1.new.$$" "$1" 2>/dev/null || { rm -f "$1.new.$$"; return 1; }
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
	[ -f "$STATE" ] || return 0
	while IFS='=' read -r k v; do
		case $k in
		phase) phase=$v ;;
		applied) applied=$v ;;
		trial_id) trial_id=$v ;;
		trial_deadline) trial_deadline=$v ;;
		prev_ip) prev_ip=$v ;;
		prev_mask) prev_mask=$v ;;
		old_LAN_IP_ADDR|old_LAN_SUBNET|old_LAN_ENABLE_IP2|old_LAN_IP_ADDR2|old_LAN_SUBNET2) setv "$k" "$v" ;;
		new_LAN_IP_ADDR|new_LAN_SUBNET|new_LAN_ENABLE_IP2|new_LAN_IP_ADDR2|new_LAN_SUBNET2) setv "$k" "$v" ;;
		event) ev=$v ;;
		event_uptime) ev_up=$v ;;
		message) ev_msg=$v ;;
		esac
	done < "$STATE"
}

save() {
	{
		echo "phase=$phase"
		echo "applied=$applied"
		echo "trial_id=$trial_id"
		echo "trial_deadline=$trial_deadline"
		echo "prev_ip=$prev_ip"
		echo "prev_mask=$prev_mask"
		for k in $KEYS; do echo "old_$k=$(getv "old_$k")"; done
		for k in $KEYS; do echo "new_$k=$(getv "new_$k")"; done
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
		[ "$i" -lt "$LOCK_S" ] || die "busy: another mgmt-addr.sh holds $LOCKDIR"
		"$SLEEP" 1
		i=$((i + 1))
	done
	echo $$ > "$LOCKDIR/pid"
}
unlock() { rm -rf "$LOCKDIR"; }

# spawn VERB ARGS...: this script again, detached, so it outlives the caller
# (confd). Its output goes to the services log.
spawn() {
	setsid sh "$SELF" "$@" < /dev/null >> "$LOG" 2>&1 &
}

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
	event trial_started "trying for $secs s: confirm over${fresh:- a client on the new netmask} (mgmt-addr.sh confirm) or it reverts to ${prev_ip:-the stored addresses}"
	put_in_place "$TRIAL" || { unlock; die "could not write $TRIAL"; }
	save
	unlock
	spawn trial-run "$trial_id" "$secs"
	return 0
}

# trial-run ID SECONDS: one second for the caller answer to leave, the
# apply, then the window, then the expiry.
trial_run() {
	id=$1 secs=$2
	"$SLEEP" 1
	lock
	load
	if [ "$phase" != trial ] || [ "$trial_id" != "$id" ]; then
		unlock
		return 0
	fi
	if net_addr; then
		applied=1
		save
	else
		back_to_prev trial_failed "network.sh could not apply the addresses"
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

# back_to_prev NAME WHY [defer]: ends the trial. The trial file goes, so
# network.sh addr reads the store again, and that moves the primary back and
# takes the kept address down. Run at once, or with defer from a caller that is
# itself on the address being left: its answer has to leave first.
back_to_prev() {
	name=$1 why=$2
	rm -f "$TRIAL"
	if [ "${3:-}" = defer ]; then
		spawn settle
	else
		net_addr
	fi
	phase=steady applied=0 trial_id='' trial_deadline=''
	event "$name" "$why: back to ${prev_ip:-the stored addresses}; nothing was stored"
	save
}

# settle: the move back, one second after the answer that asked for it.
settle() {
	"$SLEEP" 1
	[ ! -s "$TRIAL" ] || return 0
	net_addr
}

# via_check LOCAL PEER: whether a confirm that arrived on LOCAL from PEER
# proves the new addresses. Sets why when it does not.
via_check() {
	_local=$1 _peer=$2
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
	# ones written back and leaves the trial running.
	wrote=''
	for k in $KEYS; do
		n=$(getv "new_$k") o=$(getv "old_$k")
		[ "$n" != "$o" ] || continue
		"$TIMEOUT" "$IO_S" "$FLASH" set "$k" "$n" > /dev/null 2>&1
		if [ "$(store_get "$k")" != "$n" ]; then
			for w in $wrote; do "$TIMEOUT" "$IO_S" "$FLASH" set "$w" "$(getv "old_$w")" > /dev/null 2>&1; done
			"$TIMEOUT" "$IO_S" "$FLASH" set "$k" "$o" > /dev/null 2>&1
			unlock
			die "could not store $k=$n; nothing is kept and the trial carries on and reverts unless confirmed"
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

revert() {
	lock
	load
	if [ "$phase" != trial ]; then
		unlock
		die "no trial is running"
	fi
	back_to_prev trial_reverted "reverted on request" defer
	unlock
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
settle)     settle ;;
confirm)    shift; confirm "$@" ;;
revert)     revert ;;
status)     status ;;
*)          echo "usage: $0 try [-s SECONDS] KEY=VALUE...|confirm [LOCAL [PEER]]|revert|status" >&2; exit 1 ;;
esac
