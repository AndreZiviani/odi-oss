#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# Our /etc/scripts/flash, the config accessor confd reaches device settings
# through. Runs on the HOST against a sample config, so it is fast and needs no
# container; test/rootfs_chroot_inner.sh separately proves the same script runs
# under busybox ash on MIPS, which is what will actually interpret it.
#
# The four invocations here are not chosen freely -- they are exactly the
# command lines the stock confd is observed to execve(): one for each of the
# two tables, a set whose KEY=value echo it parses, and the reset.
set -u
cd "$(dirname "$0")/.." || exit 1
# shellcheck source=test/host-flock.sh
. test/host-flock.sh
F=rootfs/skeleton/etc/scripts/flash
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/var"

cat > "$T/cs.xml" <<'XML'
<Config>
  <Value Name="LAN_IP_ADDR" Value="192.168.0.1"/>
  <Value Name="LAN_IP_ADDR2" Value="192.168.100.1"/>
  <Value Name="VLAN_MANU_TAG_VID" Value="11"/>
</Config>
XML
cat > "$T/hs.xml" <<'XML'
<Config>
  <Value Name="ELAN_MAC_ADDR" Value="aabbccddeeff"/>
</Config>
XML

export CS="$T/cs.xml" HS="$T/hs.xml" TMPDIR_="$T/var"
pass=0; fail=0
t() {
	if [ "$2" = "$3" ]; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        want [$2]"; echo "        got  [$3]"; fail=$((fail + 1)); fi
}

t "get reads the CS table"            "LAN_IP_ADDR=192.168.0.1"    "$(sh $F get LAN_IP_ADDR)"
t "get reads the HS table"            "ELAN_MAC_ADDR=aabbccddeeff" "$(sh $F get ELAN_MAC_ADDR)"
# The closing quote in the pattern is what stops LAN_IP_ADDR matching
# LAN_IP_ADDR2. network.sh depends on the same property.
t "a key cannot match a longer key"   "LAN_IP_ADDR2=192.168.100.1" "$(sh $F get LAN_IP_ADDR2)"
t "all cs dumps the file"             "5"                          "$(sh $F all cs | wc -l | tr -d ' ')"
t "all hs dumps the other file"       "3"                          "$(sh $F all hs | wc -l | tr -d ' ')"
t "set echoes KEY=value, as confd parses" "VLAN_MANU_TAG_VID=10"   "$(sh $F set VLAN_MANU_TAG_VID 10)"
t "the set persisted"                 "VLAN_MANU_TAG_VID=10"       "$(sh $F get VLAN_MANU_TAG_VID)"
t "neighbouring keys untouched"       "192.168.0.1"                "$(sh $F get LAN_IP_ADDR | sed 's/.*=//')"
t "set finds the key in HS too"       "ELAN_MAC_ADDR=001122334455" "$(sh $F set ELAN_MAC_ADDR 001122334455)"
t "the file is still valid XML"       "1"                          "$(grep -c '</Config>' "$T/cs.xml")"

# Refusals. Each of these would otherwise corrupt the store quietly.
t "an unknown key is refused"         "1" "$(sh $F set NOPE 1 >/dev/null 2>&1; echo $?)"
t "a quote in the value is refused"   "1" "$(sh $F set LAN_IP_ADDR 'a"b' >/dev/null 2>&1; echo $?)"
t "an ampersand is refused"           "1" "$(sh $F set LAN_IP_ADDR 'a&b' >/dev/null 2>&1; echo $?)"
t "a key with sed metacharacters is refused" "1" "$(sh $F set 'LAN.*' 1 >/dev/null 2>&1; echo $?)"
t "an empty key is refused"           "1" "$(sh $F set '' 1 >/dev/null 2>&1; echo $?)"
# default cs: a merge of the image defaults into CS. Keys the defaults name
# take their default value (present: rewritten; absent from both files:
# added under MIB_TABLE), a default key kept in HS is left there, and every
# other key is untouched.
cat > "$T/def.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="DEVICE_TYPE" Value="0"/>
		<Value Name="LOID" Value=""/>
		<Value Name="LOID_PASSWD_OLD" Value=""/>
		<Value Name="DUAL_MGMT_MODE" Value="1"/>
	</Dir>
