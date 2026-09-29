#!/bin/sh
# Exercise omcid without a stick.
#
# qemu-user cannot open our netlink protocol (NETLINK_ODI; the stock firmware
# used NETLINK_USERSOCK) -- it translates only NETLINK_ROUTE and NETLINK_AUDIT
# and answers EPROTONOSUPPORT for anything else, whatever is or is not behind
# it. So there is no line side here, and that is the point:
# omcid serves its two System V message queues regardless, which is everything
# except the hardware calls.
#
# What this covers: the injected-frame path (a frame arriving on the vendor
# queue with msgType 0), the MIB store, the 45-command vendor protocol, and
# the vendor-format dump renderers. Expected outputs are captures from a live
# stick (for example the instance-0x03 block of `omcicli mib get 84`).
#
# Run from src/omci, inside the toolchain image:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh qemu-test.sh
set -u
Q=qemu-mips-static
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

$Q respond/build/omcid -w 3 > /tmp/omcid.log 2>&1 &
sleep 2

# A Create for VlanTagFilterData instance 3, carrying one filter entry for
# VID 10 -- the same VID the captured stick had at that instance, so the
# rendered output can be compared literally.
FRAME=0001440a00540003000a00000000000000000000000000000000000000000000100100000000000000000028c007c78a

$Q cli/build/omcli --inject "$FRAME" > /dev/null 2>&1
sleep 1

got=$(grep -c 'injected frame, 48 bytes' /tmp/omcid.log)
check "the injected frame arrives" "$got" "1"

got=$(grep -c '<- create .*class 84 ' /tmp/omcid.log)
check "it parses as a create for class 84" "$got" "1"

# Byte 2 of the answer is 0x24: AK set, message type 4. Anything else is the
# wrong acknowledgement.
got=$(grep -o '^-> 0001240a' /tmp/omcid.log | head -1)
check "the answer is acknowledged as a create" "$got" "-> 0001240a"

got=$(grep -c 'created (1 rows held)' /tmp/omcid.log)
check "the row is stored" "$got" "1"

# And back out through the vendor protocol, in the vendor format.
want='=================================
EntityID: 0x03
FilterTbl[0]: PRI 0,CFI 0, VID 10
FwdOp:  0x10
NumOfEntries: 1
================================='
got=$($Q cli/build/omcli mib get 84 --vendor 2>&1 | sed -n '4,9p')
check "the dump matches the vendor byte for byte" "$got" "$want"

# The same flag before the command must not be read as the group.
got=$($Q cli/build/omcli --vendor mib get 84 2>&1 | sed -n '5p')
check "a leading flag is not read as the group" "$got" "EntityID: 0x03"

# `get tables` deadlocks the stock omci_app (MIB_ShowAll locks the table mutex
# twice, so its only message-loop thread never returns and the ONU stops
# answering the line). omcid has no mutex and answers it. The client refuses
# it unless its probe of the native queue finds omcid, or -f is given; here
# omcid is listening, so it goes through without -f.
got=$($Q cli/build/omcli get tables --vendor 2>&1 | sed -n '1p')
check "get tables goes through when omcid is the one listening" \
      "$got" "TableId [0] Name: ExtendedMcastOperProf!"
got=$($Q cli/build/omcli -f get tables --vendor 2>&1 | grep -c '^TableId \[')
check "with -f it lists every registered table" "$got" "81"
got=$($Q cli/build/omcli -f get tables --vendor 2>&1 | sed -n '2p')
check "in the vendor's own format" "$got" "TableId [1] Name: OntData!"
# And the daemon is still alive afterwards.
got=$($Q cli/build/omcli mib get 84 --vendor 2>&1 | sed -n '5p')
check "and the daemon still answers after it" "$got" "EntityID: 0x03"
# `get sn` is a vendor-table command: with omcid on both queues it takes the
# vendor path and answers in the vendor shape (no -s, so the serial is empty).
got=$($Q cli/build/omcli get sn 2>&1 | grep -c '^SerialNumber: ')
check "get sn takes the vendor queue and its shape" "$got" "1"
got=$($Q cli/build/omcli get devmode 2>&1)
check "get devmode answers as the vendor does" "$got" "DevMode: bridge"

# Entities the ONU creates for itself (omci_autonomous[]) have no store row
# until the OLT sets one of their attributes, and most never are. The dump
# used to walk the rows alone, so `mib get 7` (SWImage) and `mib get 262`
# (T-CONT) answered "0 rows" on a provisioned stick while a MIB upload listed
# every one of them. Nothing here has set either class.
got=$($Q cli/build/omcli mib get 7 --vendor 2>&1 | tail -1)
check "an autonomous class is dumped: both SWImage instances" "$got" "2 rows"
got=$($Q cli/build/omcli mib get 7 --vendor 2>&1 | sed -n '1p')
check "in the same block as an OLT-created entity" "$got" "7 SWImage 0"
# attr_value, not the zeroed row: instance 0 is the active image.
got=$($Q cli/build/omcli mib get 7 0 --vendor 2>&1 | grep '^    Active ')
check "with the values a Get returns, not zeros" "$got" "    Active                   01"
got=$($Q cli/build/omcli mib 262 2>&1 | grep -c '^262 Tcont ')
check "omcli mib lists every autonomous T-CONT" "$got" "16"
got=$($Q cli/build/omcli mib getattr 7 0 3 2>&1)
check "mib getattr reads an autonomous entity" "$got" "class 7 entity 0 attr 3 = 01"
# One shape for every outcome: a vendor-rendered class with instances, one
# with none, and a name that is no class all end with the row count.
got=$($Q cli/build/omcli mib get 256 --vendor 2>&1 | tail -1)
check "a vendor-rendered autonomous class (ONU-G) ends with its count" "$got" "1 row"
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1)
check "a vendor-rendered class with no instance says 0 rows" "$got" "0 rows"
got=$($Q cli/build/omcli mib get NoSuchTable --vendor 2>&1)
check "a name that is no class is refused, not read as every class" "$got" \
      "no managed entity called NoSuchTable
0 rows"

# The rest needs the capability blob (port map, flow and T-CONT counts), so
# restart with one captured from a real stick.
kill %1 2>/dev/null
wait 2>/dev/null
sleep 1

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

$Q respond/build/omcid -w 9 -c "$CAPS" > /tmp/omcid2.log 2>&1 &
sleep 2

got=$($Q cli/build/omcli caps 2>&1 | sed -n '4p')
check "the capability blob decodes as the stick's does" \
      "$got" "pon port / cpu port     : 2 / 3"

# A short reply exercises the tail of cli_end: out_flush() pushes the text into
# cli_used, which must survive until the end marker is built from it.
got=$($Q cli/build/omcli help 2>&1 | sed -n '1p')
check "a short reply is not dropped" "$got" "omcid commands"

# T-CONT, then a Set to give it an Alloc-ID, then the GEM CTP that points at
# it -- entity 0x0002 of ISP1, port 1562, upstream queue 0x8006.
for f in 0001440a010680000000000000000000000000000000000000000000000000000000000000000000000000283056d14e \
         0002480a010680008000040000000000000000000000000000000000000000000000000000000000000000288068e530 \
         0003440a010c0002061a80000380060000000000000000000000000000000000000000000000000000000028063a1fc9; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
# The upstream flow is programmed by the deferred rebuild, OLT_QUIET_MS after
# the last frame.
sleep 2

got=$($Q cli/build/omcli flows 2>&1 | sed -n '3p')
check "both flow directions are allocated" "$got" "     0  gem 1562  gem 1562 "

# UsTraffMgmtPtr is a class-277 ME id, not the queue number accepted by the
# driver. The sole queue is programmed and the flow uses normalized ordinal 0.
got=$(grep -o 'us priq [0-9]* ordinal 0 tcont 0' /tmp/omcid2.log | head -1 | \
      sed 's/us priq [0-9]*/us priq ME/')
check "the upstream queue is normalized within its T-CONT" \
      "$got" "us priq ME ordinal 0 tcont 0"
