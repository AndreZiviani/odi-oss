#!/bin/sh
# FEC performance monitoring history data (class 312) on omcid's 15-minute
# interval framework (respond/pm.c).
#
# The counters come in through -n, a file in the shape of
# /proc/odi_gpon/counters, and the interval clock through -t, a file holding
# seconds, so a test moves 15 minutes by writing a number.
#
# Covered: Create as the OLT does it (threshold data id only); a Get before
# anything happens; Get current data against Get (the interval in progress
# against the completed one); each attribute following its driver counter
# (corrected bytes, corrected code words, uncorrectable code words; total code
# words stays 0); FEC seconds counting only seconds with new uncorrectable
# code words; the interval ending on the 15-minute mark and Interval end time
# stepping; two intervals passing unobserved; Synchronize time moving the
# boundary to the OLT's quarter hour and clearing the counts; saturation at
# 32 bits; a second entity with its own baseline; Delete, MIB reset and a new
# Create starting from zero.
#
# Run from src/omci, inside the toolchain image (make test-omci-pm):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh pm-test.sh
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

frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}
send() {   # send <tci> <type> <class> <inst> <contents>
	inject_wait "$log" "$(frame "$1" "$2" "$3" "$4" "$5")"
}
answer() { grep -- "^-> $1" "$log" | tail -1 | cut -c20-83; }

# A fresh transaction id each call; a file, so a call inside $(...) counts too.
rm -f /tmp/pm-n
tci() {
	_n=$(cat /tmp/pm-n 2>/dev/null || echo 0)
	_n=$((_n + 1))
	echo "$_n" > /tmp/pm-n
	printf '%04x' "$_n"
}

# ctrs <cor bytes> <cor cw> <uncor cw> [total cw]: the driver totals. Written
# by rename, so omcid never reads half a file. The total code words line is
# only there when given, as with a driver that does not publish it.
ctrs() {
	printf 'ctr_samples 9\nctr ds_fec_corrected_bits 0\nctr ds_fec_corrected_bytes %s\nctr ds_fec_corrected_cw %s\nctr ds_fec_uncorrectable_cw %s\nctr ds_superframe_loss 0\n' \
		"$1" "$2" "$3" > /tmp/pm-ctr.new
	[ -z "${4:-}" ] || printf 'ctr ds_fec_total_cw %s\n' "$4" >> /tmp/pm-ctr.new
	mv /tmp/pm-ctr.new /tmp/pm-ctr
}
clock() { printf '%s\n' "$1" > /tmp/pm-clk.new; mv /tmp/pm-clk.new /tmp/pm-clk; }

# get <inst> <hist|cur>: attributes 1 and 3-7 of class 312 as hex: interval
# end time (1 byte), corrected bytes, corrected code words, uncorrectable code
# words, total code words (4 each), FEC seconds (2).
get() {
	_t=$(tci)
	if [ "$2" = cur ]; then _ty=5c; else _ty=49; fi
	send "$_t" "$_ty" 0138 "$1" be00
	answer "$_t" | cut -c7-44
}
Z="00000000"
CLS=0138
I1=8001
I2=8002

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/pm-gpon
ctrs 1000 100 5
clock 1000

log=/tmp/omcid-pm.log
: > "$log"
$Q respond/build/omcid -d -g /tmp/pm-gpon -n /tmp/pm-ctr -t /tmp/pm-clk > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$(tci)" 4f 0002 0000 ""                      # MIB reset, as the OLT starts

# Create as an OLT does: the threshold data id, nothing else.
send "$(tci)" 44 $CLS $I1 000a
check "Get after Create: interval 0, every count 0" \
      "$(get $I1 hist)" "00${Z}${Z}${Z}${Z}0000"

# 100 bytes in 10 code words corrected, 3 uncorrectable, in one second.
ctrs 1100 110 8
clock 1001
check "Get current data: the three counts since Create, FEC seconds 1" \
      "$(get $I1 cur)" "00$(printf '%08x%08x%08x' 100 10 3)${Z}0001"
check "Get: the completed interval is still empty" \
      "$(get $I1 hist)" "00${Z}${Z}${Z}${Z}0000"
T=$(tci)
send "$T" 49 $CLS $I1 4000
check "the threshold data id is the OLT's, untouched" "$(answer "$T" | cut -c7-10)" "000a"

# The interval ends at 1000 + 900.
clock 1900
check "interval end: Get returns the counts, interval end time 1" \
      "$(get $I1 hist)" "01$(printf '%08x%08x%08x' 100 10 3)${Z}0001"
check "interval end: Get current data starts again from 0" \
      "$(get $I1 cur)" "01${Z}${Z}${Z}${Z}0000"

# Corrections only: a working FEC, not a FEC second.
ctrs 1200 115 8
clock 1901
check "corrected-only second: counted in bytes and code words, not in FEC seconds" \
      "$(get $I1 cur)" "01$(printf '%08x%08x' 100 5)${Z}${Z}0000"

# Synchronize time, 10:07:30: the next boundary is 7.5 minutes away (10:15:00),
# and every count restarts. Contents: year 2026, month 10, day 20, 10:07:30.
send "$(tci)" 58 0100 0000 "07ea0a140a071e"
check "Synchronize time: interval end time and history cleared" \
      "$(get $I1 hist)" "00${Z}${Z}${Z}${Z}0000"
