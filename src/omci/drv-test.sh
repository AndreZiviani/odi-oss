#!/bin/sh
# The driver-call golden: what omcid asks of the switch driver for a whole
# OLT session, per ISP, compared call for call.
#
# omcid-drvtrace (respond/Makefile) is omcid with every driver call logged as
# "drv <cmd> <len> <args hex>" and answered with success, so it runs with -a
# under qemu, where there is no netlink. The sessions in test/fixtures are the
# OLT side of one provisioning run of each ISP, rebuilt from omcid logs by
# tools/session-from-log.py; their headers say what was scrubbed. They are
# injected back to back from one omcli process, a sentinel Get after them
# says when omcid has handled the last one, and the run ends when the
# one-second rebuilds have run (the log says so) -- so it does not depend on
# how fast the host is, and does not wait out omcid's idle exit.
#
# The Alloc-IDs each OLT assigned by PLOAM come in through omcid -g, a file
# in the /proc/odi_gpon/status shape; the scenarios after the goldens swap them for
# other ones, none at all, and a list that changes while omcid runs.
#
# Any change to what omcid sends the driver shows up here, which is the point:
# a refactor of respond/drv.c or apply*.c must leave both goldens identical. A change
# that means to alter them regenerates them with UPDATE=1 and says why.
#
# Run from src/omci, inside the toolchain image (make test-omci-drv):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci odi-diag-toolchain \
#          sh drv-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0
. ./testlib.sh

# The capability blob of a real stick, the same one qemu-test.sh uses.
CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

# The config store of each run holds only the manual VLAN keys of that ISP
# (test/fixtures/omci-store-isp*.xml): the connection builder tags the
# untagged handoff with that VID, as on the stick. Nothing else is in it, so
# nothing qemu-test.sh left in this container is read either.
mkdir -p /var/config
rm -f /var/config/lastgood_hs.xml

# The Alloc-IDs each OLT assigned by PLOAM, in the shape /proc/odi_gpon/status
# prints them (omcid -g reads this in its place). ISP1 assigns five and
# never sets a T-CONT over OMCI, so omcid binds 0x8000..0x8004 to them in
# assignment order; ISP2 sets T-CONT 0x8000 to 348 itself.
gpon() {   # gpon <file> <alloc_ids line value>
	printf 'state 5 (O5)\nonu_id 26\nsn 4f44495400000001\neqd multiframe 0 inframe 0\nploam ds_rx 0 us_tx 0\nalloc_ids %s\n' \
		"$2" > "$1"
}
get_frame() {   # get_frame <tci> <class> <inst> <mask>: one Get, as omcli --inject-file reads it
	printf '%s490a%s%s%s%060d00000028%08d\n' "$1" "$2" "$3" "$4" 0 0
}
gpon /tmp/gpon-isp1 "5 282 794 1050 1306 538"
gpon /tmp/gpon-isp2 "1 348"
# ISP3: six data GEMs on three T-CONTs, the Alloc-IDs by PLOAM, in the order
# the OLT assigns them (0x8000..0x8002 bound to them in turn).
gpon /tmp/gpon-isp3 "3 269 525 781"

# finish <log>: settled (testlib.sh: the session is handled and the rebuild
# after it has run, so every driver call is in the log), and the daemon, the
# omcid under test ($pid), stopped.
finish() {
	settled "$1"
	omcid_stop "$pid"
}

for isp in isp1 isp2 isp3; do
	log=/tmp/omcid-drv-$isp.log
	got=/tmp/omci-drv-$isp.txt
	want=$FIX/omci-drv-golden-$isp.txt
	cp "$FIX/omci-store-$isp.xml" /var/config/lastgood.xml

	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-$isp > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$isp.txt" > /tmp/inject-$isp.txt 2>&1
	finish "$log"
	grep '^drv ' "$log" > "$got"

	frames=$(grep -c '^[0-9a-f]' "$FIX/omci-session-$isp.txt")
	if ! grep -q "injected $frames frames" /tmp/inject-$isp.txt; then
		echo "FAIL  $isp: omcli did not inject all $frames frames"
		cat /tmp/inject-$isp.txt
		fail=$((fail + 1))
		continue
	fi
	if [ "${UPDATE:-0}" = 1 ]; then
		cp "$got" "$want"
		echo "updated $want ($(wc -l < "$got") driver calls)"
		continue
	fi
	if cmp -s "$want" "$got"; then
		echo "ok    $isp: $(wc -l < "$got") driver calls identical ($frames frames)"
	else
		echo "FAIL  $isp: driver calls differ from $want"
		diff "$want" "$got" | head -40
		keep_log "$log" "drv-test-$isp.log"
		fail=$((fail + 1))
	fi
done

