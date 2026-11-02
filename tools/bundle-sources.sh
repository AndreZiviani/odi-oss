#!/usr/bin/env bash
#
# The complete corresponding source of a release image, as one file.
#
#     tools/bundle-sources.sh <version> [out.tar]    # default out/image/<version>-sources.tar
#     tools/bundle-sources.sh --check <out.tar>      # the release gate
#
# For GPL and LGPL compliance: whoever gets the image can get, from the same
# release page, the exact upstream source of what it contains, and what is
# needed to rebuild it or to relink the statically linked LGPL library
# (uClibc-ng). The bundle holds, byte for byte:
#
#   - the upstream tarballs this tree builds, the ones packages/fetch.sh and
#     kernel/618/fetch.sh already downloaded and verified against their pinned
#     SHA-256 (the Linux kernel and its signature, busybox, dropbear,
#     iproute2), taken from the same dl/ cache, so the release build does not
#     download them twice;
#   - uClibc-ng (LGPL, linked statically into every target binary) and the
#     odi-toolchain tag the image pinned by toolchain/images.env was built
#     from (its patches, configs and build scripts). gcc, binutils and the
#     Linux headers are build tools that do not ship: SOURCES.md names them
#     with URL and pinned SHA-256, the files are not included;
#   - confd and metricsd at the tags this image fetched (releases.env), as
#     GitHub tag archives of their own repositories;
#   - this repository at the release commit (git archive): our patches,
#     configs, drivers, tools and build scripts.
#
# SOURCES.md inside lists every component with version, license, URL and
# SHA-256; SOURCES.SHA256SUMS checks every file. --check unpacks a bundle and
# verifies exactly that, and fails on a missing, empty or altered one: the
# release job runs it before publishing anything.
#
# Every network call is bounded (packages/fetch.sh: 900 s and a retry per
# source; curl below: --max-time and --retry). DL moves the cache, as for the
# fetch scripts; RELEASES_ENV is the file src/fetch-releases.sh wrote.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

sha256() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }
die() { echo "bundle-sources.sh: $*" >&2; exit 1; }

