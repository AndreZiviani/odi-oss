#!/bin/sh
# VLAN handling, both line shapes, both modes: the bridge rules omcid builds
# for a whole OLT session of each ISP with the manual tag on (the stick adds
# VLAN_MANU_TAG_VID to untagged frames) and off (transparent: no tag added
# or removed, the router tags). docs/SETTINGS.md, "VLAN handling".
#
# ISP1 is the shape with class 84: five GEM-side bridge ports, each a
# FwdOp 0x10 list of one VID (10 to 14; the manual VID is 11). ISP2 is the
# shape without: one unicast GEM behind a VEIP, no class 84, the manual VID
# 10, and a multicast GEM. The sessions are the same fixtures drv-test.sh
# replays (test/fixtures/omci-session-isp*.txt); only the store changes.
#
# The rebuild logs one "rule" line per bridge rule of a GEM-side port
# (respond/apply_bridge.c, bp_rules()); the last rebuild of the run is
# compared.
#
# Run from src/omci, inside the toolchain image, after qemu-test.sh (the
# Makefile test-omci target runs it):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh vlan-test.sh
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

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

mkdir -p /var/config /var/run
rm -f /var/config/lastgood_hs.xml /var/run/omcid-mib.snap /var/run/omcid-resume-decision

# rules <isp> <manual|transparent>: the rules of the last rebuild, one per
# line, "47/<port> gem <n> rule <kind> vid <v> pri <p>", sorted.
rules() {
	if [ "$2" = transparent ]; then
		sed 's/"VLAN_MANU_MODE" Value="1"/"VLAN_MANU_MODE" Value="0"/' \
			"$FIX/omci-store-$1.xml" > /var/config/lastgood.xml
	else
		cp "$FIX/omci-store-$1.xml" /var/config/lastgood.xml
	fi
	log=/tmp/omcid-vlan-$1-$2.log
	$Q respond/build/omcid -w 1 -c "$CAPS" > "$log" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$1.txt" > /dev/null 2>&1
	wait "$pid"
	awk '/\] 47\/[0-9a-f]+ gem [0-9]+ rule / { sub(/.*\] /, ""); cur = cur $0 "\n" }
	     /bridge connections rebuilt/ { last = cur; cur = "" }
	     END { printf "%s", last }' "$log" | sort
}

got=$(rules isp1 manual)
check "ISP1, manual tag on: VID 11 is the add-tag rule, multicast untagged" "$got" \
"47/0003 gem 1434 rule vid-filter vid 10 pri -1
47/0004 gem 1690 rule vid-filter vid 12 pri 5
47/0005 gem 1818 rule vid-filter vid 13 pri 4
47/0006 gem 1946 rule vid-filter vid 14 pri -1
47/0007 gem 1562 rule manual-add vid 11 pri 0
47/ffff gem 4095 rule manual-remove vid 11 pri 0"
got=$(rules isp1 transparent)
check "ISP1, transparent: every VID a filter, multicast as it comes" "$got" \
"47/0003 gem 1434 rule vid-filter vid 10 pri -1
47/0004 gem 1690 rule vid-filter vid 12 pri 5
47/0005 gem 1818 rule vid-filter vid 13 pri 4
47/0006 gem 1946 rule vid-filter vid 14 pri -1
47/0007 gem 1562 rule vid-filter vid 11 pri -1
47/ffff gem 4095 rule forward-all vid -1 pri -1"
got=$(grep -c 'manual vlan off (transparent)' /tmp/omcid-vlan-isp1-transparent.log)
check "and omcid says so at start" "$got" "1"

got=$(rules isp2 manual)
check "ISP2, manual tag on: VID 10 added upstream, removed from multicast" "$got" \
"47/0ffe gem 4094 rule manual-remove vid 10 pri 0
47/1002 gem 657 rule manual-add vid 10 pri 0"
got=$(rules isp2 transparent)
check "ISP2, transparent: forward-all, every tag passes both ways" "$got" \
"47/0ffe gem 4094 rule forward-all vid -1 pri -1
47/1002 gem 657 rule forward-all vid -1 pri -1"

sed 's/"VLAN_MANU_MODE" Value="1"/"VLAN_MANU_MODE" Value="0"/' \
	"$FIX/omci-store-isp2.xml" > /tmp/store-transparent.xml
$Q respond/build/omcid -w 2 > /tmp/omcid-vlan-cli.log 2>&1 &
pid=$!
sleep 2
got=$($Q cli/build/omcli vlan "$FIX/omci-store-isp2.xml" 2>&1 | sed -n '6p')
check "omcli vlan: the manual tag is stick tags" "$got" \
      "handling    stick tags: vid 10 added to untagged frames"
got=$($Q cli/build/omcli vlan /tmp/store-transparent.xml 2>&1 | sed -n '6p')
check "omcli vlan: mode 0 is transparent" "$got" \
      "handling    transparent: no tag added or removed, the router tags"
wait "$pid"

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
exit "$fail"