</Config>
XML
cat > "$T/rcs.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="DEVICE_TYPE" Value="2"/>
		<Value Name="LOID" Value="someone"/>
		<Value Name="LAN_IP_ADDR" Value="192.168.0.1"/>
	</Dir>
</Config>
XML
cat > "$T/rhs.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="HW_MIB_TABLE">
		<Value Name="LOID_PASSWD_OLD" Value="old-secret"/>
	</Dir>
</Config>
XML
rd() { CS="$T/rcs.xml" HS="$T/rhs.xml" DEFAULTS="$T/def.xml" sh $F "$@"; }
out=$(rd default cs 2>&1); rc=$?
t "default cs succeeds"                      "0" "$rc"
t "and says so in the words confd shows"     "1" "$(printf '%s\n' "$out" | grep -c 'Reset CS to default configuration success.')"
t "a default key present in CS is reset"     "DEVICE_TYPE=0" "$(CS="$T/rcs.xml" HS="$T/rhs.xml" sh $F get DEVICE_TYPE)"
t "an empty default clears the value"        "LOID=" "$(CS="$T/rcs.xml" HS="$T/rhs.xml" sh $F get LOID)"
t "a default key absent from both is added"  "DUAL_MGMT_MODE=1" "$(CS="$T/rcs.xml" HS="$T/rhs.xml" sh $F get DUAL_MGMT_MODE)"
t "the added key sits inside MIB_TABLE"      "3" "$(grep -n 'DUAL_MGMT_MODE' "$T/rcs.xml" | cut -d: -f1)"
t "a key the defaults do not name survives"  "LAN_IP_ADDR=192.168.0.1" "$(CS="$T/rcs.xml" HS="$T/rhs.xml" sh $F get LAN_IP_ADDR)"
t "a default key kept in HS is left there"   "old-secret" "$(sed -n 's/.*LOID_PASSWD_OLD" Value="\([^"]*\)".*/\1/p' "$T/rhs.xml")"
t "and no second copy is put in CS"          "0" "$(grep -c LOID_PASSWD_OLD "$T/rcs.xml")"
t "the reset file is still valid XML"        "1" "$(grep -c '</Config>' "$T/rcs.xml")"
t "hs cannot be reset"                       "1" "$(rd default hs >/dev/null 2>&1; echo $?)"
t "no defaults file refuses, CS untouched"   "1" "$(CS="$T/rcs.xml" HS="$T/rhs.xml" DEFAULTS=/nonexistent sh $F default cs >/dev/null 2>&1; echo $?)"
# The shipped defaults parse: every Value line is one key merged.
t "the image defaults file merges whole" \
  "$(grep -c '<Value Name=' rootfs/skeleton/etc/config_default.xml)" \
  "$(cp "$T/rcs.xml" "$T/img.xml"; CS="$T/img.xml" HS="$T/rhs.xml" DEFAULTS=rootfs/skeleton/etc/config_default.xml sh $F default cs 2>/dev/null | grep -c -e '=' -e 'kept')"
t "a missing config file is an error, not an empty dump" "1" \
  "$(CS=/nonexistent sh $F all cs >/dev/null 2>&1; echo $?)"

# The value is a sed REPLACEMENT as well as an XML attribute, so \ and / are
# special to sed even though neither can break the XML. Before these were
# escaped: `a\` stored `a` at exit 0, and a value carrying / ran the rest of
# itself as sed commands -- one set on LAN_IP_ADDR rewrote VLAN_MANU_TAG_VID
# and left a stray quote in the document, still at exit 0.
cat > "$T/cs.xml" <<'XML'
<Config>
  <Value Name="LAN_IP_ADDR" Value="192.168.0.1"/>
  <Value Name="LAN_IP_ADDR2" Value="192.168.100.1"/>
  <Value Name="VLAN_MANU_TAG_VID" Value="11"/>
