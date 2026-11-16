#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/config-reseed.sh and the template it installs
# (rootfs/skeleton/etc/config-template), off the device, on a temporary
# config directory; and the PON steps of rcs-lib.sh that an empty GPON_SN
# skips.
#
# What is checked: the template itself (the stock key set less the four
# login keys nothing reads, the line identity empty, the management address
# network.sh needs); both files missing, then recreated with a MAC of the
# Realtek prefix and three random bytes, the record and the kernel log line;
# an empty file counts as missing, a file that is there is never touched,
# whatever it holds; no template, a directory that cannot be written and a
# random source that cannot be read; status and ack; the real flash wrapper
# can get and set every key of a recreated store, the serial number
# included; and the gponsn and gponact steps leave the PON down while
# GPON_SN is empty. That network.sh counts the recreated address as
# configured is in network_addr_test.sh.
set -u
cd "$(dirname "$0")/.." || exit 1
# shellcheck source=test/host-flock.sh
. test/host-flock.sh
S=$PWD/rootfs/skeleton/etc/scripts/config-reseed.sh
TPL=$PWD/rootfs/skeleton/etc/config-template
F=$PWD/rootfs/skeleton/etc/scripts/flash
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
t() {   # t <label> <wanted, an ERE> <got>
	if printf '%s\n' "$3" | grep -E -q -- "$2"; then ok "$1"; else no "$1: wanted /$2/" "$3"; fi
}
eq() { if [ "$2" = "$3" ]; then ok "$1"; else no "$1: want [$2], got [$3]"; fi; }
val() { sed -n "s/.*Name=\"$1\" Value=\"\([^\"]*\)\".*/\1/p" "$2" | head -n 1; }
count() { grep -c '<Value Name=' "$1"; }

mkdir -p "$T/config" "$T/var"
echo "100.42 90.00" > "$T/uptime"
echo "image=test-build" > "$T/odi-build"
reseed() {   # reseed <verb>: the script on the temporary directory
	env CONFIG_DIR="$T/config" TEMPLATE_DIR="${TEMPLATE_DIR:-$TPL}" KMSG="$T/kmsg" \
	    UPTIME="$T/uptime" BUILD="$T/odi-build" RANDOM_DEV="${RANDOM_DEV:-/dev/urandom}" \
	    sh "$S" "$1" 2>&1
}

# ---- the template
eq "template: the CS keys of a stock store" 118 "$(count "$TPL/lastgood.xml")"
eq "template: the HS keys of a stock store" 62 "$(count "$TPL/lastgood_hs.xml")"
for k in USER_NAME USER_PASSWORD E8BDUSER_NAME E8BDUSER_PASSWORD; do
	grep -q "Name=\"$k\"" "$TPL/lastgood.xml" && no "template: $k is left out" || ok "template: $k is left out"
done
for k in GPON_SN ELAN_MAC_ADDR; do
	grep -q "Name=\"$k\" Value=\"\"/>" "$TPL/lastgood_hs.xml" && ok "template: $k is empty" || no "template: $k is empty"
done
for k in GPON_PLOAM_PASSWD LOID LOID_PASSWD; do
	grep -q "Name=\"$k\" Value=\"\"/>" "$TPL/lastgood.xml" && ok "template: $k is empty" || no "template: $k is empty"
done
eq "template: the management address" "192.168.1.1 255.255.255.0" \
	"$(val LAN_IP_ADDR "$TPL/lastgood.xml") $(val LAN_SUBNET "$TPL/lastgood.xml")"
eq "template: the login the stock firmware keeps" "admin admin" \
	"$(val SUSER_NAME "$TPL/lastgood.xml") $(val SUSER_PASSWORD "$TPL/lastgood.xml")"
for f in lastgood.xml lastgood_hs.xml; do
	eq "template: $f ends with the root element" "</Config>" "$(tail -n 1 "$TPL/$f")"
	grep -q '[&<]' <(sed -n 's/.*Value="\([^"]*\)".*/\1/p' "$TPL/$f") \
		&& no "template: no value in $f that flash set would refuse" || ok "template: no value in $f that flash set would refuse"
done

