#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# /etc/scripts/identity.sh, the one-time identity snapshot and its restore.
# Runs on the HOST against sample stores, like test/flash_test.sh; with
# SH=busybox it runs the script under busybox ash instead of the host sh.
# The values are made up: no identity of a real stick belongs in the tree.
set -u
cd "$(dirname "$0")/.." || exit 1
# shellcheck source=test/host-flock.sh
. test/host-flock.sh
SH=${SH:-sh}
I=rootfs/skeleton/etc/scripts/identity.sh
F=rootfs/skeleton/etc/scripts/flash
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
t() {
	if [ "$2" = "$3" ]; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        want [$2]"; echo "        got  [$3]"; fail=$((fail + 1)); fi
}

fresh() {
	rm -rf "$T/c" "$T/tmp"
	mkdir -p "$T/c" "$T/tmp"
	cat > "$T/c/cs.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="LOID" Value="line-7"/>
		<Value Name="LOID_PASSWD" Value="a&amp;b"/>
		<Value Name="LOID_OLD" Value=""/>
		<Value Name="GPON_PLOAM_PASSWD" Value="0x31323334"/>
		<Value Name="LAN_IP_ADDR" Value="192.168.1.1"/>
	</Dir>
</Config>
XML
	cat > "$T/c/hs.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="HW_MIB_TABLE">
		<Value Name="GPON_SN" Value="TEST00000001"/>
		<Value Name="ELAN_MAC_ADDR" Value="020000000001"/>
		<Value Name="ELAN_MAC_ADDR2" Value="020000000002"/>
		<Value Name="GPON_ONU_MODEL" Value="MODEL-A"/>
		<Value Name="LOID_PASSWD_OLD" Value="old/pw\x"/>
		<Value Name="MAC_KEY" Value="00112233"/>
		<Value Name="PON_VENDOR_ID" Value="TEST"/>
	</Dir>
</Config>
XML
	printf 'NTP_SERVER=pool.example\nONU_HW_VERSION=HW1\n' > "$T/c/odi.conf"
	echo "image=odi-oss-test" > "$T/build"
}
export CS="$T/c/cs.xml" HS="$T/c/hs.xml" ODI_CONF="$T/c/odi.conf" SNAP="$T/c/identity.orig" \
	TMPDIR_="$T/tmp" SAFE_FLAG="$T/safe-mode" BUILD="$T/build" OMCI_IDENTITY_ON="$T/omci-identity.on"
id() { $SH "$I" "$@"; }
fl() { $SH "$F" "$@"; }
get() { fl get "$1" 2>/dev/null; }

# ---- snapshot
fresh
out=$(id snapshot 2>&1); rc=$?
t "snapshot succeeds"                       "0" "$rc"
t "and says where it wrote"                 "1" "$(printf '%s\n' "$out" | grep -c 'written to')"
t "the file is mode 600"                    "600" "$(stat -c %a "$SNAP")"
t "it names the image that took it"         "image=odi-oss-test" "$(grep '^image=' "$SNAP")"
t "an HS key with its store"                "hs GPON_SN=TEST00000001" "$(grep ' GPON_SN=' "$SNAP")"
t "a CS key, raw XML text kept"             "cs LOID_PASSWD=a&amp;b" "$(grep ' LOID_PASSWD=' "$SNAP")"
t "an empty key is recorded empty"          "cs LOID_OLD=" "$(grep ' LOID_OLD=' "$SNAP")"
t "an odi-only key from odi.conf"           "odi ONU_HW_VERSION=HW1" "$(grep ' ONU_HW_VERSION=' "$SNAP")"
t "a key in no store is recorded absent"    "- GPON_PLOAM_FORMAT" "$(grep 'GPON_PLOAM_FORMAT' "$SNAP")"
t "ELAN_MAC_ADDR2 does not match the MAC"   "hs ELAN_MAC_ADDR=020000000001" "$(grep ' ELAN_MAC_ADDR=' "$SNAP")"
t "twelve key lines"                        "12" "$(grep -c -E '^(cs|hs|odi|-) ' "$SNAP")"
t "no temp file left"                       "0" "$(find "$T/c" -name '*.tmp.*' | wc -l | tr -d ' ')"