# The rebuilds wait for one quiet second of OMCI frames, not of all
# activity: a client polling omcid every 0.3 s (a metrics scrape does the
# same) must not hold the bridge connections back. The rebuild has to be in
# the log while the client is still polling.
if [ "${UPDATE:-0}" != 1 ]; then
	log=/tmp/omcid-drv-poll.log
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	# The 0.3 s is the point of this scenario, a client polling at that
	# rate; the loop ends as soon as the rebuild shows, or after 13 polls.
	i=0
	while [ "$i" -lt 13 ] && ! grep -q "bridge connections rebuilt" "$log"; do
		$Q cli/build/omcli state > /dev/null 2>&1
		sleep 0.3
		i=$((i + 1))
	done
	if grep -q "bridge connections rebuilt" "$log"; then
		echo "ok    isp1: bridge connections rebuilt while a client polls"
	else
		echo "FAIL  isp1: no bridge rebuild while a client polls every 0.3 s"
		fail=$((fail + 1))
	fi
	omcid_stop "$pid"
fi

# ------------------------------------------------ phone ports (OMCI_POTS_PORTS)
#
# At 0 the key is off: every session's driver calls are the golden, call for
# call. At 2 the ONU tells the OLT of two phone ports, and still nothing of
# them reaches the driver: an OLT session with no voice in it programs the
# switch exactly as before.
if [ "${UPDATE:-0}" != 1 ]; then
	for run in "isp1 0" "isp2 0" "isp3 0" "isp1 2"; do
		set -- $run
		isp=$1
		log=/tmp/omcid-drv-pots-$isp-$2.log
		got=/tmp/omci-drv-pots-$isp-$2.txt
		cp "$FIX/omci-store-$isp.xml" /var/config/lastgood.xml
		printf 'OMCI_POTS_PORTS=%s\n' "$2" > /var/config/odi.conf
		$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-$isp > "$log" 2>&1 &
		pid=$!
		omcid_up "$log"
		$Q cli/build/omcli --inject-file "$FIX/omci-session-$isp.txt" > /dev/null 2>&1
		finish "$log"
		grep '^drv ' "$log" > "$got"
		if cmp -s "$FIX/omci-drv-golden-$isp.txt" "$got"; then
			echo "ok    $isp with OMCI_POTS_PORTS=$2: the driver calls are the golden"
		else
			echo "FAIL  $isp with OMCI_POTS_PORTS=$2: driver calls differ from the golden"
			diff "$FIX/omci-drv-golden-$isp.txt" "$got" | head -20
			keep_log "$log" "drv-test-pots-$isp-$2.log"
			fail=$((fail + 1))
		fi
	done
	rm -f /var/config/odi.conf
fi

