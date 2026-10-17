#!/usr/bin/env bash
#
# image/fwu.sh write ORDER: both partitions erased first, the rootfs written,
# the kernel written LAST. U-Boot's bootm refuses a kernel that is erased or
# short (bad magic, bad data CRC), so "the kernel is the last thing on the
# slot" is what makes an interrupted flash unbootable rather than a valid new
# kernel over a missing rootfs.
#
# The partitions are plain files (DEV_DIR), flash_eraseall is a stub that
# logs and blanks its file to 0xff-looking 'E' bytes, so the run is host-only.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
FWU=$ROOT/image/fwu.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
SH=$(command -v sh)

B=$T/bin
mkdir -p "$B" "$T/img" "$T/dev"
# fwu.sh puts -O after the member name, which GNU and busybox tar accept and
# bsdtar (macOS) does not.
if tar --version 2>/dev/null | grep -q GNU; then
	ln -s "$(command -v tar)" "$B/tar"
elif p=$(command -v gtar); then
	ln -s "$p" "$B/tar"
else
	cat > "$B/tar" <<SHIM
#!/bin/sh
if [ "\$1" = -xf ] && [ "\$4" = -O ]; then exec $(command -v tar) -xOf "\$2" "\$3"; fi
exec $(command -v tar) "\$@"
SHIM
	chmod +x "$B/tar"
fi
for c in sed cksum cmp cat; do
	p=$(command -v "$c") || { echo "SKIP  host has no $c"; exit 0; }
	ln -s "$p" "$B/$c"
done
if p=$(command -v md5sum); then
	ln -s "$p" "$B/md5sum"
else
	printf '#!/bin/sh\nexec md5 -r\n' > "$B/md5sum"; chmod +x "$B/md5sum"
fi
cat > "$B/nv" <<'STUB'
#!/bin/sh
[ "$1" = getenv ] || exit 1
case "$2" in sw_commit) echo sw_commit=0 ;; esac
exit 0
STUB
# The stub erase: log the call, then leave the file holding only 'E' bytes, so
# a later "the kernel was never written" is visible as content, not absence.
cat > "$B/flash_eraseall" <<STUB
#!/bin/sh
echo "ERASE \$1" >> "$T/log"
[ -d "\$1" ] || printf 'EEEE' > "\$1"
exit 0
STUB
chmod +x "$B/nv" "$B/flash_eraseall"

cat > "$T/mtd" <<'EOF2'
dev:    size   erasesize  name
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
EOF2
echo "console=ttyS0,115200 root=31:5" > "$T/cmdline"

printf 'KERNEL-IMAGE-BYTES' > "$T/img/uImage"
printf 'ROOTFS-IMAGE-BYTES-LONGER' > "$T/img/rootfs"
( cd "$T/img" && tar -cf ../good.tar uImage rootfs &&
  for m in uImage rootfs; do "$B/md5sum" < "$m" | sed "s/ .*/  $m/"; done > md5.txt )

run() {
	( cd "$T/img" && env -i PATH="$B" PROC_MTD="$T/mtd" PROC_CMDLINE="$T/cmdline" \
	    DEV_DIR="$T/dev" "$SH" "$FWU" 1 "$T/good.tar" 2>&1 )
}
t() {   # t <name> <want> <got>
	if [ "$2" = "$3" ]; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        want [$2]"; echo "        got  [$3]"; fail=$((fail + 1)); fi
}

# 1. A whole run: the order of the steps, then the bytes.
: > "$T/log"
out=$(run); rc=$?
t "a full run exits 0" "0" "$rc"
order=
while IFS= read -r line; do
	case $line in
	"fwu: erasing "*) order="${order}erase ${line##*/}," ;;
	"fwu: writing "*) order="${order}write ${line##*/}," ;;
	esac
done <<EOF_OUT
$out
EOF_OUT
t "erase kernel, erase rootfs, write rootfs, write kernel" \
  "erase mtd6,erase mtd7,write mtd7,write mtd6," "$order"
t "the kernel partition holds the kernel" "KERNEL-IMAGE-BYTES" "$(cat "$T/dev/mtd6")"
t "the rootfs partition holds the rootfs" "ROOTFS-IMAGE-BYTES-LONGER" "$(cat "$T/dev/mtd7")"

# 2. The rootfs write fails (its device is a directory): the kernel must still
#    be erased, never written.
rm -f "$T/dev/mtd6" "$T/dev/mtd7"; mkdir "$T/dev/mtd7"; : > "$T/log"
out=$(run); rc=$?
t "a failed rootfs write exits 1" "1" "$rc"
t "the kernel partition was erased and NOT written" "EEEE" "$(cat "$T/dev/mtd6")"
if printf '%s' "$out" | grep -q 'WRITE FAILED on .*mtd7 -- slot 1 is now UNBOOTABLE'; then
	echo "ok    the failure names the slot"; pass=$((pass + 1))
else
	echo "FAIL  the failure does not name the slot:"; printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
fi
rmdir "$T/dev/mtd7"

# 3. The rootfs erase fails: nothing is written at all, the kernel is erased.
printf '#!/bin/sh\necho "ERASE $1" >> "%s/log"\n[ "$1" = "%s/dev/mtd7" ] && exit 1\nprintf EEEE > "$1"\nexit 0\n' "$T" "$T" > "$B/flash_eraseall"
rm -f "$T/dev/mtd6" "$T/dev/mtd7"; : > "$T/log"
out=$(run); rc=$?
t "a failed rootfs erase exits 1" "1" "$rc"
t "the kernel partition is erased" "EEEE" "$(cat "$T/dev/mtd6")"
t "the rootfs partition was never written" "no" "$([ -e "$T/dev/mtd7" ] && echo yes || echo no)"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
