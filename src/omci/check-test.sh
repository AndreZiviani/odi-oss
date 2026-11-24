#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# omcli check (respond/apply_qos.c, qos_check): the MIB against the chip
# state the kernel publishes in /proc/odi_gpon/ponq and /proc/odi_gpon/cam.
# qemu has no kernel of ours, so both files are written here, next to the
# status file -g names, in the shapes odi_switch_ponq_show() and
# odi_gpon_proc_cam_show() print.
#
# Covered: no chip state at all; one T-CONT, its queue and its GEM port in
# both directions matching; and each kind of difference: the Alloc-ID CAM
# row, the T-CONT enable bit, the queue mask, the queue rate, the upstream
# flow (valid bit, GEM port, queue), the downstream GEM CAM row, and a rate
# the OLT sets after the chip was read.
#
# Run from src/omci, inside the toolchain image (make test-omci-check):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh check-test.sh
set -u
Q=qemu-mips-static
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

D=/tmp/omci-check
rm -rf "$D"
mkdir -p "$D" /var/config
# OMCI_GEM_US_RATE absent until the traffic descriptor case below.
rm -f /var/config/odi.conf
# The kernel's PLOAM list: one Alloc-ID, 1024.
printf 'state 5 (O5)\nalloc_ids 1 1024\n' > "$D/status"

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

: > /tmp/omcid-check.log
$Q respond/build/omcid -w 12 -c "$CAPS" -g "$D/status" > /tmp/omcid-check.log 2>&1 &
pid=$!
omcid_up /tmp/omcid-check.log

# Nothing provisioned yet (the ONU in O2): our kernel prints no ponq row,
# and the cam holds only its two summary lines. That is an empty chip, not
# an unreadable one; with nothing in the MIB either there is nothing to
# compare.
NOTHING="nothing provisioned yet: the OLT has set up no T-CONT or GEM port"
: > "$D/ponq"
printf 'alloc_cam_valid 0\ngem_cam_valid 0\n' > "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | tail -1)
check "empty chip, nothing provisioned: says so" "$got" "$NOTHING"
$Q cli/build/omcli check > /dev/null 2>&1
check "and the status is ok" "$?" "0"
NOREAD="the chip state is not readable (/proc/odi_gpon/ponq and cam: not our kernel, or an older one)"
printf 'gem_cam_valid 0\n' > "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | head -1)
check "a cam without both summary lines: not our kernel" "$got" "$NOREAD"
got=$($Q cli/build/omcli check 2>&1 | tail -1)
check "with status 3" "$got" "% command failed, status 3"
printf 'alloc_cam_valid 0\ngem_cam_valid 0\n' > "$D/cam"
rm -f "$D/ponq"; mkdir "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | head -1)
check "a ponq that cannot be read" "$got" "$NOREAD"
rmdir "$D/ponq"
: > "$D/ponq"; rm -f "$D/cam"; mkdir "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | head -1)
check "a cam that cannot be read" "$got" "$NOREAD"
rmdir "$D/cam"
rm -f "$D/ponq" "$D/cam"

# T-CONT 0x8000 Set to Alloc-ID 1024, then GEM CTP 0x0002: port 1562, on
# that T-CONT, both directions, upstream queue 0x8006 (the frames of
# qemu-test.sh).
for f in 0001440a010680000000000000000000000000000000000000000000000000000000000000000000000000283056d14e \
         0002480a010680008000040000000000000000000000000000000000000000000000000000000000000000288068e530 \
         0003440a010c0002061a80000380060000000000000000000000000000000000000000000000000000000028063a1fc9; do
	inject_wait /tmp/omcid-check.log "$f"
done
settled /tmp/omcid-check.log

got=$($Q cli/build/omcli check 2>&1 | head -1)
check "no chip state: says so, compares nothing" "$got" \
"the chip state is not readable (/proc/odi_gpon/ponq and cam: not our kernel, or an older one)"