</Config>
XML
t "a backslash is stored, not swallowed" 'LAN_IP_ADDR=a\b' "$(sh $F set LAN_IP_ADDR 'a\b')"
t "and reads back the same"              'LAN_IP_ADDR=a\b' "$(sh $F get LAN_IP_ADDR)"
# A LONE trailing backslash, built with printf: written literally it is
# 'a\', which shellcheck reads as an attempt to escape the closing quote.
bs=$(printf '\134')
t "a trailing backslash survives too"    "LAN_IP_ADDR=a$bs" "$(sh $F set LAN_IP_ADDR "a$bs")"
t "a slash is stored verbatim"     "LAN_IP_ADDR=192.168.0.1/24" "$(sh $F set LAN_IP_ADDR '192.168.0.1/24')"
sh $F set LAN_IP_ADDR 'x/;s/11/999' >/dev/null 2>&1
t "a slash cannot run sed commands against another key" "VLAN_MANU_TAG_VID=11" \
  "$(sh $F get VLAN_MANU_TAG_VID)"
t "and leaves no stray quote in the document" "0" \
  "$(grep -c 'Value="999""' "$T/cs.xml")"

# odi-only keys (SYSLOG_SERVER, NTP_SERVER, OLT_SW_DOWNLOAD): a plain KEY=value file, since the
# stock XML has never carried them. A real config dir, so the rename is real.
cfg="$T/config"
mkdir -p "$cfg"
cp "$T/cs.xml" "$cfg/lastgood.xml"
cp "$T/hs.xml" "$cfg/lastgood_hs.xml"
export CS="$cfg/lastgood.xml" HS="$cfg/lastgood_hs.xml" ODI_CONF="$cfg/odi.conf"
xml_before=$(cksum < "$CS")
t "an odi key never set reads empty, before odi.conf exists" "NTP_SERVER=" "$(sh $F get NTP_SERVER 2>&1)"
t "a name that is no key at all still fails" "GET fail." "$(sh $F get NO_SUCH_KEY 2>&1)"
sh $F get NO_SUCH_KEY >/dev/null 2>&1; rc=$?
t "and exits 1"                             "1" "$rc"
t "set on an odi key stores it and echoes it" "SYSLOG_SERVER=192.168.0.3" "$(sh $F set SYSLOG_SERVER 192.168.0.3)"
t "and get reads it back"                   "SYSLOG_SERVER=192.168.0.3" "$(sh $F get SYSLOG_SERVER)"
t "it landed in odi.conf, not the XML"      "SYSLOG_SERVER=192.168.0.3" "$(cat "$ODI_CONF")"
t "and the stock XML is byte for byte as it was" "$xml_before" "$(cksum < "$CS")"
t "a second odi key sits beside the first"  "NTP_SERVER=pool.ntp.org:123" "$(sh $F set NTP_SERVER pool.ntp.org:123)"
t "and does not disturb it"                 "SYSLOG_SERVER=192.168.0.3" "$(sh $F get SYSLOG_SERVER)"
t "an odi key can be replaced"              "SYSLOG_SERVER=logs.lan" "$(sh $F set SYSLOG_SERVER logs.lan)"
t "and only one line holds it"              "1" "$(grep -c '^SYSLOG_SERVER=' "$ODI_CONF")"
t "flash all cs carries the odi keys too"   "2" "$(sh $F all cs | grep -c 'Name="\(SYSLOG\|NTP\)_SERVER" Value=')"
t "and still ends with the closing tag"     "</Config>" "$(sh $F all cs | tail -n 1)"
t "a stock key still goes to the XML"       "LAN_IP_ADDR=10.0.0.9" "$(sh $F set LAN_IP_ADDR 10.0.0.9)"
t "and the odi file is untouched by it"     "2" "$(wc -l < "$ODI_CONF" | tr -d ' ')"
t "an empty value clears the key"           "SYSLOG_SERVER=" "$(sh $F set SYSLOG_SERVER '')"
t "so get prints it empty"                  "SYSLOG_SERVER=" "$(sh $F get SYSLOG_SERVER 2>&1)"
sh $F get SYSLOG_SERVER >/dev/null 2>&1; rc=$?
t "and succeeds: a known key, only empty"   "0" "$rc"
t "the svc-*.sh read of it is empty"        "" "$(sh $F get SYSLOG_SERVER 2>/dev/null | sed 's/^[^=]*=//' | grep .)"
t "and the other key survives"              "NTP_SERVER=pool.ntp.org:123" "$(sh $F get NTP_SERVER)"
t "OLT_SW_DOWNLOAD is an odi key too"       "OLT_SW_DOWNLOAD=reject" "$(sh $F set OLT_SW_DOWNLOAD reject)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OLT_SW_DOWNLOAD=reject$' "$ODI_CONF")"
t "ODI_LASER_UNVERIFIED is an odi key too" "ODI_LASER_UNVERIFIED=1" "$(sh $F set ODI_LASER_UNVERIFIED 1)"
t "in odi.conf, the exact line rcS.pon greps" "1" "$(grep -c '^ODI_LASER_UNVERIFIED=1$' "$ODI_CONF")"
t "OMCI_ACCEPT_REBOOT is an odi key too" "OMCI_ACCEPT_REBOOT=1" "$(sh $F set OMCI_ACCEPT_REBOOT 1)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_ACCEPT_REBOOT=1$' "$ODI_CONF")"
t "OMCI_TCONT_ALLOC_UNSET is an odi key too" "OMCI_TCONT_ALLOC_UNSET=unassigned" "$(sh $F set OMCI_TCONT_ALLOC_UNSET unassigned)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_TCONT_ALLOC_UNSET=unassigned$' "$ODI_CONF")"
t "OMCI_UNI_LOCK is an odi key too" "OMCI_UNI_LOCK=0" "$(sh $F set OMCI_UNI_LOCK 0)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_UNI_LOCK=0$' "$ODI_CONF")"
t "OMCI_VEIP is an odi key too" "OMCI_VEIP=0" "$(sh $F set OMCI_VEIP 0)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_VEIP=0$' "$ODI_CONF")"
t "OMCI_PRIVATE_ME is an odi key too" "OMCI_PRIVATE_ME=0" "$(sh $F set OMCI_PRIVATE_ME 0)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_PRIVATE_ME=0$' "$ODI_CONF")"
t "OMCI_POTS_PORTS is an odi key too" "OMCI_POTS_PORTS=2" "$(sh $F set OMCI_POTS_PORTS 2)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_POTS_PORTS=2$' "$ODI_CONF")"
t "OMCI_VOIP_LINE_REPORT is an odi key too" "OMCI_VOIP_LINE_REPORT=1" "$(sh $F set OMCI_VOIP_LINE_REPORT 1)"
t "OMCI_POTS_SLOT_ID is an odi key too" "OMCI_POTS_SLOT_ID=9" "$(sh $F set OMCI_POTS_SLOT_ID 9)"
t "and reads back from odi.conf, where omcid reads it" "1" "$(grep -c '^OMCI_POTS_SLOT_ID=9$' "$ODI_CONF")"
t "cleared, it reads empty (omcid then accepts)" "OLT_SW_DOWNLOAD=" "$(sh $F set OLT_SW_DOWNLOAD '')"
t "flash all cs no longer lists the cleared one" "0" "$(sh $F all cs | grep -c 'SYSLOG_SERVER')"
printf 'garbage line\n=novalue\nSYSLOG_SERVERX=nope\n#SYSLOG_SERVER=no\n' >> "$ODI_CONF"
t "garbage lines are never read as a value" "SYSLOG_SERVER=" "$(sh $F get SYSLOG_SERVER 2>&1)"
t "a set keeps other lines as they are"     "SYSLOG_SERVER=a.b" "$(sh $F set SYSLOG_SERVER a.b)"
t "and the garbage is still there"          "4" "$(grep -c 'garbage line\|=novalue\|SYSLOG_SERVERX\|#SYSLOG' "$ODI_CONF")"
t "no temp file is left behind"             "0" "$(find "$cfg" -name '*tmp*' | wc -l | tr -d ' ')"
# flash all cs reads odi.conf in one pass. The same lines as before: the first
# line of a key wins, as get reads it; keys print in ODI_KEYS order; empty and
# unknown keys are left out; a value may hold an =.
oc="$T/odi-all.conf"
printf 'NTP_SERVER=b=c\nSYSLOG_SERVER=a\nNTP_SERVER=d\nOMCI_VEIP=\nNOT_A_KEY=x\n=v\nSYSLOG_SERVERX=no\n' > "$oc"
t "flash all cs: ODI_KEYS order, first line wins, no empty or unknown key" \
  "$(printf '\t\t<Value Name="SYSLOG_SERVER" Value="a"/>\n\t\t<Value Name="NTP_SERVER" Value="b=c"/>')" \
  "$(ODI_CONF="$oc" sh $F all cs | grep 'Name="\(SYSLOG_SERVER\|NTP_SERVER\|OMCI_VEIP\|NOT_A_KEY\|SYSLOG_SERVERX\)"')"
