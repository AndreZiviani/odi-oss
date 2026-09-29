#!/bin/sh
# The driver-call golden: what omcid asks of the switch driver for a whole
# OLT session, per ISP, compared call for call.
#
# omcid-drvtrace (respond/Makefile) is omcid with every driver call logged as
# "drv <cmd> <len> <args hex>" and answered with success, so it runs with -a
# under qemu, where there is no netlink. The sessions in test/fixtures are the
# OLT side of one provisioning run of each ISP, rebuilt from omcid logs by
# tools/session-from-log.py; their headers say what was scrubbed. They are
# injected back to back from one omcli process and omcid exits five idle
# seconds after the last one, after its one-second rebuilds have run -- so
# the run does not depend on how fast the host is.
#
# The Alloc-IDs each OLT assigned by PLOAM come in through omcid -g, a file
# in the /proc/odi_gpon shape; the scenarios after the goldens swap them for
# other ones, none at all, and a list that changes while omcid runs.
#
# Any change to what omcid sends the driver shows up here, which is the point:
# a refactor of respond/drv.c or apply*.c must leave both goldens identical. A change
# that means to alter them regenerates them with UPDATE=1 and says why.
#
# Run from src/omci, inside the toolchain image, after qemu-test.sh (the
# Makefile test-omci target runs both):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci odi-diag-toolchain \
#          sh drv-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0

# The capability blob of a real stick, the same one qemu-test.sh uses.
CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

# The config store of each run holds only the manual VLAN keys of that ISP
# (test/fixtures/omci-store-isp*.xml): the connection builder tags the
# untagged handoff with that VID, as on the stick. Nothing else is in it, so
# nothing qemu-test.sh left in this container is read either.
mkdir -p /var/config
rm -f /var/config/lastgood_hs.xml

# The Alloc-IDs each OLT assigned by PLOAM, in the shape /proc/odi_gpon
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

for isp in isp1 isp2; do
	log=/tmp/omcid-drv-$isp.log
	got=/tmp/omci-drv-$isp.txt
	want=$FIX/omci-drv-golden-$isp.txt
	cp "$FIX/omci-store-$isp.xml" /var/config/lastgood.xml

	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/gpon-$isp > "$log" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$isp.txt" > /tmp/inject-$isp.txt 2>&1
	wait "$pid"
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
		cp "$log" "/src/src/omci/drv-test-$isp.log" 2>/dev/null || true
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
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/gpon-isp1 > "$log" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	i=0
	while [ "$i" -lt 13 ]; do
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
	wait "$pid"
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
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/gpon-other > "$log" 2>&1 &
	pid=$!
	sleep 2
	{
		cat "$FIX/omci-session-isp1.txt"
		# Get 262/0x8000 and 0x8005, attribute 1 (AllocID).
		get_frame 0e30 0106 8000 8000
		get_frame 0e31 0106 8005 8000
	} > /tmp/session-other.txt
	$Q cli/build/omcli --inject-file /tmp/session-other.txt > /dev/null 2>&1
	wait "$pid"
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
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/gpon-none > "$log" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	wait "$pid"
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
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/gpon-isp2b > "$log" 2>&1 &
	pid=$!
	sleep 2
	{
		cat "$FIX/omci-session-isp2.txt"
		get_frame 0e40 0106 8000 8000
		get_frame 0e41 0106 8001 8000
	} > /tmp/session-isp2b.txt
	$Q cli/build/omcli --inject-file /tmp/session-isp2b.txt > /dev/null 2>&1
	wait "$pid"
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
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -g /tmp/gpon-isp2b > "$log" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	sleep 3
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp2.txt" > /dev/null 2>&1
	sleep 3
	map=$($Q cli/build/omcli tcont 2>&1 | grep -c '^  me [0-9a-f]* -> index')
	wait "$pid"
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
	$Q respond/build/omcid-drvtrace -a -w 3 -c "$CAPS" -g /tmp/gpon-move > "$log" 2>&1 &
	pid=$!
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
	sleep 3
	gpon /tmp/gpon-move "5 282 2000 1050 1306 538"
	sleep 4
	wait "$pid"
	if grep -q '^drv 21 8 00000001000007d0$' "$log" &&
	   grep -q '^event=alloc_ids count=5 ids=282,2000,1050,1306,538$' "$log"; then
		echo "ok    a reassigned Alloc-ID reprograms the T-CONT bound to it"
	else
		echo "FAIL  a reassigned Alloc-ID did not reach the driver"
		grep '^drv 21\|^event=alloc_ids' "$log"
		fail=$((fail + 1))
	fi
fi

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
exit "$fail"
