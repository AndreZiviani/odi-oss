#!/bin/sh
# Exercise omcid without a stick.
#
# qemu-user cannot open NETLINK_USERSOCK -- it translates only NETLINK_ROUTE
# and NETLINK_AUDIT and answers EPROTONOSUPPORT for anything else, whatever is
# or is not behind it. So there is no line side here, and that is the point:
# omcid serves its two System V message queues regardless, which is everything
# except the hardware calls.
#
# What this covers: the injected-frame path (a frame arriving on the vendor
# queue with msgType 0), the MIB store, the vendor's 45-command protocol, and
# the vendor-format dump renderers. The expected output is not invented -- it
# is the instance-0x03 block of a real `omcicli mib get 84` captured from a
# live stick.
#
# Run from src/omci, inside the toolchain image:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci odi-diag-toolchain \
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

# And back out through the vendor's own protocol, in the vendor's own format.
want='=================================
EntityID: 0x03
FilterTbl[0]: PRI 0,CFI 0, VID 10
FwdOp:  0x10
NumOfEntries: 1
================================='
got=$($Q cli/build/omcli mib get 84 --vendor 2>&1 | sed -n '4,9p')
check "the dump matches the vendor byte for byte" "$got" "$want"

# The same flag before the command, which used to be read as the group.
got=$($Q cli/build/omcli --vendor mib get 84 2>&1 | sed -n '5p')
check "a leading flag is not read as the group" "$got" "EntityID: 0x03"

# `get tables` is the command that deadlocks the vendor: MIB_ShowAll locks the
# table mutex twice, so omci_app's only message-loop thread never returns and
# the ONU stops answering the line. omcid has no mutex and answers it.
#
# The client still refuses it by default, because it cannot tell which daemon
# owns queue 0x800 -- sending it to a stock stick is a way to take it off the
# air. -f lifts that.
# omcid IS listening here, so the client's probe of its native queue says so
# and the command goes through without -f. The refusal is for the case where
# that probe fails, which means omci_app is what would receive it.
got=$($Q cli/build/omcli get tables --vendor 2>&1 | sed -n '1p')
check "get tables goes through when omcid is the one listening" \
      "$got" "TableId [0] Name: ExtendedMcastOperProf!"
got=$($Q cli/build/omcli -f get tables --vendor 2>&1 | grep -c '^TableId \[')
check "with -f it lists every registered table" "$got" "81"
got=$($Q cli/build/omcli -f get tables --vendor 2>&1 | sed -n '2p')
check "in the vendor's own format" "$got" "TableId [1] Name: OntData!"
# And the daemon is still alive afterwards, which is the whole point.
got=$($Q cli/build/omcli mib get 84 --vendor 2>&1 | sed -n '5p')
check "and the daemon still answers after it" "$got" "EntityID: 0x03"
# `get sn` is a vendor-table command, so with omcid listening on both queues
# it must take the vendor path and answer in the vendor shape (no -s here, so
# the serial itself is empty).
got=$($Q cli/build/omcli get sn 2>&1 | grep -c '^SerialNumber: ')
check "get sn takes the vendor queue and its shape" "$got" "1"
got=$($Q cli/build/omcli get devmode 2>&1)
check "get devmode answers as the vendor does" "$got" "DevMode: bridge"

# The rest needs the capability blob -- port map, flow and T-CONT counts -- so
# restart with one captured from a real stick. Everything downstream of caps is
# otherwise untestable off the device.
kill %1 2>/dev/null
wait 2>/dev/null
sleep 1

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

$Q respond/build/omcid -w 9 -c "$CAPS" > /tmp/omcid2.log 2>&1 &
sleep 2

got=$($Q cli/build/omcli caps 2>&1 | sed -n '4p')
check "the capability blob decodes as the stick's does" \
      "$got" "pon port / cpu port     : 2 / 3"

