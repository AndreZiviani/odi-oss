#!/bin/sh
# Software download from the OLT (respond/swimage.c), both OLT_SW_DOWNLOAD
# modes, driven end to end under qemu: Start, windows of Download sections,
# End with the image CRC, Activate, Commit, and the software image flags the
# OLT reads back after each.
#
# The image is 100 bytes, 0x00 to 0x63, in windows of two sections: four
# sections, the last one padded. Its CRC-32 (ITU-T I.363.5, the OMCI trailer
# CRC: poly 0x04c11db7, not reflected, all ones in and out) is f2653e82,
# computed off the stick with the same algorithm. On the way: a window with a
# missing section (answered with a processing error, then sent again), the
# repeat of a window close already accepted (answered, not counted twice),
# a Start to the active image (refused), a download with a wrong CRC, and
# ISP2's own End software download to ONU-G (class 256, no AR), which is not
# a software image step.
#
# "Nothing touches flash, the environment or the boot": the accept run is
# made under qemu -strace, which logs every syscall omcid makes, and the log
# must hold no open of /dev/mtd, no execve (nv, reboot or anything else),
# no fork, clone or vfork, and no reboot syscall -- while it DOES hold the
# open of omcid own tmpfs state file, so a log that captured nothing cannot
# pass. The binary itself is checked for the names too.
#
# Run from src/omci, inside the toolchain image, after qemu-test.sh (the
# Makefile test-omci target runs them all):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh swdl-test.sh
set -u
Q=qemu-mips-static
fail=0

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