got=$(grep -o 'us flow [0-9]* tcont me [0-9]* index 0 queue 0' /tmp/omcid2.log | \
      head -1 | sed 's/us flow [0-9]*/us flow N/; s/tcont me [0-9]*/tcont me ME/')
check "the upstream flow uses the normalized queue ordinal" \
      "$got" "us flow N tcont me ME index 0 queue 0"

# The DsPriQPtr of that CTP is 0 and no class 277 row exists, so the downstream
# words come from the DERIVED RelatedPort: queue 0 is priority 7 of its block,
# and ds_prio reverses that against queues-per-UNI (8 - 1 - 7 = 0).
got=$(grep -o 'ds flow 0 priq 7 pri 0 wrr 0 weight 1 port 0' /tmp/omcid2.log | head -1)
check "an auto-created queue is derived, not stored" \
      "$got" "ds flow 0 priq 7 pri 0 wrr 0 weight 1 port 0"

# `port 0` is the assertion that matters: queue 0 belongs to the FIRST UNI, and
# the PPTP Ethernet UNI lives in omci_autonomous[], not the MIB store (the OLT
# never creates it). A lookup in the store alone falls back to the PON port and
# reads `port 2`.
got=$(grep -c 'ds flow 0 .* port 2' /tmp/omcid2.log)
check "the UNI resolves, so the PON-port fallback does NOT fire" "$got" "0"

# The bridge descriptor, built and printed rather than sent. These 160 bytes
# either make the stick forward or stop it.
out=$($Q cli/build/omcli bridge 0x101 1562 3 2>&1)

# Matched by their offset label rather than by line number, which moves when
# a line is added above.
row() { echo "$out" | grep -F "   $1  " | head -1; }

check "uni_mask is the UNI's switch port" \
      "$(row 000)" "   000  00000001000000000000000000000001"
check "direction and rule_gen" \
      "$(row 020)" "   020  00000003000000000000000000000000"
check "both tag filters are NO_CARE and outer_act is TRANSPARENT" \
      "$(row 040)" "   040  00000001000000010000000000000004"
# The ignore sentinels are NOT zero.
check "out_tag carries the ignore sentinels, not zeroes" \
      "$(row 090)" "   090  00000000000000080000100000000000"
check "with no config store the rule stays transparent" \
      "$(echo "$out" | grep -c 'rule: transparent')" "1"

# A VEIP ingress is the PON port, 2 on this blob, so uni_mask is 1<<2. The VEIP
# is created by the ONU and lives in omci_autonomous[], so the MIB store alone
# never finds it; (0x601-1)&0xff and (0x101-1)&0xff are both 0, so a fall-through
# to the PPTP lookup would still give a plausible port here.
out=$($Q cli/build/omcli bridge 0x601 1562 3 transparent 2>&1)
rowv() { echo "$out" | grep -F "   $1  " | head -1; }
# 5, not 4: this is the SECOND ingress on the same GEM, and bdgconn_add ORs it
# into the service the Ethernet UNI created. 5 is the UNIMASK that
# `omcicli dump srvflow` reports on every service of ISP1.
check "a VEIP ingress adds the PON port to the service the UNI already made" \
      "$(rowv 000)" "   000  00000001000000000000000000000005"

# The rule a stick in manual VLAN mode runs. The service tag is NOT in class
# 171 (every treatment field there is the 4096 "no VID" sentinel) but
# VLAN_MANU_TAG_VID in the config store: 10 on ISP2, 11 on ISP1. Every field
# below is pinned by `omcicli dump conn` on those sticks.
out=$($Q cli/build/omcli bridge 0x101 1562 3 10 2>&1)
row() { echo "$out" | grep -F "   $1  " | head -1; }

check "an explicit vid selects the extended-VLAN rule" \
      "$(echo "$out" | grep -c 'rule: c-tag add vid 10 pri 0')" "1"
check "rule_gen is EXTENDED VLAN, not forward-all" \
      "$(row 020)" "   020  00000003000000050000000000000000"
check "both tag filters are NO TAG and the S-TAG stays transparent" \
      "$(row 040)" "   040  00000004000000040000000000000004"
check "the C-TAG action is ADD" \
      "$(row 060)" "   060  00000000000000010000000000000000"
check "and it assigns vid 10 with tpid copy-from-inner" \
      "$(row 070)" "   070  000000000000000a0000000000000000"
check "the out-style carries one tag at the same vid" \
      "$(row 090)" "   090  00000000000000000000000a00000000"

# Downstream matches the OEM multicast rule: C-TAG VID+PBIT zero, remove both
# tags, set is_mcast, and emit no output tag.
# GEM 1562 again: the broadcast port is not created until much later in
# this script, and bdgconn_add refuses a flow it has never allocated.
out=$($Q cli/build/omcli bridge 0x101 1562 2 10 2>&1)
check "downstream removes the tag instead of adding it" \
      "$(echo "$out" | grep -c 'rule: c-tag remove vid 10')" "1"
check "the multicast filter and S-TAG removal match OEM" \
      "$(row 040)" "   040  00000004000000180000000000000002"
check "the C-TAG action is REMOVE" \
      "$(row 060)" "   060  00000000000000020000000000000000"
check "is_mcast is set with no output tag" \
      "$(row 080)" "   080  00000001000000000000000000000000"
check "the multicast out-style is all zero as OEM reports" \
      "$(row 090)" "   090  00000000000000000000000000000000"

# `transparent` asks for the transparent rule by name.
out=$($Q cli/build/omcli bridge 0x101 1562 3 transparent 2>&1)
check "transparent is still reachable by name" \
      "$(echo "$out" | grep -c 'rule: transparent')" "1"

# A multicast GEM IW TP points through class 281 to a downstream-only GEM CTP.
# The rebuild must keep Direction=2: GEM 4095 has no upstream flow, so a
# bidirectional connection silently drops that service. The frames reproduce
# the ISP1 graph: CTP 5 / GEM 4095, class 281/1, an Ethernet UNI bridge port,
# and type-6 bridge port ffff.
for f in 009e440a010a0002000200000000000000000000000000000000000000000000000000000000000000000028d967f77d \
         009f440a002f0002000102030002000000000000000000000000000000000000000000000000000000000028e0d29244 \
         00a0440a010c00050fff00000200000000000000000000000000000000000000000000000000000000000028c8e941e1 \
         00a1440a01190001000500000000000000000000000000000000000000000000000000000000000000000028e389a109 \
         00a2440a002f0001000101010101000000000000000000000000000000000000000000000000000000000028c9b690de \
         00a3440a002fffff0001ff06000100000000000101000000000000000000000000000000000000000000002849e60ded; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
sleep 2
got=$($Q cli/build/omcli flows 2>&1 | grep 'gem 4095')
check "class 281 designates the downstream-only broadcast flow" \
      "$got" "     1  -         gem 4095 "
got=$($Q cli/build/omcli conn 2>&1 | grep 'DIR=2, USFID=0, DSFID=1' | \
      sed 's/^SERVID [0-9]*:/SERVID N:/; s/SERVID=[0-9]*/SERVID=N/')
check "the class-281 rebuild preserves downstream-only direction" \
      "$got" "SERVID N: Used: 1, (DIR=2, USFID=0, DSFID=1, SERVID=N, UNIMASK=1)"
got=$($Q cli/build/omcli conn 2>&1 | \
      grep -E 'DIR=3, USFID=0, DSFID=0.*UNIMASK=1|DIR=2, USFID=0, DSFID=1.*UNIMASK=1' | \
      sed 's/^SERVID [0-9]*:/SERVID N:/; s/SERVID=[0-9]*/SERVID=N/')
check "ordinary services are installed before the broad multicast rule" \
      "$got" "SERVID N: Used: 1, (DIR=3, USFID=0, DSFID=0, SERVID=N, UNIMASK=1)
SERVID N: Used: 1, (DIR=2, USFID=0, DSFID=1, SERVID=N, UNIMASK=1)"

