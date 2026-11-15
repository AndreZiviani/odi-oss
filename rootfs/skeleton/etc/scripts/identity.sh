#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# identity.sh -- a one-time copy of the provisioning identity, kept on the
# config partition, and the way back to it (docs/SETTINGS.md, "The identity
# snapshot").
#
#     identity.sh snapshot   rcS, every boot: write the snapshot when there
#                            is none yet; otherwise leave it alone
#     identity.sh show       the snapshot against the current values,
#                            secrets hidden
#     identity.sh restore    write the snapshot values back
#     identity.sh replace --yes
#                            overwrite the snapshot with the current values,
#                            on purpose
#
# The identity keys cannot be regenerated, and nothing else on the stick
# keeps an old value: `flash set` overwrites in place, and `flash default cs`
# empties the LOID keys. The snapshot is taken by the first boot of an image
# that has this script, so it holds what the stick had then, which is not
# necessarily what it left the factory with.
#
# The file is /var/config/identity.orig, mode 600, never in lastgood*.xml
# (the stock firmware drops a key it does not know at its first save) and not
# in odi.conf (flash get reads that as live settings). One line per key:
#
#     <store> <KEY>=<value>     store is cs, hs or odi; the value is the raw
#                               text of the store (XML-escaped for cs and hs)
#     - <KEY>                   the key was in no store
#
# after the header lines image= (the image that took it) and taken= (UTC, or
# unknown when the clock was not set yet, as at every boot before NTP).
#
# Writes go through the writer lock of flash, so a snapshot or a restore can
# never interleave with a `flash set` from the web UI, and every file is built
# beside its target and renamed over it, so a power cut leaves the old file
# whole. restore writes CS, HS and odi.conf one after the other: a power cut
# between two of them leaves some keys restored, and running restore again
# finishes the job.
#
# restore does not apply anything: it prints what each change needs
# (apply.sh omci, or a reboot for the MAC).
set -u

PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

CS=${CS:-/var/config/lastgood.xml}
HS=${HS:-/var/config/lastgood_hs.xml}
ODI_CONF=${ODI_CONF:-/var/config/odi.conf}
SNAP=${SNAP:-/var/config/identity.orig}
TMPDIR_=${TMPDIR_:-/var/tmp}
FLASH_LOCK=${FLASH_LOCK:-$TMPDIR_/flash-writer.lock}
LOCK_S=${LOCK_S:-5}
SAFE_FLAG=${SAFE_FLAG:-/var/run/safe-mode}
BUILD=${BUILD:-/etc/odi-build}
VERSION_FILE=${VERSION_FILE:-/etc/version}
OMCI_IDENTITY_ON=${OMCI_IDENTITY_ON:-/etc/config/omci-identity.on}

# The keys, in the order they are written. ONU_HW_VERSION is an odi-only key
# (odi.conf); the rest are stock keys. MAC_KEY, PON_VENDOR_ID and
# GPON_PLOAM_FORMAT are read by the stock firmware only.
KEYS="GPON_SN GPON_PLOAM_PASSWD ELAN_MAC_ADDR GPON_ONU_MODEL LOID LOID_PASSWD LOID_OLD LOID_PASSWD_OLD ONU_HW_VERSION MAC_KEY PON_VENDOR_ID GPON_PLOAM_FORMAT"
ODI_ONLY="ONU_HW_VERSION"
SECRETS="GPON_PLOAM_PASSWD LOID_PASSWD LOID_PASSWD_OLD MAC_KEY"
# What a change needs to take effect (docs/SETTINGS.md, the key table).
OMCI_KEYS="GPON_SN GPON_PLOAM_PASSWD LOID LOID_PASSWD LOID_OLD LOID_PASSWD_OLD GPON_ONU_MODEL ONU_HW_VERSION"
OMCI_SWITCHED="GPON_ONU_MODEL ONU_HW_VERSION"
STOCK_ONLY="MAC_KEY PON_VENDOR_ID GPON_PLOAM_FORMAT"

say() { echo "identity: $*"; }
die() { echo "identity: $*" >&2; exit 1; }

in_list() {  # in_list WORD LIST
	for w_ in $2; do [ "$w_" = "$1" ] && return 0; done
	return 1
}

