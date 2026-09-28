#!/usr/bin/env bash
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

# odi-only keys (SYSLOG_SERVER, NTP_SERVER): a plain KEY=value file, since the
# stock XML has never carried them. A real config dir, so the rename is real.
cfg="$T/config"
mkdir -p "$cfg"
cp "$T/cs.xml" "$cfg/lastgood.xml"
cp "$T/hs.xml" "$cfg/lastgood_hs.xml"
export CS="$cfg/lastgood.xml" HS="$cfg/lastgood_hs.xml" ODI_CONF="$cfg/odi.conf"
xml_before=$(cksum < "$CS")
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
t "so get fails for it"                     "GET fail." "$(sh $F get SYSLOG_SERVER 2>&1)"
t "and the other key survives"              "NTP_SERVER=pool.ntp.org:123" "$(sh $F get NTP_SERVER)"
t "flash all cs no longer lists the cleared one" "0" "$(sh $F all cs | grep -c 'SYSLOG_SERVER')"
printf 'garbage line\n=novalue\nSYSLOG_SERVERX=nope\n#SYSLOG_SERVER=no\n' >> "$ODI_CONF"
t "garbage lines are never read as a value" "GET fail." "$(sh $F get SYSLOG_SERVER 2>&1)"
t "a set keeps other lines as they are"     "SYSLOG_SERVER=a.b" "$(sh $F set SYSLOG_SERVER a.b)"
t "and the garbage is still there"          "4" "$(grep -c 'garbage line\|=novalue\|SYSLOG_SERVERX\|#SYSLOG' "$ODI_CONF")"
t "no temp file is left behind"             "0" "$(find "$cfg" -name '*tmp*' | wc -l | tr -d ' ')"
sh $F set SYSLOG_SERVER 'a"b' >/dev/null 2>&1
t "a quote in an odi value is refused"      "SYSLOG_SERVER=a.b" "$(sh $F get SYSLOG_SERVER)"
nl=$(printf 'a\nNTP_SERVER=evil')
sh $F set SYSLOG_SERVER "$nl" >/dev/null 2>&1
t "a newline cannot smuggle a second key"   "NTP_SERVER=pool.ntp.org:123" "$(sh $F get NTP_SERVER)"
rm -rf "$cfg"
sh $F set SYSLOG_SERVER x >/dev/null 2>&1
t "with no config dir a set fails rather than inventing one" "1" "$([ -e "$cfg" ] && echo 0 || echo 1)"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
