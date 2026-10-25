#!/bin/sh
# The resume golden: omcid killed mid-session comes back answering exactly
# as before, with zero switch-programming driver calls.
#
# docs/BOOT.md, "Resume without re-registration": a respawned omcid, PON
# still O5, with a valid snapshot for this device, loads it and resumes --
# no re-registration, no reprogramming (the datapath is already in
# hardware). This is the same driver-call golden as drv-test.sh (omcid
# built with OMCI_DRV_TRACE, every driver call logged and answered with
# success), replayed against a KILLED AND RESPAWNED omcid instead of a
# fresh one:
#
#   1. inject a whole ISP session into a first instance, as drv-test.sh does;
#   2. capture `omcli mib get all`, `conn` and `flows` -- the MIB and the
#      state that maps managed entities to switch programming;
#   3. kill -9 it;
#   4. start a second instance with -s 5 (an ONU-state override, for
#      testability -- see the -c flag in main.c for the same pattern with
#      the capability blob; there is no netlink under qemu to ask the
#      driver for real);
#   5. capture the same three dumps from the second instance, with NOTHING
#      re-injected;
#   6. the dumps must be byte-identical, and the driver-call log of that
#      second instance must hold not one `drv ` line -- the switch was
#      never touched.
#
# Run from src/omci, inside the toolchain image (make test-omci-resume):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh resume-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0
. ./testlib.sh

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

mkdir -p /var/config /var/run
rm -f /var/config/lastgood_hs.xml /var/run/omcid-mib.snap /var/run/omcid-mib.snap.tmp \
      /var/run/omcid-resume-decision

# The PLOAM Alloc-IDs of each line, as drv-test.sh gives them (omcid -g):
# the T-CONT bindings are part of what a resumed instance must answer.
printf 'alloc_ids 5 282 794 1050 1306 538\n' > /tmp/resume-gpon-isp1
printf 'alloc_ids 1 348\n' > /tmp/resume-gpon-isp2

for isp in isp1 isp2; do
	cp "$FIX/omci-store-$isp.xml" /var/config/lastgood.xml

	before_log=/tmp/omcid-resume-$isp-before.log
	after_log=/tmp/omcid-resume-$isp-after.log

	# 1: a whole session into a first, daemon-mode instance (-d: no idle
	# exit -- this one has to still be up when we kill it).
	$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -g /tmp/resume-gpon-$isp > "$before_log" 2>&1 &
	pid=$!
	omcid_up "$before_log"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$isp.txt" > /tmp/resume-inject-$isp.txt 2>&1
	frames=$(grep -c '^[0-9a-f]' "$FIX/omci-session-$isp.txt")
	if ! grep -q "injected $frames frames" /tmp/resume-inject-$isp.txt; then
		echo "FAIL  $isp: omcli did not inject all $frames frames"
		cat /tmp/resume-inject-$isp.txt
		kill -9 "$pid" 2>/dev/null
		fail=$((fail + 1))
		continue
	fi
	settled "$before_log"                    # the 1 s rebuilds have run

	# SysUpTime is live: it is the one line two instances cannot share.
	mib_before=$($Q cli/build/omcli mib get all 2>&1 | grep -v '^    SysUpTime ')
	conn_before=$($Q cli/build/omcli conn 2>&1)
	flows_before=$($Q cli/build/omcli flows 2>&1)

	# 2/3: kill -9 -- no signal handler, no chance to clean up, exactly
	# what a respawn follows.
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null

	# 4: a fresh instance, -s 5 standing in for "PON is O5" (qemu has no
	# netlink to ask the driver over). -w 6 is only a backstop: it is stopped
	# once our three dumps are done.
	$Q respond/build/omcid-drvtrace -a -w 6 -c "$CAPS" -s 5 -g /tmp/resume-gpon-$isp > "$after_log" 2>&1 &
	pid=$!
	omcid_up "$after_log"

	if ! grep -q 'resume: onu state 5, snapshot loaded' "$after_log"; then
		echo "FAIL  $isp: the respawned instance did not take the resume path"
		sed -n '1,20p' "$after_log"
		fail=$((fail + 1))
		omcid_stop "$pid"
		continue
	fi

	mib_after=$($Q cli/build/omcli mib get all 2>&1 | grep -v '^    SysUpTime ')
	conn_after=$($Q cli/build/omcli conn 2>&1)
	flows_after=$($Q cli/build/omcli flows 2>&1)
	omcid_stop "$pid"

	# Plain temp files, not <(...): this runs under dash inside the
	# toolchain image, which has no process substitution.
	if [ "$mib_before" = "$mib_after" ]; then
		echo "ok    $isp: mib get all identical across the resume"
	else
		echo "FAIL  $isp: mib get all differs after resume"
		printf '%s\n' "$mib_before" > /tmp/resume-$isp-mib-before.txt
		printf '%s\n' "$mib_after" > /tmp/resume-$isp-mib-after.txt
		diff /tmp/resume-$isp-mib-before.txt /tmp/resume-$isp-mib-after.txt | head -40
		fail=$((fail + 1))
	fi
	if [ "$conn_before" = "$conn_after" ]; then
		echo "ok    $isp: conn identical across the resume"
	else
		echo "FAIL  $isp: conn differs after resume"
		printf '%s\n' "$conn_before" > /tmp/resume-$isp-conn-before.txt
		printf '%s\n' "$conn_after" > /tmp/resume-$isp-conn-after.txt
		diff /tmp/resume-$isp-conn-before.txt /tmp/resume-$isp-conn-after.txt | head -40
		fail=$((fail + 1))
	fi
	if [ "$flows_before" = "$flows_after" ]; then
		echo "ok    $isp: flows identical across the resume"
	else
		echo "FAIL  $isp: flows differs after resume"
		printf '%s\n' "$flows_before" > /tmp/resume-$isp-flows-before.txt
		printf '%s\n' "$flows_after" > /tmp/resume-$isp-flows-after.txt
		diff /tmp/resume-$isp-flows-before.txt /tmp/resume-$isp-flows-after.txt | head -40
		fail=$((fail + 1))
	fi

	drv_calls=$(grep -c '^drv ' "$after_log")
	if [ "$drv_calls" = 0 ]; then
		echo "ok    $isp: zero switch-programming driver calls on resume"
	else
		echo "FAIL  $isp: $drv_calls driver call(s) on resume -- the switch was touched"
		grep '^drv ' "$after_log" | head -20
		fail=$((fail + 1))
	fi