# A short reply exercises cli_end's tail. This returned an empty success for
# fourteen commits: out_flush() pushes the text into cli_used and the next line
# cleared it before the end marker was built from it.
got=$($Q cli/build/omcli help 2>&1 | sed -n '1p')
check "a short reply is not dropped" "$got" "omcid commands"

# T-CONT, then a Set to give it an Alloc-ID, then the GEM CTP that points at
# it -- isp1's entity 0x0002, port 1562, upstream queue 0x8006.
for f in 0001440a010680000000000000000000000000000000000000000000000000000000000000000000000000283056d14e \
         0002480a010680008000040000000000000000000000000000000000000000000000000000000000000000288068e530 \
         0003440a010c0002061a80000380060000000000000000000000000000000000000000000000000000000028063a1fc9; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
# The upstream flow is programmed by the deferred rebuild, one quiet second
# after the last frame; the inject above ate part of that second.
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

# That CTP's DsPriQPtr is 0, and nothing has provisioned a class 277 row, so
# the downstream words come from the DERIVED RelatedPort: queue 0 is the
# highest priority of its block, 7, and ds_prio reverses that against
# queues-per-UNI count (8 - 1 - 7 = 0). No UNI entity exists in the MIB yet, so the
# related entity does not resolve and the port falls back to the PON port --
# which is the vendor's own path when the lookup fails, not an accident here.
got=$(grep -o 'ds flow 0 priq 7 pri 0 wrr 0 weight 1 port 0' /tmp/omcid2.log | head -1)
check "an auto-created queue is derived, not stored" \
      "$got" "ds flow 0 priq 7 pri 0 wrr 0 weight 1 port 0"

# `port 0` is the assertion that matters, and the one the first version of this
# test got wrong. Queue 0 belongs to the FIRST UNI, and the PPTP Ethernet UNI
# lives in omci_autonomous[] rather than in the MIB store -- the OLT never
# creates it. Looking only in the store returned 0 for the related entity, the
# port fell back to the PON port (2), and the old assertion checked for 2 and
# so agreed with the bug. A regression here reads `port 2` again.
got=$(grep -c 'ds flow 0 .* port 2' /tmp/omcid2.log)
check "the UNI resolves, so the PON-port fallback does NOT fire" "$got" "0"

# The bridge descriptor, built and printed rather than sent. These 160 bytes
# either make the stick forward or stop it; being able to read one without
# sending it is the whole point.
out=$($Q cli/build/omcli bridge 0x101 1562 3 2>&1)

# Matched by their offset label rather than by line number. The line numbers
# were right until a line was added above them, which is the whole argument.
row() { echo "$out" | grep -F "   $1  " | head -1; }

check "uni_mask is the UNI's switch port" \
      "$(row 000)" "   000  00000001000000000000000000000001"
check "direction and rule_gen" \
      "$(row 020)" "   020  00000003000000000000000000000000"
check "both tag filters are NO_CARE and outer_act is TRANSPARENT" \
      "$(row 040)" "   040  00000001000000010000000000000004"
# The sentinels that are NOT zero, which is the whole trap this rule exists for.
check "out_tag carries the ignore sentinels, not zeroes" \
      "$(row 090)" "   090  00000000000000080000100000000000"
check "with no config store the rule stays transparent" \
      "$(echo "$out" | grep -c 'rule: transparent')" "1"