# ---- both files missing
out=$(reseed run); rc=$?
eq "missing: exits 0" 0 "$rc"
cmp -s "$TPL/lastgood.xml" "$T/config/lastgood.xml" && ok "missing: CS is the template" || no "missing: CS is the template"
eq "missing: HS has every key of the template" "$(count "$TPL/lastgood_hs.xml")" "$(count "$T/config/lastgood_hs.xml")"
t "missing: HS gets a MAC of the Realtek prefix and three random bytes" '^00e04c[0-9a-f]{6}$' \
	"$(val ELAN_MAC_ADDR "$T/config/lastgood_hs.xml")"
eq "missing: HS differs from the template in the MAC line only" 1 \
	"$(diff "$TPL/lastgood_hs.xml" "$T/config/lastgood_hs.xml" | grep -c '^>')"
eq "missing: the serial number stays empty" "" "$(val GPON_SN "$T/config/lastgood_hs.xml")"
eq "missing: the record" "files=lastgood.xml lastgood_hs.xml build=test-build uptime=100" \
	"$(tr '\n' ' ' < "$T/config/config-reseeded" | sed 's/ $//')"
t "missing: one warning line in the kernel log" '^<4>config-reseed: event=config_reseeded files=lastgood.xml lastgood_hs.xml ' "$(cat "$T/kmsg")"
t "missing: and on stdout" 'event=config_reseeded' "$out"
left=$(for x in "$T/config/"*.new.*; do [ -e "$x" ] && echo "$x"; done)
eq "missing: no temporary file left" "" "$left"
mac1=$(val ELAN_MAC_ADDR "$T/config/lastgood_hs.xml")

# ---- status and ack
t "status: the record" '^record=1$' "$(reseed status)"
t "status: its files" '^record_files=lastgood.xml lastgood_hs.xml$' "$(reseed status)"
reseed ack > /dev/null
[ -f "$T/config/config-reseeded" ] && no "ack: removes the record" || ok "ack: removes the record"
t "ack: logged" 'event=config_reseed_ack' "$(cat "$T/kmsg")"
eq "status after ack" "record=0" "$(reseed status)"

# ---- files that are there are never touched, whatever they hold
: > "$T/kmsg"
printf '<Config>\n  <Value Name="GPON_SN" Value="ODIT00000001"/>\n</Config>\n' > "$T/config/lastgood_hs.xml"
echo "not even xml" > "$T/config/lastgood.xml"
before=$(cat "$T/config/lastgood.xml" "$T/config/lastgood_hs.xml" | cksum)
reseed run > /dev/null
eq "present: both files left as they were" "$before" "$(cat "$T/config/lastgood.xml" "$T/config/lastgood_hs.xml" | cksum)"
[ -f "$T/config/config-reseeded" ] && no "present: no record" || ok "present: no record"
eq "present: nothing logged" "" "$(cat "$T/kmsg")"

# ---- an empty file counts as missing; the other one is kept
: > "$T/config/lastgood.xml"
reseed run > /dev/null
cmp -s "$TPL/lastgood.xml" "$T/config/lastgood.xml" && ok "empty CS: recreated" || no "empty CS: recreated"
eq "empty CS: HS kept" "ODIT00000001" "$(val GPON_SN "$T/config/lastgood_hs.xml")"
eq "empty CS: the record names CS only" "files=lastgood.xml" "$(head -n 1 "$T/config/config-reseeded")"

# ---- two runs give two MACs (three random bytes, not a constant)
rm -f "$T/config/lastgood_hs.xml"
reseed run > /dev/null
mac2=$(val ELAN_MAC_ADDR "$T/config/lastgood_hs.xml")
[ -n "$mac2" ] && [ "$mac1" != "$mac2" ] && ok "a second recreation draws another MAC ($mac1, $mac2)" \
	|| no "a second recreation draws another MAC ($mac1, $mac2)"

# ---- no random source: the MAC stays empty (network.sh keeps the driver one)
rm -f "$T/config/lastgood_hs.xml"
RANDOM_DEV=$T/no-such-device reseed run > /dev/null
cmp -s "$TPL/lastgood_hs.xml" "$T/config/lastgood_hs.xml" && ok "no random source: HS is the template, MAC empty" \
	|| no "no random source: HS is the template, MAC empty"