# ISP2 (FHTT OLT): bridge 0x10 with the VEIP as port 1 and, as port 2, TPType 3
# pointing at an 802.1p MAPPER (0x1002) whose eight p-bits all name GEM IW TP
# 0x1001 -> CTP 2 / GEM 657. ISP1 points TPType 3 at the IW TP itself; both
# shapes must yield the unicast service. The frames are those the ISP2 OLT sent.
for f in 00c0440a002d00100001010000280008001e000000000000000000000000000000000000000000000000000000000000 \
         00c1440a002f00050010010b0601001400c8000100000000000000000000000000000000000000000000000000000000 \
         00c2440a00821002ffffffffffffffffffffffffffffffff000000000000000000000000000000000000000000000000 \
         00c3480a008210027f801001100110011001100110011001100100000000000000000000000000000000000000000000 \
         00c4440a002f1002001002031002001400c8000100000000000000000000000000000000000000000000000000000000 \
         00c5480a010680018001015c000000000000000000000000000000000000000000000000000000000000000000000000 \
         00c6440a010c002202918001038009000000080000000000000000000000000000000000000000000000000000000000 \
         00c7440a010a100100220510020000000200000000000000000000000000000000000000000000000000000000000000; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
done
sleep 3
got=$(grep -c 'serv [0-9]* ingress 0601 gem 657 dir 3' /tmp/omcid2.log)
check "a mapper-typed bridge port reaches its GEM through the mapper" "$got" "1"
# Bridge 0x1 (the earlier fixtures) has a UNI of its own; it must NOT be wired to
# a GEM that hangs off bridge 0x10.
got=$(grep -c 'serv [0-9]* ingress 0101 gem 657' /tmp/omcid2.log)
check "an ingress of another bridge is not connected to it" "$got" "0"

# Leave the daemon in the state the rest of this sequential test expects.
for f in 00a4460a002fffff000000000000000000000000000000000000000000000000000000000000000000000028818c255d \
         00a5460a002f00020000000000000000000000000000000000000000000000000000000000000000000000287606f78d \
         00a6460a01190001000000000000000000000000000000000000000000000000000000000000000000000028cc6203d2 \
         00a7460a010c000500000000000000000000000000000000000000000000000000000000000000000000002816526ae7 \
         00a8460a010a000200000000000000000000000000000000000000000000000000000000000000000000002873249473; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done

# The four config-store keys, read from a fixture.
got=$($Q cli/build/omcli vlan test/cfg_agree.xml 2>&1 | sed -n '1p')
check "manual vlan mode is read from the store" "$got" "vlan mode   1 (manual tagging on)"
got=$($Q cli/build/omcli vlan test/cfg_agree.xml 2>&1 | sed -n '3p')
check "and so is the vid" "$got" "vlan vid    10"
got=$($Q cli/build/omcli vlan test/cfg_agree.xml 2>&1 | sed -n '5p')
check "type 1 and mode 1 with a vid and a pri apply the manual tag" \
      "$got" "manual tag  applied: vid 10 pri 0"

# The manual tag is gated the way /etc/runomci.sh gates it: type 1, mode 1,
# and a VID and a priority both present. Anything else is no manual tag, and
# the connection builder and `omcli bridge` read the same gate.
got=$($Q cli/build/omcli vlan test/cfg_vlan_type0.xml 2>&1 | sed -n '5p')
check "VLAN_CFG_TYPE 0 turns the manual tag off, whatever the vid" \
      "$got" "manual tag  not applied (needs type 1, mode 1, a vid and a pri)"
got=$($Q cli/build/omcli bridge 0x101 1562 3 2>&1 | grep -c 'rule: transparent')
check "and a connection built under it carries no tag" "$got" "1"
got=$($Q cli/build/omcli vlan test/cfg_vlan_nopri.xml 2>&1 | sed -n '5p')
check "an empty priority turns it off too, as on the vendor stack" \
      "$got" "manual tag  not applied (needs type 1, mode 1, a vid and a pri)"
got=$($Q cli/build/omcli vlan test/cfg_agree.xml 2>&1 | sed -n '5p')
got2=$($Q cli/build/omcli bridge 0x101 1562 3 2>&1 | grep -o 'rule: c-tag add vid 10 pri 0')
check "back on 1/1 the same connection carries the tag again" \
      "$got / $got2" "manual tag  applied: vid 10 pri 0 / rule: c-tag add vid 10 pri 0"

# ------------------------------------------------------- priority queues
#
# Six words of the downstream GEM flow come from the class 277 row the CTP
# points at, and driver command 23 is built from the same three attributes.
# ISP1 provisions no queues, so replaying ISP1 alone never runs either path.
#
# Priority queue 1 (bit 15 clear, so downstream): RelatedPort names PPTP
# Ethernet UNI 0x0101 -- switch port 0 in the capability blob above -- at
# priority 3; weight 5, drop-precedence marking 2. Then a downstream-only GEM
# CTP pointing at it.
for f in 000c480a01150001050101010003050200000000000000000000000000000000000000000000000000000028ae8b933b \
         000d440a010c00040640000002000000000001000000000000000000000000000000000000000000000000281d152d55; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done

# The queue is programmed before the flow that uses it, and dp/weight/wrr all
# come from the row rather than from the pointer.
got=$(grep -o 'priq 1 port 0 pri 3 weight 5 wrr 1 dp 2' /tmp/omcid2.log | head -1)
check "driver command 23 is built from the class 277 row" \
      "$got" "priq 1 port 0 pri 3 weight 5 wrr 1 dp 2"

# ds_pq_pri is the LOW half of RelatedPort (3), not the pointer (1).
# ds_prio is reversed against queues-per-UNI count: 8 - 1 - 3 = 4.
# wrr is (weight >= 2).
got=$(grep -o 'ds flow [0-9]* priq 3 pri 4 wrr 1 weight 5 port 0' /tmp/omcid2.log | \
      sed 's/ds flow [0-9]*/ds flow N/')
check "the flow's six downstream words come from the queue" \
      "$got" "ds flow N priq 3 pri 4 wrr 1 weight 5 port 0"

# Driver command 23 goes out on the class 277 Set, as the stock PriQDrvCfg
# sends it, NOT once per downstream flow. Two downstream flows and one queue
# exist by now, so there is exactly one.
got=$(grep -c '\] priq ' /tmp/omcid2.log)
check "setPriQueue is sent per queue, not per flow" "$got" "1"

# General upstream normalization: explicitly Set a second SP queue on the same
# T-CONT, then create a second GEM CTP that references it. Both queues have
# priority 0, so their ME IDs break the tie and produce ordinals 0 and 1.
$Q cli/build/omcli --inject "0025480a011580070501800000000100$(printf '%064d' 0)" \
    > /dev/null 2>&1
$Q cli/build/omcli --inject "0026440a010c0003059a80000380070000$(printf '%062d' 0)" \
    > /dev/null 2>&1
sleep 3
got=$(grep 'tcont me 8000.*queues 2' /tmp/omcid2.log | tail -1 | \
      sed 's/.*tcont me/tcont me/; s/ ->.*//')
check "the T-CONT reserves the complete active queue set" \
      "$got" "tcont me 8000 alloc 1024 queues 2"
got=$(grep 'us priq 32775 ordinal 1 tcont 0' /tmp/omcid2.log | tail -1 | \
      sed 's/.*us priq/us priq/; s/ weight.*//')
check "SP queues receive dense priority-ordered ordinals" \
      "$got" "us priq 32775 ordinal 1 tcont 0"
got=$(grep 'gem 1434 us flow.*queue 1' /tmp/omcid2.log | tail -1 | \
      sed 's/.*gem/gem/; s/ ->.*//; s/us flow [0-9]*/us flow N/')
check "a second GEM binds to its normalized queue" \
      "$got" "gem 1434 us flow N tcont me 32768 index 0 queue 1"

# ISP1 Sets several class-277 queues per T-CONT that no GEM CTP references;
# counting them exhausts the shared 32-queue pool and hands the fifth T-CONT
# the invalid index 8. A third SP queue referenced by nothing must NOT grow
# the reservation.
$Q cli/build/omcli --inject "0027480a011580080501800000000100$(printf '%064d' 0)" \
    > /dev/null 2>&1