# A VEIP ingress is the PON port, which on this blob is 2, so uni_mask is 1<<2.
# The guard used to be mib_find(329,...) alone, and a VEIP is created by the ONU
# rather than the OLT -- it lives in omci_autonomous[] and the store never sees
# it -- so the branch never fired and the call fell through to the PPTP lookup.
# That gave a working port here only because (0x601-1)&0xff and (0x101-1)&0xff
# are both 0: a collision, not a rule.
#
# The mapping is the vendor's, measured: isp1 reports UNIMASK=5 on every
# service, reachable only as the VEIP's 1<<2 ORed with the Ethernet UNI's 1<<0.
out=$($Q cli/build/omcli bridge 0x601 1562 3 transparent 2>&1)
rowv() { echo "$out" | grep -F "   $1  " | head -1; }
# And the answer is 5, not 4, because this is the SECOND ingress on the same
# GEM: bdgconn_add finds the service the Ethernet UNI already created and ORs
# the new port in rather than allocating another. So this one assertion covers
# both the VEIP mapping and the accumulation -- and 5 is precisely the UNIMASK
# isp1's own `omcicli dump srvflow` reports on every service.
check "a VEIP ingress adds the PON port to the service the UNI already made" \
      "$(rowv 000)" "   000  00000001000000000000000000000005"

# The rule a stick in manual VLAN mode actually runs. The service tag is NOT in
# class 171 -- every treatment field there carries the 4096 "no VID" sentinel --
# it is VLAN_MANU_TAG_VID in the config store: 10 on isp2, 11 on isp1, each
# matching its own `omcicli dump conn`. Every field below is pinned by that dump.
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

# `transparent` asks for the old rule by name, so the previous behaviour is
# still reachable and still asserted.
out=$($Q cli/build/omcli bridge 0x101 1562 3 transparent 2>&1)
check "transparent is still reachable by name" \
      "$(echo "$out" | grep -c 'rule: transparent')" "1"

# A multicast GEM IW TP points through class 281 to a downstream-only GEM
# CTP. The rebuild used to read Direction=2 and then discard it, forcing every
# connection to bidirectional; GEM 4095 has no upstream flow, so that silently
# omitted the sixth service. These injected frames reproduce ISP1's graph:
# CTP 5 / GEM 4095, class 281/1, an Ethernet UNI bridge port, and type-6 bridge
# port ffff. The quiet-period rebuild must retain the CTP direction.
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

# ISP2 (FHTT OLT, 2026-09-21): bridge 0x10 with the VEIP as port 1 and, as
# port 2, TPType 3 pointing at an 802.1p MAPPER (0x1002) whose eight p-bits
# all name GEM IW TP 0x1001 -> CTP 2 / GEM 657. ISP1 points TPType 3 at the
# IW TP itself, and until p61 the rebuild only followed that shape, so isp2
# came up at O5 with the multicast connection and no unicast service. The
# frames are the ones isp2's OLT sent, from the boot 1 omcid log.
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
# Bridge 0x1 (the earlier fixtures) has its own UNI; it must NOT be wired to
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
# Neither path ran at all before: isp1 provisions no queues, so a test that
# only replays isp1 proves nothing here.
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
# wrr is (weight >= 2), which is where the hard-coded 48 used to sit.
got=$(grep -o 'ds flow [0-9]* priq 3 pri 4 wrr 1 weight 5 port 0' /tmp/omcid2.log | \
      sed 's/ds flow [0-9]*/ds flow N/')
check "the flow's six downstream words come from the queue" \
      "$got" "ds flow N priq 3 pri 4 wrr 1 weight 5 port 0"

# Driver command 23 goes out on the class 277 Set, which is where PriQDrvCfg
# sends it -- NOT once per downstream flow, which is what the first version of
# this did. Two downstream flows have been created by now and exactly one
# queue provisioned, so there is exactly one of these.
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

# Boot 46 (p46): ISP1 Sets several class-277 queues per T-CONT that no GEM
# CTP references. Including them exhausted the shared 32-queue pool and handed
# the fifth T-CONT the invalid index 8. A Set of a third SP queue on the same
# T-CONT, referenced by nothing, must therefore NOT grow the reservation.
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
# All 208 priority queues and all 16 schedulers are in omci_autonomous[], so a
# MIB upload has always listed them -- with every attribute zero, which tells
# an OLT that this ONU has 208 identical queues. attr_value now answers both
# the upload and a plain Get from the derivation.
#
# Both replies below are checked against isp1's own rows: queue 0x8006 really
# does carry RelatedPort 0x80000001, and scheduler 0x8003 really does carry
# TcontPtr 0x8003 and Policy 2. See ~/tmp/odi/ref/isp1-mib-27{7,8}.txt.
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
# The three entries are isp1's instance 0x01, set in reverse so that walking
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

