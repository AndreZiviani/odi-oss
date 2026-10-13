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

# ---- the OLT's class 171 drives the tagging (issue #29) --------------------
#
# With VLAN_CFG_TYPE other than 1 the stock firmware applies the OLT's own
# extended-VLAN rule. The ISP2-shaped session (one unicast GEM behind a VEIP,
# a multicast GEM, no class 84) with its class 171 frames replaced by what a
# Huawei OLT provisions: instance 1 on the Ethernet UNI (association type 2,
# point 0x0101) whose table adds a VLAN to untagged frames and drops the
# single- and double-tagged default, and instance 2 on the multicast side
# (type 0x0a, point 0x0601) with the default drops only.
#
# The entries, as the table attribute carries them (docs, class 171):
#   untagged -> add VLAN 100 pri 0 tpid 0x8100   f8000000f8000000000f800000000324
#   claro's  -> add VLAN 1 pri 0                 f8000000f8000000000f80000000000c
DEF1=f8000000e8000000c00f8000000f8000
DEF2=e8000000e8000000c00f8000000f8000
ADD100=f8000000f8000000000f800000000324
ADD1=f8000000f8000000000f80000000000c
ADD100P5=f8000000f8000000000f800000050324
ADD200=f8000000f8000000000f800000000644
ADD200ET=f8000000f8000001000f800000000644
COPYVID=f8000000f8000000000f800000008004
TAGGED=d8000000a8000000000f800000000324

tid=0
frame() {   # frame <cmd-byte> <class4> <inst4> <contents-hex>: one 48-byte frame
	tid=$((tid + 1))
	printf '%04x%s0a%s%s%s' "$tid" "$1" "$2" "$3" "$4" |
		awk '{ printf "%s", $0; for (i = length($0); i < 86; i++) printf "0"; print "2800000000" }'
}
set171() { frame 48 00ab "$1" "0400$2"; }

# session <entry>...: the isp2 session without its class 171 frames, then the
# two instances above with each <entry> set on instance 1 ("none": no 171).
session() {
	grep -v '^#' "$FIX/omci-session-isp2.txt" |
		awk 'substr($0, 7, 6) == "0a00ab" { next } { print }'
	if [ "$1" = none ]; then return; fi
	frame 44 00ab 0001 "020101"
	frame 48 00ab 0001 "3800810081000000"
	for e in "$@"; do set171 0001 "$e"; done
	frame 44 00ab 0002 "0a0601"
	frame 48 00ab 0002 "3800810081000000"
	set171 0002 "$DEF1"
	set171 0002 "$DEF2"
}

# store <type> <mode> -> /var/config/lastgood.xml, on the isp2 store (VID 10)
store() {
	sed -e "s/\"VLAN_CFG_TYPE\" Value=\"1\"/\"VLAN_CFG_TYPE\" Value=\"$1\"/" \
	    -e "s/\"VLAN_MANU_MODE\" Value=\"1\"/\"VLAN_MANU_MODE\" Value=\"$2\"/" \
	    "$FIX/omci-store-isp2.xml" > /var/config/lastgood.xml
}

# run171 <name> <phase-file>...: omcid with the store in place, each phase file
# injected after the previous one settled (a quiet second rebuilds).
run171() {
	name=$1; shift
	log=/tmp/omcid-171-$name.log
	$Q respond/build/omcid -w 2 -c "$CAPS" > "$log" 2>&1 &
	pid=$!
	sleep 2
	for f in "$@"; do
		$Q cli/build/omcli --inject-file "$f" > /dev/null 2>&1
		sleep 2
	done
	wait "$pid"
}

# rebuild <log> <n>: the rules of the n-th rebuild (0 = the last), sorted.
rebuild() {
	awk -v want="$2" '/\] 47\/[0-9a-f]+ gem [0-9]+ rule / { sub(/.*\] /, ""); cur = cur $0 "\n" }
	     /bridge connections rebuilt/ { n++; if (want == 0 || n == want) last = cur; cur = "" }
	     END { printf "%s", last }' "$1" | sort
}

TRANSPARENT="47/0ffe gem 4094 rule forward-all vid -1 pri -1
47/1002 gem 657 rule forward-all vid -1 pri -1"

