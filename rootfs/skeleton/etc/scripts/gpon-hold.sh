#!/bin/sh
#
# gpon-hold.sh -- the marker of a kept Emergency Stop (GPON_KEEP_DISABLE,
# docs/SETTINGS.md): when the OLT disables this ONU (G.984.3
# Disable_Serial_Number, state O7), the ONU stays disabled across a restart
# of its own PON, a reboot and a power-off until the OLT enables it. The
# driver keeps O7 within a boot; this file is what carries it across the
# next one.
#
#     gpon-hold.sh sync      the kernel runs this, once for every change of
#                            the hold: the marker file follows what
#                            /proc/odi_gpon/status says (o7_hold ... held N)
#     gpon-hold.sh clear     release by hand: removes the marker and tells
#                            the driver (gponhold 0), which ends the hold and
#                            activates an ONU that is in O7. Needs no OLT.
#                            Whatever the key says: it is the way back from a
#                            stale marker
#     gpon-hold.sh status    key=value lines: the key, the marker, the driver
#
# Why a file and why from here. The kernel cannot write the jffs2 config
# partition, and a hook the kernel runs is the native way out of it
# (call_usermodehelper, as for modprobe or poweroff): no daemon, no polling.
# The marker is /etc/config/gpon-hold, a plain flag file beside optics.off
# and modules.off, so it survives a reboot, a power-off and a firmware
# update, and is not a setting: odi.conf holds what the user chose, this is
# what the OLT did. It holds one line of text and nothing else.
#
# sync is told nothing: it reads the driver, and writes the marker for what
# it found. Two runs that overtake each other (a disable and an enable a few
# milliseconds apart) therefore end on the current state, and each run looks
# again after it wrote, up to three times.
#
# The boot reads the marker in rcs-lib.sh (pon_step_keepdisable), only with
# GPON_KEEP_DISABLE=1 and never in a safe-mode boot. With the key off nothing
# here runs and no marker is read or written.
#
# Every wait is bounded. Always exits 0 from sync; clear and status report.
set -u

# The kernel runs this with a bare environment.
PATH=${PATH:+$PATH:}/etc/scripts:/sbin:/usr/sbin:/bin:/usr/bin
export PATH

MARKER=${GPON_HOLD_MARKER:-/etc/config/gpon-hold}
STATUS=${GPON_STATUS:-/proc/odi_gpon/status}
ODI_INIT=${ODI_INIT:-/proc/odi_init}
ODI_CONF=${ODI_CONF:-/etc/config/odi.conf}
KMSG=${KMSG:-/dev/kmsg}
TIMEOUT=${TIMEOUT:-timeout}
IO_S=${IO_S:-5}                 # one write to the driver, one sync

say() { echo "gpon-hold: $*"; }

# event NAME TEXT: one line to the kernel log at notice (dmesg, the ramlog,
# and syslog through klogd), like the driver own event=o7_hold lines.
event() {
	printf '<5>gpon-hold: event=marker action=%s%s\n' "$1" "${2:+ $2}" 2>/dev/null >> "$KMSG"
}

# held: 1 or 0 as the driver says, empty when it says nothing (no driver,
# or an older one).
held() {
	sed -n 's/^o7_hold keep [01] held \([01]\)$/\1/p' "$STATUS" 2>/dev/null | head -n 1
}

key_on() { grep -q '^GPON_KEEP_DISABLE=1$' "$ODI_CONF" 2>/dev/null; }

# flush: the marker is what a power cut right after the disable would lose.
flush() { "$TIMEOUT" "$IO_S" sync 2>/dev/null || true; }

put_marker() {
	tmp=$MARKER.$$
	if printf 'The OLT disabled this ONU (Disable_Serial_Number). GPON_KEEP_DISABLE keeps it in O7 across a reboot until the OLT enables it; gpon-hold.sh clear releases it by hand.\n' 2>/dev/null > "$tmp" &&
		mv "$tmp" "$MARKER" 2>/dev/null; then
		flush
		event written
		return 0
	fi
	rm -f "$tmp"
	event failed "what=write"
	return 1
}

drop_marker() {
	[ -e "$MARKER" ] || return 0
	if rm -f "$MARKER" 2>/dev/null; then
		flush
		event removed
		return 0
	fi
	event failed "what=remove"
	return 1
}

do_sync() {
	n=0
	while [ "$n" -lt 3 ]; do
		want=$(held)
		[ -n "$want" ] || exit 0
		if [ "$want" = 1 ]; then
			put_marker || exit 0
		else
			drop_marker || exit 0
		fi
		[ "$(held)" = "$want" ] && exit 0
		n=$((n + 1))
	done
	exit 0
}

do_clear() {
	rc=0
	if [ -w "$ODI_INIT" ]; then
		if "$TIMEOUT" "$IO_S" sh -c 'echo "gponhold 0" > "$1"' sh "$ODI_INIT" 2>/dev/null; then
			say "the driver released the hold"
		else
			say "the driver refused gponhold 0"
			rc=1
		fi
	fi
	if [ -e "$MARKER" ]; then
		drop_marker && say "marker removed" || rc=1
	else
		say "no marker"
	fi
	return $rc
}

do_status() {
	if key_on; then echo "key=1"; else echo "key=0"; fi
	if [ -e "$MARKER" ]; then echo "marker=1"; else echo "marker=0"; fi
	h=$(held)
	echo "held=${h:-unknown}"
}

case ${1:-} in
sync)   do_sync ;;
clear)  do_clear; exit $? ;;
status) do_status ;;
*)
	echo "usage: gpon-hold.sh sync|clear|status" >&2
	exit 2
	;;
esac
exit 0