# Byte for byte against ~/tmp/odi/ref/vendor-mib-171.txt, instance 0x01.
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
got=$($Q cli/build/omcli mib get 171 --vendor 2>&1 | sed -n '4,38p')
check "class 171 matches the captured dump byte for byte" "$got" "$want"

# The same table in words, in OUR dump only -- the vendor one above is asserted
# byte for byte and must stay that way. "PRI 15, VID 4096" is not something an
# operator can read; this is the same three entries saying what they do.
want='       0  untagged frames: add VLAN 1 pri 0 tpid 0x8100
       1  double-tagged frames (the default rule): DISCARD
       2  single-tagged frames (the default rule): DISCARD'
got=$($Q cli/build/omcli mib 171 2>&1 | sed -n '/^    tagging$/,$p' | sed 1d | head -3)
check "class 171 explains itself in our own dump" "$got" "$want"

# Setting the same key again replaces rather than appending: the entry count
# stays at three and INDEX 1's treatment changes.
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
# Two entries are left in the list above, so the table is 32 bytes and reads
# back in two chunks: 29 and then 3. Every number here is the vendor's --
# `dataType == OCTETS || len >= 26` selects the path (OMCI_OnGetMsg 0x413abc)
# and 29 is the baseline chunk (the responder at 0x413400). Before this, a Get
# of attribute 6 answered with 16 bytes of the last entry written and a
# Get-Next was refused outright as "command not supported".
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
# the remaining 3, which is the vendor's len - 29*seq and not a padded chunk.
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
# The VLAN rules do not reach the driver from this class -- they travel inside
# the bridge connection -- but the DSCP-to-P-bit map does, straight off a Set.
# ExtVlanTagOperCfgDataDrvCfg's tail takes the 24-byte attribute, three bits per
# code point MSB first, and sends 64 unpacked P-bits as command 52. It refuses
# an all-zero map, which is why isp1 has never run this path.
#
# The map here is pbit = dscp & 7, so the first eight P-bits count 0..7 and the
# last is 7. Anything that read the bits in the wrong order would still produce
# eight plausible small numbers, which is the argument for a pattern that is
# wrong in an obvious way if reversed.
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
# Get All Alarms answers a two-byte count of the Next commands to follow, and
# nothing here raises an alarm, so the count is zero. Get All Alarms Next was
# falling through to "not supported yet" -- the wrong answer to a question this
# ME understands. With an empty snapshot every sequence number is past the end,
# and the vendor's own out-of-range answer is a failure result rather than a
# zero-filled record (OMCI_OnGetAllAlarmsNext, 0x412c58).
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
# The config store is two XML files on jffs2, and which key is in which is
# xmlconfig's own split, not the obvious one: LOID, LOID_OLD and LOID_PASSWD are
# service config while LOID_PASSWD_OLD is hardware identity. The fixtures under
# test/ carry invented values -- real ones are private by design.
#
# /etc/runomci.sh resolves LOID against LOID_OLD by letting the OLD one win
# whenever the two differ, which is surprising in one direction that matters:
# setting LOID while LOID_OLD is empty means the vendor passes no -l at all.
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

