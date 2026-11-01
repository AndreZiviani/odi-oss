#!/usr/bin/env bash
# src/fetch-releases.sh: the committed release hashes and the assets tar member
# policy. Runs a copy of the script in a scratch tree against a pre-filled
# download cache, so nothing is fetched and nothing in out/ is touched.
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$PWD
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; fail=$((fail + 1)); }
sha() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }

MR=AndreZiviani/odi-sfp-exporter
CR=AndreZiviani/odi-ui
MT=v9.8.0
CT=v9.12.0

# fresh: a scratch tree with the script, a cache holding a metricsd, a confd
# and an assets tarball built from $1 (a directory), SHA256SUMS files that
# agree with them, and a release-sha256.txt that also agrees.
fresh() {
	rm -rf "$T/tree" "$T/dl"
	mkdir -p "$T/tree/src" "$T/dl/releases"
	cp "$REPO/src/fetch-releases.sh" "$T/tree/src/"
	c=$T/dl/releases
	printf 'metricsd bytes\n' > "$c/${MR//\//_}-$MT-metricsd"
	printf 'confd bytes\n' > "$c/${CR//\//_}-$CT-confd"
	tar -czf "$c/${CR//\//_}-$CT-confd-assets.tar.gz" -C "$1" .
	( cd "$c" && printf '%s  metricsd\n' "$(sha "${MR//\//_}-$MT-metricsd")" > "${MR//\//_}-$MT-SHA256SUMS" )
	( cd "$c" && {
		printf '%s  confd\n' "$(sha "${CR//\//_}-$CT-confd")"
		printf '%s  confd-assets.tar.gz\n' "$(sha "${CR//\//_}-$CT-confd-assets.tar.gz")"
	} > "${CR//\//_}-$CT-SHA256SUMS" )
	pins
}
pins() {
	c=$T/dl/releases
	{
		echo "$MR $MT metricsd $(sha "$c/${MR//\//_}-$MT-metricsd")"
		echo "$CR $CT confd $(sha "$c/${CR//\//_}-$CT-confd")"
		echo "$CR $CT confd-assets.tar.gz $(sha "$c/${CR//\//_}-$CT-confd-assets.tar.gz")"
	} > "$T/tree/src/release-sha256.txt"
}
run() {
	( cd "$T/tree" && METRICSD_TAG=$MT CONFD_TAG=$CT DL=$T/dl USE_CURL=1 \
		bash src/fetch-releases.sh ) > "$T/log" 2>&1
}

good=$T/good
mkdir -p "$good/sub"
echo page > "$good/index.html"
echo row > "$good/sub/keys.tsv"

fresh "$good"
if run && [ -f "$T/tree/out/confd-assets/sub/keys.tsv" ]; then ok "matching hashes and a plain tarball are accepted"
else no "matching hashes and a plain tarball are accepted"; cat "$T/log"; fi

# A release that replaced the asset AND its SHA256SUMS: consistent with each
# other, different from the committed hash.
fresh "$good"
c=$T/dl/releases
printf 'evil confd\n' > "$c/${CR//\//_}-$CT-confd"
printf '%s  confd\n%s  confd-assets.tar.gz\n' "$(sha "$c/${CR//\//_}-$CT-confd")" \
	"$(sha "$c/${CR//\//_}-$CT-confd-assets.tar.gz")" > "$c/${CR//\//_}-$CT-SHA256SUMS"
if ! run && grep -q "does not match the committed" "$T/log"; then ok "an asset and SHA256SUMS replaced together are refused"
else no "an asset and SHA256SUMS replaced together are refused"; cat "$T/log"; fi

# A tag with no committed line.
fresh "$good"
grep -v " metricsd " "$T/tree/src/release-sha256.txt" > "$T/p" && mv "$T/p" "$T/tree/src/release-sha256.txt"
if ! run && grep -q "no committed sha256" "$T/log"; then ok "a tag without a committed hash stops the build"
else no "a tag without a committed hash stops the build"; cat "$T/log"; fi
if ALLOW_UNPINNED_RELEASES=1 run && grep -q WARNING "$T/log"; then ok "ALLOW_UNPINNED_RELEASES=1 downgrades it to a warning"
else no "ALLOW_UNPINNED_RELEASES=1 downgrades it to a warning"; cat "$T/log"; fi

# Hostile tarballs, each correctly hashed and listed (so only the member
# policy can stop them).
evil() {
	name=$1 snippet=$2
	rm -rf "$T/evil"; mkdir -p "$T/evil"
	python3 -I - "$T/evil/a.tar.gz" "$snippet" <<'PY'
import sys, tarfile, io
out, kind = sys.argv[1], sys.argv[2]
with tarfile.open(out, "w:gz") as t:
    def add(name, typ=tarfile.REGTYPE, link=""):
        i = tarfile.TarInfo(name); i.type = typ; i.linkname = link
        d = b"x" if typ == tarfile.REGTYPE else b""
        i.size = len(d); t.addfile(i, io.BytesIO(d))
    add("ok.html")
    if kind == "symlink": add("l", tarfile.SYMTYPE, "/etc/passwd")
    elif kind == "hardlink": add("h", tarfile.LNKTYPE, "ok.html")
    elif kind == "absolute": add("/tmp/odi-fetch-test-abs")
    elif kind == "dotdot": add("a/../../escape")
    elif kind == "device": add("dev", tarfile.CHRTYPE)
PY
	fresh "$good"
	cp "$T/evil/a.tar.gz" "$T/dl/releases/${CR//\//_}-$CT-confd-assets.tar.gz"
	c=$T/dl/releases
	printf '%s  confd\n%s  confd-assets.tar.gz\n' "$(sha "$c/${CR//\//_}-$CT-confd")" \
		"$(sha "$c/${CR//\//_}-$CT-confd-assets.tar.gz")" > "$c/${CR//\//_}-$CT-SHA256SUMS"
	pins
	if ! run && grep -q "refusing" "$T/log" && [ ! -e /tmp/odi-fetch-test-abs ] && [ ! -e "$T/tree/out/escape" ]; then ok "tarball with $name member refused"
	else no "tarball with $name member refused"; cat "$T/log"; fi
}
for k in symlink hardlink absolute dotdot device; do evil "$k" "$k"; done

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