# ------------------------------------------------ Alloc-IDs from this OLT
#
# The T-CONT Alloc-IDs come from the OLT, never from a table: set over OMCI
# (ISP2), or else the PLOAM assignments bound in order (ISP1). An OLT that
# assigns other Alloc-IDs gets exactly the ISP1 driver calls with its own
# Alloc-IDs in them, and nothing else changes.
if [ "${UPDATE:-0}" != 1 ]; then
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml

	# A synthetic OLT: the ISP1 session, other Alloc-IDs by PLOAM.
	gpon /tmp/gpon-other "5 1000 1001 1002 1003 1004"
	log=/tmp/omcid-drv-other.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-other > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	{
		cat "$FIX/omci-session-isp1.txt"
		# Get 262/0x8000 and 0x8005, attribute 1 (AllocID).
		get_frame 0e30 0106 8000 8000
		get_frame 0e31 0106 8005 8000
	} > /tmp/session-other.txt
	$Q cli/build/omcli --inject-file /tmp/session-other.txt > /dev/null 2>&1
	finish "$log"
	sed -e '/^drv 21 /s/0000011a$/000003e8/' -e '/^drv 21 /s/0000031a$/000003e9/' \
	    -e '/^drv 21 /s/0000041a$/000003ea/' -e '/^drv 21 /s/0000051a$/000003eb/' \
	    -e '/^drv 21 /s/0000021a$/000003ec/' "$FIX/omci-drv-golden-isp1.txt" > /tmp/want-other.txt
	if grep '^drv ' "$log" | cmp -s /tmp/want-other.txt -; then
		echo "ok    other OLT: the ISP1 calls, with the Alloc-IDs this OLT assigned"
	else
		echo "FAIL  other OLT: driver calls are not the ISP1 golden with its own Alloc-IDs"
		grep '^drv ' "$log" | diff /tmp/want-other.txt - | head -20
		fail=$((fail + 1))
	fi
	# What the OLT reads back: 0x8000 bound to the first, 0x8005 unassigned.
	if grep -q -- '-> 0e30290a0106800000800003e8' "$log" &&
	   grep -q -- '-> 0e31290a0106800500800000ff' "$log"; then
		echo "ok    other OLT: a Get of the T-CONT AllocID answers the PLOAM binding"
	else
		echo "FAIL  other OLT: T-CONT AllocID Gets"
		grep -- '-> 0e3[01]290a' "$log"
		fail=$((fail + 1))
	fi

	# No Alloc-ID assigned yet: nothing to bind, so no T-CONT is programmed
	# (0x00FF is refused), rather than one a table says this OLT uses.
	gpon /tmp/gpon-none "0"
	log=/tmp/omcid-drv-none.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-none > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	finish "$log"
	got=$(grep -c '^drv 21 ' "$log")
	if [ "$got" = 0 ]; then
		echo "ok    no PLOAM Alloc-ID: no T-CONT is programmed"
	else
		echo "FAIL  no PLOAM Alloc-ID: $got T-CONTs programmed anyway"
		fail=$((fail + 1))
	fi

	# ISP2 sets T-CONT 0x8000 to 348. With a second Alloc-ID assigned by
	# PLOAM, the set one keeps 348 and the next unset T-CONT takes 400:
	# an Alloc-ID a T-CONT was set to is never bound twice.
	cp "$FIX/omci-store-isp2.xml" /var/config/lastgood.xml
	gpon /tmp/gpon-isp2b "2 400 348"
	log=/tmp/omcid-drv-isp2b.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp2b > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	{
		cat "$FIX/omci-session-isp2.txt"
		get_frame 0e40 0106 8000 8000
		get_frame 0e41 0106 8001 8000
	} > /tmp/session-isp2b.txt
	$Q cli/build/omcli --inject-file /tmp/session-isp2b.txt > /dev/null 2>&1
	finish "$log"
	if grep '^drv ' "$log" | cmp -s "$FIX/omci-drv-golden-isp2.txt" - &&
	   grep -q -- '-> 0e40290a01068000008000015c' "$log" &&
	   grep -q -- '-> 0e41290a010680010080000190' "$log"; then
		echo "ok    isp2: the set T-CONT keeps 348, the next unset one takes 400"
	else
		echo "FAIL  isp2: OMCI-set and PLOAM-bound Alloc-IDs side by side"
		grep '^drv ' "$log" | diff "$FIX/omci-drv-golden-isp2.txt" - | head -10
		grep -- '-> 0e4[01]290a' "$log"
		fail=$((fail + 1))
	fi

	# One omcid, ISP1 and then ISP2: the MIB reset between them throws the
	# whole ISP1 MIB away, T-CONT map included, and nothing of ISP1 is
	# programmed again after it.
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-both.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp2b > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	settled "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp2.txt" > /dev/null 2>&1
	settled "$log"
	map=$($Q cli/build/omcli tcont 2>&1 | grep -c '^  me [0-9a-f]* -> index')
	omcid_stop "$pid"
	after=$(awk '/<- mib-reset/ { n++ } n >= 2 && /\] gem (1434|1690|1818|1946|1562) us flow/' "$log" | wc -l)
	if [ "$map" = 1 ] && [ "$after" = 0 ]; then
		echo "ok    a MIB reset clears the T-CONT map: ISP1 then ISP2, one T-CONT left"
	else
		echo "FAIL  after ISP1 then ISP2: $map T-CONTs mapped, $after ISP1 upstream flows re-programmed"
		fail=$((fail + 1))
	fi

	# The OLT reassigns: an Alloc-ID released and another assigned, seen
	# in the kernel list while omcid runs. The T-CONT bound to it is
	# reprogrammed with the new one, a quiet second later.
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	gpon /tmp/gpon-move "5 282 794 1050 1306 538"
	log=/tmp/omcid-drv-move.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-move > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	settled "$log"
	gpon /tmp/gpon-move "5 282 2000 1050 1306 538"
	# Seen at the next one-second sample, programmed a quiet second later.
	need "$log" '^drv 21 8 00000001000007d0$' 30
	omcid_stop "$pid"
	if grep -q '^drv 21 8 00000001000007d0$' "$log" &&
	   grep -q '^event=alloc_ids count=5 ids=282,2000,1050,1306,538$' "$log"; then
		echo "ok    a reassigned Alloc-ID reprograms the T-CONT bound to it"
	else
		echo "FAIL  a reassigned Alloc-ID did not reach the driver"
		grep '^drv 21\|^event=alloc_ids' "$log"
		fail=$((fail + 1))
	fi
fi