sleep 3
got=$(grep 'tcont me 8000.*queues' /tmp/omcid2.log | tail -1 | \
      sed 's/.*tcont me/tcont me/; s/ ->.*//')
check "an unreferenced Set queue does not join the reservation" \
      "$got" "tcont me 8000 alloc 1024 queues 2"
got=$(grep -c 'us priq 32776 ' /tmp/omcid2.log)
check "and it is never programmed as an upstream queue" "$got" "0"

# --------------------------------------------- what a MIB upload reports
#
# All 208 priority queues and 16 schedulers are in omci_autonomous[]; with
# every attribute zero an OLT would see 208 identical queues, so attr_value
# answers both the upload and a plain Get from the derivation. The replies
# are checked against the ISP1 MIB dump of classes 277 and 278: queue 0x8006
# carries RelatedPort 0x80000001, scheduler 0x8003 TcontPtr 0x8003, Policy 2.
for f in 0020490a0115800604000000000000000000000000000000000000000000000000000000000000000000002800f8706a \
         0021490a01168003a00000000000000000000000000000000000000000000000000000000000000000000028ac80e49a; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done

got=$(grep -o -- '-> 0020290a0115800600040080000001' /tmp/omcid2.log | head -1)
check "a Get of priority queue 0x8006 answers isp1's RelatedPort" \
      "$got" "-> 0020290a0115800600040080000001"
got=$(grep -o -- '-> 0021290a0116800300a00080030200' /tmp/omcid2.log | head -1)
check "a Get of scheduler 0x8003 answers its T-CONT and policy" \
      "$got" "-> 0021290a0116800300a00080030200"

# ------------------------------------------------------- Ethernet UNI controls
# Auto full-duplex abilities (0x04), downstream PHY loopback (3), and raw pause
# time 0x1234 in one Set. The capability fixture maps EthUni 0x0101 to port 0.
$Q cli/build/omcli --inject "0030480a000b0101304004031234$(printf '%068d' 0)" \
    > /dev/null 2>&1
sleep 1
got=$(grep -o 'uni 0101 port 0 auto 4 ability 54000000' /tmp/omcid2.log | tail -1)
check "auto-negotiation uses the deployed plugin's ability encoding" \
      "$got" "uni 0101 port 0 auto 4 ability 54000000"
got=$(grep -o 'uni 0101 port 0 loopback 1' /tmp/omcid2.log | tail -1)
check "OMCI loopback value 3 enables PHY loopback" \
      "$got" "uni 0101 port 0 loopback 1"
got=$(grep -o 'uni 0101 port 0 pause 4660' /tmp/omcid2.log | tail -1)
check "pause time is passed raw to the driver ABI" \
      "$got" "uni 0101 port 0 pause 4660"

# ---------------------------------------------------------------- class 171
#
# The extended-VLAN table is the one attribute that accumulates, and its rules
# are not guessable: an entry is keyed by its filter half, a repeat replaces,
# an all-ones treatment half deletes, and new entries go on the FRONT of the
# list. All four are asserted below.
#
# The three entries are instance 0x01 of ISP1, set in reverse so that walking
# the list reproduces its INDEX 0/1/2 exactly. A fourth set then deletes the
# middle one.
for f in 0004440a00ab0001020101000000000000000000000000000000000000000000000000000000000000000028b94121e4 \
         0005480a00ab000178000000810081000000000000000000000000000000000000000000000000000000002879236fdb \
         0006480a00ab00010400f8000000e8000000c00f0000000f00000000000000000000000000000000000000288e40fcf7 \
         0007480a00ab00010400e8000000e8000000c00f0000000f0000000000000000000000000000000000000028eff20302 \
         0008480a00ab00010400f8000000f8000000000f80000000000c000000000000000000000000000000000028eb2af4d6; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done

# Byte for byte against the stock `omcicli mib get 171` dump of ISP1, instance 0x01.
want='=================================
EntityId: 0x01
AssociationType: 2
ReceivedFrameVlanTagOperTableMaxSize: 0
InputTPID: 0x8100
OutputTPID: 0x8100
DsMode: 0
ReceivedFrameVlanTaggingOperTable
INDEX 0
Filter Outer   : PRI 15,VID 4096, TPID 0
Filter Inner   : PRI 15,VID 4096, TPID 0, EthType 0x00
Treatment Outer   : PRI 15,VID 4096, TPID 0, RemoveTags 0
Treatment Inner   : PRI 0,VID 1, TPID 4
INDEX 1
Filter Outer   : PRI 14,VID 4096, TPID 0
Filter Inner   : PRI 14,VID 4096, TPID 0, EthType 0x00
Treatment Outer   : PRI 15,VID 0, TPID 0, RemoveTags 3
Treatment Inner   : PRI 15,VID 0, TPID 0
INDEX 2
Filter Outer   : PRI 15,VID 4096, TPID 0
Filter Inner   : PRI 14,VID 4096, TPID 0, EthType 0x00
Treatment Outer   : PRI 15,VID 0, TPID 0, RemoveTags 3
Treatment Inner   : PRI 15,VID 0, TPID 0
AssociatedMePoint: 0x101
DscpToPbitMapping:
	0x000000
	0x000000
	0x000000
	0x000000
	0x000000
	0x000000
	0x000000
	0x000000
================================='
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1 | sed -n '4,37p')
check "class 171 matches the captured dump byte for byte" "$got" "$want"

# The same table in words, in OUR dump only; the vendor dump above stays byte
# for byte.
want='       0  untagged frames: add VLAN 1 pri 0 tpid 0x8100
       1  double-tagged frames (the default rule): DISCARD
       2  single-tagged frames (the default rule): DISCARD'
got=$($Q cli/build/omcli mib 171 2>&1 | sed -n '/^    tagging$/,$p' | sed 1d | head -3)
check "class 171 explains itself in our own dump" "$got" "$want"

# Setting the same key again replaces rather than appending: the entry count
# stays at three and the treatment of INDEX 1 changes.
$Q cli/build/omcli --inject 000a480a00ab00010400e8000000e8000000800f0008000f00000000000000000000000000000000000000281d137c84 \
   > /dev/null 2>&1
sleep 1
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1 | grep -c '^INDEX ')
check "a repeated key replaces, it does not append" "$got" "3"
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1 | grep 'RemoveTags 2')
check "the replaced entry carries the new treatment" \
      "$got" "Treatment Outer   : PRI 15,VID 1, TPID 0, RemoveTags 2"

# An all-ones treatment half is the delete sentinel. RemoveTags 3 alone is not:
# INDEX 2 has it and must survive.
$Q cli/build/omcli --inject 000b480a00ab00010400e8000000e8000000ffffffffffffffff0000000000000000000000000000000000281a80122e \
   > /dev/null 2>&1
sleep 1
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1 | grep -c '^INDEX ')
check "an all-ones treatment deletes the keyed entry" "$got" "2"
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1 | grep -c 'RemoveTags 3')
check "an ordinary RemoveTags 3 entry survives it" "$got" "1"

# ------------------------------------------------ table Get and Get-Next
#
# Two entries are left, so the table is 32 bytes and reads back in two chunks:
# 29, then 3. Both numbers match the stock omci_app: `dataType == OCTETS ||
# len >= 26` selects the path (OMCI_OnGetMsg 0x413abc), 29 is the baseline
# chunk (the responder at 0x413400).
for f in 0020490a00ab0001040000000000000000000000000000000000000000000000000000000000000000000028689f21b0 \
         00215a0a00ab000104000000000000000000000000000000000000000000000000000000000000000000002878879781 \
         00225a0a00ab0001040000010000000000000000000000000000000000000000000000000000000000000028e5c37636 \
         00235a0a00ab0001040000020000000000000000000000000000000000000000000000000000000000000028f2808ba9 \
         00245a0a00ab0002040000000000000000000000000000000000000000000000000000000000000000000028d9326d55; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done

# The table is the two entries that survived the delete, newest first -- the
# same 32 bytes the dump above renders. Chunk 0 carries 29 of them and chunk 1
# the remaining 3, which is len - 29*seq as the vendor sends, not a padded chunk.
got=$(grep -o -- '-> 0020290a00ab00010004000000002000' /tmp/omcid2.log | head -1)
check "a Get of the table answers its byte length, not an entry" \
      "$got" "-> 0020290a00ab00010004000000002000"
