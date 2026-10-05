#!/usr/bin/env bash
# Fetch and verify linux-6.18.53 (the current longterm release, per
# kernel.org/releases.json checked 2026-09-23) from cdn.kernel.org for
# kernel/build.sh KVER=6.18. Two independent checks: a pinned SHA-256 of the
# tarball, and a GPG verification of the upstream .tar.sign against Greg
# Kroah-Hartman stable-release key, fingerprint pinned so a compromised
# keyserver cannot hand back a different key under the same short id.
# keyserver.ubuntu.com is tried first here (it is the one that has actually
# worked for this key in this repo); keys.openpgp.org is tried second.
#
# If neither keyserver answers at all (offline, blocked egress), this falls
# back to trusting the SHA-256 alone rather than failing the whole build --
# loudly, on stderr, not silently. A SHA-256 mismatch or a bad signature
# both fail closed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DL=${DL:-$ROOT/dl}
OUT=${OUT:-$ROOT/kernel/618/mainline}

VERSION=6.18.55
BASE=https://cdn.kernel.org/pub/linux/kernel/v6.x
TARBALL="linux-$VERSION.tar.xz"
SIGFILE="linux-$VERSION.tar.sign"
# Verified 2026-09-23 against the file this URL actually served.
SHA256_XZ=f410638061a165c12f42ab871d2f3fcd525515359b5faeee80969cff84524df9
GPG_FPR=647F28654894E3BD457199BE38DBBDC86092693E
KEYSERVERS="hkps://keyserver.ubuntu.com hkps://keys.openpgp.org"

mkdir -p "$DL"

fetch_one() {
	url=$1; dest=$2
	[ -s "$dest" ] && return 0
	echo "==> fetching $(basename "$dest")" >&2
	curl -fsSL --retry 3 --max-time 1800 -o "$dest.part" "$url"
	mv "$dest.part" "$dest"
}

fetch_one "$BASE/$TARBALL" "$DL/$TARBALL"
fetch_one "$BASE/$SIGFILE" "$DL/$SIGFILE"

got=$(shasum -a 256 "$DL/$TARBALL" | cut -d' ' -f1)
if [ "$got" != "$SHA256_XZ" ]; then
	echo "kernel/618/fetch.sh: SHA256 MISMATCH for $TARBALL" >&2
	echo "  expected $SHA256_XZ" >&2
	echo "  got      $got" >&2
	rm -f "$DL/$TARBALL"
	exit 1
fi
echo "sha256: OK ($SHA256_XZ)"

# GPG verification needs the decompressed .tar (the .sign is a detached
# signature of the .tar, not the .tar.xz); decompressed into a scratch copy,
# the pinned .xz in dl/ stays untouched.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
xz -dk -c "$DL/$TARBALL" > "$WORK/linux-$VERSION.tar"
cp "$DL/$SIGFILE" "$WORK/"

GNUPGHOME=$(mktemp -d)
export GNUPGHOME
gpg_ok=0
for ks in $KEYSERVERS; do
	echo "==> trying $ks for $GPG_FPR" >&2
	if gpg --batch --keyserver "$ks" --recv-keys "$GPG_FPR" 2>/dev/null &&
	   gpg --batch --list-keys "$GPG_FPR" 2>/dev/null | grep -q "$GPG_FPR"; then
		gpg_ok=1
		break
	fi
	echo "    no usable key from $ks, trying the next one" >&2
done

if [ "$gpg_ok" = 1 ]; then
	if gpg --batch --verify "$WORK/$SIGFILE" "$WORK/linux-$VERSION.tar" 2>&1 | tee "$WORK/verify.log" >&2; then
		grep -q "Good signature" "$WORK/verify.log" || { echo "kernel/618/fetch.sh: gpg exited 0 but no Good signature line -- treating as failed" >&2; exit 1; }
		echo "gpg: OK (Good signature, $GPG_FPR)"
	else
		echo "kernel/618/fetch.sh: GPG VERIFICATION FAILED for $TARBALL" >&2
		exit 1
	fi
else
	echo "kernel/618/fetch.sh: WARNING -- no keyserver answered for $GPG_FPR" >&2
	echo "  ($KEYSERVERS), falling back to the pinned SHA-256 alone (already OK above)." >&2
	echo "  Re-run when network egress to a keyserver is available to get the GPG check too." >&2
fi

rm -rf "$OUT"
mkdir -p "$OUT"
tar xf "$DL/$TARBALL" -C "$OUT" --strip-components=1
echo "kernel/618/mainline: linux-$VERSION extracted to $OUT"