# The same empty chip while omcid has set things up: every object differs.
: > "$D/ponq"
printf 'alloc_cam_valid 0\ngem_cam_valid 0\n' > "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | tail -1)
check "empty chip, provisioned MIB: count" "$got" "0 ok, 4 differ"
got=$($Q cli/build/omcli check 2>&1 | grep -c differs)
check "every object shows as differing" "$got" "4"
rm -f "$D/ponq" "$D/cam"

# The chip as the driver leaves it for that provisioning: T-CONT index 0
# (Alloc-ID CAM row 0) with logical queue 0, strict and unlimited, flow 0
# on it, GEM CAM row 0 for the downstream flow; the OMCC's rows as the
# replays leave them.
cam() {
	printf 'alloc_cam 0 hw %s recorded 1024\nalloc_cam 16 hw 26 recorded 26\nalloc_cam_valid 2\n' "$1"
	printf 'gem_cam 0 hw %s type 0x02\ngem_cam 64 hw 26 type 0x03\ngem_cam_valid 2\n' "$2"
}
ponq() {
	printf 'tcont 0 en %s qmap %s block 0+1\ntcont 16 en 1 qmap 0x00000001 block -\n' "$1" "$2"
	printf 'queue 0 wfq 0 weight 1 cir 0 pir %s\nqueue 64 wfq 0 weight 1 cir 0 pir 262143\n' "$3"
	printf 'flow 0 valid %s gem %s queue %s recorded 1\nflow 64 valid 1 gem 0 queue 64 recorded 0\n' \
		"$4" "$5" "$6"
}
cam 1024 1562 > "$D/cam"
ponq 1 0x00000001 262143 1 1562 0 > "$D/ponq"

ALL_OK="programming is off: omcid sent nothing, so differences are expected
tcont me 8000 alloc 1024 index 0: ok
queue me 8006 tcont 0 ordinal 0 (queue 0): ok
us gem 1562 (ctp 0002) flow 0: ok
ds gem 1562 flow 0: ok
4 ok, 0 differ"
got=$($Q cli/build/omcli check 2>&1)
check "chip matches the MIB: every object ok" "$got" "$ALL_OK"

cam 1030 1562 > "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | grep '^tcont')
check "another Alloc-ID in the T-CONT's CAM row" "$got" \
      "tcont me 8000 alloc 1024 index 0: differs: cam row 0 holds 1030"
cam - 1562 > "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | grep '^tcont')
check "nothing in the T-CONT's CAM row" "$got" \
      "tcont me 8000 alloc 1024 index 0: differs: cam row 0 holds nothing"
cam 1024 1562 > "$D/cam"

ponq 0 0x00000001 262143 1 1562 0 > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^tcont')
check "T-CONT not enabled" "$got" \
      "tcont me 8000 alloc 1024 index 0: differs: t-cont not enabled in the chip"

ponq 1 0x00000000 262143 1 1562 0 > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "queue not in the T-CONT's mask" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): differs: not in the t-cont's queue mask"

ponq 1 0x00000001 99 1 1562 0 > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "a rate limit the MIB does not ask for" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): differs: mib cir 0 pir 262143, chip cir 0 pir 99 (register units)"

ponq 1 0x00000001 262143 0 1562 0 > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^us gem')
check "upstream flow not valid" "$got" \
      "us gem 1562 (ctp 0002) flow 0: differs: not valid in the chip"
ponq 1 0x00000001 262143 1 1563 0 > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^us gem')
check "upstream flow on another GEM port" "$got" \
      "us gem 1562 (ctp 0002) flow 0: differs: chip maps the flow to gem 1563"
ponq 1 0x00000001 262143 1 1562 63 > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^us gem')
check "upstream flow on another queue" "$got" \
      "us gem 1562 (ctp 0002) flow 0: differs: mib queue 0, chip queue 63"
got=$($Q cli/build/omcli check 2>&1 | tail -1)
check "the count" "$got" "3 ok, 1 differ"
ponq 1 0x00000001 262143 1 1562 0 > "$D/ponq"

cam 1024 - > "$D/cam"
got=$($Q cli/build/omcli check 2>&1 | grep '^ds gem')
check "downstream GEM CAM row empty" "$got" \
      "ds gem 1562 flow 0: differs: gem cam row 0 holds nothing"