t "and get agrees on the first line"        "NTP_SERVER=b=c" "$(ODI_CONF="$oc" sh $F get NTP_SERVER)"
sh $F set SYSLOG_SERVER 'a"b' >/dev/null 2>&1
t "a quote in an odi value is refused"      "SYSLOG_SERVER=a.b" "$(sh $F get SYSLOG_SERVER)"
nl=$(printf 'a\nNTP_SERVER=evil')
sh $F set SYSLOG_SERVER "$nl" >/dev/null 2>&1
t "a newline cannot smuggle a second key"   "NTP_SERVER=pool.ntp.org:123" "$(sh $F get NTP_SERVER)"
t "the ONU-G hardware version is an odi key too" "ONU_HW_VERSION=TEST-HW-1" "$(sh $F set ONU_HW_VERSION TEST-HW-1)"
t "and lands in odi.conf, where omcid reads it" "1" "$(grep -c '^ONU_HW_VERSION=TEST-HW-1$' "$ODI_CONF")"
rm -rf "$cfg"
sh $F set SYSLOG_SERVER x >/dev/null 2>&1
t "with no config dir a set fails rather than inventing one" "1" "$([ -e "$cfg" ] && echo 0 || echo 1)"

# Installing over the store: /var/tmp is a tmpfs and /var/config jffs2, so an
# mv from the temp directory is a copy that removes the target first. The
# file must reach the target by a rename inside the target's own directory.
# A shim mv records every move; a shim cp can be made to fail.
mkdir -p "$cfg"
cp "$T/cs.xml" "$CS"
cp "$T/hs.xml" "$HS"
shim="$T/shim"; mkdir -p "$shim"
cat > "$shim/mv" <<'SH'
#!/bin/sh
echo "$1 $2" >> "$MVLOG"
exec /bin/mv "$@"
SH
cat > "$shim/cp" <<'SH'
#!/bin/sh
[ -n "${CP_FAIL:-}" ] && exit 1
exec /bin/cp "$@"
SH
chmod +x "$shim/mv" "$shim/cp"
export MVLOG="$T/mv.log"
: > "$MVLOG"
PATH="$shim:$PATH" sh $F set VLAN_MANU_TAG_VID 12 >/dev/null
last=$(grep " $CS\$" "$MVLOG" | tail -1)
t "a set reaches CS by a rename from beside it" "$(dirname "$CS")" "$(dirname "${last%% *}")"
t "and the value is stored"           "VLAN_MANU_TAG_VID=12" "$(sh $F get VLAN_MANU_TAG_VID)"
t "no .new file is left beside it"    "0" "$(find "$(dirname "$CS")" -maxdepth 1 -name '*.new.*' | wc -l | tr -d ' ')"
before=$(cat "$CS")
CP_FAIL=1 PATH="$shim:$PATH" sh $F set VLAN_MANU_TAG_VID 13 >/dev/null 2>&1
t "a failed copy fails the set"       "1" "$(CP_FAIL=1 PATH="$shim:$PATH" sh $F set VLAN_MANU_TAG_VID 13 >/dev/null 2>&1; echo $?)"
t "and leaves CS as it was"           "$before" "$(cat "$CS")"
t "and leaves no temp files"          "0" "$(find "$T/var" "$(dirname "$CS")" -maxdepth 1 \( -name 'flash.*' -o -name '*.new.*' \) | wc -l | tr -d ' ')"
: > "$MVLOG"
cp "$T/rcs.xml" "$cfg/reset.xml"
CS="$cfg/reset.xml" HS="$T/rhs.xml" DEFAULTS="$T/def.xml" PATH="$shim:$PATH" sh $F default cs >/dev/null 2>&1
last=$(grep " $cfg/reset.xml\$" "$MVLOG" | tail -1)
t "default cs reaches CS by a rename from beside it" "$cfg" "$(dirname "${last%% *}")"