# ------------------------------------------------ an Alloc-ID PLOAM has not assigned
#
# The kernel binds a T-CONT only to an Alloc-ID the OLT assigned by PLOAM in
# this activation (G.984.3 5.5.4; odi_switch_cmd.c cmd 21, which drvtrace
# answers the same way against the -g list). An OLT that sets T-CONT 0x8000
# to 348 over OMCI before its Assign_Alloc-ID: cmd 21 is refused and no
# upstream flow is created; the assignment then reaches the driver a quiet
# second after the kernel list shows it. Then the OLT takes 348 back (a
# deallocation, or the ONU-ID lost): the flow is deleted, cmd 21 is refused
# again, and nothing re-creates the flow or keeps the old T-CONT index.
if [ "${UPDATE:-0}" != 1 ]; then
	cp "$FIX/omci-store-isp2.xml" /var/config/lastgood.xml
	gpon /tmp/gpon-late "0"
	log=/tmp/omcid-drv-late.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-late > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp2.txt" > /dev/null 2>&1
	settled "$log"
	us_create='^drv 25 68 0000000000000291.*00000001$'
	if grep -q 'drvtrace: cmd 21 refused, alloc 348 not assigned' "$log" &&
	   ! grep -q "$us_create" "$log"; then
		echo "ok    an Alloc-ID set over OMCI only: cmd 21 refused, no upstream flow"
	else
		echo "FAIL  an Alloc-ID set over OMCI only reached the hardware"
		grep '^drv 2[15] \|drvtrace:' "$log"
		fail=$((fail + 1))
	fi
	gpon /tmp/gpon-late "1 348"
	if need "$log" "$us_create" 30; then
		echo "ok    the Assign_Alloc-ID that follows binds the T-CONT and creates the flow"
	fi
	gpon /tmp/gpon-late "0"
	if need "$log" 'drvtrace: cmd 21 refused, alloc 348 not assigned' 30 2; then
		map=$($Q cli/build/omcli tcont 2>&1 | grep -c '^  me [0-9a-f]* -> index')
		creates=$(grep -c "$us_create" "$log")
		deletes=$(grep -c '^drv 25 68 0000000000000000.*00000001$' "$log")
		if [ "$map" = 0 ] && [ "$creates" = 1 ] && [ "$deletes" -ge 1 ]; then
			echo "ok    a deallocated Alloc-ID: flow deleted, T-CONT unbound, nothing re-created"
		else
			echo "FAIL  after the deallocation: $map T-CONTs mapped, $creates creates, $deletes deletes"
			grep '^drv 2[15] \|drvtrace:' "$log"
			fail=$((fail + 1))
		fi
	fi
	omcid_stop "$pid"
fi

