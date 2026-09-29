#!/bin/sh
# The event lines (respond/events.c) and `omcli provision` (respond/show.c),
# against a whole ISP1 provisioning session: the same OLT side drv-test.sh
# replays for the driver-call golden (test/fixtures/omci-session-isp1.txt),
# so the counts below are the counts of that real session.
#
# What this covers: every omcid event line on the omcid.log side -- start,
# the OLT MIB reset, the MIB upload, one provisioning burst and its summary,
# a CLI (local) MIB reset, an OLT reboot request, a software download start --
# and the rate limit. The syslog side, the same line through /dev/log to
# busybox syslogd, needs a syslogd, so test/qemu/run-qemu.sh checks it on the
# full-system image instead.
#
# Run from src/omci, inside the toolchain image, after qemu-test.sh (the
# Makefile test-omci target runs all four):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh events-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0

check() {
	if [ "$2" = "$3" ]; then
		echo "ok    $1"
	else
		echo "FAIL  $1"
		echo "--- expected"; printf '%s\n' "$3"
		echo "--- got";      printf '%s\n' "$2"
		fail=$((fail + 1))
	fi
}

# One baseline frame: tci, message type (with AR), class, instance, zero
# contents, the trailer and a zero CRC (omcid does not check it, and the
# session fixtures carry zero too).
frame() {
	printf '%s%s0a%s%s%s0000002800000000\n' "$1" "$2" "$3" "$4" \
		"0000000000000000000000000000000000000000000000000000000000000000"
}

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

mkdir -p /var/config /var/run
cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
rm -f /var/config/lastgood_hs.xml
# No decision file: to omcid this is the first start of the boot.
rm -f /var/run/omcid-resume-decision

log=/tmp/omcid-events.log
$Q respond/build/omcid -w 8 -c "$CAPS" > "$log" 2>&1 &
sleep 2

# No netlink under qemu, so no ONU state: not O5, so no resume.
got=$(grep '^event=start ' "$log")
check "start: first of the boot, back to re-registration, and why" "$got" \
      "event=start run=boot mode=reregister reason=not_o5 onu_state=unknown rows=0"

$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
# The burst closes EV_PROV_QUIET_S (10 s) after its last write; the check
# runs once a second.
sleep 13

got=$(grep -c '^event=mib_reset side=olt ' "$log")
check "the OLT MIB reset, as the OLT's" "$got" "1"
got=$(grep '^event=mib_upload_begin ' "$log")
check "the MIB upload, with the count the OLT was told" "$got" \
      "event=mib_upload_begin entities=301"
got=$(grep -c '^event=provision_begin .*after_mib_reset=1$' "$log")
check "one burst opens, after the reset" "$got" "1"
got=$(grep '^event=mib_upload_end ' "$log" | sed 's/ duration_s=[0-9.]*//')
check "and its end, at the Next that answers the last entity" "$got" \
      "event=mib_upload_end entities=301"
# 83 creates in the session, one of them for class 351, which the model
# does not have: refused, so not counted.
got=$(grep '^event=provision_end ' "$log" | sed 's/ duration_s=[0-9.]*//')
check "and closes with the session's own counts" "$got" \
      "event=provision_end creates=82 sets=102 deletes=0 rows=161 services=6 after_mib_reset=1"
# Nothing per message: the whole session is a handful of lines.
got=$(grep -c '^event=' "$log")
check "a whole provisioning session is six event lines" "$got" "6"

# `omcli provision`: what that session provisioned.
out=$($Q cli/build/omcli provision 2>&1)
got=$(echo "$out" | grep '^summary ')
check "provision: the summary" "$got" \
      "summary rows=161 tconts=5 gem_ports=6 vlans=6 traffic_descriptors=0 services=6 mib_data_sync=184"
got=$(echo "$out" | grep -c '^gem me=[0-9]* port=[0-9]* direction=[123] tcont_me=')
check "provision: one line per GEM port" "$got" "6"
got=$(echo "$out" | grep '^vlan ' | tr '\n' ';')
check "provision: the VLANs, once per source" "$got" \
      "vlan vid=1 source=ext_vlan_treatment;vlan vid=10 source=vlan_filter;vlan vid=11 source=vlan_filter;vlan vid=12 source=vlan_filter;vlan vid=13 source=vlan_filter;vlan vid=14 source=vlan_filter;"

# A MIB reset from the CLI is this side's, not the OLT's.
$Q cli/build/omcli -f mib reset > /dev/null 2>&1
sleep 1
got=$(grep -c '^event=mib_reset side=local rows=161 ' "$log")
check "a CLI MIB reset is logged as local" "$got" "1"

# What the OLT may act on: a reboot, which omcid refuses, and a software
# download, which it accepts and discards (OLT_SW_DOWNLOAD, swdl-test.sh).
$Q cli/build/omcli --inject "$(frame 0e01 59 0100 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e02 53 0007 0001)" > /dev/null 2>&1
sleep 1
got=$(grep '^event=olt_reboot ' "$log")
check "an OLT reboot request" "$got" \
      "event=olt_reboot class=256 inst=0 result=not_supported"
got=$(grep '^event=sw_image ' "$log")
check "a software download start, accepted by default (swdl-test.sh has the rest)" "$got" \
      "event=sw_image op=download_start inst=1 size=0 window=1 result=ok"

# Rate limited: 30 more reboot requests, back to back, and the window of 20
# lines a minute already holds 9 (start, OLT reset, upload begin and end,
# provision begin and end, CLI reset, reboot, sw_image).
: > /tmp/reboots.txt
i=0
while [ "$i" -lt 30 ]; do
	frame "0f$(printf '%02x' "$i")" 59 0100 0000 >> /tmp/reboots.txt
	i=$((i + 1))
done
$Q cli/build/omcli --inject-file /tmp/reboots.txt > /dev/null 2>&1
sleep 1
got=$(grep -c '^event=' "$log")
check "a flood is cut at 20 event lines a minute" "$got" "20"

[ "$fail" -eq 0 ] || cp "$log" /src/src/omci/events-test.log 2>/dev/null || true
kill %1 2>/dev/null
wait 2>/dev/null

if [ "$fail" -eq 0 ]; then echo "events-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
exit "$fail"