# Never overwritten by a later boot.
fl set GPON_SN TEST00000002 >/dev/null
before=$(cat "$SNAP")
out=$(id snapshot 2>&1); rc=$?
t "a second snapshot is a quiet no-op"      "0:" "$rc:$out"
t "and leaves the first one as it was"      "$before" "$(cat "$SNAP")"

# Guards.
fresh
touch "$SAFE_FLAG"
out=$(id snapshot 2>&1)
t "a safe-mode boot takes none"             "0" "$([ -e "$SNAP" ] && echo 1 || echo 0)"
t "and says so"                             "1" "$(printf '%s\n' "$out" | grep -c 'safe-mode boot')"
rm -f "$SAFE_FLAG"
fresh; rm "$T/c/hs.xml"
t "a missing store takes none, exit 1"      "1:0" "$(id snapshot >/dev/null 2>&1; echo $?):$([ -e "$SNAP" ] && echo 1 || echo 0)"
fresh; head -n 3 "$T/c/hs.xml" > "$T/c/h"; mv "$T/c/h" "$T/c/hs.xml"
t "a cut-off store takes none"              "1:0" "$(id snapshot >/dev/null 2>&1; echo $?):$([ -e "$SNAP" ] && echo 1 || echo 0)"
fresh; sed -i 's/Value="TEST00000001"/Value=""/' "$T/c/hs.xml"
t "a store with no serial takes none"       "1:0" "$(id snapshot >/dev/null 2>&1; echo $?):$([ -e "$SNAP" ] && echo 1 || echo 0)"
fresh
t "a held writer lock fails, nothing written" "1:0" \
	"$(exec 9>>"$T/tmp/flash-writer.lock"; flock -n 9; LOCK_S=1 id snapshot >/dev/null 2>&1; echo "$?:$([ -e "$SNAP" ] && echo 1 || echo 0)")"

# ---- restore
fresh
id snapshot >/dev/null
cp "$CS" "$T/cs.orig"; cp "$HS" "$T/hs.orig"; cp "$ODI_CONF" "$T/odi.orig"
fl set GPON_SN TEST00000099 >/dev/null
fl set ELAN_MAC_ADDR 02000000ffff >/dev/null
fl set GPON_PLOAM_PASSWD 0x39393939 >/dev/null
fl set ONU_HW_VERSION "" >/dev/null      # an odi key cleared: its line is gone
fl default cs >/dev/null 2>&1 || true    # no defaults file here: nothing changes
sed -i 's/Value="line-7"/Value=""/' "$CS"   # what flash default cs does to LOID
fl set NTP_SERVER other.example >/dev/null
out=$(id show 2>&1)
t "show marks a changed key"                "GPON_SN: TEST00000001 [hs] differs" "$(printf '%s\n' "$out" | grep '^GPON_SN:')"
t "show hides a secret"                     "GPON_PLOAM_PASSWD: (hidden) [cs] differs" "$(printf '%s\n' "$out" | grep '^GPON_PLOAM_PASSWD:')"
t "show never prints a secret value"        "0" "$(printf '%s\n' "$out" | grep -c -e 0x31323334 -e 'a&amp;b' -e 00112233)"
t "show reports a cleared odi key"          "ONU_HW_VERSION: HW1 [odi] differs (now absent)" "$(printf '%s\n' "$out" | grep '^ONU_HW_VERSION:')"
out=$(id restore 2>&1); rc=$?
t "restore succeeds"                        "0" "$rc"
t "the serial is back"                      "GPON_SN=TEST00000001" "$(get GPON_SN)"
t "the MAC is back"                         "ELAN_MAC_ADDR=020000000001" "$(get ELAN_MAC_ADDR)"
t "the PLOAM password is back"              "GPON_PLOAM_PASSWD=0x31323334" "$(get GPON_PLOAM_PASSWD)"
t "the LOID a reset emptied is back"        "LOID=line-7" "$(get LOID)"
t "the cleared odi key is back"             "ONU_HW_VERSION=HW1" "$(get ONU_HW_VERSION)"
t "CS is byte for byte the original"        "same" "$(cmp -s "$CS" "$T/cs.orig" && echo same || echo differs)"
t "HS is byte for byte the original"        "same" "$(cmp -s "$HS" "$T/hs.orig" && echo same || echo differs)"
t "an odi key outside the list is kept"     "NTP_SERVER=other.example" "$(get NTP_SERVER)"
t "restore says apply.sh omci is next"      "1" "$(printf '%s\n' "$out" | grep -c 'next: apply.sh omci')"
t "and that the MAC needs a reboot"         "1" "$(printf '%s\n' "$out" | grep -c 'next: reboot')"
t "and that the model needs the switch"     "1" "$(printf '%s\n' "$out" | grep -c 'omci-identity.on exists')"
t "restore never prints a secret value"     "0" "$(printf '%s\n' "$out" | grep -c -e 0x31323334 -e 'a&amp;b')"
t "no work directory left"                  "0" "$(find "$T/tmp" -mindepth 1 -name 'identity.*' | wc -l | tr -d ' ')"
out=$(id restore 2>&1); rc=$?
t "a second restore has nothing to do"      "0:identity: nothing to restore, the stores hold the snapshot values" "$rc:$out"