cam 1024 1562 > "$D/cam"

# Traffic descriptor 0x0001 (class 280): CIR 128000 and PIR 256000 byte/s,
# then the CTP's upstream descriptor pointer (attribute 5) at it. Without
# OMCI_GEM_US_RATE=1 the rebuild leaves the queue unlimited and says so,
# and check expects no limit either.
inject_wait /tmp/omcid-check.log "0f40440a01180001$(printf '0001f4000003e800%064d' 0)"
inject_wait /tmp/omcid-check.log "0f41480a010c0002$(printf '08000001%072d' 0)"
settled /tmp/omcid-check.log
got=$(grep -c 'us traffic descriptors of 1 GEM ports not applied (OMCI_GEM_US_RATE not 1)' /tmp/omcid-check.log)
check "a traffic descriptor is not applied by default, and the log says so" "$got" "1"
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "and check expects the queue unlimited" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): ok"
# With OMCI_GEM_US_RATE=1 (read by check at once, by omcid at its next
# rebuild): the chip, read before the rebuild could reach it, still holds
# no limit; the MIB asks for 125 and 250 (byte/s / 1024), 124 and 249 in
# the register.
printf 'OMCI_GEM_US_RATE=1\n' > /var/config/odi.conf
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "a traffic descriptor the chip does not have yet" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): differs: mib cir 124 pir 249, chip cir 0 pir 262143 (register units)"
ponq 1 0x00000001 249 1 1562 0 | sed 's/^queue 0 wfq 0 weight 1 cir 0 /queue 0 wfq 0 weight 1 cir 124 /' > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "and once it has" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): ok"

# A CTP on T-CONT 0x8001, which has no Alloc-ID (the PLOAM list has one,
# and 0x8000 takes it): the rebuild sets up no flow for it, so check lists
# it as skipped rather than as a difference.
inject_wait /tmp/omcid-check.log "0f50440a010c0005$(printf '06a48001038009%066d' 0)"
settled /tmp/omcid-check.log
got=$($Q cli/build/omcli check 2>&1 | grep '^us gem 1700')
check "a CTP omcid set up no flow for is skipped" "$got" \
      "us gem 1700 (ctp 0005): not set up by omcid, skipped"
got=$($Q cli/build/omcli check 2>&1 | tail -1)
check "and counted apart" "$got" "4 ok, 1 differ, 1 skipped"
# Its downstream flow omcid did set up, and the GEM CAM written here has
# no row for it.
got=$($Q cli/build/omcli check 2>&1 | grep '^ds gem 1700' | sed 's/flow [0-9]*/flow N/; s/row [0-9]*/row N/')
check "while its downstream flow is checked" "$got" \
      "ds gem 1700 flow N: differs: gem cam row N holds nothing"

# A CTP whose priority queue 0x8010 hangs off T-CONT 0x8001 while the CTP
# names T-CONT 0x8000: the rebuild puts its flow on a queue of 0x8000 with
# the same type and priority (us_queue_bound()), and check expects it
# there too.
inject_wait /tmp/omcid-check.log "0f51480a01158010040080010001$(printf '%068d' 0)"
inject_wait /tmp/omcid-check.log "0f52440a010c0006$(printf '06ae8000038010%066d' 0)"
settled /tmp/omcid-check.log
grep 'gem 1710 us flow' /tmp/omcid-check.log | tail -1
slot=$($Q cli/build/omcli flows 2>&1 | awk '$3 == 1710 { print $1 }')
ponq 1 0x00000001 249 1 1562 0 | sed 's/^queue 0 wfq 0 weight 1 cir 0 /queue 0 wfq 0 weight 1 cir 124 /' > "$D/ponq"
printf 'flow %s valid 1 gem 1710 queue 0 recorded 1\n' "$slot" >> "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^us gem 1710' | sed 's/ flow [0-9]*/ flow N/')
check "a flow bound to a stand-in queue is ok" "$got" \
      "us gem 1710 (ctp 0006) flow N: ok"

omcid_stop "$pid"
rm -rf "$D" /var/config/odi.conf
suite_exit
