#!/bin/sh
# GEM port network CTP (class 268) attribute 8, Encryption state: answered
# from the enc_ports line of the driver status file (omcid -g here), which
# lists the GEM ports the downstream path decrypts.
#
# Covered: a status file with no enc_ports line (a kernel that does not
# publish it) reads 0; a port on the line reads 1 and one that is not reads
# 0; the line is read at every Get, so a port the OLT stops encrypting reads
# 0 at once and one it starts to encrypt reads 1.
#
# Run from src/omci, inside the toolchain image (make test-omci-enc):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh enc-test.sh
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
answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}
send() {   # send <log> <tci> <type> <class> <inst> <contents>
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# status <extra lines>: the driver status file as the kernel prints it, written
# by rename so omcid never reads half a file.
status() {
	printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n%s' "$1" > /tmp/enc-gpon.new
	mv /tmp/enc-gpon.new /tmp/enc-gpon
}

# enc <log> <tci> <inst>: attribute 8 of the CTP, one byte (contents: result
# code, attribute mask, the value).
enc() {
	send "$1" "$2" 49 010c "$3" 0100
	answer "$1" "$2" | cut -c7-8
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
status ""

log=/tmp/omcid-enc.log
: > "$log"
$Q respond/build/omcid -d -g /tmp/enc-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset

# Three CTPs, on GEM ports 0x0123 (291), 0x0456 (1110) and 0x0789 (1929).
send "$log" 0002 44 010c 0001 "012300000300000000"
send "$log" 0003 44 010c 0002 "045600000300000000"
send "$log" 0004 44 010c 0003 "078900000300000000"

check "no enc_ports line: unencrypted" "$(enc "$log" 0010 0001)" "00"

status "enc_ports 2 291 1110
"
check "a port on the line reads 1" "$(enc "$log" 0011 0001)" "01"
check "the second port on the line reads 1" "$(enc "$log" 0012 0002)" "01"
check "a port that is not on it reads 0" "$(enc "$log" 0013 0003)" "00"

status "enc_ports 0
"
check "the OLT stopped encrypting: 0 at the next Get" "$(enc "$log" 0014 0001)" "00"

status "enc_ports 1 1929
"
check "a port the OLT started to encrypt reads 1" "$(enc "$log" 0015 0003)" "01"
check "and the others stay 0" "$(enc "$log" 0016 0001)" "00"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" enc-test.log
if [ "$fail" -eq 0 ]; then echo "enc-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