# isp1 keeps GPON_PLOAM_PASSWD and LOID_PASSWD_OLD in the cs file, not where
# xmlconfig's split puts them. Looking in hs alone reported the PLOAM password
# missing and emptied the LOID password; each key is now read from whichever
# file carries it.
got=$($Q cli/build/omcli ident test/cfg_csonly.xml test/cfg_csonly_hs.xml 2>&1 | sed -n '2p')
check "a ploam password kept in cs is found" "$got" "ploam       set, 10 bytes (withheld)"
got=$($Q cli/build/omcli ident test/cfg_csonly.xml test/cfg_csonly_hs.xml 2>&1 | sed -n '4p')
check "and so is a LOID_PASSWD_OLD kept in cs" "$got" "loid pw     set (withheld)"
got=$($Q cli/build/omcli ident test/cfg_csonly.xml test/cfg_csonly_hs.xml 2>&1 | sed -n '1p')
check "the serial still comes out of hs" "$got" "serial      ODIX00000002"

# ------------------------------------------------ what the OLT is told
#
# Five keys can override what the software-image (7) and ONU2-G (257) entities
# answer. An empty or absent key keeps today's answer, and so does a set key
# while /var/config/omci-identity.on is absent: both our sticks already store
# the vendor's values in all five, so honouring them by default would change
# what both OLTs see.
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
# MacBriServProfDrvCfg has four arms; this is the one needing nothing we lack.
# A Set carrying DynamicFilteringAgeingTime sends setAgeingTime -- and a zero
# attribute means the DEFAULT, 300, not zero. That is a `li v0,300` followed by
# a movz which objdump prints as `.word 0x0044200a`, so it is easy to miss and
# worth an assertion of its own.
#
# The delete arm sends setAgeingTime(300) and setPortBridging(1), both
# constants. Before this, a delete reached the driver from no class at all, and
# a Set of attribute 3 wrongly sent setPortBridging with the attribute's value.
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
# OltGDrvCfg is 176 bytes and does one thing: on a Set carrying ToDInfo, hand
# fourteen bytes straight to command 70. That is how an OLT distributes time of
# day, so it arrives unsolicited on a working line.
#
# The offset matters more than the command, and there are two of them. In the
# VENDOR's row ToDInfo is at 44, which is what proves it keeps the NUL in a
# string attribute (EquipId 21, Version 15; the wire lengths would give 42). In
# OURS it is at 40, because attr_offset packs at wire widths on purpose. This
# asserts our own layout -- the first four bytes surviving the round trip is
# what catches a fence-post in either convention.
$Q cli/build/omcli --inject 0050480a008300001000deadbeef111111111111111111110000000000000000000000000000000000000028bd607c90 > /dev/null 2>&1
sleep 1
got=$(grep -o 'tod info deadbeef ...' /tmp/omcid2.log | head -1)
check "class 131 ToDInfo reaches the driver path at the right row offset" \
      "$got" "tod info deadbeef ..."

# --------------------------------------------- the config store, escaped and written
#
# xmlconfig escapes the five XML entities, so a reader that hands back the raw
# attribute is wrong -- and wrong exactly on the two password fields, silently.
# The first version of cfgstore.c had that bug.
got=$($Q cli/build/omcli ident test/cfg_esc.xml test/cfg_esc_hs.xml 2>&1 | sed -n '3p')
check "the reader decodes every XML entity" "$got" 'loid        a&b<c>d"e'"'"'f'

# Writing: one attribute in place, never a whole-file rewrite. Work on a copy,
# because a test that edits its own fixture passes once.
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

# A Dir that is not in the file is refused, not created -- writing a key to the
# wrong file makes a duplicate the vendor's own reader never sees.
got=$($Q cli/build/omcli cfgset /tmp/w.xml HW_MIB_TABLE GPON_SN nope 2>&1 | sed -n '1p')
check "a Dir the file does not have is refused" "$got" "write failed"