done

# ---------------------------------------------------- a wide row resumes whole
# A large string and an authentication method keep their later attributes in
# the side pool; the snapshot carries them (format 5), so a respawned omcid
# still answers part 15 and username 2, not the zeros of a row cut at 96.
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}
amask() { printf '%04x' $((1 << (16 - $1))); }
aval() {
	_b=$(printf '%02x' $((0x10 + $1)))
	_s=""; _n=0
	while [ "$_n" -lt 25 ]; do _s="$_s$_b"; _n=$((_n + 1)); done
	printf '%s' "$_s"
}

cp "$FIX/omci-store-isp2.xml" /var/config/lastgood.xml
rm -f /var/run/omcid-mib.snap /var/run/omcid-resume-decision
wlog1=/tmp/omcid-resume-wide-before.log
wlog2=/tmp/omcid-resume-wide-after.log
$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -g /tmp/resume-gpon-isp2 > "$wlog1" 2>&1 &
pid=$!
omcid_up "$wlog1"
inject_wait "$wlog1" "$(frame 0001 44 009d 0001 "")"
inject_wait "$wlog1" "$(frame 0002 44 0094 0002 "")"
inject_wait "$wlog1" "$(frame 0003 48 009d 0001 "$(amask 16)$(aval 16)")"
inject_wait "$wlog1" "$(frame 0004 48 0094 0002 "$(amask 5)$(aval 5)")"
# the snapshot is saved before each answer, so the last answer is enough
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null

$Q respond/build/omcid-drvtrace -a -w 20 -c "$CAPS" -s 5 -g /tmp/resume-gpon-isp2 > "$wlog2" 2>&1 &
pid=$!
omcid_up "$wlog2"
if grep -q 'resume: onu state 5, snapshot loaded' "$wlog2"; then
	echo "ok    wide rows: the respawned instance took the resume path"
else
	echo "FAIL  wide rows: the respawned instance did not take the resume path"
	sed -n '1,20p' "$wlog2"
	fail=$((fail + 1))
fi
inject_wait "$wlog2" "$(frame 0011 49 009d 0001 "$(amask 16)")"
inject_wait "$wlog2" "$(frame 0012 49 0094 0002 "$(amask 5)")"
got=$(grep -- '^-> 0011' "$wlog2" | tail -1 | cut -c20-75)
if [ "$got" = "00$(amask 16)$(aval 16)" ]; then
	echo "ok    wide rows: part 15 of the large string survives the resume"
else
	echo "FAIL  wide rows: part 15 of the large string after resume: $got"
	fail=$((fail + 1))
fi
got=$(grep -- '^-> 0012' "$wlog2" | tail -1 | cut -c20-75)
if [ "$got" = "00$(amask 5)$(aval 5)" ]; then
	echo "ok    wide rows: username 2 survives the resume"
else
	echo "FAIL  wide rows: username 2 after resume: $got"
	fail=$((fail + 1))
fi
omcid_stop "$pid"

echo
[ "$fail" -eq 0 ] && echo "resume-test.sh: all checks passed" || echo "resume-test.sh: $fail check(s) failed"
suite_exit
