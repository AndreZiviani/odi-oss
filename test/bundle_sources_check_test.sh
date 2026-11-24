#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# tools/bundle-sources.sh --check, the release gate: a complete bundle passes,
# and a missing, empty, altered or incomplete one fails. A synthetic bundle, no
# network: building the real one needs the pinned downloads.
set -u
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { # name expected-rc command...
	n=$1 want=$2; shift 2
	"$@" >/dev/null 2>&1; got=$?
	if [ "$got" = "$want" ]; then echo "ok    $n"; pass=$((pass + 1))
	else echo "FAIL  $n (rc $got, want $want)"; fail=$((fail + 1)); fi
}
sum() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }

# make <dir> <skip-prefix>: a bundle directory with every required component
make_dir() {
	D=$T/$1/v9-sources; rm -rf "${T:?}/$1"; mkdir -p "$D"
	for f in linux-6.tar.xz linux-6.tar.sign busybox-1.tar.bz2 dropbear-1.tar.bz2 \
		iproute2-1.tar.xz uClibc-ng-1.tar.xz \
		odi-ui-v1-source.tar.gz odi-sfp-exporter-v1-source.tar.gz \
		odi-toolchain-uclibc-v4.tar.gz odi-oss-v9.tar.gz; do
		[ -z "$2" ] || case $f in "$2"*) continue ;; esac
		echo "$f" > "$D/$f"
	done
	ls "$D" > "$T/names"; mv "$T/names" "$D/SOURCES.md"
	( cd "$D" && for f in *; do [ "$f" = SOURCES.md ] || printf '%s  %s\n' "$(sum "$f")" "$f"; done > "$T/sums"
	  mv "$T/sums" SOURCES.SHA256SUMS )
	( cd "$D" && printf '%s  %s\n' "$(sum SOURCES.md)" SOURCES.md >> SOURCES.SHA256SUMS )
	tar -cf "$T/$1.tar" -C "$T/$1" v9-sources
}
B=tools/bundle-sources.sh
make_dir good ""
ok "complete bundle passes" 0 $B --check "$T/good.tar"
ok "missing file fails" 1 $B --check "$T/none.tar"
: > "$T/empty.tar"
ok "empty file fails" 1 $B --check "$T/empty.tar"
make_dir nogcc uClibc-
ok "bundle without uClibc-ng fails" 1 $B --check "$T/nogcc.tar"
make_dir bad ""
echo tamper >> "$T/bad/v9-sources/busybox-1.tar.bz2"
tar -cf "$T/bad.tar" -C "$T/bad" v9-sources
ok "altered file fails" 1 $B --check "$T/bad.tar"

# tag_archive, which the bundle build runs on every GitHub tag archive: the
# committed source.tar.gz line decides, also for a file already in dl/. Taken
# out of the script with its two helpers, so no network is needed.
A=$T/archive; mkdir -p "$A/src"
{ sed -n '/^sha256()/p; /^die()/p' tools/bundle-sources.sh
  sed -n '/^curl_get()/,/^}/p; /^tag_archive()/,/^}/p' tools/bundle-sources.sh; } > "$A/fn.sh"
printf 'the tag\n' > "$A/good.tar.gz"
printf 'another tag\n' > "$A/moved.tar.gz"
printf 'R/x v1 source.tar.gz %s\n' "$(sum "$A/good.tar.gz")" > "$A/src/release-sha256.txt"
ta() { ROOT=$A bash -c '. "$1/fn.sh"; tag_archive "$2" "$3" http://127.0.0.1:9/none "$4"' _ "$A" "$@"; }
ok "a tag archive that matches its committed line passes" 0 ta R/x v1 "$A/good.tar.gz"
ok "a tag archive that differs from it fails" 1 ta R/x v1 "$A/moved.tar.gz"
ok "a tag with no source.tar.gz line fails" 1 ta R/x v2 "$A/good.tar.gz"

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