# ------------------------------------------------ OMCI_TCONT_ALLOC_UNSET
#
# What a T-CONT the OLT never Set reports as its Alloc-ID (ME 262 attribute
# 1). "ploam" (the default) is the PLOAM-bound value; "unassigned" is 0x00FF
# (G.988 9.2.2). An Alloc-ID the OLT Set is reported as set in both modes,
# and the key never changes what the datapath is given (cmd 21) or the
# pointers of the GEM port network CTP (ME 268). A MIB upload answers from
# the same code as a Get; the upload record of T-CONT 0x8000 is read too.
if [ "${UPDATE:-0}" != 1 ]; then
	up_next() {   # up_next <tci> <seq>: MIB upload next
		printf '%s4e0a00020000%04x%060d00000028%08d\n' "$1" "$2" 0 0
	}
	# unset_run <name> <store> <gpon> <session> <extra frames file>: omcid
	# on the store with the key value of $KEYVAL, the session and the extras
	unset_run() {
		cp "$2" /var/config/lastgood.xml
		if [ -n "$KEYVAL" ]; then
			printf 'OMCI_TCONT_ALLOC_UNSET=%s\n' "$KEYVAL" > /var/config/odi.conf
		else
			rm -f /var/config/odi.conf
		fi
		log=/tmp/omcid-drv-$1.log
		: > "$log"
		$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g "$3" > "$log" 2>&1 &
		pid=$!
		omcid_up "$log"
		{
			cat "$4"
			get_frame 0e30 0106 8000 8000
			get_frame 0e31 0106 8005 8000
			get_frame 0e32 010c 0001 e000
			get_frame 0e33 010c 0003 e000
			printf '0d014d0a00020000%064d00000028%08d\n' 0 0
			q=40
			while [ "$q" -lt 100 ]; do up_next "$(printf '1%03x' "$q")" "$q"; q=$((q + 1)); done
		} > /tmp/session-$1.txt
		$Q cli/build/omcli --inject-file /tmp/session-$1.txt > /dev/null 2>&1
		finish "$log"
		rm -f /var/config/odi.conf
	}
	# the upload record of T-CONT 0x8000 and 0x8005: class 262 (0106),
	# the entity id, then attribute 1 (the Alloc-ID, two bytes)
	upload_val() {   # upload_val <log> <inst hex>
		grep -h -- '^-> 1[0-9a-f]\{3\}2e0a00020000' "$1" | grep -o -- "0106${2}[0-9a-f]\{8\}" | head -1 | cut -c13-16
	}
	chk() { if [ "$2" = "$3" ]; then echo "ok    $1"; else echo "FAIL  $1: '$2', want '$3'"; fail=$((fail + 1)); fi; }

	gpon /tmp/gpon-other "5 1000 1001 1002 1003 1004"

	KEYVAL=ploam
	unset_run unset-ploam "$FIX/omci-store-isp1.xml" /tmp/gpon-other "$FIX/omci-session-isp1.txt"
	cp "$log" /tmp/omcid-drv-ploam.log
	grep '^drv ' /tmp/omcid-drv-ploam.log > /tmp/drv-ploam.txt
	chk "ploam: a Get of 0x8000 is the PLOAM binding" "$(grep -c -- '-> 0e30290a0106800000800003e8' /tmp/omcid-drv-ploam.log)" 1
	chk "ploam: a Get of 0x8005 is 0x00ff" "$(grep -c -- '-> 0e31290a0106800500800000ff' /tmp/omcid-drv-ploam.log)" 1
	chk "ploam: the MIB upload record of 0x8000" "$(upload_val /tmp/omcid-drv-ploam.log 8000)" 03e8
	chk "ploam: the MIB upload record of 0x8005" "$(upload_val /tmp/omcid-drv-ploam.log 8005)" 00ff

	KEYVAL=unassigned
	unset_run unset-unassigned "$FIX/omci-store-isp1.xml" /tmp/gpon-other "$FIX/omci-session-isp1.txt"
	cp "$log" /tmp/omcid-drv-unassigned.log
	grep '^drv ' /tmp/omcid-drv-unassigned.log > /tmp/drv-unassigned.txt
	chk "unassigned: a Get of 0x8000 is 0x00ff" "$(grep -c -- '-> 0e30290a0106800000800000ff' /tmp/omcid-drv-unassigned.log)" 1
	chk "unassigned: a Get of 0x8005 is 0x00ff" "$(grep -c -- '-> 0e31290a0106800500800000ff' /tmp/omcid-drv-unassigned.log)" 1
	chk "unassigned: the MIB upload record of 0x8000" "$(upload_val /tmp/omcid-drv-unassigned.log 8000)" 00ff
	chk "unassigned: the MIB upload record of 0x8005" "$(upload_val /tmp/omcid-drv-unassigned.log 8005)" 00ff
	if cmp -s /tmp/drv-ploam.txt /tmp/drv-unassigned.txt; then
		echo "ok    unassigned: the driver calls (cmd 21 included) are the ploam ones, byte for byte"
	else
		echo "FAIL  unassigned: the driver calls differ from the ploam mode"
		diff /tmp/drv-ploam.txt /tmp/drv-unassigned.txt | head -10
		fail=$((fail + 1))
	fi
	chk "unassigned: the ME 268 pointers read the same in both modes" \
	    "$(grep -- '-> 0e3[23]29' /tmp/omcid-drv-ploam.log | sort | cut -c1-200)" \
	    "$(grep -- '-> 0e3[23]29' /tmp/omcid-drv-unassigned.log | sort | cut -c1-200)"
	chk "unassigned: a warning for each T-CONT bound from PLOAM" \
	    "$(grep -c 'OMCI_TCONT_ALLOC_UNSET=unassigned' /tmp/omcid-drv-unassigned.log)" 5
	chk "ploam: no such warning" "$(grep -c 'OMCI_TCONT_ALLOC_UNSET=unassigned' /tmp/omcid-drv-ploam.log)" 0

	# An OLT that Sets T-CONT 0x8000 (ISP2): the set value is reported and
	# programmed in both modes, and the run is the ISP2 golden.
	for KEYVAL in ploam unassigned; do
		gpon /tmp/gpon-isp2b "2 400 348"
		unset_run "unset-isp2-$KEYVAL" "$FIX/omci-store-isp2.xml" /tmp/gpon-isp2b "$FIX/omci-session-isp2.txt"
		chk "$KEYVAL: an OLT Set of 0x8000 (348) then a Get answers 348" \
		    "$(grep -c -- '-> 0e30290a01068000008000015c' "$log")" 1
		chk "$KEYVAL: the Set T-CONT is in the MIB upload as 348" "$(upload_val "$log" 8000)" 015c
		grep '^drv ' "$log" > "/tmp/drv-isp2-$KEYVAL.txt"
	done
	if cmp -s /tmp/drv-isp2-ploam.txt /tmp/drv-isp2-unassigned.txt &&
	   grep -q '^drv 21 .*0000015c$' /tmp/drv-isp2-unassigned.txt; then
		echo "ok    an OLT that Sets the Alloc-ID: the same driver calls in both modes, T-CONT 348 programmed"
	else
		echo "FAIL  an OLT that Sets the Alloc-ID: the driver calls differ between the modes"
		diff /tmp/drv-isp2-ploam.txt /tmp/drv-isp2-unassigned.txt | head -10
		fail=$((fail + 1))
	fi
fi

