#!/usr/bin/env bash
#
# image/fwu.sh with ONLY the tools it declares on PATH, run through the size
# check and the md5 check of both members.
#
# fwu.sh is flashed from whichever stock firmware the stick came with, and
# those differ: the V1.0-220923 image has `cut`, the HSGQ M110 has none, and a
# user had to patch the script to get a first flash done. test/fwu_guard_test.sh
# runs the script with the host's full PATH, and the allowlist in
# fwu_stock_commands_test.sh describes ONE stock image, so neither could see
# it. Here the PATH holds tar, sed, cksum, md5sum and cmp and nothing else
# (no cut, head, tail, tr, wc, awk, dd, seq, expr, timeout), plus two stubs: nv
# and a flash_eraseall that always fails, so the run ends at the first erase
# without writing anything.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
FWU=$ROOT/image/fwu.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
SH=$(command -v sh)

B=$T/bin
mkdir -p "$B" "$T/img"
# fwu.sh puts -O after the member name, which busybox and GNU tar accept and
# bsdtar (macOS) does not: stand a reordering shim in front of bsdtar.
if tar --version 2>/dev/null | grep -q GNU; then
	ln -s "$(command -v tar)" "$B/tar"
elif p=$(command -v gtar); then
	ln -s "$p" "$B/tar"
else
	cat > "$B/tar" <<SHIM
#!/bin/sh
# tar -xf <file> <member> -O  ->  tar -xOf <file> <member>
if [ "\$1" = -xf ] && [ "\$4" = -O ]; then exec $(command -v tar) -xOf "\$2" "\$3"; fi
exec $(command -v tar) "\$@"
SHIM
	chmod +x "$B/tar"
fi
for c in sed cksum cmp; do
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
printf '#!/bin/sh\necho "flash_eraseall $*" >&2\nexit 1\n' > "$B/flash_eraseall"
chmod +x "$B/nv" "$B/flash_eraseall"

cat > "$T/mtd" <<'EOF2'
dev:    size   erasesize  name
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
EOF2
echo "console=ttyS0,115200 root=31:5" > "$T/cmdline"

# Two members of known size, and the md5.txt the upgrade path unpacks.
head -c 5000 /dev/zero | tr '\0' 'k' > "$T/img/uImage"
head -c 70000 /dev/zero | tr '\0' 'r' > "$T/img/rootfs"
( cd "$T/img" && tar -cf ../good.tar uImage rootfs &&
  for m in uImage rootfs; do "$B/md5sum" < "$m" | sed "s/ .*/  $m/"; done > md5.txt )

run() {   # run <tarball> [PATH]
	( cd "$T/img" && env -i PATH="${2:-$B}" PROC_MTD="$T/mtd" PROC_CMDLINE="$T/cmdline" \
	    "$SH" "$FWU" 1 "$1" 2>&1 )
}
t() {   # t <name> <regex> <output>
	if printf '%s' "$3" | grep -q -- "$2"; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        wanted /$2/, got:"; printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1)); fi
}

# The harness must itself prove that cut is gone, or it proves nothing.
if PATH=$B command -v cut >/dev/null 2>&1; then
	echo "FAIL  the test PATH still has cut"; exit 1
fi
echo "ok    the test PATH has no cut"

out=$(run "$T/good.tar")
t "sizes uImage without cut"   "fwu: uImage 5000 of 1359872 bytes" "$out"
t "sizes rootfs without cut"   "fwu: rootfs 70000 of 2572288 bytes" "$out"
t "md5 of uImage without cut"  "fwu: uImage ok ([0-9a-f]\{32\})" "$out"
t "md5 of rootfs without cut"  "fwu: rootfs ok ([0-9a-f]\{32\})" "$out"
t "reaches the erase"          "flash_eraseall /dev/mtd6" "$out"

if printf '%s' "$out" | grep -q 'not found'; then
	echo "FAIL  something on the PATH was not found:"; printf '%s\n' "$out" | sed 's/^/        /'
	fail=$((fail + 1))
else
	echo "ok    nothing was not found"; pass=$((pass + 1))
fi

# A corrupt member is still caught (the md5 parse is not just a pass-through).
sed 's/^[0-9a-f]\{32\}/00000000000000000000000000000000/' "$T/img/md5.txt" > "$T/img/md5.bad"
mv "$T/img/md5.txt" "$T/img/md5.good"; mv "$T/img/md5.bad" "$T/img/md5.txt"
t "a wrong md5 is still refused" "uImage md5 mismatch" "$(run "$T/good.tar")"
mv "$T/img/md5.good" "$T/img/md5.txt"

# An image that is too big for the partition is refused before any erase.
head -c 1400000 /dev/zero | tr '\0' 'k' > "$T/img/uImage"
( cd "$T/img" && tar -cf ../big.tar uImage rootfs )
t "an oversize member is refused" "REFUSING" "$(run "$T/big.tar")"

# A tool the script needs and the image lacks is named up front.
rm "$B/cmp"
t "a missing cmp is named before anything runs" "lacks: cmp" "$(run "$T/good.tar")"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