got=$(grep -o -- '-> 00213a0a00ab0001000400f8000000f8000000000f80000000000cf8000000e8000000c00f000000000000' /tmp/omcid2.log | head -1)
check "Get-Next chunk 0 is 29 bytes off the front of the table" \
      "$got" "-> 00213a0a00ab0001000400f8000000f8000000000f80000000000cf8000000e8000000c00f000000000000"
got=$(grep -o -- '-> 00223a0a00ab00010004000f000000000000' /tmp/omcid2.log | head -1)
check "the last chunk is the remainder, not a padded 29" \
      "$got" "-> 00223a0a00ab00010004000f000000000000"
got=$(grep -o -- '-> 00233a0a00ab000103000000' /tmp/omcid2.log | head -1)
check "a sequence past the last chunk is refused" \
      "$got" "-> 00233a0a00ab000103000000"
got=$(grep -o -- '-> 00243a0a00ab000203000000' /tmp/omcid2.log | head -1)
check "a Get-Next the Get did not prime is refused, not answered" \
      "$got" "-> 00243a0a00ab000203000000"

# ------------------------------------------------ class 171's one driver call
#
# The VLAN rules reach the driver inside the bridge connection, but the
# DSCP-to-P-bit map goes straight off a Set: the 24-byte attribute, three bits
# per code point MSB first, unpacked into 64 P-bits as command 52 (stock
# ExtVlanTagOperCfgDataDrvCfg). An all-zero map is refused, so ISP1 never runs it.
#
# The map is pbit = dscp & 7, a pattern that is obviously wrong if reversed.
for f in 0030480a00ab00010100053977053977053977053977053977053977053977053977000000000000000000284e01d043 \
         0031480a00ab00020100000000000000000000000000000000000000000000000000000000000000000000281399d8d8; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o 'dscp remap 0 1 2 3 ... 7' /tmp/omcid2.log | head -1)
check "the DSCP map unpacks three bits per code point, MSB first" \
      "$got" "dscp remap 0 1 2 3 ... 7"
got=$(grep -c 'dscp remap' /tmp/omcid2.log)
check "an all-zero map is not programmed at all" "$got" "1"

# ------------------------------------------------------------------- alarms
#
# Get All Alarms answers a two-byte count of the Next commands to follow; no
# alarm is raised here, so it is zero. With an empty snapshot every sequence
# number is past the end, and the stock answer out of range is a failure result,
# not a zero-filled record (OMCI_OnGetAllAlarmsNext, 0x412c58).
for f in 00404b0a0100000000000000000000000000000000000000000000000000000000000000000000000000002886f0fc16 \
         00414c0a01000000000000000000000000000000000000000000000000000000000000000000000000000028408fa5d3; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o -- '-> 00402b0a010000000000' /tmp/omcid2.log | head -1)
check "get-all-alarms answers a two-byte count" "$got" "-> 00402b0a010000000000"
got=$(grep -o -- '-> 00412c0a010000000300' /tmp/omcid2.log | head -1)
check "get-all-alarms-next past an empty snapshot is a failure, not a record" \
      "$got" "-> 00412c0a010000000300"

# ------------------------------------------------------ identity from the store
#
# The config store is two XML files on jffs2, split the way the stock xmlconfig
# splits them: LOID, LOID_OLD and LOID_PASSWD are service config,
# LOID_PASSWD_OLD is hardware identity. The fixtures carry invented values.
#
# /etc/runomci.sh lets LOID_OLD win whenever the two differ, so setting LOID
# while LOID_OLD is empty means the vendor passes no -l at all.
got=$($Q cli/build/omcli ident test/cfg_agree.xml test/cfg_agree_hs.xml 2>&1 | sed -n '1p')
check "the serial comes out of the hs file" "$got" "serial      ODIX00000001"
got=$($Q cli/build/omcli ident test/cfg_agree.xml test/cfg_agree_hs.xml 2>&1 | sed -n '2p')
check "the ploam password is reported, never printed" \
      "$got" "ploam       set, 10 bytes (withheld)"
got=$($Q cli/build/omcli ident test/cfg_agree.xml test/cfg_agree_hs.xml 2>&1 | sed -n '3p')
check "LOID is used when it agrees with LOID_OLD" "$got" "loid        user-one"
got=$($Q cli/build/omcli ident test/cfg_differ.xml test/cfg_differ_hs.xml 2>&1 | sed -n '3p')
check "an empty LOID_OLD beats a set LOID, so nothing is sent" \
      "$got" "loid        (none -- nothing would be sent)"
got=$($Q cli/build/omcli ident test/cfg_differ.xml test/cfg_differ_hs.xml 2>&1 | sed -n '4p')
check "and the same rule empties the password" "$got" "loid pw     (none)"
got=$($Q cli/build/omcli ident test/cfg_agree.xml test/cfg_agree_hs.xml 2>&1 | sed -n '4p')
check "a password that agrees across the two files survives" \
      "$got" "loid pw     set (withheld)"

# ISP1 keeps GPON_PLOAM_PASSWD and LOID_PASSWD_OLD in the cs file, not where
# the xmlconfig split puts them, so each key is read from whichever file
# carries it.
got=$($Q cli/build/omcli ident test/cfg_csonly.xml test/cfg_csonly_hs.xml 2>&1 | sed -n '2p')
check "a ploam password kept in cs is found" "$got" "ploam       set, 10 bytes (withheld)"
got=$($Q cli/build/omcli ident test/cfg_csonly.xml test/cfg_csonly_hs.xml 2>&1 | sed -n '4p')
check "and so is a LOID_PASSWD_OLD kept in cs" "$got" "loid pw     set (withheld)"
got=$($Q cli/build/omcli ident test/cfg_csonly.xml test/cfg_csonly_hs.xml 2>&1 | sed -n '1p')
check "the serial still comes out of hs" "$got" "serial      ODIX00000002"

# ------------------------------------------------ what the OLT is told
#
# Five keys can override what the software-image (7) and ONU2-G (257) entities
# answer. An empty or absent key keeps the default answer, and so does a set
# key while /var/config/omci-identity.on is absent: both test sticks store the
# vendor values in all five, so honouring them by default would change what
# both OLTs see.
#
# Get frames: class 7 instance 0 and 1, attribute 1 (mask 8000); class 257
# instance 0, attributes 1-3 (mask e000). Under qemu the driver has no device
# id, so the equipment id answers twenty zero bytes -- the baseline to compare.
get_frame() {   # get_frame <tci> <class> <inst> <mask>
	printf '%s490a%s%s%s%060d00000028%08d' "$1" "$2" "$3" "$4" 0 0
}
ask() {   # ask <tci> <class> <inst> <mask> -> the answer, as logged
	$Q cli/build/omcli --inject "$(get_frame "$1" "$2" "$3" "$4")" > /dev/null 2>&1
	sleep 1
	grep -o -- "-> $1290a$2$3" /tmp/omcid2.log > /dev/null &&
		grep -- "-> $1290a$2$3" /tmp/omcid2.log | tail -1 | cut -c4-
}
Z9=000000000000000000        # 9 bytes
Z13=00000000000000000000000000
Z20=0000000000000000000000000000000000000000
V000=302e302e30$Z9           # "0.0.0", 14 bytes
mkdir -p /var/config
rm -f /var/config/omci-identity.on

got=$(ask 0e10 0007 0000 8000 | cut -c1-50)
check "no keys: software image 0 answers 0.0.0" "$got" "0e10290a00070000008000$V000"
got=$(ask 0e11 0101 0000 e000 | cut -c1-68)
check "no keys: ONU2-G answers the device id, 0x80 and product code 15" \
      "$got" "0e11290a0101000000e000${Z20}80000f"

$Q cli/build/omcli ident test/cfg_report.xml test/cfg_report_hs.xml > /dev/null 2>&1
got=$(ask 0e12 0007 0000 8000 | cut -c1-50)
check "keys set, switch off: the software version does not change" \
      "$got" "0e12290a00070000008000$V000"
