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
mkdir -p "$D"
# The kernel's PLOAM list: one Alloc-ID, 1024.
printf 'state 5 (O5)\nalloc_ids 1 1024\n' > "$D/status"

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

: > /tmp/omcid-check.log
$Q respond/build/omcid -w 12 -c "$CAPS" -g "$D/status" > /tmp/omcid-check.log 2>&1 &
pid=$!
omcid_up /tmp/omcid-check.log

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
# then the CTP's upstream descriptor pointer (attribute 5) at it. The
# chip, read before the rebuild could reach it, still holds no limit; the
# MIB asks for 125 and 250 (byte/s / 1024), 124 and 249 in the register.
inject_wait /tmp/omcid-check.log "0f40440a01180001$(printf '0001f4000003e800%064d' 0)"
inject_wait /tmp/omcid-check.log "0f41480a010c0002$(printf '08000001%072d' 0)"
settled /tmp/omcid-check.log
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "a traffic descriptor the chip does not have yet" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): differs: mib cir 124 pir 249, chip cir 0 pir 262143 (register units)"
ponq 1 0x00000001 249 1 1562 0 | sed 's/^queue 0 wfq 0 weight 1 cir 0 /queue 0 wfq 0 weight 1 cir 124 /' > "$D/ponq"
got=$($Q cli/build/omcli check 2>&1 | grep '^queue')
check "and once it has" "$got" \
      "queue me 8006 tcont 0 ordinal 0 (queue 0): ok"

omcid_stop "$pid"
rm -rf "$D"
suite_exit