take_lock() {
	command -v flock >/dev/null 2>&1 || die "no flock to take the writer lock"
	( : >> "$FLASH_LOCK" ) 2>/dev/null || die "cannot open the writer lock $FLASH_LOCK"
	exec 9>>"$FLASH_LOCK"
	_i=0
	while ! flock -n 9; do
		[ "$_i" -lt "$LOCK_S" ] || die "busy, a flash holds $FLASH_LOCK"
		sleep 1
		_i=$((_i + 1))
	done
}

# xml_has KEY FILE, xml_value KEY FILE: the same patterns as flash, so the
# closing quote stops ELAN_MAC_ADDR matching ELAN_MAC_ADDR2.
xml_has() {
	grep -q "Name=\"$1\" Value=" "$2" 2>/dev/null
}
xml_value() {
	sed -n "s/.*Name=\"$1\" Value=\"\([^\"]*\)\".*/\1/p" "$2" 2>/dev/null | head -n 1
}
odi_has() {
	grep -q "^$1=" "$2" 2>/dev/null
}
odi_value() {
	sed -n "s/^$1=\(.*\)\$/\1/p" "$2" 2>/dev/null | head -n 1
}

# where KEY [CSFILE HSFILE ODIFILE]: the store that holds KEY now (cs, hs,
# odi) or -. CS before HS, as flash get reads them.
where() {
	_c=${2:-$CS} _h=${3:-$HS} _o=${4:-$ODI_CONF}
	if in_list "$1" "$ODI_ONLY"; then
		if odi_has "$1" "$_o"; then echo odi; else echo -; fi
	elif xml_has "$1" "$_c"; then echo cs
	elif xml_has "$1" "$_h"; then echo hs
	else echo -
	fi
}

# value_in STORE KEY [CSFILE HSFILE ODIFILE]
value_in() {
	case "$1" in
	cs)  xml_value "$2" "${3:-$CS}" ;;
	hs)  xml_value "$2" "${4:-$HS}" ;;
	odi) odi_value "$2" "${5:-$ODI_CONF}" ;;
	esac
}

# The stores must be there and whole, and carry a serial: a store that does
# not is never frozen as the identity of this stick.
check_stores() {
	[ -f "$CS" ] && [ -f "$HS" ] || die "$CS or $HS is not there -- is the config partition mounted?"
	grep -q '</Config>' "$CS" && grep -q '</Config>' "$HS" || die "$CS or $HS is not a whole config file, no snapshot taken"
	_w=$(where GPON_SN)
	[ "$_w" != - ] && [ -n "$(value_in "$_w" GPON_SN)" ] || die "no GPON_SN in the config store, no snapshot taken"
}

image_name() {
	_n=$(sed -n 's/^image=//p' "$BUILD" 2>/dev/null | head -n 1)
	[ -n "$_n" ] || _n=$(head -n 1 "$VERSION_FILE" 2>/dev/null)
	echo "${_n:-unknown}"
}

# The clock: no RTC, so before NTP every boot starts in 1970. A year before
# 2026 means unset.
taken_stamp() {
	_y=$(date -u +%Y 2>/dev/null)
	case "$_y" in
	[0-9][0-9][0-9][0-9]) [ "$_y" -ge 2026 ] && { date -u +%Y-%m-%dT%H:%M:%SZ; return; } ;;
	esac
	echo unknown
}

# dump IMAGE TAKEN: the snapshot text of the current stores.
dump() {
	echo "# identity snapshot (identity.sh, docs/SETTINGS.md); restore with identity.sh restore"
	echo "image=$1"
	echo "taken=$2"
	for k in $KEYS; do
		s=$(where "$k")
		if [ "$s" = - ]; then
			echo "- $k"
		else
			printf '%s %s=%s\n' "$s" "$k" "$(value_in "$s" "$k")"
		fi
	done
}

# write_snap: the snapshot, built beside its target, compared, renamed.
write_snap() {
	img=$(image_name)
	tk=$(taken_stamp)
	tmp="$SNAP.tmp.$$"
	if ! ( umask 077; dump "$img" "$tk" > "$tmp" ) 2>/dev/null ||
	   ! dump "$img" "$tk" | cmp -s - "$tmp" ||
	   ! chmod 600 "$tmp" || ! mv "$tmp" "$SNAP"; then
		rm -f "$tmp"
		die "could not write $SNAP (config partition full?), no snapshot taken"
	fi
}