# ------------------------------------------------------------------ the gate
if [ "${1:-}" = --check ]; then
	f=${2:?usage: bundle-sources.sh --check <bundle.tar>}
	[ -s "$f" ] || die "$f is missing or empty"
	w=$(mktemp -d)
	trap 'rm -rf "$w"' EXIT
	tar -xf "$f" -C "$w" || die "$f is not a readable tar"
	set -- "$w"/*/
	[ -d "$1" ] && [ ! -e "${2:-}" ] || die "$f must hold exactly one top directory"
	d=${1%/}
	[ -s "$d/SOURCES.md" ] || die "no SOURCES.md in $f"
	[ -s "$d/SOURCES.SHA256SUMS" ] || die "no SOURCES.SHA256SUMS in $f"
	n=0
	while read -r want name; do
		[ -s "$d/$name" ] || die "$name is missing or empty in $f"
		[ "$(sha256 "$d/$name")" = "$want" ] || die "$name does not match SOURCES.SHA256SUMS"
		n=$((n + 1))
	done < "$d/SOURCES.SHA256SUMS"
	[ "$n" -ge 10 ] || die "only $n files listed in $f; expected the whole set"
	for need in linux- busybox- dropbear- iproute2- uClibc-ng- odi-ui- odi-sfp-exporter- odi-toolchain- odi-oss-; do
		grep -q "^[0-9a-f]\{64\}  $need" "$d/SOURCES.SHA256SUMS" || die "no $need* source in $f"
		grep -q "$need" "$d/SOURCES.md" || die "$need not described in SOURCES.md"
	done
	echo "bundle-sources.sh: $f OK ($n files)"
	exit 0
fi

# ---------------------------------------------------------------- the bundle
VERSION=${1:?usage: bundle-sources.sh <version> [out.tar]}
OUT=${2:-$ROOT/out/image/$VERSION-sources.tar}
DL=${DL:-$ROOT/dl}
export DL
RELEASES_ENV=${RELEASES_ENV:-$ROOT/out/bin/releases.env}
[ -f "$RELEASES_ENV" ] || die "no $RELEASES_ENV: run make releases first, the tags in it say which confd and metricsd the image has"
mkdir -p "$DL" "$(dirname "$OUT")"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
D=$work/$VERSION-sources
mkdir -p "$D"
manifest=$D/SOURCES.md
rows=$work/rows

curl_get() { # url dest: bounded, atomic
	curl -fsSL --retry 3 --max-time 600 -o "$2.part.$$" "$1" || { rm -f "$2.part.$$"; die "could not download $1"; }
	mv "$2.part.$$" "$2"
}

# add <name> <version> <license> <role> <url> <file>: copy the file in, record the row
add() {
	cp "$6" "$D/$(basename "$6")"
	printf '| %s | %s | %s | %s | `%s` | `%s` | `%s` |\n' \
		"$1" "$2" "$3" "$4" "$(basename "$6")" "$5" "$(sha256 "$6")" >> "$rows"
}

# note <name> <version> <role> <url> <sha256>: a build tool, named but not included
note() {
	printf '| %s | %s | %s | `%s` | `%s` |\n' "$1" "$2" "$3" "$4" "$5" >> "$work/notrows"
}

# pin <file> <VAR>: the first top-level VAR=value of a script, unquoted
pin() { sed -n "s/^$2=//p" "$1" | head -n 1 | tr -d '"'; }

# ---- the kernel: fetched and verified by its own script, cache shared
kv=$(pin "$ROOT/kernel/618/fetch.sh" VERSION)
ksum=$(pin "$ROOT/kernel/618/fetch.sh" SHA256_XZ)
[ -n "$kv" ] && [ -n "$ksum" ] || die "no pinned kernel version in kernel/618/fetch.sh"
if [ ! -s "$DL/linux-$kv.tar.xz" ] || [ ! -s "$DL/linux-$kv.tar.sign" ]; then
	"$ROOT/kernel/618/fetch.sh" >&2
fi
[ "$(sha256 "$DL/linux-$kv.tar.xz")" = "$ksum" ] || die "linux-$kv.tar.xz does not match the pin"
add linux "$kv" GPL-2.0-only kernel "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$kv.tar.xz" "$DL/linux-$kv.tar.xz"
add "linux (signature)" "$kv" - "detached .sign of the uncompressed tar" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$kv.tar.sign" "$DL/linux-$kv.tar.sign"

# ---- the packages: the pins in each build.sh, fetched by packages/fetch.sh
for p in busybox:GPL-2.0-only dropbear:MIT iproute2:GPL-2.0-only; do
	name=${p%%:*}
	lic=${p#*:}
	# VERSION, URL, SHA256 and ALT_URLS are plain assignments at the top of
	# each build.sh; evaluate just those lines, never the script.
	pins=$(grep -E '^(VERSION|URL|SHA256|ALT_URLS)=' "$ROOT/packages/$name/build.sh")
	f=$(
		unset ALT_URLS
		eval "$pins"
		export ALT_URLS="${ALT_URLS:-}"
		"$ROOT/packages/fetch.sh" "$URL" "$SHA256" "$DL"
	)
	v=$(unset VERSION; eval "$pins"; echo "$VERSION")
	u=$(unset URL; eval "$pins"; echo "$URL")
	add "$name" "$v" "$lic" "package, built in the image" "$u" "$f"
done

# ---- the toolchain: the odi-toolchain tag the pinned image was built from
img=$(pin "$ROOT/toolchain/images.env" OSS_IMAGE_PINNED)
itag=$(echo "$img" | sed -n 's/^[^:]*:\(v[0-9][0-9]*\)@sha256:.*/\1/p')
[ -n "$itag" ] || die "cannot read the image tag from OSS_IMAGE_PINNED in toolchain/images.env ($img)"
ttag=uclibc-$itag
tarc=$DL/odi-toolchain-$ttag.tar.gz
[ -s "$tarc" ] || curl_get "https://github.com/AndreZiviani/odi-toolchain/archive/refs/tags/$ttag.tar.gz" "$tarc"
tb=$work/toolchain-build.sh
member=$(tar -tzf "$tarc" | grep -m1 '/uclibc/build\.sh$') || die "no uclibc/build.sh in $tarc"
tar -xzf "$tarc" -O "$member" > "$tb"
tpins=$(grep -E '^(BINUTILS|GCC|UCLIBC|LINUX)(_SHA256)?=' "$tb")
[ "$(echo "$tpins" | wc -l | tr -d ' ')" = 8 ] || die "unexpected pins in odi-toolchain $ttag uclibc/build.sh"
add odi-toolchain "$ttag" GPL-3.0-or-later "patches, configs and build scripts of the toolchain image" \
	"https://github.com/AndreZiviani/odi-toolchain/archive/refs/tags/$ttag.tar.gz" "$tarc"