# A write that fails part way. ulimit -f caps the size of any file the
# command writes and SIGXFSZ is ignored, so a write past the cap returns an
# error, as it does on a full partition, from the real grep, sed and printf.
# The store must be left exactly as it was and no temp file may remain.
bigdir="$T/big"; mkdir -p "$bigdir"
fill() {   # fill <file> <lines>: comment lines, which flash keeps as they are
	i=0
	while [ "$i" -lt "$2" ]; do echo "# filler line number $i, keeps the file large"; i=$((i + 1)); done > "$1"
}
failing() { ( ulimit -f 1; trap '' XFSZ; "$@" ); }
oc="$bigdir/odi.conf"
fill "$oc" 200
echo "HOST_SERDES=2500basex" >> "$oc"
before=$(cat "$oc")
t "a set on a full partition fails" "1" "$(ODI_CONF="$oc" failing sh $F set SYSLOG_SERVER 10.0.0.9 >/dev/null 2>&1; echo $?)"
t "and odi.conf is exactly as it was" "$before" "$(cat "$oc")"
t "and no temp file is left"          "0" "$(find "$bigdir" -name '*.tmp.*' | wc -l | tr -d ' ')"
t "the error message names the file"  "1" "$(ODI_CONF="$oc" failing sh $F set SYSLOG_SERVER 10.0.0.9 2>&1 >/dev/null | grep -c 'left alone')"

