#!/bin/sh
# What a Create, Set or Delete does to the MIB and to MIB data sync, against
# G.988: a request the OLT repeats (same transaction id, same frame) is
# answered again with the same answer and changes nothing; a Create of an
# instance that exists is refused with result 7; a Set that names a read-only
# or set-by-create attribute fails with result 9, the failed attributes in the
# answer, and stores none of them.
#
# Run from src/omci, inside the toolchain image (make test-omci-writes):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh writes-test.sh
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

# frame <tci> <type> <class> <inst> <contents hex>: one baseline frame, the
# contents padded with zeros to 32 bytes; trailer, zero CRC (not checked).
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}

# answer <log> <tci>: the contents of the last answer to that tci (64 hex).
answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}

send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# mds <log> <tci>: the MIB data sync the ONU reports (a Get of ONT data).
mds() {
	send "$1" "$2" 49 0002 0000 8000
	answer "$1" "$2" | cut -c7-8
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/writes-gpon

log=/tmp/omcid-writes.log
$Q respond/build/omcid -d -g /tmp/writes-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset
check "after a MIB reset the sync is 0" "$(mds "$log" 0002)" "00"

# ------------------------------------------------ Create, and its repeat
# GemPortCtp (268) instance 5: port id, T-CONT pointer, direction, upstream
# traffic manager, upstream descriptor, downstream queue. The UNI counter is
# read-only and is not in a Create.
GEM=01230000030000000000000000
send "$log" 0010 44 010c 0005 "$GEM"
check "a Create is answered ok" "$(answer "$log" 0010 | cut -c1-2)" "00"
check "and counts once" "$(mds "$log" 0011)" "01"
first=$(answer "$log" 0010)
send "$log" 0010 44 010c 0005 "$GEM"
check "the same frame again gets the same answer" "$(answer "$log" 0010)" "$first"
check "and is logged as a repeat" \
      "$(grep -c 'retransmission, answer repeated' "$log")" "1"
check "the repeat does not count again" "$(mds "$log" 0012)" "01"

# A different request for the instance that exists is a Create of an
# existing instance: result 7, no count.
send "$log" 0013 44 010c 0005 "$GEM"
check "a new Create of an existing instance is refused with result 7" \
      "$(answer "$log" 0013 | cut -c1-2)" "07"
check "and does not count" "$(mds "$log" 0014)" "01"

# ------------------------------------------- Set: read-only attributes
# Attributes 1 (port id, writable) and 6 (UNI counter, read-only), mask 8400.
send "$log" 0020 48 010c 0005 "84000456aa"
check "a Set naming a read-only attribute fails with result 9, the failed mask 0400" \
      "$(answer "$log" 0020 | cut -c1-10)" "0900000400"
send "$log" 0021 49 010c 0005 "8400"
check "the writable one was stored and the read-only one was not" \
      "$(answer "$log" 0021 | cut -c1-12)" "008400045600"
check "the Set that stored something counts" "$(mds "$log" 0022)" "02"
reply=$(answer "$log" 0020)
send "$log" 0020 48 010c 0005 "84000456aa"
check "a repeated Set gets the same answer" "$(answer "$log" 0020)" "$reply"
check "and does not count again" "$(mds "$log" 0023)" "02"
send "$log" 0024 48 010c 0005 "0400aa"
check "a Set of only a read-only attribute fails with result 9" \
      "$(answer "$log" 0024 | cut -c1-10)" "0900000400"
check "and, storing nothing, does not count" "$(mds "$log" 0025)" "02"
send "$log" 0026 48 010c 0005 "8000789a"
check "a Set of writable attributes alone is ok, with no failed mask" \
      "$(answer "$log" 0026 | cut -c1-2)" "00"
check "and counts" "$(mds "$log" 0027)" "03"

# Set-by-create only: CircuitPack (6) Type is written at Create and never by
# a Set; AdminState (attribute 6) next to it is writable.
send "$log" 0030 48 0006 0101 "84000501"
check "a Set of a set-by-create attribute fails, the writable one beside it does not" \
      "$(answer "$log" 0030 | cut -c1-10)" "0900008000"
send "$log" 0031 49 0006 0101 "0400"
check "AdminState was stored" "$(answer "$log" 0031 | cut -c1-8)" "00040001"

# ------------------------------------------------------ Delete and repeat
send "$log" 0040 46 010c 0005 ""
check "a Delete is answered ok" "$(answer "$log" 0040 | cut -c1-2)" "00"
before=$(mds "$log" 0041)
send "$log" 0040 46 010c 0005 ""
check "a repeated Delete is answered again" "$(answer "$log" 0040 | cut -c1-2)" "00"
check "and does not count again" "$(mds "$log" 0042)" "$before"

# A MIB reset starts the OLT's transaction ids over: the same Create as the
# first one must run, not be answered from the cache.
send "$log" 0050 4f 0002 0000 ""
send "$log" 0010 44 010c 0005 "$GEM"
check "after a MIB reset the same frame is a new request" "$(mds "$log" 0051)" "01"

omcid_stop "$pid"
[ "$fail" -eq 0 ] || keep_log "$log" writes-test.log
if [ "$fail" -eq 0 ]; then echo "writes-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
