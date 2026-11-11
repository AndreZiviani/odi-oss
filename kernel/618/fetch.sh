#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# Fetch and verify the 6.18 longterm release pinned in VERSION below
# (tools/bump-deps.sh moves the pin) from cdn.kernel.org for
# kernel/build.sh KVER=6.18. Two independent checks: a pinned SHA-256 of the
# tarball, and a GPG verification of the upstream .tar.sign against Greg
# Kroah-Hartman stable-release signing key, which is pinned here twice: the
# key itself (GPG_KEY) and its fingerprint (GPG_FPR), which the signature
# must name. gpgv checks the signature against that one key and nothing
# else. No keyserver is asked: the check needs no network, cannot be skipped,
# and a SHA-256 mismatch or a signature that is bad, missing or made by any
# other key fails closed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DL=${DL:-$ROOT/dl}
OUT=${OUT:-$ROOT/kernel/618/mainline}

VERSION=6.18.58
BASE=https://cdn.kernel.org/pub/linux/kernel/v6.x
TARBALL="linux-$VERSION.tar.xz"
SIGFILE="linux-$VERSION.tar.sign"
# Verified 2026-09-23 against the file this URL actually served.
SHA256_XZ=069b19f9032c4dac8149ae4b6520c35debca0f42a9f6d52ee55ba6ca9b818013
GPG_FPR=647F28654894E3BD457199BE38DBBDC86092693E
# The public key of GPG_FPR: keys/38DBBDC86092693E.asc of
# https://git.kernel.org/pub/scm/docs/kernel/pgpkeys.git (the keyring
# kernel.org publishes for its developers), exported with
# `gpg --export-options export-minimal --armor --export` so only its own
# self-signatures are kept. It carries no expiry date. Taken 2026-11-11 and
# checked against the signature of linux-6.18.58.
GPG_KEY='-----BEGIN PGP PUBLIC KEY BLOCK-----

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
W/O4joD1VJvsoXKf34YX5AIiAN8aOavh
=YiBg
-----END PGP PUBLIC KEY BLOCK-----'

mkdir -p "$DL"

fetch_one() {
	url=$1; dest=$2
	[ -s "$dest" ] && return 0
	echo "==> fetching $(basename "$dest")" >&2
	curl -fsSL --retry 3 --max-time 1800 -o "$dest.part.$$" "$url"
	mv "$dest.part.$$" "$dest"
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

# gpgv takes a binary keyring; gpg --dearmor writes one from GPG_KEY. Its
# GNUPGHOME is a directory under WORK, so nothing is read from or written to
# the home of the user, and the dearmor starts no agent.
GNUPGHOME=$WORK/gnupg
export GNUPGHOME
mkdir -m 700 "$GNUPGHOME"
printf '%s\n' "$GPG_KEY" | gpg --batch --dearmor > "$WORK/keyring.gpg"

# The VALIDSIG status line ends with the fingerprint of the primary key that
# made the signature; it has to be GPG_FPR.
if gpgv --status-fd 3 --keyring "$WORK/keyring.gpg" \
	"$WORK/$SIGFILE" "$WORK/linux-$VERSION.tar" 3>"$WORK/status" >&2 &&
   awk -v fpr="$GPG_FPR" '$1 == "[GNUPG:]" && $2 == "VALIDSIG" && $NF == fpr { ok = 1 } END { exit !ok }' "$WORK/status"; then
	echo "gpg: OK (Good signature, $GPG_FPR)"
else
	echo "kernel/618/fetch.sh: GPG VERIFICATION FAILED for $TARBALL (signature by $GPG_FPR expected)" >&2
	exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"
tar xf "$DL/$TARBALL" -C "$OUT" --strip-components=1
echo "kernel/618/mainline: linux-$VERSION extracted to $OUT"