# A short file whose writer reported success: a grep that stops half way and
# exits 0, as BusyBox 1.36 did under /dev/full. The status says nothing here;
# only the check of the new file can.
REAL_GREP=$(command -v grep); REAL_SED=$(command -v sed); export REAL_GREP REAL_SED
mkdir -p "$T/shortbin"
cat > "$T/shortbin/grep" <<'SH'
#!/bin/sh
# Real grep, but the first call (the write) keeps only the first line.
if [ ! -e "$SHORT_ONCE" ]; then
	: > "$SHORT_ONCE"
	"$REAL_GREP" "$@" | "$REAL_SED" 1q
	exit 0
fi
exec "$REAL_GREP" "$@"
SH
chmod +x "$T/shortbin/grep"
fill "$oc" 20
echo "HOST_SERDES=2500basex" >> "$oc"
before=$(cat "$oc")
rm -f "$T/once"
t "a short file with status 0 fails the set" "1" "$(SHORT_ONCE="$T/once" ODI_CONF="$oc" PATH="$T/shortbin:$PATH" sh $F set SYSLOG_SERVER 10.0.0.9 >/dev/null 2>&1; echo $?)"
t "and odi.conf is exactly as it was (short file)" "$before" "$(cat "$oc")"
t "and no temp file is left (short file)" "0" "$(find "$bigdir" -name '*.tmp.*' | wc -l | tr -d ' ')"
t "a clear of an absent key still succeeds" "SYSLOG_SERVER=" "$(ODI_CONF="$oc" sh $F set SYSLOG_SERVER '')"
t "and every other line is kept"      "$before" "$(cat "$oc")"
t "the new line goes last"            "SYSLOG_SERVER=10.0.0.9" "$(ODI_CONF="$oc" sh $F set SYSLOG_SERVER 10.0.0.9 >/dev/null; tail -n 1 "$oc")"
t "a set with no odi.conf yet creates it" "OMCI_VEIP=1" "$(rm -f "$bigdir/new.conf"; ODI_CONF="$bigdir/new.conf" sh $F set OMCI_VEIP 1; )"
t "and the file holds that line only" "OMCI_VEIP=1" "$(cat "$bigdir/new.conf")"

