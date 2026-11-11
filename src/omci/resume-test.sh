#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

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
# Then two more: a wide row comes back whole, and an omcid killed between
# the answer to a Set and the rebuild that programs it leaves that rebuild
# to the respawned one.
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
	: > "$before_log"
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
	: > "$after_log"
	$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -s 5 -g /tmp/resume-gpon-$isp > "$after_log" 2>&1 &
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
	# The first later rebuild must withdraw every restored service before
	# adding rules. MIB reset is independent of the stored VLAN mode.
	# Every drv 50 in this log is counted, so the check holds only while
	# the resumed omcid sends none before the rebuild: it is started
	# without -r, whose clear of all 256 ids would land here too, and the
	# check above has just found no driver call at all.
	n=$(rebuilt "$after_log")
	inject_wait "$after_log" "$(printf '7f004f0a00020000%064d00000028%08d' 0 0)"
	wait_for "$after_log" 'bridge connections rebuilt' 30 $((n + 1))
	want_ids=$(printf '%s\n' "$conn_before" | sed -n 's/^SERVID \([0-9]*\):.*/\1/p' | sort -n)
	got_ids=$(sed -n 's/^drv 50 4 //p' "$after_log" | while read -r id; do printf '%d\n' "$((0x$id))"; done | sort -n)
	if [ -n "$want_ids" ] && [ "$got_ids" = "$want_ids" ]; then
		echo "ok    $isp: later MIB reset withdrew exactly the restored service ids"
	else
		echo "FAIL  $isp: later reset did not withdraw the restored service id set"
		fail=$((fail + 1))
	fi
	omcid_stop "$pid"
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
: > "$wlog1"
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

: > "$wlog2"
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

# ---------------------------------------- a rebuild pending at the kill
# A Set of a GEM port network CTP, which the bridge connections and the
# upstream QoS are both built from, is answered and saved at once; the
# rebuilds that program it run a quiet second later.
# Killed inside that second, omcid leaves a snapshot whose MIB is ahead of
# the switch. The snapshot says which rebuilds are pending (format 7), so the
# respawned omcid runs them after its own quiet second, with nothing
# injected; and the snapshot it saves after them has none pending.
#
# The kill has to land inside the quiet second. A machine slow enough for
# the rebuild to run first leaves nothing pending, so that try is repeated.
cp "$FIX/omci-store-isp2.xml" /var/config/lastgood.xml
olog1=/tmp/omcid-resume-pending-before.log
olog2=/tmp/omcid-resume-pending-after.log
olog3=/tmp/omcid-resume-pending-third.log
# Set of GEM port network CTP 2, attribute 1 (the Port-ID), to the value it has.
pending_set=$(printf '9001480a010c0002%s%056d00000028%08d' 80000291 0 0)
try=0
inside=0
while [ "$try" -lt 5 ] && [ "$inside" = 0 ]; do
	try=$((try + 1))
	rm -f /var/run/omcid-mib.snap /var/run/omcid-resume-decision
	: > "$olog1"
	$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -g /tmp/resume-gpon-isp2 > "$olog1" 2>&1 &
	pid=$!
	omcid_up "$olog1"
	$Q cli/build/omcli --inject-file "$FIX/omci-session-isp2.txt" > /dev/null 2>&1
	settled "$olog1"
	n=$(rebuilt "$olog1")
	inject_wait "$olog1" "$pending_set"
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	[ "$(rebuilt "$olog1")" = "$n" ] && inside=1
done
if [ "$inside" = 1 ]; then
	echo "ok    pending rebuild: omcid killed between the answer and the rebuild (try $try)"
else
	echo "FAIL  pending rebuild: the kill did not land inside the quiet second in $try tries"
	fail=$((fail + 1))
fi

: > "$olog2"
$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -s 5 -g /tmp/resume-gpon-isp2 > "$olog2" 2>&1 &
pid=$!
omcid_up "$olog2"
if grep -q 'resume: onu state 5, snapshot loaded' "$olog2"; then
	echo "ok    pending rebuild: the respawned instance took the resume path"
else
	echo "FAIL  pending rebuild: the respawned instance did not take the resume path"
	sed -n '1,20p' "$olog2"
	fail=$((fail + 1))
fi
if grep -q 'resume: a rebuild was pending (bridge connections, upstream QoS)' "$olog2"; then
	echo "ok    pending rebuild: the snapshot says both rebuilds are pending"
else
	echo "FAIL  pending rebuild: the resumed omcid knows of no pending rebuild"
	fail=$((fail + 1))
fi
# wait_for counts a timeout as a failed check and says what it waited for.
if wait_for "$olog2" 'bridge connections rebuilt' 30; then
	echo "ok    pending rebuild: the bridge connections are rebuilt after the resume, with nothing injected"
fi
# The QoS arm of the loop runs before the bridge one, in the same pass.
if grep -q '\] us qos' "$olog2"; then
	echo "ok    pending rebuild: and the upstream QoS"
else
	echo "FAIL  pending rebuild: no upstream QoS rebuild after the resume"
	fail=$((fail + 1))
fi
# The reply to an omcli request means the loop pass with the rebuild, and
# the save after it, is done.
$Q cli/build/omcli help > /dev/null 2>&1
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null

: > "$olog3"
$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -s 5 -g /tmp/resume-gpon-isp2 > "$olog3" 2>&1 &
pid=$!
omcid_up "$olog3"
if grep -q 'resume: onu state 5, snapshot loaded' "$olog3" &&
   ! grep -q 'a rebuild was pending' "$olog3"; then
	echo "ok    pending rebuild: the snapshot saved after the rebuild has none pending"
else
	echo "FAIL  pending rebuild: the snapshot saved after the rebuild still has one pending, or was not resumed"
	sed -n '1,20p' "$olog3"
	fail=$((fail + 1))
fi
omcid_stop "$pid"

echo
[ "$fail" -eq 0 ] && echo "resume-test.sh: all checks passed" || echo "resume-test.sh: $fail check(s) failed"
suite_exit