got=$(ask 0e13 0101 0000 e000 | cut -c1-68)
check "keys set, switch off: nor does ONU2-G" "$got" "0e13290a0101000000e000${Z20}80000f"
got=$($Q cli/build/omcli ident test/cfg_report.xml test/cfg_report_hs.xml 2>&1 | sed -n '5p')
check "omcli ident says the keys are stored and not reported" \
      "$got" "report      off -- the keys below are stored, not reported (/var/config/omci-identity.on)"

: > /var/config/omci-identity.on
$Q cli/build/omcli ident test/cfg_report.xml test/cfg_report_hs.xml > /dev/null 2>&1
got=$(ask 0e14 0007 0000 8000 | cut -c1-50)
check "switch on: OMCI_SW_VER1 is software image 0" \
      "$got" "0e14290a000700000080004f44492d544553542d4100000000"
got=$(ask 0e15 0007 0001 8000 | cut -c1-50)
check "and OMCI_SW_VER2 is software image 1" \
      "$got" "0e15290a000700010080004f44492d544553542d4200000000"
got=$(ask 0e16 0101 0000 e000 | cut -c1-68)
check "GPON_ONU_MODEL, OMCC_VER and the product code reach ONU2-G" \
      "$got" "0e16290a0101000000e0004f444954455354${Z13}a01234"

$Q cli/build/omcli ident test/cfg_report_empty.xml test/cfg_report_empty_hs.xml > /dev/null 2>&1
got=$(ask 0e17 0007 0000 8000 | cut -c1-50)
check "switch on, keys empty: the software version stays 0.0.0" \
      "$got" "0e17290a00070000008000$V000"
got=$(ask 0e18 0101 0000 e000 | cut -c1-68)
check "switch on, keys empty: ONU2-G stays as it was" "$got" "0e18290a0101000000e000${Z20}80000f"
rm -f /var/config/omci-identity.on
$Q cli/build/omcli ident test/cfg_agree.xml test/cfg_agree_hs.xml > /dev/null 2>&1

# ------------------------------------------------------------------- class 45
#
# The stock MacBriServProfDrvCfg has four arms. A Set carrying
# DynamicFilteringAgeingTime sends setAgeingTime, and a zero attribute means
# the DEFAULT, 300 (a `li v0,300` then a movz that objdump prints as
# `.word 0x0044200a`). The delete arm sends setAgeingTime(300) and
# setPortBridging(1), both constants.
for f in 0060480a002d00010040000004d200000000000000000000000000000000000000000000000000000000002871e3b48f \
         0061480a002d000100400000000000000000000000000000000000000000000000000000000000000000002848f9300f \
         0062460a002d0001000000000000000000000000000000000000000000000000000000000000000000000028a511151a; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o 'ageing time 1234' /tmp/omcid2.log | head -1)
check "class 45 sends the ageing time it was given" "$got" "ageing time 1234"
got=$(grep -o 'ageing time 300' /tmp/omcid2.log | head -1)
check "a zero ageing time means the default, not zero" "$got" "ageing time 300"
got=$(grep -o '\[45/0001\] delete: ageing 300, port bridging 1' /tmp/omcid2.log | head -1)
check "and a delete resets both, which nothing used to do" \
      "$got" "[45/0001] delete: ageing 300, port bridging 1"

# ------------------------------------------------------------------ class 131
#
# The stock OltGDrvCfg (176 bytes) does one thing: on a Set carrying ToDInfo it
# hands fourteen bytes to command 70. That is how an OLT distributes time of
# day, so it arrives unsolicited on a working line.
#
# In the VENDOR row ToDInfo is at 44 (the NUL of each string attribute is kept:
# EquipId 21, Version 15); in OURS at 40, as attr_offset packs at wire widths.
# The first four bytes surviving the round trip catch a fence-post in either.
$Q cli/build/omcli --inject 0050480a008300001000deadbeef111111111111111111110000000000000000000000000000000000000028bd607c90 > /dev/null 2>&1
sleep 1
got=$(grep -o 'tod info deadbeef ...' /tmp/omcid2.log | head -1)
check "class 131 ToDInfo reaches the driver path at the right row offset" \
      "$got" "tod info deadbeef ..."

# --------------------------------------------- the config store, escaped and written
#
# xmlconfig escapes the five XML entities; a reader that hands back the raw
# attribute is silently wrong on the two password fields.
got=$($Q cli/build/omcli ident test/cfg_esc.xml test/cfg_esc_hs.xml 2>&1 | sed -n '3p')
check "the reader decodes every XML entity" "$got" 'loid        a&b<c>d"e'"'"'f'

# Writing: one attribute in place, never a whole-file rewrite. Work on a copy,
# because a test that edits its fixture passes once.
cp test/cfg_agree.xml /tmp/w.xml
cp test/cfg_agree_hs.xml /tmp/w_hs.xml
$Q cli/build/omcli cfgset /tmp/w.xml MIB_TABLE LOID "new&name" > /dev/null 2>&1
$Q cli/build/omcli cfgset /tmp/w.xml MIB_TABLE LOID_OLD "new&name" > /dev/null 2>&1
got=$($Q cli/build/omcli ident /tmp/w.xml /tmp/w_hs.xml 2>&1 | sed -n '3p')
check "a written value round-trips through the reader" "$got" "loid        new&name"
got=$(grep -c 'Value="new&amp;name"' /tmp/w.xml)
check "and is escaped on disk, not stored raw" "$got" "2"
got=$(grep -c 'DEVICE_TYPE' /tmp/w.xml)
check "a key the writer does not touch survives" "$got" "1"

# An absent key is inserted into the named Dir rather than refused.
got=$($Q cli/build/omcli cfgset /tmp/w.xml MIB_TABLE BRAND_NEW_KEY hello 2>&1 | sed -n '1p')
check "an absent key is inserted" "$got" "ok"
got=$(grep -c 'Name="BRAND_NEW_KEY" Value="hello"' /tmp/w.xml)
check "the inserted element is well formed" "$got" "1"
got=$($Q cli/build/omcli ident /tmp/w.xml /tmp/w_hs.xml 2>&1 | sed -n '1p')
check "the file still parses after an insert" "$got" "serial      ODIX00000001"

# A Dir that is not in the file is refused, not created: a key written to the
# wrong file is a duplicate the vendor reader never sees.
got=$($Q cli/build/omcli cfgset /tmp/w.xml HW_MIB_TABLE GPON_SN nope 2>&1 | sed -n '1p')
check "a Dir the file does not have is refused" "$got" "write failed"

# ------------------------------------------------------------------- class 47
#
# The part of the stock MacBriPortCfgDataDrvCfg (4888 bytes) that reaches
# driver commands we have: MAC learning limit (53), flooding port mask (64),
# UNI rate limiter (67) and broadcast GEM flow (26).
#
# The learning limit falls back from NumOfAllowedMac of the port to
# MacLearningDepth of the bridge service profile, then to a global default NOT
# decoded; so class 45 gets depth 5 and class 47 leaves its attribute at zero.
# On a UNI port direction 1 is the INBOUND descriptor: traffic entering the
# bridge from a UNI is upstream.
#
# PIRs land on round numbers: (PIR << 3) >> 10, so 131072 B/s is 1024 kbit/s
# and 65536 is 512; a reversed or missing shift gives no plausible rate.
for f in 0070480a002d000201800005000000000000000000000000000000000000000000000000000000000000002835d250bf \
         0072440a0118800100000000000200000000000000000000000000000000000000000000000000000000002886306c0b \
         0071440a01188002000000000001000000000000000000000000000000000000000000000000000000000028291f2f05 \
         0073440a002f0001000201010101000000000000000000000000000000000000000000000000000000000028206c2e82 \
         0074480a002f00010030800180020000000000000000000000000000000000000000000000000000000000286fd85c08 \
         0075460a002f0001000000000000000000000000000000000000000000000000000000000000000000000028e5daea9b; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o '\[dry 47/0001\] mac learn limit port 0 = 5 -> 0' /tmp/omcid2.log | head -1)
