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

for isp in isp1 isp2; do
	log=/tmp/omcid-drv-$isp.log
	got=/tmp/omci-drv-$isp.txt
	want=$FIX/omci-drv-golden-$isp.txt
	cp "$FIX/omci-store-$isp.xml" /var/config/lastgood.xml

	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" > "$log" 2>&1 &
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
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" > "$log" 2>&1 &
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

if [ "$fail" -eq 0 ]; then echo "all ok"; else echo "FAILED ($fail)"; fi
exit "$fail"