ctrs 1300 120 9
clock 2000
check "after the sync: counts accumulate again, one FEC second" \
      "$(get $I1 cur)" "00$(printf '%08x%08x%08x' 100 5 1)${Z}0001"
clock 2350
check "one second before 10:15:00 the interval is still open" \
      "$(get $I1 hist)" "00${Z}${Z}${Z}${Z}0000"
clock 2351
check "on 10:15:00 it ends: the 10:07:30-10:15:00 counts" \
      "$(get $I1 hist)" "01$(printf '%08x%08x%08x' 100 5 1)${Z}0001"
clock 3251
check "the next boundary is 15 minutes later: interval end time 2, empty" \
      "$(get $I1 hist)" "02${Z}${Z}${Z}${Z}0000"

# A second entity is created now: its own baseline, its own interval numbers.
ctrs 1400 130 9
clock 3300
send "$(tci)" 44 $CLS $I2 000b
ctrs 1450 131 9
clock 3301
check "a second entity counts from its own Create" \
      "$(get $I2 cur)" "00$(printf '%08x%08x' 50 1)${Z}${Z}0000"
check "the first entity is unaffected by the second" \
      "$(get $I1 cur)" "02$(printf '%08x%08x' 150 11)${Z}${Z}0000"

# Two intervals pass with nobody looking: the older counts cannot be told
# apart and are dropped; the interval number still advances by two.
ctrs 1500 140 9
clock 5200
check "two intervals unobserved: interval end time +2, the completed one empty" \
      "$(get $I1 hist)" "04${Z}${Z}${Z}${Z}0000"

# Saturation: a count above 32 bits holds at the most the attribute holds.
ctrs 9000000000 140 9
clock 5201
check "a count past 32 bits stops at 4294967295" \
      "$(get $I1 cur)" "04ffffffff${Z}${Z}${Z}0000"

# Delete, and a new Create: back to interval 0 and a fresh baseline.
send "$(tci)" 46 $CLS $I2 ""
send "$(tci)" 44 $CLS $I2 000b
check "Delete then Create: interval 0 and nothing carried over" \
      "$(get $I2 hist)" "00${Z}${Z}${Z}${Z}0000"

# A MIB reset removes the entity and its history.
send "$(tci)" 4f 0002 0000 ""
send "$(tci)" 44 $CLS $I1 000a
check "MIB reset then Create: interval 0 and nothing carried over" \
      "$(get $I1 hist)" "00${Z}${Z}${Z}${Z}0000"
ctrs 9000000010 141 9
clock 5202
check "after the reset the baseline is the Create's" \
      "$(get $I1 cur)" "00$(printf '%08x%08x' 10 1)${Z}${Z}0000"

# Total code words: the driver's ds_fec_total_cw, counted per interval like
# the rest. One second of frames with FEC on is 8000 x 153 = 1224000 (0x12ad40).
ctrs 9000000010 141 9 5000000
clock 5203
send "$(tci)" 4f 0002 0000 ""
send "$(tci)" 44 $CLS $I1 000a
ctrs 9000000010 141 9 6224000
clock 5204
check "total code words: the count since Create, one second of frames" \
      "$(get $I1 cur)" "00${Z}${Z}${Z}0012ad400000"
ctrs 9000000010 141 9 99000000000
clock 5205
check "total code words: a count past 32 bits stops at 4294967295" \
      "$(get $I1 cur)" "00${Z}${Z}${Z}ffffffff0000"

# An entity that does not exist answers nothing it was not given.
check "a Get of an instance that was never created: zeros" \
      "$(get 8009 hist)" "00${Z}${Z}${Z}${Z}0000"

# A Get whose attributes do not all fit the 25 bytes of values: GEM port PM
# history data (class 267) asked for attributes 1 and 3-7, as an OLT polls it,
# is 1 + 4 + 4 + 8 + 8 + 8 = 33 bytes. Result 9: the first five (25 bytes)
# under the mask answered, attribute 7 in the attribute execution mask
# (contents 30-31), none in the optional-attribute mask (28-29).
send "$(tci)" 44 010b 0001 000a
T=$(tci)
send "$T" 49 010b 0001 be00
A=$(answer "$T")
check "Get past 25 bytes: result 9, mask answered bc00" "$(echo "$A" | cut -c1-6)" "09bc00"
check "Get past 25 bytes: attribute 7 failed, none unsupported" "$(echo "$A" | cut -c57-64)" "00000200"
T=$(tci)
send "$T" 49 010b 0001 0200
check "the attribute that failed, asked alone: ok" "$(answer "$T" | cut -c1-6)" "000200"
# Attribute 16 is past the class's last: unsupported, the rest answered.
T=$(tci)
send "$T" 49 010b 0001 8001
A=$(answer "$T")
check "an attribute the class does not have: result 9, the rest answered" \
      "$(echo "$A" | cut -c1-6)" "098000"
check "and it is in the optional-attribute mask, not the failed one" \
      "$(echo "$A" | cut -c57-64)" "00010000"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" pm-test.log
if [ "$fail" -eq 0 ]; then echo "pm-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