pad64() {
	s=$1
	while [ ${#s} -lt 64 ]; do s="${s}0"; done
	printf '%s' "$s"
}
# frame <tci> <type byte> <class> <inst> <contents hex>
frame() {
	printf '%s%s0a%s%s%s0000002800000000\n' "$1" "$2" "$3" "$4" "$(pad64 "$5")"
}
# The image bytes from offset $1, 31 of them, zero past the 100-byte image.
section_data() {
	i=$1
	end=$(($1 + 31))
	while [ "$i" -lt "$end" ]; do
		if [ "$i" -lt 100 ]; then printf '%02x' "$i"; else printf '00'; fi
		i=$((i + 1))
	done
}
# section <tci> <type byte> <inst> <number> <image offset>
section() {
	frame "$1" "$2" 0007 "$3" "$(printf '%02x' "$4")$(section_data "$5")"
}
getflags() {   # getflags <tci> <inst>: attributes 2-4 of the software image
	frame "$1" 49 0007 "$2" 7000
}
answer() {     # the logged answer to a tci, from the byte after the class
	grep -- "^-> $2" "$1" | tail -1 | cut -c4-
}

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

mkdir -p /var/config /var/run
rm -f /var/config/odi.conf /var/run/omcid-swimage /var/run/omcid-swimage.tmp \
      /var/run/omcid-mib.snap /var/run/omcid-resume-decision

# -------------------------------------------------------- the binary itself
for name in /dev/mtd sw_commit sw_tryactive fw_setenv /bin/nv; do
	got=$(grep -c -F "$name" respond/build/omcid)
	check "the omcid binary does not name $name" "$got" "0"
done

# -------------------------------------------------------- accept (default)
log=/tmp/omcid-swdl.log
trace=/tmp/omcid-swdl.strace
S=/tmp/swdl-accept.txt
{
	# ISP2 sends this at the start of its sessions: End software
	# download, no AR, to ONU-G. Not a software image step.
	frame 170a 15 0100 0000 ""
	getflags 0a01 0001
	# Start: window size 2 (sent as 1), 100 bytes, one image, ME 7/1.
	frame 0a02 53 0007 0001 0100000064010001
	getflags 0a03 0001
	# Window 1, complete.
	section 0a10 14 0001 0 0
	section 0a11 54 0001 1 31
	# Window 2 closes with its section 0 missing: refused.
	section 0a12 54 0001 1 93
	# Window 2 again, whole; then its close repeated, same tci.
	section 0a13 14 0001 0 62
	section 0a14 54 0001 1 93
	section 0a14 54 0001 1 93
	# End: the CRC and the size.
	frame 0a20 55 0007 0001 f2653e8200000064010001
	getflags 0a21 0001
	frame 0a22 56 0007 0001 00
	getflags 0a23 0000
	getflags 0a24 0001
	frame 0a25 57 0007 0001 ""
	getflags 0a26 0000
	getflags 0a27 0001
	# A download to the image now active and committed is refused.
	frame 0a30 53 0007 0001 0100000064010001
	# Image 0 is neither any more: one window, a wrong CRC.
	frame 0a31 53 0007 0000 000000001f010000
	section 0a32 54 0000 0 0
	frame 0a33 55 0007 0000 deadbeef0000001f010000
	getflags 0a34 0000
} > "$S"

$Q -strace respond/build/omcid -w 3 -c "$CAPS" > "$log" 2> "$trace" &
pid=$!
sleep 3
$Q cli/build/omcli --inject-file "$S" > /tmp/swdl-inject.txt 2>&1
wait "$pid"

got=$(grep -c "injected $(grep -c . "$S") frames" /tmp/swdl-inject.txt)
check "every frame of the accept session is injected" "$got" "1"

# Two ends reach the software image (image 1, image 0); the ONU-G one does
# not, and draws no event line.
got=$(grep -c '^event=sw_image op=download_end ' "$log")
check "ISP2 End download to ONU-G is not a software image step" "$got" "2"

check "before: image 1 is valid, not active, not committed" \
      "$(answer "$log" 0a01 | cut -c1-28)" "0a01290a00070001007000000001"
check "start is accepted, granting the whole window" \
      "$(answer "$log" 0a02 | cut -c1-20)" "0a02330a000700010001"
check "and image 1 reads invalid while it downloads" \
      "$(answer "$log" 0a03 | cut -c1-28)" "0a03290a00070001007000000000"
check "a whole window is acknowledged by its last section" \
      "$(answer "$log" 0a11 | cut -c1-20)" "0a11340a000700010001"
check "a window with a section missing is a processing error" \
      "$(answer "$log" 0a12 | cut -c1-20)" "0a12340a000700010101"
check "the resent window is acknowledged" \
      "$(answer "$log" 0a14 | cut -c1-20)" "0a14340a000700010001"
got=$(grep -c -- '^-> 0a14340a' "$log")
check "and its repeated close is answered again" "$got" "2"
check "end: the CRC matches, success" \
      "$(answer "$log" 0a20 | cut -c1-20)" "0a20350a000700010000"
got=$(grep '^event=sw_image op=download_end ' "$log" | head -1)
check "the end line counts four sections, not five or six" "$got" \
      "event=sw_image op=download_end inst=1 sections=4 crc=ok result=ok"
got=$(grep '^event=sw_image op=download_start ' "$log" | head -1)
check "the start line has the size and the window" "$got" \
      "event=sw_image op=download_start inst=1 size=100 window=2 result=ok"
check "after the end image 1 is valid again" \
      "$(answer "$log" 0a21 | cut -c1-28)" "0a21290a00070001007000000001"
check "activate is answered with success" \
      "$(answer "$log" 0a22 | cut -c1-18)" "0a22360a0007000100"
check "after activate: image 0 committed, not active" \
      "$(answer "$log" 0a23 | cut -c1-28)" "0a23290a00070000007000010001"
check "and image 1 active, not committed" \
      "$(answer "$log" 0a24 | cut -c1-28)" "0a24290a00070001007000000101"
check "commit is answered with success" \
      "$(answer "$log" 0a25 | cut -c1-18)" "0a25370a0007000100"
check "after commit: image 0 neither committed nor active" \
      "$(answer "$log" 0a26 | cut -c1-28)" "0a26290a00070000007000000001"
check "and image 1 committed and active" \
      "$(answer "$log" 0a27 | cut -c1-28)" "0a27290a00070001007000010101"
check "a download to the active image is refused" \
      "$(answer "$log" 0a30 | cut -c1-18)" "0a30330a0007000103"
check "a wrong CRC fails the end" \
      "$(answer "$log" 0a33 | cut -c1-18)" "0a33350a0007000001"
got=$(grep '^event=sw_image op=download_end inst=0 ' "$log")
check "and says so" "$got" \
      "event=sw_image op=download_end inst=0 sections=1 crc=bad result=crc_error"
check "and leaves image 0 invalid" \
      "$(answer "$log" 0a34 | cut -c1-28)" "0a34290a00070000007000000000"
got=$(grep -c '^event=sw_image op=\(activate\|commit\) inst=1 result=ok$' "$log")
check "activate and commit each log one line" "$got" "2"

# Nothing but omcid own files, and the proof the trace saw them.
got=$(grep -c '/var/run/omcid-swimage' "$trace")
check "the trace sees the state file written (it captures opens at all)" \
      "$([ "$got" -gt 0 ] && echo yes || echo "no ($got)")" "yes"
got=$(grep -c '/dev/mtd' "$trace")
check "no open of /dev/mtd" "$got" "0"
got=$(grep -c -E '(^|[^a-z_])(execve|fork|vfork|clone|reboot)\(' "$trace")
check "no execve, fork, clone or reboot syscall" "$got" "0"

# The flags hold across a respawn, for the rest of the boot.
$Q respond/build/omcid -w 2 -c "$CAPS" > /tmp/omcid-swdl2.log 2>&1 &
pid=$!
sleep 2
$Q cli/build/omcli --inject "$(getflags 0a40 0001)" > /dev/null 2>&1
wait "$pid"
check "a respawned omcid reports the flags of this boot" \
      "$(answer /tmp/omcid-swdl2.log 0a40 | cut -c1-28)" "0a40290a00070001007000010101"

# ---------------------------------------------------------------- reject
rm -f /var/run/omcid-swimage
echo "OLT_SW_DOWNLOAD=reject" > /var/config/odi.conf
log=/tmp/omcid-swdl-reject.log
R=/tmp/swdl-reject.txt
{
	frame 0b01 53 0007 0001 0100000064010001
	section 0b10 14 0001 0 0
	section 0b11 54 0001 1 31
	frame 0b20 55 0007 0001 f2653e8200000064010001
	frame 0b22 56 0007 0001 00
	frame 0b25 57 0007 0001 ""
	getflags 0b26 0000
	getflags 0b27 0001
} > "$R"
$Q respond/build/omcid -w 2 -c "$CAPS" > "$log" 2>&1 &
pid=$!
sleep 2
$Q cli/build/omcli --inject-file "$R" > /dev/null 2>&1
wait "$pid"
check "reject: start is not supported" "$(answer "$log" 0b01 | cut -c1-18)" "0b01330a0007000101"
check "reject: a window close is not supported" "$(answer "$log" 0b11 | cut -c1-18)" "0b11340a0007000101"
check "reject: end is not supported" "$(answer "$log" 0b20 | cut -c1-18)" "0b20350a0007000101"
check "reject: activate is not supported" "$(answer "$log" 0b22 | cut -c1-18)" "0b22360a0007000101"
check "reject: commit is not supported" "$(answer "$log" 0b25 | cut -c1-18)" "0b25370a0007000101"
got=$(grep '^event=sw_image ' "$log" | tr '\n' ';')
check "reject: every step logged not_supported, the sections counted" "$got" \
      "event=sw_image op=download_start inst=1 result=not_supported;event=sw_image op=download_end inst=1 sections=2 result=not_supported;event=sw_image op=activate inst=1 result=not_supported;event=sw_image op=commit inst=1 result=not_supported;"
check "reject: image 0 still committed, active and valid" \
      "$(answer "$log" 0b26 | cut -c1-28)" "0b26290a00070000007000010101"
check "reject: image 1 still only valid" \
      "$(answer "$log" 0b27 | cut -c1-28)" "0b27290a00070001007000000001"
rm -f /var/config/odi.conf /var/run/omcid-swimage

[ "$fail" -eq 0 ] || cp /tmp/omcid-swdl*.log /src/src/omci/ 2>/dev/null || true
if [ "$fail" -eq 0 ]; then echo "swdl-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
exit "$fail"