# The same for the XML store: a rewrite cut short must not replace a good file.
cs="$bigdir/cs.xml"
{ echo '<Config>'; i=0; while [ "$i" -lt 200 ]; do echo "  <Value Name=\"K$i\" Value=\"v$i\"/>"; i=$((i + 1)); done; echo '</Config>'; } > "$cs"
before=$(cat "$cs")
t "an XML set on a full partition fails" "1" "$(CS="$cs" failing sh $F set K7 changed >/dev/null 2>&1; echo $?)"
t "and the XML file is exactly as it was" "$before" "$(cat "$cs")"
t "and no temp file is left (xml)"    "0" "$(find "$bigdir" "$T/var" -name 'flash.*' -o -name '*.new.*' | wc -l | tr -d ' ')"
mkdir -p "$T/shortsed"
cat > "$T/shortsed/sed" <<'SH'
#!/bin/sh
# Only the rewrite of the store (its script names a Value), not the escaping.
case "$*" in *'Value='*) [ -e "$SHORT_ONCE" ] || {
	: > "$SHORT_ONCE"
	"$REAL_SED" "$@" | "$REAL_SED" 3q
	exit 0
} ;; esac
exec "$REAL_SED" "$@"
SH
chmod +x "$T/shortsed/sed"
rm -f "$T/once"
t "a short XML file with status 0 fails the set" "1" "$(SHORT_ONCE="$T/once" CS="$cs" PATH="$T/shortsed:$PATH" sh $F set K7 changed >/dev/null 2>&1; echo $?)"
t "and the XML file is exactly as it was (short file)" "$before" "$(cat "$cs")"
t "a good XML set on the same file works" "K7=changed" "$(CS="$cs" sh $F set K7 changed)"

# Two sets at once. The shim grep sleeps before it reads, so without a lock
# every set reads the file before any of them has written it and all but one
# change is lost. With the lock they run one after the other.
mkdir -p "$T/slowbin"
cat > "$T/slowbin/grep" <<'SH'
#!/bin/sh
sleep 0.3
exec "$REAL_GREP" "$@"
SH
chmod +x "$T/slowbin/grep"
oc="$bigdir/race.conf"
: > "$oc"
for k in SYSLOG_SERVER NTP_SERVER OMCI_VEIP HOST_SERDES; do
	ODI_CONF="$oc" LOCK_S=30 PATH="$T/slowbin:$PATH" sh $F set "$k" "v-$k" >/dev/null 2>&1 &
done
wait
t "four concurrent sets are all kept" "4" "$(grep -c '^[A-Z_]*=v-' "$oc")"
t "and each key once"                 "4" "$(sort -u "$oc" | wc -l | tr -d ' ')"

# The wait is bounded: a lock that is held makes a set give up, with the
# file as it was.
before=$(cat "$oc")
t "a held lock makes a set fail after LOCK_S" "1" "$( (
	exec 9>>"$T/var/flash-writer.lock"; flock -n 9 || exit 2
	ODI_CONF="$oc" LOCK_S=1 sh $F set NTP_SERVER other >/dev/null 2>&1; echo $? ) )"
t "and odi.conf is as it was"         "$before" "$(cat "$oc")"
t "the lock is free again once the holder ended" "NTP_SERVER=other" "$(ODI_CONF="$oc" sh $F set NTP_SERVER other)"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