# The tarball URLs are written in build.sh as fetch "<url with $VARS>" "$SHA"
# lines; take them from there, so the bundle cannot name other sources than
# the toolchain used.
n=0
while read -r line; do
	eval "$tpins"
	url=$(echo "$line" | sed -n 's/^.*fetch "\([^"]*\)".*$/\1/p')
	sha=$(echo "$line" | sed -n 's/^.*fetch "[^"]*" "\$\([A-Z_0-9]*\)".*$/\1/p')
	eval "url=\"$url\" sha=\"\${$sha}\""
	# Only uClibc-ng ships (linked statically into the image). The rest of
	# the toolchain is build tools: named in SOURCES.md, not downloaded.
	case $(basename "$url") in
	uClibc-ng-*)
		f=$("$ROOT/packages/fetch.sh" "$url" "$sha" "$DL")
		add uClibc-ng "$UCLIBC" "LGPL-2.1-or-later" "C library, linked statically into every target binary" "$url" "$f" ;;
	binutils-*) note binutils "$BINUTILS" "assembler and linker" "$url" "$sha" ;;
	gcc-*) note gcc "$GCC" "compiler; its runtime library parts linked into binaries fall under the GCC Runtime Library Exception" "$url" "$sha" ;;
	linux-*) note "linux (headers)" "$LINUX" "UAPI headers uClibc-ng was built against" "$url" "$sha" ;;
	*) die "unknown toolchain source $(basename "$url")" ;;
	esac
	n=$((n + 1))
done < <(grep -E '^[[:space:]]*t=\$\(fetch "' "$tb")
[ "$n" = 4 ] || die "expected 4 toolchain sources in odi-toolchain $ttag, found $n"

# ---- confd and metricsd: their own repositories, at the tags the image has
# shellcheck disable=SC1090
. "$RELEASES_ENV"
for p in "odi-ui:${CONFD_REPO:-AndreZiviani/odi-ui}:${CONFD_TAG:-}:confd:web UI" \
	"odi-sfp-exporter:${METRICSD_REPO:-AndreZiviani/odi-sfp-exporter}:${METRICSD_TAG:-}:metricsd:Prometheus exporter"; do
	IFS=: read -r repo path tag comp what <<EOF
$p
EOF
	case $tag in "" | local) die "$comp is \"${tag:-unset}\" in $RELEASES_ENV: a release image uses a published tag" ;; esac
	f=$DL/releases/$repo-$tag-source.tar.gz
	mkdir -p "$DL/releases"
	[ -s "$f" ] || curl_get "https://github.com/$path/archive/refs/tags/$tag.tar.gz" "$f"
	add "$comp" "$tag" "see the repository" "$what, separate project, prebuilt binary in the image" \
		"https://github.com/$path/archive/refs/tags/$tag.tar.gz" "$f"