# ------------------------------------------------------------------- class 47
#
# MacBriPortCfgDataDrvCfg is 4888 bytes and this covers the part of it that
# reaches driver commands we have: the MAC learning limit (53), the flooding
# port mask (64), the UNI rate limiter (67) and the broadcast GEM flow (26).
#
# Two things here are worth the assertion rather than the code review. The
# learning limit has a three-step fallback -- the port's own NumOfAllowedMac,
# then the bridge service profile's MacLearningDepth, then a global default we
# have NOT recovered -- so the class 45 row is set up first with depth 5 and the
# class 47 row leaves its own attribute at zero. And the two traffic descriptor
# directions are crossed against the obvious reading: on a UNI port direction 1
# is the INBOUND descriptor, because traffic entering the bridge from a UNI is
# upstream.
#
# PIRs are chosen to land on round numbers: (PIR << 3) >> 10, so 131072 B/s is
# 1024 kbit/s and 65536 is 512. A reversed shift would print 8388608 and a
# missing one 131072, neither of which looks like a rate.
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
# names the downstream broadcast port; a class 47 whose TP points at it is what
# makes the flow programmable, and DELETING the last such bridge port is what
# withdraws it. The vendor walks every other class 47 row before giving up, and
# hands the driver 0xffffffff -- we had the create half and no delete half.
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
# pointers -- upstream unicast flood, broadcast, multicast payload -- each one
# programming a driver slot of its own. Commands 59 and 60 are the same twenty
# bytes, { slot, portMask, kind, CIR, CBS }, and only the first two words
# identify the slot, so the slot number is ours the way a flow id is.
#
# The earlier block deleted both class 47 rows, so the bridge has no UNI left
# and a rate limiter on it would be refused. A fresh bridge port is created
# first -- which also re-exercises the class 47 create arm.
#
# The third pointer is left at zero here and filled in by a Set, so the same
# run covers "a pointer that resolves to nothing is a delete, not an error" and
# "a later Set allocates a slot the create did not".
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

# A Set that fills the third pointer. The vendor gates this on the pointer
# having CHANGED, against the row as it was before the Set; our store keeps one
# row and overwrites it, so the guard is dropped and all three are re-sent.
# That is more than the vendor sends and never less -- see PLAN.md's backlog.
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
# Both are SET-only and both fire only when the value CHANGED, against the row
# as it was before the Set. That guard used to be inexpressible -- the store
# kept one row and overwrote it -- so the arms were left out rather than sent
# unguarded, which would have sent MORE than the vendor does. `prev` expresses
# it now.
#
# Bridge 2 still has exactly one PPTP Ethernet UNI bridge port (47/3, created in
# the class 298 block), which is what the learning arm requires: the command
# names one port, so the vendor only sends it when the bridge has one.
for f in 0090480a002d00020100020000000000000000000000000000000000000000000000000000000000000000281dff325d \
         0091480a002d0002010002000000000000000000000000000000000000000000000000000000000000000028f3bc0b57 \
         0092480a002d00020080090000000000000000000000000000000000000000000000000000000000000000289fad2249; do
	$Q cli/build/omcli --inject "$f" > /dev/null 2>&1
	sleep 1
done
got=$(grep -o '\[dry 45/0002\] flooding mask 1 enable 1 -> 0' /tmp/omcid2.log | head -1)
check "a changed DiscardUnknow floods the bridge's whole membership" \
      "$got" "[dry 45/0002] flooding mask 1 enable 1 -> 0"
# 2 is not 1, and the vendor's test is (byte != 1), not (!byte).
got=$(grep -c '45/0002. flooding mask' /tmp/omcid2.log)
check "and setting it again to the same value sends nothing" "$got" "1"
got=$(grep -o '\[dry 45/0002\] mac learn limit port 0 = 9 -> 0' /tmp/omcid2.log | head -1)
check "a changed MacLearningDepth sets the single UNI's limit" \
      "$got" "[dry 45/0002] mac learn limit port 0 = 9 -> 0"

# The daemon's log lives inside the container and is otherwise unreachable when
# a check fails, so keep it on the way out -- but only then.
[ "$fail" -eq 0 ] || cp /tmp/omcid2.log /src/src/omci/qemu-test.log 2>/dev/null || true
kill %1 2>/dev/null
wait 2>/dev/null

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
exit "$fail"
