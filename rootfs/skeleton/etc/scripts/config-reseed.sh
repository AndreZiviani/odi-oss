#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
#
# config-reseed.sh -- a config store that is not there is recreated from the
# defaults in the image, so an erased config partition still boots to a stick
# that can be reached and configured.
#
#     config-reseed.sh run      rcS, once, right after the config partition
#                               is mounted and before anything reads the store
#     config-reseed.sh status   key=value lines: the record, if any
#     config-reseed.sh ack      acknowledge a recreated store: removes the record
#
# The store is two files, lastgood.xml (CS) and lastgood_hs.xml (HS). The stock
# firmware writes both from defaults built into its own binaries when they are
# missing; our image does not run those binaries, so after `flash_eraseall` of
# the config partition it came up with no store at all: no management address
# (network.sh then guesses 192.168.1.1 and does not count it as configured, so
# the boot is never confirmed and the watchdog ends in the safe mode), and no
# key for `flash set` to edit, so every save from the web UI failed.
#
# /etc/config-template/ holds what the stock firmware writes into an empty
# partition (docs/FLASHING.md, "An erased config partition", says how it was
# taken), with the line identity left empty: GPON_SN, GPON_PLOAM_PASSWD and
# ELAN_MAC_ADDR. A file that is missing or empty is installed from there; a
# file that is there is never touched, whatever it holds, and odi.conf is not
# a store file (every odi.conf key has its default when the file is missing).
#
# With GPON_SN empty the PON is never activated (rcs-lib.sh, the gponsn and
# gponact steps), so the stick does not range with a made-up serial; it stays
# on 192.168.1.1 until the owner types the serial in or restores a backup.
# A recreated HS file gets an ELAN_MAC_ADDR of the Realtek prefix the stock
# default uses and three random bytes, so two recreated sticks on one LAN do
# not share a MAC.
#
# The record, /etc/config/config-reseeded, says which files were recreated, by
# which build and when, until it is acknowledged; the diagnostics summary and
# the web UI show it. Each file is written beside its target and renamed, so a
# power cut leaves either no file (recreated again next boot) or a whole one.
# Always exits 0: a failure here is logged and the boot goes on.

CONFIG_DIR=${CONFIG_DIR:-/etc/config}
TEMPLATE_DIR=${TEMPLATE_DIR:-/etc/config-template}
RECORD=${RECORD:-$CONFIG_DIR/config-reseeded}
RANDOM_DEV=${RANDOM_DEV:-/dev/urandom}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
BUILD=${BUILD:-/etc/odi-build}
MAC_PREFIX=00e04c

say() { echo "config-reseed: $*"; }
uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

# event NAME TEXT: one line to the kernel log (dmesg, the ramlog, syslog
# through klogd), at warning, and to stdout.
event() {
	name=$1
	shift
	printf '<4>config-reseed: event=%s %s\n' "$name" "$*" 2>/dev/null >> "$KMSG"
	say "event=$name $*"
}

# Three random bytes as six hex digits, or nothing.
mac_tail() {
	t=$(od -An -N3 -tx1 "$RANDOM_DEV" 2>/dev/null | tr -d ' \n')
	case "$t" in
	[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) echo "$t" ;;
	esac
}

# install NAME: CONFIG_DIR/NAME from TEMPLATE_DIR/NAME, through NAME.new.$$.
# The new file must hold every line of the template (one changed, for the
# MAC); anything shorter is a full partition and is not installed.
install_one() {
	src=$TEMPLATE_DIR/$1
	dst=$CONFIG_DIR/$1
	tmp=$dst.new.$$
	if [ ! -s "$src" ]; then
		say "no $src in this image, $1 not recreated" >&2
		return 1
	fi
	mac=
	[ "$1" = lastgood_hs.xml ] && mac=$(mac_tail)
	if [ -n "$mac" ]; then
		sed "s/\(Name=\"ELAN_MAC_ADDR\" Value=\"\)\"/\1$MAC_PREFIX$mac\"/" "$src" > "$tmp" 2>/dev/null
	else
		cp "$src" "$tmp" 2>/dev/null
	fi
	if [ "$(wc -l < "$tmp" 2>/dev/null)" != "$(wc -l < "$src")" ] ||
	   ! grep -q '^</Config>' "$tmp" 2>/dev/null; then
		rm -f "$tmp"
		say "could not write $tmp, $1 not recreated" >&2
		return 1
	fi
	mv "$tmp" "$dst" 2>/dev/null || { rm -f "$tmp"; say "could not install $dst" >&2; return 1; }
	return 0
}

run() {
	if [ ! -d "$CONFIG_DIR" ]; then
		say "$CONFIG_DIR is not there, nothing to do" >&2
		return 0
	fi
	done_files=
	failed=
	for f in lastgood.xml lastgood_hs.xml; do
		[ -s "$CONFIG_DIR/$f" ] && continue
		if install_one "$f"; then
			done_files="$done_files $f"
		else
			failed="$failed $f"
		fi
	done
	done_files=${done_files# }
	failed=${failed# }
	[ -n "$failed" ] && event config_reseed_failed "files=$failed: missing or empty, and the default could not be installed"
	[ -n "$done_files" ] || return 0

	build=$(sed -n 's/^image=//p' "$BUILD" 2>/dev/null | head -n 1)
	{
		echo "files=$done_files"
		echo "build=${build:-unknown}"
		echo "uptime=$(uptime_s)"
	} > "$RECORD.new.$$" 2>/dev/null && mv "$RECORD.new.$$" "$RECORD" 2>/dev/null ||
		{ rm -f "$RECORD.new.$$"; say "could not write $RECORD" >&2; }
	event config_reseeded "files=$done_files were missing or empty and are recreated from $TEMPLATE_DIR; the line identity (GPON_SN, PLOAM password) is empty and the PON stays down until it is set"
	return 0
}

ack() {
	if [ -f "$RECORD" ]; then
		rm -f "$RECORD"
		event config_reseed_ack "the record of a recreated config store was acknowledged"
	fi
	return 0
}

status() {
	if [ -f "$RECORD" ]; then
		echo "record=1"
		sed -n 's/^\([a-z_]*\)=/record_\1=/p' "$RECORD"
	else
		echo "record=0"
	fi
	return 0
}

case "${1:-}" in
run)    run ;;
ack)    ack ;;
status) status ;;
*)      echo "usage: config-reseed.sh run|status|ack" >&2; exit 2 ;;
esac
exit 0