# 1. The #29 table, type 0: VID 100 added upstream, removed from multicast.
store 0 0
session "$ADD100" "$DEF1" "$DEF2" > /tmp/s171-29.txt
run171 29 /tmp/s171-29.txt
got=$(rebuild /tmp/omcid-171-29.log 0)
check "#29, VLAN_CFG_TYPE 0: the OLT's VLAN 100 is the add-tag rule" "$got" \
"47/0ffe gem 4094 rule manual-remove vid 100 pri 0
47/1002 gem 657 rule manual-add vid 100 pri 0"
got=$(grep -c 'untagged frames get VLAN 100 pri 0 from the OLT' /tmp/omcid-171-29.log)
check "and the log says where the tag came from" "$got" "1"

# 2. Its priority is the table's.
session "$ADD100P5" "$DEF1" "$DEF2" > /tmp/s171-p5.txt
run171 p5 /tmp/s171-p5.txt
got=$(rebuild /tmp/omcid-171-p5.log 0 | sed -n 2p)
check "the OLT's priority 5 is kept" "$got" "47/1002 gem 657 rule manual-add vid 100 pri 5"

# 3. Manual mode wins: claro's table (VLAN 1, not its service VID) and the
#    #29 table both leave the stick's own VID 10.
for t in "$ADD1" "$ADD100"; do
	store 1 1
	session "$t" "$DEF1" "$DEF2" > /tmp/s171-man.txt
	run171 man /tmp/s171-man.txt
	got=$(rebuild /tmp/omcid-171-man.log 0)
	check "manual tag on (type 1, mode 1): VLAN_MANU_TAG_VID 10 wins over $t" "$got" \
"47/0ffe gem 4094 rule manual-remove vid 10 pri 0
47/1002 gem 657 rule manual-add vid 10 pri 0"
done
store 1 0
run171 trans /tmp/s171-man.txt
got=$(rebuild /tmp/omcid-171-trans.log 0)
check "transparent (type 1, mode 0): the OLT rule is not applied either" "$got" "$TRANSPARENT"

# 4. Shapes that are not applied stay transparent and say so, once.
store 0 0
for shape in "copy-vid:$COPYVID:the added VID is not a concrete" \
            "tagged:$TAGGED,$DEF1:a rule for tagged frames" \
            "two:$ADD100,$ADD200ET:more than one rule for untagged" \
            "etype:$ADD200ET:the untagged rule filters on the EtherType" \
            "noadd:$DEF1,$DEF2:no rule for untagged"; do
	sn=${shape%%:*}; rest=${shape#*:}; ents=${rest%%:*}; why=${rest#*:}
	# shellcheck disable=SC2046
	session $(echo "$ents" | tr , ' ') > /tmp/s171-bad.txt
	run171 "bad-$sn" /tmp/s171-bad.txt
	got=$(rebuild "/tmp/omcid-171-bad-$sn.log" 0)
	check "unsupported ($sn): transparent" "$got" "$TRANSPARENT"
	got=$(grep -c "ext vlan: OLT rule not applied ($why" "/tmp/omcid-171-bad-$sn.log")
	check "unsupported ($sn): logged once" "$got" "1"
done
session none > /tmp/s171-none.txt
run171 none /tmp/s171-none.txt
got=$(rebuild /tmp/omcid-171-none.log 0)
check "no class 171 at all: transparent" "$got" "$TRANSPARENT"
got=$(grep -c 'ext vlan' /tmp/omcid-171-none.log)
check "and nothing to log" "$got" "0"

# 5. The table changes under a running omcid: Set, then Delete, each rebuilds.
session "$ADD100" "$DEF1" "$DEF2" > /tmp/s171-a.txt
set171 0001 "$ADD200" > /tmp/s171-b.txt
frame 46 00ab 0001 "" > /tmp/s171-c.txt
run171 live /tmp/s171-a.txt /tmp/s171-b.txt /tmp/s171-c.txt
got=$(rebuild /tmp/omcid-171-live.log 1 | sed -n 2p)
check "Create/Set: the table says VLAN 100" "$got" "47/1002 gem 657 rule manual-add vid 100 pri 0"
got=$(rebuild /tmp/omcid-171-live.log 2 | sed -n 2p)
check "Set on the table: the rule follows to VLAN 200" "$got" "47/1002 gem 657 rule manual-add vid 200 pri 0"
got=$(rebuild /tmp/omcid-171-live.log 0 | sed -n 2p)
check "Delete of the instance: back to transparent" "$got" "47/1002 gem 657 rule forward-all vid -1 pri -1"

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