# The snapshot lines for KEY: "<store> <value>", or "-" for absent. The file
# was validated before any caller reads it.
snap_line() {
	sed -n -e "s/^cs $1=/cs /p" -e "s/^hs $1=/hs /p" -e "s/^odi $1=/odi /p" -e "s/^- $1\$/-/p" "$SNAP"
}

# validate: the file is ours and whole. Every key once, a known store, no
# quote, < or newline in an XML value, and a serial.
validate() {
	[ -f "$SNAP" ] || die "no snapshot at $SNAP"
	grep -q '^image=' "$SNAP" || die "$SNAP has no image= line, not restoring from it"
	for k in $KEYS; do
		n=$(grep -c -e "^cs $k=" -e "^hs $k=" -e "^odi $k=" -e "^- $k\$" "$SNAP")
		[ "$n" = 1 ] || die "$SNAP has $n lines for $k, not restoring from it"
	done
	l=$(snap_line GPON_SN)
	case "$l" in
	cs\ ?*|hs\ ?*) ;;
	*) die "$SNAP holds no GPON_SN, not restoring from it" ;;
	esac
	if grep -E '^(cs|hs) [A-Z0-9_]+=.*[<"]' "$SNAP" >/dev/null; then
		die "$SNAP holds a value that cannot be an XML attribute, not restoring from it"
	fi
}

show_value() {  # show_value KEY VALUE
	if in_list "$1" "$SECRETS"; then
		if [ -n "$2" ]; then echo "(hidden)"; else echo "(empty)"; fi
	else
		echo "$2"
	fi
}

cmd_snapshot() {
	if [ -e "$SAFE_FLAG" ]; then
		say "safe-mode boot, no snapshot taken"
		return 0
	fi
	[ -e "$SNAP" ] && return 0
	check_stores
	take_lock
	[ -e "$SNAP" ] && return 0
	write_snap
	say "snapshot of the identity written to $SNAP"
}

cmd_replace() {
	[ "${1:-}" = --yes ] || die "replace overwrites the snapshot with the current values; run identity.sh replace --yes to do that"
	check_stores
	take_lock
	write_snap
	say "snapshot replaced with the current values ($SNAP)"
}

