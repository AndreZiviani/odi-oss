#!/usr/bin/env bash
# The uImage header, against the one U-Boot on the stick already boots.
#
# QEMU never sees this: it boots vmlinux directly. On the board `b0` is
# `bootm ${img0_kernel}` from the memory-mapped NOR window, and bootm reads
# load address, entry point, OS, architecture, type and compression out of the
# 64-byte header and refuses anything it does not like -- a wrong compression
# byte or a load address it will not honour is "Bad Magic" or "Wrong Image
# Format" on a serial console nobody is watching, and the trial reverts having
# taught nothing. So every field is compared to the STOCK kernel taken off a
# stick, byte for byte, and the payload is held under the partition size.
#
# Reference: a stock k0 partition dump (mtd4), passed as REF=<file>. Its
# header is what this U-Boot has booted hundreds of times. Without REF the
# reference values are pinned below, read from such a dump once.
set -u
cd "$(dirname "$0")/.." || exit 1

IMG=${IMG:-build/kernel/uImage}
REF=${REF:-}
KERNEL_PART=1359872   # 1328K, k0 and k1 alike

pass=0; fail=0
ok()  { printf 'ok    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

[ -s "$IMG" ] || { echo "no uImage at $IMG -- run image/build.sh" >&2; exit 1; }

hdr() {
	python3 - "$1" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(64)
magic, hcrc, t, size, load, ep, dcrc, os_, arch, typ, comp = struct.unpack('>IIIIIIIBBBB', b[:32])
print(f"magic={magic:08x} size={size} load={load:08x} ep={ep:08x} os={os_} arch={arch} type={typ} comp={comp}")
PY
}
ours=$(hdr "$IMG")
if [ -s "$REF" ]; then
	theirs=$(hdr "$REF"); src="the isp1 k0 dump"
else
	# Pinned from that dump on 2026-09-15, so the test still runs without it.
	theirs="magic=27051956 size=874247 load=80000000 ep=80000000 os=5 arch=5 type=2 comp=3"
	src="the pinned reference"
fi
field() { printf '%s\n' "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"; }

for f in magic load ep os arch type comp; do
	o=$(field $f "$ours"); t=$(field $f "$theirs")
	if [ "$o" = "$t" ]; then ok "$f matches stock ($o)"
	else bad "$f is $o, stock is $t (from $src)"; fi
done

size=$(field size "$ours")
total=$((size + 64))
actual=$(wc -c < "$IMG" | tr -d ' ')
if [ "$total" = "$actual" ]; then ok "header size field + 64 equals the file ($actual bytes)"
else bad "header says $size + 64 but the file is $actual bytes"; fi
if [ "$actual" -le "$KERNEL_PART" ]; then ok "fits the 1328K kernel partition ($((KERNEL_PART - actual)) spare)"
else bad "does NOT fit the kernel partition: $actual > $KERNEL_PART"; fi

# The CRCs bootm checks. A truncated or mis-packed image fails here on the
# board with "Bad Data CRC" and nothing else.
if python3 - "$IMG" <<'PY'
import struct, sys, zlib
b = open(sys.argv[1], 'rb').read()
h = bytearray(b[:64]); hcrc = struct.unpack('>I', h[4:8])[0]; h[4:8] = b'\0\0\0\0'
size = struct.unpack('>I', b[12:16])[0]; dcrc = struct.unpack('>I', b[24:28])[0]
ok = zlib.crc32(bytes(h)) & 0xffffffff == hcrc and zlib.crc32(b[64:64+size]) & 0xffffffff == dcrc
sys.exit(0 if ok else 1)
PY
then ok "header CRC and data CRC verify"
else bad "a CRC in the header does not verify"; fi

# The payload really is what the header says: comp=3 is LZMA, and U-Boot
# hands it to its LZMA decoder. A gzip body under an lzma byte is a boot
# failure with a header that looks perfect.
if python3 - "$IMG" <<'PY'
import lzma, struct, sys
b = open(sys.argv[1], 'rb').read()
size = struct.unpack('>I', b[12:16])[0]
d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(b[64:64+size])
sys.exit(0 if len(d) > 1000000 and b"Linux version" in d else 1)
PY
then ok "the payload decompresses as LZMA to a Linux kernel"
else bad "the payload is not an LZMA-compressed Linux kernel"; fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