# ------------------------------------------------ the OLT locks the UNI
#
# Class 11 attribute 5 (AdministrativeState) = 1 forces the host link down
# (driver command 32, state 0). The lock must not outlive the MIB it came
# from: omcid unlocks when it starts with an empty MIB and on every MIB reset,
# because the reset puts the attribute back to 0 and the OLT then sends no
# unlock of its own. OMCI_UNI_LOCK=0 in odi.conf ignores the lock.
if [ "${UPDATE:-0}" != 1 ]; then
	uni_set() {   # uni_set <tci> <admin state>: Set of ME 11 0x0101, attribute 5
		printf '%s480a000b01010800%02d%058d00000028%08d\n' "$1" "$2" 0 0
	}
	mib_reset() {   # mib_reset <tci>
		printf '%s4f0a00020000%064d00000028%08d\n' "$1" 0 0
	}
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-lock.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	{
		uni_set 0e60 01
		get_frame 0e61 0106 8000 8000
		mib_reset 0e62
		get_frame 0e63 0106 8000 8000
	} > /tmp/session-lock.txt
	$Q cli/build/omcli --inject-file /tmp/session-lock.txt > /dev/null 2>&1
	need "$log" '^drv 32 ' 20 3
	finish "$log"
	calls=$(grep '^drv 32 ' "$log" | sed 's/^drv 32 8 0*//; s/^$/0/' | tr '\n' ' ')
	if [ "$calls" = "1 0 1 " ]; then
		echo "ok    UNI lock: unlock at start, the OLT lock honoured, unlock again on the MIB reset"
	else
		echo "FAIL  UNI lock: cmd 32 states were '$calls', want '1 0 1 '"
		grep '^drv 32 ' "$log"
		fail=$((fail + 1))
	fi

	printf 'OMCI_UNI_LOCK=0\n' > /var/config/odi.conf
	log=/tmp/omcid-drv-lock0.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	uni_set 0e64 01 > /tmp/session-lock0.txt
	$Q cli/build/omcli --inject-file /tmp/session-lock0.txt > /dev/null 2>&1
	need "$log" 'lock ignored (OMCI_UNI_LOCK=0)' 20
	# No bridge rebuild follows (nothing here touches a connection), so
	# settled would wait for one that never comes: the Get answered is the
	# sync point.
	sync_send "$log"
	omcid_stop "$pid"
	rm -f /var/config/odi.conf
	calls=$(grep '^drv 32 ' "$log" | sed 's/^drv 32 8 0*//; s/^$/0/' | tr '\n' ' ')
	if [ "$calls" = "1 1 " ]; then
		echo "ok    OMCI_UNI_LOCK=0: the OLT lock is answered but the port stays up"
	else
		echo "FAIL  OMCI_UNI_LOCK=0: cmd 32 states were '$calls', want '1 1 '"
		fail=$((fail + 1))
	fi
fi

# ------------------------------------------------ ANI-G GEM block length
#
# Class 263 attribute 3 is the GEM block length (G.988 9.2.1), the one
# ANI-G attribute omcid hands to the driver (setGemBlkLen, command 18). A
# Set of attribute 3 reaches it; a Set of attribute 12, the upper optical
# threshold, does not. Neither ISP session sets ANI-G, so both are
# appended to the ISP1 one: 96 bytes, then a threshold of 0xf0.
if [ "${UPDATE:-0}" != 1 ]; then
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-anig.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	{
		cat "$FIX/omci-session-isp1.txt"
		printf '0e50480a01078001%s%056d00000028%08d\n' 20000060 0 0
		printf '0e51480a01078001%s%058d00000028%08d\n' 0010f0 0 0
	} > /tmp/session-anig.txt
	$Q cli/build/omcli --inject-file /tmp/session-anig.txt > /dev/null 2>&1
	finish "$log"
	calls=$(grep '^drv 18 ' "$log" | tr '\n' ' ')
	if [ "$calls" = "drv 18 2 0060 " ]; then
		echo "ok    ANI-G: attribute 3 sets the GEM block length, attribute 12 does not"
	else
		echo "FAIL  ANI-G: setGemBlkLen calls were '$calls', want 'drv 18 2 0060 '"
		fail=$((fail + 1))
	fi
fi

# ------------------------------------------------ upstream traffic descriptor
#
# A GEM port CTP's upstream traffic descriptor (class 280, named by class
# 268 attribute 5) gives the rates of its upstream queue, as in the stock
# stack: byte/s divided by 1024. Neither ISP provisions one, so they are
# appended to the ISP2 session: descriptor 1 created with CIR 125000 and
# PIR 250000 (meter type 2), the data CTP 0x0002 pointed at it, then the
# descriptor's CIR raised to 250000, which alone must re-send the queue.
if [ "${UPDATE:-0}" != 1 ]; then
	cp "$FIX/omci-store-isp2.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-td.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp2 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	{
		cat "$FIX/omci-session-isp2.txt"
		printf '0e60440a01180001%s%024d00000028%08d\n' 0001e8480003d090000000000000000000000002 0 0
		printf '0e61480a010c0002%s%056d00000028%08d\n' 08000001 0 0
	} > /tmp/session-td.txt
	$Q cli/build/omcli --inject-file /tmp/session-td.txt > /dev/null 2>&1
	settled "$log"
	first=$(grep '^drv 23 ' "$log" | tail -1)
	n=$(grep -c '^drv 23 ' "$log")
	printf '0e62480a01180001%s%052d00000028%08d\n' 80000003d090 0 0 > /tmp/session-td2.txt
	$Q cli/build/omcli --inject-file /tmp/session-td2.txt > /dev/null 2>&1
	# No bridge rebuild follows a descriptor Set, only the QoS one: wait
	# for its queue call.
	wait_for "$log" '^drv 23 ' 30 $((n + 1))
	omcid_stop "$pid"
	second=$(grep '^drv 23 ' "$log" | tail -1)
	case "$first" in
	*0000007a000000f4*) echo "ok    traffic descriptor: CIR 122 and PIR 244 (x 8 kbit/s) on the queue" ;;
	*) echo "FAIL  traffic descriptor: last queue call '$first', want CIR 0x7a PIR 0xf4"
	   fail=$((fail + 1)) ;;
	esac
	case "$second" in
	*000000f4000000f4*) echo "ok    traffic descriptor: a Set of its CIR re-sends the queue" ;;
	*) echo "FAIL  traffic descriptor: after the CIR Set '$second', want CIR 0xf4 PIR 0xf4"
	   fail=$((fail + 1)) ;;
	esac