cmd_show() {
	validate
	echo "image=$(sed -n 's/^image=//p' "$SNAP" | head -n 1)"
	echo "taken=$(sed -n 's/^taken=//p' "$SNAP" | head -n 1)"
	for k in $KEYS; do
		l=$(snap_line "$k")
		now=$(where "$k")
		if [ "$l" = - ]; then
			if [ "$now" = - ]; then st=same; else st="differs (now in $now)"; fi
			echo "$k: (absent) $st"
			continue
		fi
		s=${l%% *}; v=${l#* }
		if [ "$now" = - ]; then
			st="differs (now absent)"
		elif [ "$(value_in "$now" "$k")" = "$v" ]; then
			st=same
		else
			st=differs
		fi
		echo "$k: $(show_value "$k" "$v") [$s] $st"
	done
}

# set_xml KEY RAW FILE: KEY set to RAW in FILE, in place, checked like flash
# does (the rewrite run twice and compared). RAW is the text of the store,
# so & is escaped for sed as well as \ and /.
set_xml() {
	e_=$(printf '%s' "$2" | sed 's/[\\/&]/\\&/g')
	sed "s/\(Name=\"$1\" Value=\"\)[^\"]*\"/\1$e_\"/" "$3" > "$3.n" 2>/dev/null &&
		[ -s "$3.n" ] &&
		sed "s/\(Name=\"$1\" Value=\"\)[^\"]*\"/\1$e_\"/" "$3" | cmp -s - "$3.n" &&
		mv "$3.n" "$3"
}

# set_odi KEY VALUE FILE: the first KEY= line replaced, every other line kept.
set_odi() {
	K_="$1" V_="$2" awk '
		!done && index($0, ENVIRON["K_"] "=") == 1 { print ENVIRON["K_"] "=" ENVIRON["V_"]; done = 1; next }
		{ print }' "$3" > "$3.n" 2>/dev/null &&
		[ -s "$3.n" ] && mv "$3.n" "$3"
}

cmd_restore() {
	validate
	check_stores
	take_lock
	w="$TMPDIR_/identity.$$"
	mkdir -p "$w" || die "cannot create $w"
	trap 'rm -rf "$w"' EXIT
	cp "$CS" "$w/cs" && cp "$HS" "$w/hs" || die "cannot copy the config store"
	if [ -f "$ODI_CONF" ]; then cp "$ODI_CONF" "$w/odi" || die "cannot copy $ODI_CONF"; else : > "$w/odi"; fi
	changed="" skipped=0
	for k in $KEYS; do
		l=$(snap_line "$k")
		now=$(where "$k" "$w/cs" "$w/hs" "$w/odi")
		if [ "$l" = - ]; then
			[ "$now" = - ] || say "$k: kept, it was in no store when the snapshot was taken"
			continue
		fi
		v=${l#* }
		if [ "$now" = - ] && [ "${l%% *}" = odi ]; then
			# An odi-only key is absent when it was cleared; restoring it
			# adds the line back, as flash set would.
			printf '%s=%s\n' "$k" "$v" >> "$w/odi" || die "could not rewrite $k, nothing installed"
			changed="$changed $k"
			say "$k: restored to $(show_value "$k" "$v")"
			continue
		fi
		if [ "$now" = - ]; then
			say "$k: not restored, it is in no store now"
			skipped=1
			continue
		fi
		[ "$(value_in "$now" "$k" "$w/cs" "$w/hs" "$w/odi")" = "$v" ] && continue
		case "$now" in
		odi) set_odi "$k" "$v" "$w/odi" ;;
		*)   set_xml "$k" "$v" "$w/$now" ;;
		esac || die "could not rewrite $k, nothing installed"
		changed="$changed $k"
		say "$k: restored to $(show_value "$k" "$v")"
	done
	if [ -z "$changed" ]; then
		say "nothing to restore, the stores hold the snapshot values"
		[ "$skipped" = 0 ] || exit 1
		return 0
	fi
	# Install what changed, each beside its target and renamed over it.
	for s in cs hs odi; do
		case "$s" in cs) t=$CS ;; hs) t=$HS ;; odi) t=$ODI_CONF ;; esac
		if [ -f "$t" ]; then cmp -s "$w/$s" "$t" && continue; else [ -s "$w/$s" ] || continue; fi
		n="$t.new.$$"
		if ! cp "$w/$s" "$n" || ! cmp -s "$w/$s" "$n" || ! mv "$n" "$t"; then
			rm -f "$n"
			die "could not install $t; run identity.sh restore again"
		fi
	done
	# Read back.
	for k in $changed; do
		v=$(snap_line "$k"); v=${v#* }
		now=$(where "$k")
		[ "$now" != - ] && [ "$(value_in "$now" "$k")" = "$v" ] || die "$k did not read back as restored; run identity.sh restore again"
	done
	omci=0 mac=0 sw=0 stock=0
	for k in $changed; do
		in_list "$k" "$OMCI_KEYS" && omci=1
		in_list "$k" "$OMCI_SWITCHED" && sw=1
		in_list "$k" "$STOCK_ONLY" && stock=1
		[ "$k" = ELAN_MAC_ADDR ] && mac=1
	done
	[ "$omci" = 1 ] && say "next: apply.sh omci (re-registers with the OLT: interrupts internet)"
	[ "$sw" = 1 ] && [ ! -e "$OMCI_IDENTITY_ON" ] && say "GPON_ONU_MODEL and ONU_HW_VERSION reach the OLT only while $OMCI_IDENTITY_ON exists"
	[ "$mac" = 1 ] && say "next: reboot, ELAN_MAC_ADDR takes effect at boot"
	[ "$stock" = 1 ] && say "MAC_KEY, PON_VENDOR_ID and GPON_PLOAM_FORMAT are read by the stock firmware only"
	[ "$skipped" = 0 ] || exit 1
	return 0
}

case "${1:-}" in
snapshot) cmd_snapshot ;;
replace)  shift; cmd_replace "$@" ;;
show)     cmd_show ;;
restore)  cmd_restore ;;
*)        echo "usage: identity.sh snapshot|show|restore|replace --yes" >&2; exit 2 ;;
esac