check "the learning limit falls back to the bridge service profile" \
      "$got" "[dry 47/0001] mac learn limit port 0 = 5 -> 0"
got=$(grep -o '\[dry 47/0001\] flooding mask 1 enable 1 -> 0' /tmp/omcid2.log | head -1)
check "a create floods, because DiscardUnknow is not 1" \
      "$got" "[dry 47/0001] flooding mask 1 enable 1 -> 0"
got=$(grep -o 'uni port 0 dir 1 rate 512 kbit/s (td 8002)' /tmp/omcid2.log | head -1)
check "direction 1 takes the INBOUND traffic descriptor" \
      "$got" "uni port 0 dir 1 rate 512 kbit/s (td 8002)"
got=$(grep -o 'uni port 0 dir 2 rate 1024 kbit/s (td 8001)' /tmp/omcid2.log | head -1)
check "direction 2 takes the outbound one, at its own PIR" \
      "$got" "uni port 0 dir 2 rate 1024 kbit/s (td 8001)"
got=$(grep -c 'uni port 0 dir . rate' /tmp/omcid2.log)
check "a create sends no rate at all, only a set does" "$got" "4"
got=$(grep -o '\[dry 47/0001\] flooding mask 1 enable 0 -> 0' /tmp/omcid2.log | head -1)
check "the delete arm clears the flooding bit" \
      "$got" "[dry 47/0001] flooding mask 1 enable 0 -> 0"
got=$(grep -c 'uni port 0 dir . rate 1048568 kbit/s (td ffff)' /tmp/omcid2.log)
check "and puts both rate limiters back to no limit" "$got" "2"

# The broadcast GEM flow, both halves. A class 266 with interworking option 6
# names the downstream broadcast port; a class 47 whose TP points at it makes
# the flow programmable, and DELETING the last such bridge port withdraws it
# (the vendor walks every other class 47 row, then hands the driver 0xffffffff).
for f in 0076440a010a0002000206000000000000000000000000000000000000000000000000000000000000000028e3f012e3 \
         0077440a002f000200020205000200000000000000000000000000000000000000000000000000000000002810d35593 \
         0078460a002f00020000000000000000000000000000000000000000000000000000000000000000000000282831881a; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o 'broadcast gem 1562 is ds flow 0' /tmp/omcid2.log | head -1)
check "a bridge port onto a broadcast GEM IW TP names the flow" \
      "$got" "broadcast gem 1562 is ds flow 0"
got=$(grep -o 'last broadcast bridge port -> withdraw ds flow 0' /tmp/omcid2.log | head -1)
check "deleting the last one withdraws it" \
      "$got" "last broadcast bridge port -> withdraw ds flow 0"

# ------------------------------------------------------------------ class 298
#
# Dot1 rate limiter: a parent bridge, a TP type and three traffic descriptor
# pointers (upstream unicast flood, broadcast, multicast payload), each
# programming a driver slot. Commands 59 and 60 are the same twenty bytes,
# { slot, portMask, kind, CIR, CBS }; the slot number is ours, like a flow id.
#
# The earlier block deleted both class 47 rows, so a fresh bridge port is
# created first. The third pointer starts at zero and is filled by a Set:
# a pointer resolving to nothing is a delete, and a later Set allocates a slot.
for f in 0080440a002f00030002010101010000000000000000000000000000000000000000000000000000000000286698193b \
         0081440a01188003000003e800000000000007d000000000000000000000000000000000000000000000002870be87de \
         0082440a0118800400000bb80000000000000fa00000000000000000000000000000000000000000000000284c26e2e8 \
         0083440a012a0001000201800380040000000000000000000000000000000000000000000000000000000028a2c54adc; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o '\[dry 298/0001\] set slot 0 mask 1 kind 0 cir 1000 cbs 2000 -> 0' /tmp/omcid2.log | head -1)
check "the unicast flood rate takes slot 0, with its own CIR and CBS" \
      "$got" "[dry 298/0001] set slot 0 mask 1 kind 0 cir 1000 cbs 2000 -> 0"
got=$(grep -o '\[dry 298/0001\] set slot 1 mask 1 kind 1 cir 3000 cbs 4000 -> 0' /tmp/omcid2.log | head -1)
check "the broadcast rate takes the next slot and the other descriptor" \
      "$got" "[dry 298/0001] set slot 1 mask 1 kind 1 cir 3000 cbs 4000 -> 0"
got=$(grep -c '298/0001. \(set\|del\) slot' /tmp/omcid2.log)
check "a pointer of zero programs nothing at all" "$got" "2"

# A Set that fills the third pointer. The vendor sends only pointers that
# CHANGED; omcid re-sends all three, more than the vendor and never less
# (README.md, "Where this differs from the vendor daemon, and why").
$Q cli/build/omcli --inject 0084480a012a0001080080030000000000000000000000000000000000000000000000000000000000000028bd97df34 > /dev/null 2>&1
sleep 1
got=$(grep -o 'set slot 2 mask 1 kind 2 cir 1000 cbs 2000 -> 0' /tmp/omcid2.log | head -1)
check "a later Set allocates the third slot" \
      "$got" "set slot 2 mask 1 kind 2 cir 1000 cbs 2000 -> 0"

# The two refusals the handler opens with, both logged and neither programmed.
for f in 0085440a012a000200020280030000000000000000000000000000000000000000000000000000000000002835c88250 \
         0086440a012a0003009901800300000000000000000000000000000000000000000000000000000000000028f05d5327; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o '\[298/0002\] rate limiter for 802.1p mapper is not supported' /tmp/omcid2.log | head -1)
check "TP type 2 is refused, as the vendor refuses it" \
      "$got" "[298/0002] rate limiter for 802.1p mapper is not supported"
got=$(grep -o '\[298/0003\] no associated pptp eth uni is found' /tmp/omcid2.log | head -1)
check "a parent bridge with no UNI is refused too" \
      "$got" "[298/0003] no associated pptp eth uni is found"

# And the delete arm: all three slots released, by the slot each set used.
$Q cli/build/omcli --inject 0087460a012a000100000000000000000000000000000000000000000000000000000000000000000000002874274f35 > /dev/null 2>&1
sleep 1
got=$(grep -c 'del slot [012] mask 1 kind [012] cir 0 cbs 0 -> 0' /tmp/omcid2.log)
check "a delete releases all three, naming the slots the sets used" "$got" "3"

# --------------------------------------------------- class 45's other two arms
#
# Both are SET-only and fire only when the value CHANGED against the row as it
# was before the Set (`prev`).
#
# Bridge 2 has exactly one PPTP Ethernet UNI bridge port (47/3, from the class
# 298 block), which the learning arm requires: the command names one port.
for f in 0090480a002d00020100020000000000000000000000000000000000000000000000000000000000000000281dff325d \
         0091480a002d0002010002000000000000000000000000000000000000000000000000000000000000000028f3bc0b57 \
         0092480a002d00020080090000000000000000000000000000000000000000000000000000000000000000289fad2249; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o '\[dry 45/0002\] flooding mask 1 enable 1 -> 0' /tmp/omcid2.log | head -1)
check "a changed DiscardUnknow floods the bridge's whole membership" \
      "$got" "[dry 45/0002] flooding mask 1 enable 1 -> 0"
# 2 is not 1, and the vendor test is (byte != 1), not (!byte).
got=$(grep -c '45/0002. flooding mask' /tmp/omcid2.log)
check "and setting it again to the same value sends nothing" "$got" "1"
got=$(grep -o '\[dry 45/0002\] mac learn limit port 0 = 9 -> 0' /tmp/omcid2.log | head -1)
check "a changed MacLearningDepth sets the single UNI's limit" \
      "$got" "[dry 45/0002] mac learn limit port 0 = 9 -> 0"