done

# ---- this repository at the commit the release is cut from
git -C "$ROOT" rev-parse --verify -q HEAD >/dev/null || die "not a git checkout: cannot archive the tree"
src=$work/odi-oss-$VERSION.tar.gz
git -C "$ROOT" archive --format=tar.gz --prefix="odi-oss-$VERSION/" -o "$src" HEAD
add odi-oss "$VERSION ($(git -C "$ROOT" rev-parse --short=12 HEAD))" "GPL-2.0-or-later" \
	"this repository: patches, configs, drivers, tools, build scripts" "https://github.com/AndreZiviani/odi-oss" "$src"

# ------------------------------------------------------------------ manifest
{
	echo "# Corresponding source of odi-oss $VERSION"
	echo
	echo "Everything in the first table is the upstream source of what the $VERSION image"
	echo "contains, unmodified, byte for byte what the build"
	echo "downloaded and checked. Our own changes to it are in the odi-oss"
	echo "archive: kernel patches and added files in \`kernel/618/patches/\` and"
	echo "\`kernel/extra/\`, package configs in \`packages/*/\`, rootfs in"
	echo "\`rootfs/\`. The same files are at the git tag $VERSION of"
	echo "https://github.com/AndreZiviani/odi-oss."
	echo
	echo "| component | version | license | role | file | upstream | sha256 |"
	echo "|---|---|---|---|---|---|---|"
	cat "$rows"
	cat <<'EOF'

## Build tools: not distributed, not included

Used to build the image, nothing of them is in it (gcc's runtime library
parts linked into binaries fall under the GCC Runtime Library Exception).
Pinned by URL and SHA-256 in `uclibc/build.sh` of the odi-toolchain archive
above.

| component | version | role | upstream | sha256 |
|---|---|---|---|---|
EOF
	cat "$work/notrows"
	cat <<'EOF'

## Rebuilding the image

Unpack the `odi-oss-*` archive and follow `docs/BUILDING.md`: `make image-all`
fetches the same pinned sources (the SHA-256 in `kernel/618/fetch.sh` and
`packages/*/build.sh` equal the ones above), pulls the toolchain image
pinned by digest in `toolchain/images.env`, and builds the same image with
`VERSION=<this version> ROOT_PW=locked`. To avoid any download, drop the
files above into `dl/`, the cache those scripts read first.

## Relinking uClibc-ng (LGPL-2.1)

busybox, dropbear, iproute2 and our own tools link `libc.a` statically.
To relink them against a modified uClibc-ng: build the toolchain from the
`odi-toolchain` archive (`make uclibc`; its `uclibc/patches/uclibc-ng/` are
our two changes to uClibc-ng, `uclibc/build.sh` the configuration) with your
uClibc-ng source in place of the one above, point `OSS_IMAGE=` at that image
(`toolchain/README.md`) and run `make packages src image` from the
`odi-oss` archive. Every object is rebuilt from source there, so the
relink is a rebuild with the same flags. The `confd` and `metricsd` sources
build the same way with that toolchain.

## Written offer

These sources are published with every release, free of charge, for at least
as long as the release is. Anything missing here that a license requires you
to be able to get: open an issue at https://github.com/AndreZiviani/odi-oss.
EOF
} > "$manifest"

(
	cd "$D"
	for f in *; do
		[ "$f" = SOURCES.SHA256SUMS ] || printf '%s  %s\n' "$(sha256 "$f")" "$f"
	done > SOURCES.SHA256SUMS
)

rm -f "$OUT"
tar -cf "$OUT.part.$$" -C "$work" "$VERSION-sources"
mv "$OUT.part.$$" "$OUT"
"$0" --check "$OUT"
echo "bundle-sources.sh: $OUT $(wc -c < "$OUT" | tr -d ' ') bytes" >&2
