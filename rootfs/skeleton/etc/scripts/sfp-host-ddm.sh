#!/bin/sh
#
# sfp-host-ddm.sh -- the host-side SFP EEPROM, from the odi-only key
# SFP_HOST_DDM (docs/SETTINGS.md): the module identity (A0h) and its live
# diagnostics (A2h) served to the host, the router the stick is plugged
# into, through the SoC SFP slave, on the board revisions that wire it
# (docs/KERNEL.md, odi_sfp_mirror.c). The kernel does the work; this only
# writes its parameter, odi_switch.sfp_mirror.
#
#     sfp-host-ddm.sh boot     the PON step (rcs-lib.sh), once the optics
#                              I2C answers: asks for the mirror unless the
#                              key is 0 or this is a safe-mode boot. Returns
#                              at once; the kernel fills the slave and turns
#                              it on in the background, and logs the outcome.
#     sfp-host-ddm.sh apply    the key again, live (apply.sh sfp): turns the
#                              mirror on or off, and for on waits, bounded,
#                              for the kernel to say on or failed. Exit 1 on
#                              a failure.
#     sfp-host-ddm.sh status   setting= (the key) request= (the parameter)
#                              state= (off, filling, on or failed)
#
# The key: 0 turns it off; empty, absent or 1 leave it on, the default; any
# other value runs it on and logs event=bad_value.
#
# Every outcome is one line to the kernel log (dmesg, the ramlog and syslog
# through klogd), at notice, and to stdout:
#     sfp-host-ddm: event=NAME setting=on|off TEXT
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

FLASH=${FLASH:-/etc/scripts/flash}
PARAM_DIR=${PARAM_DIR:-/sys/module/odi_switch/parameters}
KMSG=${KMSG:-/dev/kmsg}
SAFE_FLAG=${SAFE_FLAG:-/var/run/safe-mode}
TIMEOUT=${TIMEOUT:-timeout}
SLEEP=${SLEEP:-sleep}
IO_S=${IO_S:-5}              # one flash get, one parameter write or read
APPLY_WAIT=${APPLY_WAIT:-10} # apply: the fill takes well under a second

setting=''
raw=''

say() { echo "sfp-host-ddm: $*"; }

# event NAME TEXT: one line to the kernel log and to stdout.
event() {
	name=$1
	shift
	line="event=$name setting=$setting $*"
	printf '<5>sfp-host-ddm: %s\n' "$line" 2>/dev/null >> "$KMSG"
	say "$line"
}

key_raw() {
	"$TIMEOUT" "$IO_S" "$FLASH" get SFP_HOST_DDM 2>/dev/null | sed -n 's/^SFP_HOST_DDM=//p' | head -n 1
}

# parse VALUE: what SFP_HOST_DDM means; anything but 0 is on.
parse() {
	case $1 in
	0) echo off ;;
	*) echo on ;;
	esac
}

# read_key: raw and setting; a value that is neither 0 nor 1 is logged.
read_key() {
	raw=$(key_raw)
	setting=$(parse "$raw")
	if [ -n "$raw" ] && [ "$raw" != 0 ] && [ "$raw" != 1 ]; then
		event bad_value "SFP_HOST_DDM=$raw is neither 0 nor 1; serving the host EEPROM"
	fi
}

param_ok() { [ -w "$PARAM_DIR/sfp_mirror" ]; }

param_write() {
	# shellcheck disable=SC2016 # the inner shell expands them
	"$TIMEOUT" "$IO_S" sh -c 'echo "$1" > "$2"' _ "$1" "$PARAM_DIR/sfp_mirror" 2>/dev/null
}

param_read() {
	"$TIMEOUT" "$IO_S" cat "$PARAM_DIR/$1" 2>/dev/null | head -n 1
}

boot() {
	read_key
	if [ -f "$SAFE_FLAG" ]; then
		event safe_mode_boot "a safe-mode boot: host EEPROM left off (SFP_HOST_DDM=${raw:-unset})"
		return 0
	fi
	if [ "$setting" = off ]; then
		event off "SFP_HOST_DDM=0: host EEPROM left off"
		return 0
	fi
	if ! param_ok; then
		event no_kernel "this kernel has no odi_switch.sfp_mirror"
		return 0
	fi
	if param_write 1; then
		event requested "host EEPROM requested; the kernel logs when it is on (odi_sfp_mirror)"
	else
		event apply_failed "could not write odi_switch.sfp_mirror"
	fi
	return 0
}

apply() {
	read_key
	if ! param_ok; then
		event no_kernel "this kernel has no odi_switch.sfp_mirror"
		return 1
	fi
	if [ "$setting" = off ]; then
		if ! param_write 0; then
			event apply_failed "could not write odi_switch.sfp_mirror"
			return 1
		fi
		event applied "host EEPROM off"
		return 0
	fi
	if ! param_write 1; then
		event apply_failed "could not write odi_switch.sfp_mirror"
		return 1
	fi
	i=0
	while :; do
		s=$(param_read sfp_mirror_state)
		case $s in
		on)
			event applied "host EEPROM on"
			return 0 ;;
		failed)
			event apply_failed "the kernel could not turn the host EEPROM on; dmesg has why (odi_sfp_mirror)"
			return 1 ;;
		esac
		if [ "$i" -ge "$APPLY_WAIT" ]; then
			event apply_timeout "still ${s:-unknown} after ${APPLY_WAIT} s"
			return 1
		fi
		"$SLEEP" 1
		i=$((i + 1))
	done
}

status() {
	raw=$(key_raw)
	setting=$(parse "$raw")
	echo "setting=$setting"
	echo "request=$(param_read sfp_mirror)"
	echo "state=$(param_read sfp_mirror_state)"
}

case ${1:-} in
boot)   boot ;;
apply)  apply || exit 1 ;;
status) status ;;
*)      echo "usage: $0 boot|apply|status" >&2; exit 2 ;;
esac
exit 0