# --------------------------------------------------- respawn: reclaiming 0x800
#
# On real hardware inittab respawns omcid after a crash or a manual
# kill -9, and kill -9 skips on_signal()'s cleanup -- the 0x800 queue
# omcicli/dump srvflow/vqsrv serve survives the dead process. Before the
# vq_ensure() fix this made IPC_CREAT|IPC_EXCL fail EEXIST forever and the
# respawned omcid logged "the omcicli queue is still omci_app's" -- wrong,
# since omci_app never runs on this image at all (../README.md): the queue
# was only ever a previous instance of omcid's own.
kill -9 %1 2>/dev/null
wait 2>/dev/null

$Q respond/build/omcid -w 9 -c "$CAPS" > /tmp/omcid3.log 2>&1 &
sleep 2

got=$(grep -c 'still omci_app' /tmp/omcid3.log)
check "a respawned omcid does not treat the stale queue as a stranger's" \
      "$got" "0"
got=$(grep -c 'took the omcicli queue' /tmp/omcid3.log)
check "it reclaims 0x800 instead" "$got" "1"

# The exporter's own command against a respawned omcid, timed: it must not be
# one of the 42 that piled up unanswered when this was broken.
start=$(date +%s)
out=$($Q cli/build/omcli dump srvflow 2>&1)
end=$(date +%s)
got=$([ "$((end - start))" -le 2 ] && echo yes || echo no)
check "dump srvflow answers within 2s of a respawn" "$got" "yes"
# This instance has nothing provisioned (a fresh MIB, no frames injected
# into it), so cli_conn() (show.c) prints no SERVID rows at all -- it
# omits unused ones, unlike the vendor's 256-row dump. "0 services" is
# still the real vendor-format trailer line, not the empty string a
# silently dropped or malformed reply would leave.
got=$(echo "$out" | tail -n 1)
check "and it is the real vendor-format srvflow dump, not silence" "$got" "0 services"

# ------------------------------------------- class 84's forward operation
#
# FwdOp (G.988 table 9.3.11-1) says what the bridge port does with a tagged
# frame (bridge it, discard it, or filter it on VID, priority or the whole
# TCI against the list) and with an untagged one (bridge or discard).
# bp_rules() (respond/apply_bridge.c) turns each code into bridge rules;
# the rebuild logs one "rule" line per rule, which is what is checked here.
# A fresh daemon, no config store (no manual tag): bridge 1 with the UNI
# 0x0101 and a GEM-side port 47/0012 onto GEM IW TP 3, GEM 1000, whose
# class 84 row lists VID 100 priority 0 and VID 200 priority 5.
kill %1 2>/dev/null
wait 2>/dev/null
rm -f /var/config/lastgood.xml /var/config/lastgood_hs.xml
$Q respond/build/omcid -w 30 -c "$CAPS" > /tmp/omcid4.log 2>&1 &
sleep 2

f84() {   # f84 <tci> <mt> <class> <inst> <contents hex>: one frame, zero padded
	c=$5
	while [ ${#c} -lt 64 ]; do c=${c}0; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$c"
}
for f in "$(f84 0101 44 010c 0003 03e8800003800600)" \
         "$(f84 0102 44 010a 0003 00030500010000000200)" \
         "$(f84 0103 44 002f 0011 000101010101)" \
         "$(f84 0104 44 002f 0012 000102030003)" \
         "$(f84 0105 44 0054 0012 0064a0c800000000000000000000000000000000000000001002)"; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
done
sleep 3
# rules_since <line>: the rule lines of 47/0012 logged after that line.
# Only the last rebuild counts: each one prints its rules, then a summary.
rules_since() {
	tail -n +"$(($1 + 1))" /tmp/omcid4.log | awk '
		/47\/0012 gem 1000 rule / { sub(/.*47\/0012 gem 1000 rule /, ""); cur = cur $0 "\n" }
		/bridge connections rebuilt/ { last = cur; cur = "" }
		END { printf "%s", last }'
}
got=$(rules_since 0)
check "FwdOp 0x10 (the mandatory code): one VID filter per list entry" "$got" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1"

fwdop() {   # fwdop <code>: Set FwdOp, wait for the rebuild, print its rules
	n=$(wc -l < /tmp/omcid4.log)
	$Q cli/build/omcli --inject "$(f84 0110 48 0054 0012 "4000$1")" > /dev/null 2>&1
	sleep 2
	rules_since "$n"
}
check "0x04: the same, by its other number" "$(fwdop 04)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1"
check "0x03: the VID filters and untagged frames bridged" "$(fwdop 03)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1
untagged vid -1 pri -1"
got=$(grep 'bridge connections rebuilt' /tmp/omcid4.log | tail -1 | sed 's/.*rebuilt: //')
check "and each of the three is a connection the UNI gets" "$got" "1 ingress x gem = 3"
check "0x00: no investigation at all, one forward-all rule" "$(fwdop 00)" \
      "forward-all vid -1 pri -1"
check "0x01: tagged discarded, untagged bridged" "$(fwdop 01)" \
      "untagged vid -1 pri -1"
check "0x02: tagged bridged without looking, untagged discarded" "$(fwdop 02)" \
      "tagged vid -1 pri -1"
check "0x15: the same as 0x02" "$(fwdop 15)" "tagged vid -1 pri -1"
check "0x12 (the 802.1p mapper code): a priority filter per entry" "$(fwdop 12)" \
      "pri-filter vid -1 pri 0
pri-filter vid -1 pri 5"
check "0x14: VID and priority together, the whole TCI" "$(fwdop 14)" \
      "tci-filter vid 100 pri 0
tci-filter vid 200 pri 5"
check "0x13: the TCI filters and untagged frames" "$(fwdop 13)" \
      "tci-filter vid 100 pri 0
tci-filter vid 200 pri 5
untagged vid -1 pri -1"
check "0x0f and 0x1c repeat 0x03" "$(fwdop 0f) / $(fwdop 1c)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1
untagged vid -1 pri -1 / vid-filter vid 100 pri -1
vid-filter vid 200 pri -1
untagged vid -1 pri -1"

# Negative filtering (g) and positive filtering by TCI and MAC address (j)
# have no bridge rule: built as 0x10, and said once per code.
check "0x06 (negative filtering): built as 0x10" "$(fwdop 06)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1"
got=$(grep '^event=vlan_fwdop' /tmp/omcid4.log)
check "and logged as an event line" "$got" \
      "event=vlan_fwdop code=0x06 inst=18 result=unsupported built_as=0x10"
fwdop 04 > /dev/null
fwdop 06 > /dev/null
got=$(grep -c '^event=vlan_fwdop code=0x06' /tmp/omcid4.log)
check "once per code, however often it is rebuilt" "$got" "1"
check "0x17 (TCI and MAC address): built as 0x10 too" "$(fwdop 17)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1"
got=$(grep -c '^event=vlan_fwdop code=0x17 inst=18 result=unsupported built_as=0x10' /tmp/omcid4.log)
check "with its own line" "$got" "1"
check "past the table (0x22): built as 0x10" "$(fwdop 22)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1"

# The manual tag on (VID 10, priority 0 from the fixture store): an
# untagged-bridging code builds the manual add-tag rule in place of the
# plain untagged one, and a list entry that IS the manual VID becomes that
# rule too, as the stock stack builds it.
$Q cli/build/omcli vlan test/cfg_agree.xml > /dev/null 2>&1
check "0x03 under the manual tag: the add-tag rule takes untagged frames" "$(fwdop 03)" \
      "vid-filter vid 100 pri -1
vid-filter vid 200 pri -1
manual-add vid 10 pri 0"
check "0x00 under the manual tag: add-tag for untagged, tagged unchanged" "$(fwdop 00)" \
      "tagged vid -1 pri -1
manual-add vid 10 pri 0"
$Q cli/build/omcli --inject "$(f84 01f0 48 0054 0012 80000064000a)" > /dev/null 2>&1
sleep 2
check "0x10 with the manual VID in the list: that entry is the add-tag rule" "$(fwdop 10)" \
      "vid-filter vid 100 pri -1
manual-add vid 10 pri 0"

# The daemon log lives inside the container, so keep it when a check fails.
[ "$fail" -eq 0 ] || cp /tmp/omcid4.log /src/src/omci/qemu-test.log 2>/dev/null || true
kill %1 2>/dev/null
wait 2>/dev/null

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
exit "$fail"