fi

# ------------------------------------------------ the upstream rebuild changes only what changed
#
# After the ISP1 provisioning, three Sets an OLT may send on a live line:
#   A. a referenced queue (class 277) Set to the Weight it already has: the
#      rebuild runs and sends nothing;
#   B. the same queue Set to another Weight: one cmd 23 for that queue, in
#      place, and no T-CONT, queue delete or flow call;
#   C. a traffic scheduler (class 278) no queue names (ISP1 points its
#      queues at 0x8000-0x8007; 0x800f is one of its unused ones): no
#      rebuild at all.
# Before, every rebuild deleted and re-created every upstream queue and
# flow, which on the stick drains the queues and stops all upstream.
if [ "${UPDATE:-0}" != 1 ]; then
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-incr.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 8 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	settled "$log"
	me=$(sed -n 's/.*us priq \([0-9]*\) ordinal 0 tcont [0-9]* weight 24 wrr 1 .*/\1/p' "$log" | head -1)
	hex=$(printf '%04x' "${me:-0}")
	nq=$(grep -c 'us qos' "$log")
	calls=$(grep -c '^drv 2[1345] ' "$log")

	printf '0e70480a0115%s0100%s%058d00000028%08d\n' "$hex" 18 0 0 > /tmp/incr-a.txt
	$Q cli/build/omcli --inject-file /tmp/incr-a.txt > /dev/null 2>&1
	if wait_for "$log" 'us qos' 30 $((nq + 1)); then
		if [ "$(grep -c '^drv 2[1345] ' "$log")" = "$calls" ] &&
		   grep 'us qos' "$log" | tail -1 | grep -q '5 T-CONTs kept, 0 built, 5 flows unchanged'; then
			echo "ok    a queue Set that changes nothing sends no driver call"
		else
			echo "FAIL  a queue Set that changes nothing reprogrammed the upstream"
			grep 'us qos' "$log" | tail -1
			fail=$((fail + 1))
		fi
	fi

	printf '0e71480a0115%s0100%s%058d00000028%08d\n' "$hex" 19 0 0 > /tmp/incr-b.txt
	$Q cli/build/omcli --inject-file /tmp/incr-b.txt > /dev/null 2>&1
	if wait_for "$log" 'us qos' 30 $((nq + 2)); then
		new=$(grep '^drv 2[1345] ' "$log" | tail -n +$((calls + 1)))
		if [ "$(printf '%s\n' "$new" | grep -c .)" = 1 ] &&
		   printf '%s\n' "$new" | grep -q '^drv 23 28 .*00190000000000010000000000000001$'; then
			echo "ok    a Weight change re-sends that one queue in place, nothing else"
		else
			echo "FAIL  a Weight change: driver calls were"
			printf '%s\n' "$new"
			fail=$((fail + 1))
		fi
	fi

	# C has no event to wait for: it must cause none. A Get after it is
	# answered first, then twice the one quiet second a rebuild waits for.
	printf '0e72480a0116800f%s%s%058d00000028%08d\n' 2000 01 0 0 > /tmp/incr-c.txt
	get_frame 0e73 0116 800f 2000 >> /tmp/incr-c.txt
	$Q cli/build/omcli --inject-file /tmp/incr-c.txt > /dev/null 2>&1
	wait_for "$log" '0e73290a0116' 30
	sleep 2.5
	if [ "$(grep -c 'us qos' "$log")" = $((nq + 2)) ]; then
		echo "ok    a Set of a traffic scheduler no queue names causes no rebuild"
	else
		echo "FAIL  a Set of an unreferenced traffic scheduler rebuilt the upstream"
		fail=$((fail + 1))
	fi
	omcid_stop "$pid"
fi

