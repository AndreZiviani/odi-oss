#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# omcli frames: the last OMCI frames as bytes, as text or as a pcap
# (respond/frames.c). What a frame from the OLT and an answer to it look
# like there, what the sharing mask hides (the values of the classes omcid
# keeps out of its log, the serial number and the MACs of the stick, the
# MACs learned on the LAN side, the contents of a frame that is not
# baseline) and what it leaves (headers, masks, result codes, every other
# attribute), --full, the pcap layout, and the ring wrapping at its size.
#
# Run from src/omci, inside the toolchain image (make test-omci-frames):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh frames-test.sh
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
# contents padded with zeros to 32 bytes; trailer, zero CRC (omcli --inject
# fills it in).
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}

send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# frames [--full]: the frame lines of omcli frames, without the time.
frames() {
	$Q cli/build/omcli frames "$@" 2>&1 | sed -n 's/^[0-9]*\.[0-9]\{6\} //p'
}

# line <dir> <tci+type>: the hex of the last frame with that direction and
# that start, in the output of frames (from stdin).
line() {
	grep -- "^$1 $2" | tail -1 | cut -c4-
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/frames-gpon

log=/tmp/omcid-frames.log
: > "$log"
$Q respond/build/omcid -d -g /tmp/frames-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"

got=$($Q cli/build/omcli frames 2>&1)
check "with no frame yet, an empty list" "$got" "0 frames, masked, 0 since start"

send "$log" 0001 4f 0002 0000 ""                         # MIB reset

# ------------------------------------------------ a request and its answer
out=$(frames --full)
check "the request from the OLT, as it came, CRC included" \
      "$(printf '%s\n' "$out" | line '<-' 00014f | cut -c1-16)" "00014f0a00020000"
check "the answer to it, with AK" \
      "$(printf '%s\n' "$out" | line '->' 00012f | cut -c1-16)" "00012f0a00020000"
check "48 bytes each" \
      "$(printf '%s\n' "$out" | line '<-' 00014f | wc -c)" "97"
crc=$(printf '%s\n' "$out" | line '<-' 00014f | cut -c81-96)
check "the CRC is the one the frame carried, not zero" \
      "$(test "$crc" != 0000000000000000 && echo set)" "set"

# ------------------------------------------- ONU-G: the serial number only
# A Get of ONU-G (256) attributes 1 (vendor id) and 3 (serial number).
send "$log" 0002 49 0100 0000 a000
full=$(frames --full | line '->' 0002290a)
masked=$(frames | line '->' 0002290a)
check "--full: vendor id and serial number in the answer" \
      "$(printf '%s' "$full" | cut -c17-46)" "00a0004f4449544f44495400000001"
check "masked: the vendor id stays, the serial number does not" \
      "$(printf '%s' "$masked" | cut -c17-46)" "00a0004f444954eeeeeeeeeeeeeeee"
check "masked: header and trailer unchanged, original CRC removed" \
      "$(printf '%s' "$masked" | cut -c1-16):$(printf '%s' "$masked" | cut -c81-96)" \
      "$(printf '%s' "$full" | cut -c1-16):0000002800000000"
check "masking leaves the captured frame intact for --full" \
      "$(frames --full | line '->' 0002290a)" "$full"
onug_full_frame=$full
onug_masked_frame=$masked

# --------------------------------- large string: a class kept out of the log
# Create large string (157) instance 1, then Set part 1 (attribute 2) to a
# URL with a login in it, then Get it back.
URL=687474703a2f2f757365723a7365637265744061637300000000   # http://user:secret@acs
send "$log" 0003 44 009d 0001 01
send "$log" 0004 48 009d 0001 "4000$URL"
send "$log" 0005 49 009d 0001 4000
full=$(frames --full)
masked=$(frames)
check "--full: the Set carries the URL" \
      "$(printf '%s\n' "$full" | line '<-' 0004480a | cut -c21-42)" "687474703a2f2f75736572"
check "masked: the Set keeps its attribute mask and nothing after it" \
      "$(printf '%s\n' "$masked" | line '<-' 0004480a | cut -c17-80)" \
      "4000eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
check "masked: the Create of the class is masked whole" \
      "$(printf '%s\n' "$masked" | line '<-' 0003440a | cut -c17-80)" \
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
check "masked: the Get answer keeps result and mask, not the value" \
      "$(printf '%s\n' "$masked" | line '->' 0005290a | cut -c17-30)" "004000eeeeeeee"
check "the secret is nowhere in the masked list" \
      "$(printf '%s\n' "$masked" | grep -c 7365637265)" "0"

for spec in '<- 0003440a' '<- 0004480a' '-> 0005290a'; do
	set -- $spec
	check "masked: CRC removed from $spec" \
	      "$(printf '%s\n' "$masked" | line "$1" "$2" | cut -c89-96)" "00000000"
done

# ------------------------------------- the bridge table: learned LAN MACs
# A Create of MAC bridge port config data (47) instance 1 brings its bridge
# table data (50) with it. A Get-Next of that table (attribute 1) answers
# the MACs the switch learned on the port, the router's among them: none
# here, without a switch, but the answer is masked by class, not by what it
# holds.
send "$log" 0010 44 002f 0001 ""
send "$log" 0011 5a 0032 0001 80000000
full=$(frames --full | line '->' 00113a0a)
masked=$(frames | line '->' 00113a0a)
check "--full: the Get-Next answer of the bridge table, as sent" \
      "$(printf '%s' "$full" | cut -c1-16)" "00113a0a00320001"
check "masked: header and result stay, the table contents do not" \
      "$(printf '%s' "$masked" | cut -c1-18):$(printf '%s' "$masked" | cut -c23-80)" \
      "$(printf '%s' "$full" | cut -c1-18):eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

# ------------------------------------------------------ MIB upload next
send "$log" 0006 4d 0002 0000 ""                         # MIB upload
n=$(frames --full | line '->' 00062d0a | cut -c17-20)
i=0
while [ "$i" -lt "$((0x$n))" ]; do
	send "$log" "$(printf '%04x' $((0x100 + i)))" 4e 0002 0000 "$(printf '%04x' "$i")"
	i=$((i + 1))
done
# The upload-next answers that describe ONU-G (class 0100 in bytes 8-9) and
# carry its serial number (attribute 3, in mask bytes 12-13).
onug_full=$(frames --full | grep -- '^-> ....2e0a00020000' | cut -c4- |
	    grep '^.\{16\}0100' | grep -c 4f44495400000001)
onug_masked=$(frames | grep -- '^-> ....2e0a00020000' | cut -c4- |
	      grep '^.\{16\}0100' | grep -c 4f44495400000001)
check "--full: an upload-next answer for ONU-G carries the serial" \
      "$(test "$onug_full" -ge 1 && echo yes)" "yes"
check "masked: no upload-next answer carries it" "$onug_masked" "0"
# The circuit packs carry the same serial number (attribute 3).
check "masked: no frame at all carries it" \
      "$(frames | grep -c 4f44495400000001)" "0"

# ------------------------------------------- a frame that is not baseline
raw=$(frame 0007 49 0100 0000 a000 | sed 's/^\(......\)0a/\10b/')
$Q cli/build/omcli --inject-raw "$raw" > /dev/null 2>&1
sync_send "$log"
check "a dropped frame is kept too, extended device id and all" \
      "$(frames --full | line '<-' 0007490b | cut -c1-20)" "0007490b01000000a000"
check "masked: everything after its header" \
      "$(frames | line '<-' 0007490b | cut -c17-24)" "eeeeeeee"

# ------------------------------------------------------------------ pcap
pc=/tmp/frames.pcap
rm -f "$pc"
$Q cli/build/omcli frames --pcap "$pc" > /tmp/frames-pcap.out 2>&1
check "omcli frames --pcap FILE prints nothing, status 0" \
      "$?:$(cat /tmp/frames-pcap.out)" "0:"
nf=$($Q cli/build/omcli frames | tail -1 | cut -d' ' -f1)
check "the file is a header and one 78-byte record per frame" \
      "$(wc -c < "$pc" | tr -d ' ')" "$((24 + nf * 78))"
hex=$(od -An -v -tx1 "$pc" | tr -d ' \n')
check "big-endian magic, version 2.4, snaplen 65535, Ethernet" \
      "$(printf '%s' "$hex" | cut -c1-48)" "a1b2c3d40002000400000000000000000000ffff00000001"
check "first record: 62 bytes, OLT to ONU, EtherType 0x88b5, then the frame" \
      "$(printf '%s' "$hex" | cut -c65-124)" \
      "0000003e0000003e020000000002020000000001""88b5""00014f0a00020000"
check "second record: ONU to OLT" \
      "$(printf '%s' "$hex" | cut -c237-260)" "020000000001020000000002"
check "masked like the text: no serial, no secret" \
      "$(printf '%s' "$hex" | grep -c -e 4f44495400000001 -e 7365637265)" "0"
check "pcap carries the masked serial frame with its CRC removed" \
      "$(printf '%s' "$hex" | grep -F -c "$onug_masked_frame")" "1"
$Q cli/build/omcli frames --pcap "$pc" --full > /dev/null 2>&1
hex=$(od -An -v -tx1 "$pc" | tr -d ' \n')
check "--pcap with --full: the serial is there" \
      "$(printf '%s' "$hex" | grep -c 4f44495400000001)" "1"
check "--full pcap preserves the original serial frame and CRC" \
      "$(printf '%s' "$hex" | grep -F -c "$onug_full_frame")" "1"
got=$($Q cli/build/omcli frames --pcap 2>&1); rc=$?
check "--pcap without a file is refused" "$rc:$got" "1:% --pcap needs a file"
$Q cli/build/omcli frames --pcap /nonexistent/dir/x.pcap > /dev/null 2>&1
check "a file that cannot be written: status 1" "$?" "1"
$Q cli/build/omcli frames --bogus > /dev/null 2>&1
check "an unknown argument: status 1" "$?" "1"

# --------------------------------------------------------- the ring wraps
# 1100 Gets of ONT data, answered: 2200 more frames, over the 2048 kept. In
# two files: omcli --inject-file reads at most 64 KB of one.
i=0
for f in /tmp/frames-batch1 /tmp/frames-batch2; do
	: > "$f"
	n=$((i + 550))
	while [ "$i" -lt "$n" ]; do
		frame "$(printf '%04x' $((0x2000 + i)))" 49 0002 0000 8000 >> "$f"
		echo >> "$f"
		i=$((i + 1))
	done
	$Q cli/build/omcli --inject-file "$f" > /dev/null 2>&1
done
sync_send "$log"
last=$($Q cli/build/omcli frames | tail -1)
check "the ring keeps the last 2048" "$(printf '%s' "$last" | cut -d' ' -f1-3)" "2048 frames, masked,"
check "and counts every one" \
      "$(test "$(printf '%s' "$last" | cut -d' ' -f4)" -gt 2048 && echo yes)" "yes"
check "the newest is the last line" \
      "$(frames | tail -1 | cut -c1-13)" "-> ff02290a01"
check "oldest first" \
      "$($Q cli/build/omcli frames | head -1 | sed 's/^[0-9]*\.[0-9]* //' | cut -c1-3)" "<- "

# ----------------------------------------------------------- diag bundle
B=../../rootfs/skeleton/etc/scripts/diag-bundle.sh
check "diag-bundle.sh asks for the pcap" \
      "$(grep -v '^[[:space:]]*#' "$B" | grep -c 'omcli frames --pcap')" "1"
check "and never for the unmasked one" \
      "$(grep -v '^[[:space:]]*#' "$B" | grep 'omcli frames' | grep -c -e '--full')" "0"
# The text scrub never runs over it: it is captured outside the bundle
# and one place moves it in, after the scrub and before tar
# (test/diag_summary_test.sh checks the bytes).
check "and captures it outside the bundle" \
      "$(grep -v '^[[:space:]]*#' "$B" | grep -c 'omcli frames --pcap "\$work/omci.pcap"')" "1"
check "and one place puts it into the bundle" \
      "$(grep -v '^[[:space:]]*#' "$B" | grep -c '"\$b/pon/omci.pcap"')" "1"
check "after the text scrub and before tar" \
      "$(awk '/sed -f "\$script" "\$f"/ { s = NR } /mv "\$pcap" "\$b\/pon\/omci.pcap"/ { m = NR } /tar -czf/ { t = NR }
	      END { print (s && s < m && m < t) ? "yes" : "no" }' "$B")" "yes"

omcid_stop "$pid"
[ "$fail" -eq 0 ] || keep_log "$log" frames-test.log
if [ "$fail" -eq 0 ]; then echo "frames-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