# Raw values with & \ / round-trip through the sed replacement.
fl set LOID_PASSWD plain >/dev/null
fl set LOID_PASSWD_OLD plain >/dev/null
id restore >/dev/null 2>&1
t "an escaped & is restored as stored"      'a&amp;b' "$(sed -n 's/.*"LOID_PASSWD" Value="\([^"]*\)".*/\1/p' "$CS")"
t "a backslash and a slash too"             'old/pw\x' "$(sed -n 's/.*"LOID_PASSWD_OLD" Value="\([^"]*\)".*/\1/p' "$HS")"

# A key the snapshot has that is in no store now is reported, not invented.
fresh
id snapshot >/dev/null
sed -i '/"MAC_KEY"/d' "$HS"
out=$(id restore 2>&1); rc=$?
t "a key gone from every store fails restore" "1" "$rc"
t "and is named"                            "1" "$(printf '%s\n' "$out" | grep -c 'MAC_KEY: not restored')"

# A broken snapshot is refused before anything is written.
fresh
id snapshot >/dev/null
fl set GPON_SN TEST00000099 >/dev/null
cp "$HS" "$T/hs.before"
sed -i '/ LOID=/d' "$SNAP"
t "a snapshot missing a key is refused"     "1" "$(id restore >/dev/null 2>&1; echo $?)"
t "and nothing is written"                  "same" "$(cmp -s "$HS" "$T/hs.before" && echo same || echo differs)"
fresh
id snapshot >/dev/null
printf 'cs LOID=x\n' >> "$SNAP"
t "a key twice is refused"                  "1" "$(id restore >/dev/null 2>&1; echo $?)"
fresh
id snapshot >/dev/null
sed -i 's/^cs LOID=.*/cs LOID=a"b/' "$SNAP"
t "a quote in a value is refused"           "1" "$(id restore >/dev/null 2>&1; echo $?)"
fresh
t "no snapshot: show fails"                 "1" "$(id show >/dev/null 2>&1; echo $?)"

# replace needs --yes.
fresh
id snapshot >/dev/null
fl set GPON_SN TEST00000099 >/dev/null
t "replace without --yes is refused"        "1:hs GPON_SN=TEST00000001" "$(id replace >/dev/null 2>&1; echo "$?:$(grep ' GPON_SN=' "$SNAP")")"
t "replace --yes takes the current values"  "0:hs GPON_SN=TEST00000099" "$(id replace --yes >/dev/null 2>&1; echo "$?:$(grep ' GPON_SN=' "$SNAP")")"
t "a bad verb is a usage error"             "2" "$(id nope >/dev/null 2>&1; echo $?)"

echo "identity: $pass passed, $fail failed"
[ "$fail" = 0 ]
