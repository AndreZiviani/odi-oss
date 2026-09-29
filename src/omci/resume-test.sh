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
# Run from src/omci, inside the toolchain image, after drv-test.sh:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh resume-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0

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
	sleep 2
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$isp.txt" > /tmp/resume-inject-$isp.txt 2>&1
	frames=$(grep -c '^[0-9a-f]' "$FIX/omci-session-$isp.txt")
	if ! grep -q "injected $frames frames" /tmp/resume-inject-$isp.txt; then
		echo "FAIL  $isp: omcli did not inject all $frames frames"
		cat /tmp/resume-inject-$isp.txt
		kill -9 "$pid" 2>/dev/null
		fail=$((fail + 1))
		continue
	fi
	sleep 2                                  # let the 1 s rebuilds settle

	mib_before=$($Q cli/build/omcli mib get all 2>&1)
	conn_before=$($Q cli/build/omcli conn 2>&1)
	flows_before=$($Q cli/build/omcli flows 2>&1)

	# 2/3: kill -9 -- no signal handler, no chance to clean up, exactly
	# what a respawn follows.
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null

	# 4: a fresh instance, -s 5 standing in for "PON is O5" (qemu has no
	# netlink to ask the driver over). -w 1: it may idle-exit once our
	# three dumps are done, the same as a drv-test.sh run.
	$Q respond/build/omcid-drvtrace -a -w 1 -c "$CAPS" -s 5 -g /tmp/resume-gpon-$isp > "$after_log" 2>&1 &
	pid=$!
	sleep 2

	if ! grep -q 'resume: onu state 5, snapshot loaded' "$after_log"; then
		echo "FAIL  $isp: the respawned instance did not take the resume path"
		sed -n '1,20p' "$after_log"
		fail=$((fail + 1))
		wait "$pid" 2>/dev/null
		continue
	fi

	mib_after=$($Q cli/build/omcli mib get all 2>&1)
	conn_after=$($Q cli/build/omcli conn 2>&1)
	flows_after=$($Q cli/build/omcli flows 2>&1)
	wait "$pid" 2>/dev/null

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

echo
[ "$fail" -eq 0 ] && echo "resume-test.sh: all checks passed" || echo "resume-test.sh: $fail check(s) failed"
exit "$fail"