# A MIB reset from the OLT throws the MIB away, and the stock firmware's
# reset clears the datapath with it. The bridge connections built from the
# old MIB have to go: the rebuild after the reset deactivates every one of
# them, and builds none from the empty MIB.
if [ "${UPDATE:-0}" != 1 ]; then
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-reset.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	settled "$log"
	n=$(rebuilt "$log")
	inject_wait "$log" "$(printf '0e604f0a00020000%064d00000028%08d' 0 0)"
	wait_for "$log" 'bridge connections rebuilt' 30 $((n + 1))
	omcid_stop "$pid"
	after=$(awk '/<- mib-reset/{d=0; a=0} /^drv 50 /{d++} /^drv 51 /{a++} END{print d" "a}' "$log")
	if [ "${after#* }" = 0 ] && [ "${after% *}" -ge 6 ]; then
		echo "ok    MIB reset: the rebuild deactivates the old bridge connections (${after% *}) and builds none"
	else
		echo "FAIL  MIB reset: after it, deactivated/activated were '$after', want >= 6 and 0"
		fail=$((fail + 1))
	fi
fi

# ------------------------------------------------ UNI limits and ME 50
#
# What omcid hands the switch for the limits an OLT provisions on the UNI, in
# the words the kernel takes (uapi/omci_bridgeport.h), and the bridge table
# of a port, which is read from the switch on every Get (command 0x101, a
# page of 30 entries) and served through Get and Get-Next. The driver trace
# plays the switch: 2 addresses on the UNI, 35 on the PON port.
if [ "${UPDATE:-0}" != 1 ]; then
	bt_get() {   # bt_get <tci> <inst>: Get of ME 50, attribute 1
		printf '%s490a0032%s8000%060d00000028%08d\n' "$1" "$2" 0 0
	}
	bt_next() {  # bt_next <tci> <inst> <seq>: Get-Next of ME 50, attribute 1
		printf '%s5a0a0032%s8000%04x%056d00000028%08d\n' "$1" "$2" "$3" 0 0
	}
	bt_body() {  # bt_body <log> <tci>: the answer after its header
		grep -- "^-> $2" "$1" | tail -1 | cut -c20-
	}
	cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
	log=/tmp/omcid-drv-limits.log
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	omcid_up "$log"
	{
		cat "$FIX/omci-session-isp1.txt"
		# Two traffic descriptors: PIR 131072 B/s (1024 kbit/s) and
		# 524288 B/s (4096 kbit/s); then port 47/0001's outbound and
		# inbound pointers, and NumOfAllowedMac 64.
		printf '0f20440a01188001%s%048d00000028%08d\n' 0000000000020000 0 0
		printf '0f21440a01188002%s%048d00000028%08d\n' 0000000000080000 0 0
		printf '0f22480a002f00010030%s%052d00000028%08d\n' 80018002 0 0
		printf '0f23480a002f00010008%s%058d00000028%08d\n' 40 0 0
		bt_get 0f30 0001
		bt_next 0f31 0001 0
		bt_get 0f32 0002
		for q in 0 1 2 3 4 5 6 7 8 9; do bt_next "0f4$q" 0002 $q; done
	} > /tmp/session-limits.txt
	$Q cli/build/omcli --inject-file /tmp/session-limits.txt > /dev/null 2>&1
	need "$log" '^-> 0f49' 30
	sync_send "$log"
	omcid_stop "$pid"
	got=$(grep -c '^drv 53 8 0000000000000040$' "$log")
	check_n() { if [ "$2" = "$3" ]; then echo "ok    $1"; else echo "FAIL  $1: $2, want $3"; fail=$((fail + 1)); fi; }
	check_n "NumOfAllowedMac 64 reaches the driver as {port 0, limit 64} (cmd 53)" "$got" 1
	got=$(grep -c '^drv 67 12 000000000000000100001000$' "$log")
	check_n "the inbound descriptor is {port 0, dir 1, 4096 kbit/s} (cmd 67)" "$got" 1
	got=$(grep -c '^drv 67 12 000000000000000200000400$' "$log")
	check_n "the outbound descriptor is {port 0, dir 2, 1024 kbit/s} (cmd 67)" "$got" 1
	check_n "ME 50 of the UNI port: 2 entries, 16 bytes" "$(bt_body "$log" 0f30 | cut -c1-14)" 00800000000010
	check_n "and they are the switch's, in table order" "$(bt_body "$log" 0f31 | cut -c7-38)" 12640200000000401264020000000041
	check_n "ME 50 of the VEIP port, the PON port's: 35 entries, 280 bytes" \
	        "$(bt_body "$log" 0f32 | cut -c1-14)" 00800000000118
	check_n "its first entry" "$(bt_body "$log" 0f40 | cut -c7-22)" 1264020000000240
	check_n "its last entry, in the last chunk (the second page of the driver)" \
	        "$(bt_body "$log" 0f49 | cut -c29-44)" 1264020000000262
	check_n "the table cost 3 driver reads, 1 for the UNI and 2 pages for the PON port" \
	        "$(grep -c '^drv 257 256 ' "$log")" 3
	check_n "the second page asked for the 31st address of port 2" \
	        "$(grep -c '^drv 257 256 000000020000001e' "$log")" 1
fi

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
suite_exit