# ---- no template in the image: nothing installed, logged, still exit 0
rm -f "$T/config/"* ; : > "$T/kmsg"
out=$(TEMPLATE_DIR=$T/no-template reseed run); rc=$?
eq "no template: exits 0" 0 "$rc"
eq "no template: nothing installed" "" "$(ls "$T/config")"
t "no template: the failure is logged" 'event=config_reseed_failed files=lastgood.xml lastgood_hs.xml' "$(cat "$T/kmsg")"

# ---- a directory that cannot be written: nothing half-written, exit 0
if [ "$(id -u)" != 0 ]; then
	chmod 555 "$T/config"; : > "$T/kmsg"
	out=$(reseed run); rc=$?
	chmod 755 "$T/config"
	eq "read-only directory: exits 0" 0 "$rc"
	eq "read-only directory: nothing written" "" "$(ls "$T/config")"
	t "read-only directory: the failure is logged" 'event=config_reseed_failed' "$(cat "$T/kmsg")"
fi

# ---- no config directory at all: nothing to do
out=$(CONFIG_DIR=$T/none sh "$S" run 2>&1); rc=$?
eq "no config directory: exits 0" 0 "$rc"
[ -e "$T/none" ] && no "no config directory: not created" || ok "no config directory: not created"

eq "an unknown verb is refused" 2 "$(sh "$S" bogus > /dev/null 2>&1; echo $?)"

# ---- the real flash wrapper on a recreated store: every key of the main
# table (MIB_TABLE, HW_MIB_TABLE; the table rows after it are not single
# keys) reads and saves, which is what the web UI could not do after an erase
rm -f "$T/config/"*
reseed run > /dev/null
export CS="$T/config/lastgood.xml" HS="$T/config/lastgood_hs.xml" TMPDIR_="$T/var" \
	ODI_CONF="$T/config/odi.conf" REREG_HOLD="$T/hold"
bad=
for f in "$CS" "$HS"; do
	for k in $(sed -n '1,/<\/Dir>/s/.*<Value Name="\([^"]*\)" Value=.*/\1/p' "$f"); do
		v=$(val "$k" "$f")
		g=$(sh "$F" get "$k" 2>&1)
		[ "$g" = "$k=$v" ] || { bad="$bad get:$k"; continue; }
		s=$(sh "$F" set "$k" "$v" 2>&1)
		[ "$s" = "$k=$v" ] || bad="$bad set:$k"
	done
done
eq "flash: every key of the recreated store gets and sets" "" "$bad"
eq "flash: the serial number can be typed in" "GPON_SN=ODIT00000002" "$(sh "$F" set GPON_SN ODIT00000002 2>&1)"
eq "flash: and it is stored" "ODIT00000002" "$(val GPON_SN "$HS")"
unset CS HS TMPDIR_ ODI_CONF REREG_HOLD

# ---- rcs-lib.sh: no serial number, no gponsn and no gponact
pon() {   # pon <HS file>
	(
		# shellcheck source=/dev/null
		. "$LIB"
		# shellcheck disable=SC2034 # read by the sourced functions
		SAFE_FLAG=$T/no-flag HS=$1
		crumb() { echo "crumb $*"; }
		pon_step() { echo "step $1"; }
		grep() { [ "$*" = '-q type=1  /proc/odi_omci' ] && return 0; command grep "$@"; }
		pon_step_gponsn
		pon_step_gponact
	) 2>&1
}
rm -f "$T/config/"*
reseed run > /dev/null
out=$(pon "$T/config/lastgood_hs.xml")
t "empty GPON_SN: the gponsn step is skipped" '^crumb pon step gponsn: no GPON_SN in the store, the PON stays down$' "$out"
t "empty GPON_SN: and gponact" '^crumb pon step gponact: no GPON_SN in the store, not activated$' "$out"
printf '%s\n' "$out" | command grep -q '^step ' && no "empty GPON_SN: no PON verb written" "$out" || ok "empty GPON_SN: no PON verb written"
out=$(pon "$T/no-such-file")
printf '%s\n' "$out" | command grep -q '^step ' && no "no HS file: no PON verb written" "$out" || ok "no HS file: no PON verb written"
printf '<Value Name="GPON_SN" Value="ODIT00000001"/>\n' > "$T/sn.xml"
out=$(pon "$T/sn.xml")
t "a serial number: gponsn writes it" '^step gponsn ODIT00000001$' "$out"
t "  and gponact runs" '^step gponact$' "$out"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
