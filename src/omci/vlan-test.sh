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
# Run from src/omci, inside the toolchain image (the Makefile test-omci-vlan
# target):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh vlan-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0
. ./testlib.sh

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
	$Q respond/build/omcid -w 6 -c "$CAPS" > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$1.txt" > /dev/null 2>&1
	settled "$log"
	omcid_stop "$pid"
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
# OLT of issue #29 provisions: instance 1 on the Ethernet UNI (association type 2,
# point 0x0101) whose table adds a VLAN to untagged frames and drops the
# single- and double-tagged default, and instance 2 on the multicast side
# (type 0x0a, point 0x0601) with the default drops only.
#
# The entries, as the table attribute carries them (docs, class 171):
#   untagged -> add VLAN 100 pri 0 tpid 0x8100   f8000000f8000000000f800000000324
#   ISP1's  -> add VLAN 1 pri 0                 f8000000f8000000000f80000000000c
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
# injected after the previous one settled (the quiet second has rebuilt).
run171() {
	name=$1; shift
	log=/tmp/omcid-171-$name.log
	$Q respond/build/omcid -w 6 -c "$CAPS" > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	for f in "$@"; do
		$Q cli/build/omcli --inject-file "$f" > /dev/null 2>&1
		settled "$log"
	done
	omcid_stop "$pid"
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

# 3. Manual mode wins: ISP1's table (VLAN 1, not its service VID) and the
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

# 4b. The added tag named by the Output TPID attribute (treatment TPID code
# 6, DEI 0) with Output TPID 0x8100: the same tag, applied.
store 0 0
session "f8000000f8000000000f000300000966" "$DEF1" "$DEF2" > /tmp/s171-t6.txt
run171 t6 /tmp/s171-t6.txt
got=$(rebuild /tmp/omcid-171-t6.log 0 | sed -n 2p)
check "output TPID 0x8100, DEI 0: VLAN 300 added" "$got" "47/1002 gem 657 rule manual-add vid 300 pri 0"

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
$Q respond/build/omcid -w 6 > /tmp/omcid-vlan-cli.log 2>&1 &
pid=$!
omcid_up /tmp/omcid-vlan-cli.log
got=$($Q cli/build/omcli vlan "$FIX/omci-store-isp2.xml" 2>&1 | sed -n '6p')
check "omcli vlan: the manual tag is stick tags" "$got" \
      "handling    stick tags: vid 10 added to untagged frames"
got=$($Q cli/build/omcli vlan /tmp/store-transparent.xml 2>&1 | sed -n '6p')
check "omcli vlan: mode 0 is transparent" "$got" \
      "handling    transparent: no tag added or removed, the router tags"
omcid_stop "$pid"

# ---- the whole table: one rule per entry, whatever the shape ----------------
#
# Where the single untagged-add shape above is not what the OLT provisions,
# each entry of class 171 becomes its own bridge rule (respond/apply_bridge.c,
# ev_build()); the table is applied whole or not at all. "ext-vlan" lines are
# the rules of the entries, vid and pri what the rule matches (-1: not examined);
# "untagged" and "forward-all" are what the class 84 side leaves: no tagged
# pass-through once the single-tag default rule of the table discards, untagged frames
# passed unless an entry handles them.
#
# The entries, worked out with the layout of class 171 (src/omci/extvlan.h):
TRANS410=f800000080cd0000400f800000082c14   # C-VID 410 -> 1410, priority kept
SPLIT0=f800000000cd0000400f800000082c14     # the same for priority 0 only
SPLIT5=f800000050cd0000400f800000082c14
SPLIT6=f800000060cd0000400f800000082c14
PRIREMAP=f800000030320000400f800000058004   # (VID 100, pri 3) -> priority 5, VID kept
SPUSH=f800000080320000000f800000030646      # C-VID 100: S-VID 200 pri 3 pushed (Output TPID 0x88a8)
SPOP=8064000088000000400f8000000f8000       # S-VID 200 over a C-tag: S-tag popped (Input TPID 0x88a8)
UNTAG_S=f8000000f8000000000f800000030966    # untagged: S-VID 300 pri 3 added
POPANY=f800000088000000400f8000000f8000     # any single tag removed: no way back

# sessionx <input tpid> <output tpid> <entry>...: as session(), with the TPIDs
# of instance 1 and DsMode 0.
sessionx() {
	in=$1; out=$2; shift 2
	grep -v '^#' "$FIX/omci-session-isp2.txt" |
		awk 'substr($0, 7, 6) == "0a00ab" { next } { print }'
	frame 44 00ab 0001 "020101"
	frame 48 00ab 0001 "3800${in}${out}00"
	for e in "$@"; do set171 0001 "$e"; done
	frame 44 00ab 0002 "0a0601"
	frame 48 00ab 0002 "3800810081000000"
	set171 0002 "$DEF1"
	set171 0002 "$DEF2"
}

store 0 0
sessionx 8100 8100 "$TRANS410" "$DEF1" "$DEF2" > /tmp/s171-t.txt
run171 ev-trans /tmp/s171-t.txt
got=$(rebuild /tmp/omcid-171-ev-trans.log 0)
check "class 171, translation 410 -> 1410 and the default discards: its own rule, untagged passes, tagged does not" "$got" \
"47/0ffe gem 4094 rule forward-all vid -1 pri -1
47/1002 gem 657 rule ext-vlan vid 410 pri -1
47/1002 gem 657 rule untagged vid -1 pri -1"
got=$(grep -c 'OLT rule not applied' /tmp/omcid-171-ev-trans.log)
check "and nothing is said about a rule not applied" "$got" "0"

sessionx 8100 8100 "$TRANS410" > /tmp/s171-t2.txt
run171 ev-trans-pass /tmp/s171-t2.txt
got=$(rebuild /tmp/omcid-171-ev-trans-pass.log 0)
check "no default rule: the translation, and everything else forwarded" "$got" \
"47/0ffe gem 4094 rule forward-all vid -1 pri -1
47/1002 gem 657 rule ext-vlan vid 410 pri -1
47/1002 gem 657 rule forward-all vid -1 pri -1"

sessionx 8100 8100 "$SPLIT0" "$SPLIT5" "$SPLIT6" "$DEF1" "$DEF2" > /tmp/s171-v.txt
run171 ev-vec /tmp/s171-v.txt
got=$(rebuild /tmp/omcid-171-ev-vec.log 0)
check "the operator vector: one VID, priorities 0, 5 and 6, three rules" "$got" \
"47/0ffe gem 4094 rule forward-all vid -1 pri -1
47/1002 gem 657 rule ext-vlan vid 410 pri 0
47/1002 gem 657 rule ext-vlan vid 410 pri 5
47/1002 gem 657 rule ext-vlan vid 410 pri 6
47/1002 gem 657 rule untagged vid -1 pri -1"

sessionx 8100 8100 "$PRIREMAP" "$DEF1" "$DEF2" > /tmp/s171-p.txt
run171 ev-pri /tmp/s171-p.txt
got=$(rebuild /tmp/omcid-171-ev-pri.log 0 | grep ext-vlan)
check "a priority remap on a VID: the filter names both" "$got" "47/1002 gem 657 rule ext-vlan vid 100 pri 3"

sessionx 8100 88a8 "$SPUSH" "$DEF1" "$DEF2" > /tmp/s171-s.txt
run171 ev-spush /tmp/s171-s.txt
got=$(rebuild /tmp/omcid-171-ev-spush.log 0 | grep ext-vlan)
check "an S-tag pushed in front of C-VID 100 (Output TPID 0x88a8)" "$got" "47/1002 gem 657 rule ext-vlan vid 100 pri -1"

sessionx 88a8 8100 "$SPOP" "$DEF1" "$DEF2" > /tmp/s171-o.txt
run171 ev-spop /tmp/s171-o.txt
got=$(rebuild /tmp/omcid-171-ev-spop.log 0 | grep ext-vlan)
check "an S-tag popped (Input TPID 0x88a8): the match is the S-VID" "$got" "47/1002 gem 657 rule ext-vlan vid 200 pri -1"

sessionx 8100 88a8 "$UNTAG_S" "$DEF1" "$DEF2" > /tmp/s171-u.txt
run171 ev-untag-s /tmp/s171-u.txt
got=$(rebuild /tmp/omcid-171-ev-untag-s.log 0)
check "untagged frames get an S-tag: the entry is the untagged rule, the pass-through is gone" "$got" \
"47/0ffe gem 4094 rule forward-all vid -1 pri -1
47/1002 gem 657 rule ext-vlan vid -1 pri -1"

sessionx 8100 8100 "$POPANY" "$DEF1" "$DEF2" > /tmp/s171-pa.txt
run171 ev-popany /tmp/s171-pa.txt
got=$(rebuild /tmp/omcid-171-ev-popany.log 0 | grep ext-vlan)
check "every single tag removed: applied upstream (no rule comes back down), matching any VID" "$got" \
"47/1002 gem 657 rule ext-vlan vid -1 pri -1"

# One entry the classifier cannot carry out stops the whole table, and the log
# says which one and why.
sessionx 8100 8100 "$TRANS410" "$ADD200ET" "$DEF1" > /tmp/s171-bad.txt
run171 ev-bad /tmp/s171-bad.txt
got=$(rebuild /tmp/omcid-171-ev-bad.log 0)
check "an entry with an EtherType filter: the table is not applied" "$got" "$TRANSPARENT"
got=$(grep -c 'ext vlan 171/0001 entry 1: OLT rule not applied (an EtherType filter), transparent' /tmp/omcid-171-ev-bad.log)
check "and the log names entry and reason, once" "$got" "1"

# A line on the manual tag never reads the table: the rules are the same with
# a translation in it.
store 1 1
sessionx 8100 8100 "$TRANS410" "$DEF1" "$DEF2" > /tmp/s171-m.txt
run171 ev-man /tmp/s171-m.txt
got=$(rebuild /tmp/omcid-171-ev-man.log 0)
check "manual tag on (type 1, mode 1): the table is not read" "$got" \
"47/0ffe gem 4094 rule manual-remove vid 10 pri 0
47/1002 gem 657 rule manual-add vid 10 pri 0"
store 0 0

# ---- priorities split over two GEM ports -------------------------------------
#
# A mapper (class 130) that sends priorities 0-3 to one GEM port and 4-7 to
# another, on a bridge port of TPType 3. An entry that keeps the
# priority of the frame has to become a rule per priority, each on the port that carries it.
split_frames() {
	frame 44 010c 0003 "03e8800003800700000008"     # GEM CTP: port 1000
	frame 44 010a 1003 "00030510020000000200"       # GEM IW TP -> CTP 3, the mapper 0x1002
	frame 48 0082 1002 "07801003100310031003"       # priorities 4-7 to IW TP 0x1003
}
{
	sessionx 8100 8100 "$TRANS410" "$DEF1" "$DEF2"
	split_frames
} > /tmp/s171-split.txt
run171 ev-split /tmp/s171-split.txt
got=$(rebuild /tmp/omcid-171-ev-split.log 0 | grep ext-vlan)
check "a priority-keeping rule on a split mapper: one rule per priority, on its GEM port" "$got" \
"47/1002 gem 1000 rule ext-vlan vid 410 pri 4
47/1002 gem 1000 rule ext-vlan vid 410 pri 5
47/1002 gem 1000 rule ext-vlan vid 410 pri 6
47/1002 gem 1000 rule ext-vlan vid 410 pri 7
47/1002 gem 657 rule ext-vlan vid 410 pri 0
47/1002 gem 657 rule ext-vlan vid 410 pri 1
47/1002 gem 657 rule ext-vlan vid 410 pri 2
47/1002 gem 657 rule ext-vlan vid 410 pri 3"
{
	sessionx 8100 8100 "$SPLIT5" "$DEF1" "$DEF2"
	split_frames
} > /tmp/s171-split5.txt
run171 ev-split5 /tmp/s171-split5.txt
got=$(rebuild /tmp/omcid-171-ev-split5.log 0 | grep ext-vlan)
check "a rule that names priority 5 goes up the GEM port that carries 5, and only that" "$got" \
"47/1002 gem 1000 rule ext-vlan vid 410 pri 5"

# ---- a bridge port of TP type 5 (GEM IW TP, no mapper) ----------------------
#
# Some OLTs attach the data GEM to the bridge directly: class 47 TPType 5,
# TPPointer the GEM IW TP. omcid used to walk only TPType 3 (mapper) and 6
# (multicast), so such a line got no service at all. The ISP2 session plus a
# synthetic GEM (port 1000) on its bridge (0x0010) through a type-5 port, and
# a second type-5 port on an IW TP with interworking option 6 (downstream
# broadcast), which stays the broadcast flow and gets no connection.
tp5() {
	session none
	tp5_frames
}
tp5_frames() {
	frame 44 010c 0050 "03e8800003800700000008"   # GEM CTP: port 1000, T-CONT 0x8000
	frame 44 010a 0050 "0050010010"                 # GEM IW TP -> CTP 0x50, MAC bridged LAN
	frame 44 002f 0060 "0010020500500014"           # bridge port, TPType 5 -> IW TP 0x50
	frame 44 010c 0051 "0ffd000002"                 # GEM CTP: port 4093, downstream only
	frame 44 010a 0051 "0051060010"                 # GEM IW TP -> CTP 0x51, broadcast
	frame 44 002f 0061 "0010030500510014"           # bridge port, TPType 5 -> IW TP 0x51
}
tp5 > /tmp/s-tp5.txt
store 1 0
run171 tp5-trans /tmp/s-tp5.txt
got=$(rebuild /tmp/omcid-171-tp5-trans.log 0 | grep -e '47/0060' -e '47/0061')
check "TPType 5: the GEM behind it gets a connection" "$got" "47/0060 gem 1000 rule forward-all vid -1 pri -1"
store 1 1
run171 tp5-man /tmp/s-tp5.txt
got=$(rebuild /tmp/omcid-171-tp5-man.log 0 | grep -e '47/0060' -e '47/0061')
check "TPType 5, manual tag: the GEM behind it carries VID 10" "$got" "47/0060 gem 1000 rule manual-add vid 10 pri 0"

# The same port with a class 84 list of VID 300 (FwdOp 0x10: tagged frames
# by VID, untagged forwarded) and the OLT's class 171 adding VID 300 to
# untagged frames. That table leaves tagged frames on the G.988 default rule
# (forward as they are), so a router that already tags VID 300 still passes:
# both rules, the add-tag one first. The manual tag still replaces the entry.
{
	session "f8000000f8000000000f000300000966" "$DEF1" "$DEF2"
	tp5_frames
	frame 44 0054 0060 "012c$(printf '%044d' 0)1001"   # class 84: VID 300, FwdOp 0x10
} > /tmp/s-tp5-84.txt
store 0 0
run171 tp5-olt /tmp/s-tp5-84.txt
got=$(rebuild /tmp/omcid-171-tp5-olt.log 0 | grep '47/0060')
check "TPType 5, OLT tag VID 300 and class 84 VID 300: untagged gets the tag, tagged passes" "$got" \
"47/0060 gem 1000 rule manual-add vid 300 pri 0
47/0060 gem 1000 rule vid-filter vid 300 pri -1"
store 1 1
sed 's/"VLAN_MANU_TAG_VID" Value="[0-9]*"/"VLAN_MANU_TAG_VID" Value="300"/' /var/config/lastgood.xml > /tmp/lg300.xml
cp /tmp/lg300.xml /var/config/lastgood.xml
run171 tp5-man84 /tmp/s-tp5-84.txt
got=$(rebuild /tmp/omcid-171-tp5-man84.log 0 | grep '47/0060')
check "TPType 5, manual tag VID 300 and class 84 VID 300: the add-tag rule only" "$got" \
"47/0060 gem 1000 rule manual-add vid 300 pri 0"

# ---- VLAN_MANU_TAG_PRI 255 is a priority no tag carries, not "no tag" ----------
#
# A factory store holds VID 30 / priority 255 and the stock stack tags with
# that VID (the priority past 7 is taken as 0). Reading 255 as "no manual tag"
# (v1.4.2 to v1.8.0) left a six-flow line whose OLT wants untagged frames
# tagged with no service for them: DHCP never left the stick.
sed 's/"VLAN_MANU_TAG_PRI" Value="[0-9]*"/"VLAN_MANU_TAG_PRI" Value="255"/' \
	"$FIX/omci-store-isp2.xml" > /var/config/lastgood.xml
grep -q '"VLAN_MANU_TAG_PRI" Value="255"' /var/config/lastgood.xml || { echo "FAIL  the isp2 store has no VLAN_MANU_TAG_PRI"; fail=$((fail + 1)); }
run171 pri255 /tmp/s-tp5.txt
got=$(rebuild /tmp/omcid-171-pri255.log 0 | grep '47/0060')
check "VLAN_MANU_TAG_PRI 255: the manual tag is applied, priority 0" "$got" \
	"47/0060 gem 1000 rule manual-add vid 10 pri 0"

# The same store on the whole six-flow session (isp3, priority 255 in its store):
# the untagged add-tag rule lands on the GEM whose class 84 names the VID, the
# others keep their VID filters.
got=$(rules isp3 manual)
check "ISP3 (six flows), VID 30 priority 255: VID 30 is the add-tag rule" "$got" \
"47/0003 gem 269 rule vid-filter vid 10 pri -1
47/0004 gem 397 rule vid-filter vid 20 pri -1
47/0005 gem 525 rule manual-add vid 30 pri 0
47/0005 gem 525 rule vid-filter vid 32 pri -1
47/0006 gem 653 rule vid-filter vid 40 pri -1
47/0007 gem 781 rule vid-filter vid 50 pri -1
47/0008 gem 909 rule vid-filter vid 60 pri -1
47/ffff gem 4095 rule manual-remove vid 30 pri 0"

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
suite_exit
