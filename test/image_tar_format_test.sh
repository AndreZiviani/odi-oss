#!/usr/bin/env bash
#
# The image tarball must be readable by busybox 1.12.4's tar, because that is
# what unpacks it on the stock vendor stick before fwu.sh ever runs.
#
# It was not. Built on macOS with bsdtar, the stock busybox answered
#
#     tar: corrupted octal value in tar header
#
# and extracted NOTHING -- so the documented procedure could not begin. Measured
# against the stock busybox under qemu-mips, with a GNU-made tarball as the
# positive control, which extracted cleanly on the same binary.
#
# This test checks the header FORM rather than trying to run that busybox: the
# stock rootfs is disposable scratch and is not present on a clean checkout, so
# a test that needed it would skip silently on exactly the machines that most
# need the answer. The three differences that mattered are checked by name.
#
# It also checks the member list, because a macOS tar adds AppleDouble `._*`
# sidecars and pax `PaxHeader/*` entries that md5.txt does not cover, which
# makes build.sh's "md5.txt covers every member except itself" false.
set -u
cd "$(dirname "$0")/.." || exit 1

pass=0
fail=0
ok()  { printf 'ok    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

TAR=${1:-}
if [ -z "$TAR" ]; then
	newest=""
	for f in out/image/*.tar; do
		case $f in
		*smoketest*) continue ;;
		*'*'*) continue ;;
		esac
		[ -z "$newest" ] || [ "$f" -nt "$newest" ] && newest=$f
	done
	TAR=$newest
fi
if [ -z "$TAR" ] || [ ! -r "$TAR" ]; then
	echo "no image tarball found -- run image/build.sh first"
	exit 1
fi
echo "checking $TAR"

python3 - "$TAR" <<'PY'
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
if len(data) < 1024:
    print("FAIL  the tarball is too small to hold one header")
    sys.exit(1)

members, bad, off = [], [], 0
while off + 512 <= len(data):
    h = data[off:off + 512]
    if h == b'\0' * 512:
        break
    name = h[0:100].split(b'\0')[0].decode('ascii', 'replace')
    typ = h[156:157]
    magic, version = h[257:263], h[263:265]
    size_f, mtime_f = h[124:136], h[136:148]
    mode_f = h[100:108]

    # busybox 1.12.4 wants what GNU tar writes: numeric fields right-justified
    # octal and NUL-terminated, and the old GNU magic. bsdtar writes a trailing
    # space instead, and `ustar\0` / `00`.
    if not mode_f.endswith(b'\0') or b' ' in mode_f:
        bad.append(f"{name}: mode field {mode_f!r} is not NUL-terminated octal")
    for label, f in (("size", size_f), ("mtime", mtime_f)):
        if not f.endswith(b'\0'):
            bad.append(f"{name}: {label} field {f!r} does not end in NUL")
    if magic != b'ustar ' or version != b' \0':
        bad.append(f"{name}: magic/version {magic!r}{version!r}, want b'ustar '+b' \\0'")

    members.append((name, typ.decode('ascii', 'replace')))
    try:
        size = int(size_f.split(b'\0')[0].strip() or b'0', 8)
    except ValueError:
        print(f"FAIL  {name}: size field {size_f!r} is not octal at all")
        sys.exit(1)
    off += 512 + ((size + 511) // 512) * 512

want = ['fwu.sh', 'fwu_ver', 'md5.txt', 'rootfs', 'uImage']
got = [n for n, _ in members]
rc = 0

if sorted(got) == sorted(want):
    print(f"ok    exactly the five declared members, no extras")
else:
    extra = [n for n in got if n not in want]
    missing = [n for n in want if n not in got]
    if extra:
        print(f"FAIL  undeclared members md5.txt does not cover: {extra}")
    if missing:
        print(f"FAIL  missing members: {missing}")
    rc = 1

odd = [(n, t) for n, t in members if t not in ('0', '\0')]
if odd:
    print(f"FAIL  non-regular entries (pax/AppleDouble): {odd}")
    rc = 1
else:
    print("ok    every entry is a regular file, no pax extended headers")

if bad:
    for b in bad:
        print(f"FAIL  {b}")
    print("      Build the tarball with GNU tar in the container, not the")
    print("      host's bsdtar. See image/build.sh.")
    rc = 1
else:
    print(f"ok    all {len(members)} headers are in the form busybox 1.12.4 accepts")

sys.exit(rc)
PY
rc=$?
if [ "$rc" -eq 0 ]; then
	ok "the tarball is readable by the stock busybox tar"
else
	bad "the tarball would not extract on the stock image"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
