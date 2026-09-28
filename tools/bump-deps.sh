#!/usr/bin/env bash
#
# Checks every pinned upstream version against what is actually current, and
# rewrites the pin in place when a newer one exists: kernel/618/fetch.sh
# (Linux 6.18.y), packages/{busybox,dropbear,iproute2}/build.sh, the three
# image digests in toolchain/images.env (odi-toolchain releases), and the
# METRICSD_TAG/CONFD_TAG defaults in src/fetch-releases.sh (odi-sfp-exporter,
# odi-ui). Driven by .github/workflows/dependency-bump.yml, which then runs
# the test suite against whatever this script changed and opens a PR only if
# it passes -- this script itself does not decide whether a bump is safe, it
# only proposes one and computes the checksum that goes with it.
#
# A version this script updates gets no separate signature dance here: it
# computes the real sha256 of what it downloaded (kernel: also cross-checked
# with the sha256sums.asc kernel.org itself publishes for that release, same as
# the fetch scripts always required, and the later workflow step
# `make kernel-tree` (kernel/618/fetch.sh, GPG included) / `make packages` /
# `make releases` run the *existing* fetch scripts against the new pin --
# the same verification an ordinary build already does, now proving the
# candidate pin rather than trusting the arithmetic in this script alone.
#
# Exits 0 whether or not anything changed; prints one "bumped: ..." line per
# file it touched, so the workflow can tell "clean checkout, nothing to do"
# from "changed, go run the tests".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

changed=0
note() { echo "bumped: $*"; changed=1; }

sha256_of() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }

# --- Linux kernel (kernel/618/fetch.sh) -------------------------------
current=$(grep -m1 -oE 'VERSION=[0-9.]+' kernel/618/fetch.sh | cut -d= -f2)
latest=$(curl -fsSL https://www.kernel.org/releases.json |
	python3 -c '
import json, sys
d = json.load(sys.stdin)
best = None
for r in d["releases"]:
	v = r["version"]
	if v.startswith("6.18.") and not r.get("moniker") == "eol":
		if best is None or [int(x) for x in v.split(".")] > [int(x) for x in best.split(".")]:
			best = v
print(best or "")
')
if [ -n "$latest" ] && [ "$latest" != "$current" ]; then
	tarball="linux-$latest.tar.xz"
	tmp=$(mktemp -d)
	curl -fsSL --retry 3 -o "$tmp/$tarball" "https://cdn.kernel.org/pub/linux/kernel/v6.x/$tarball"
	newsum=$(sha256_of "$tmp/$tarball")
	# Cross-check against the sum kernel.org itself publishes for this file, so a
	# bad mirror cannot slip a different sha256 in here unnoticed; the real
	# GPG check still happens later, when kernel/618/fetch.sh itself runs.
	published=$(curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc" |
		awk -v f="$tarball" '$2 == f {print $1}')
	if [ -n "$published" ] && [ "$published" != "$newsum" ]; then
		echo "bump-deps.sh: kernel.org's own sha256sums.asc disagrees with the tarball for $tarball -- not bumping" >&2
	else
		sed -i.bak -E "s/^VERSION=[0-9.]+/VERSION=$latest/" kernel/618/fetch.sh
		sed -i.bak -E "s/^SHA256_XZ=[0-9a-f]+/SHA256_XZ=$newsum/" kernel/618/fetch.sh
		rm -f kernel/618/fetch.sh.bak
		note "linux $current -> $latest"
	fi
	rm -rf "$tmp"
fi

# --- busybox / dropbear / iproute2 (packages/*/build.sh) ---------------
bump_package() {
	dir=$1 listing_url=$2 name_pat=$3 url_tmpl=$4
	file="packages/$dir/build.sh"
	current=$(grep -m1 -oE 'VERSION=[0-9.]+' "$file" | cut -d= -f2)
	latest=$(curl -fsSL "$listing_url" | grep -oE "$name_pat" | sort -V | tail -1 |
		grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?')
	[ -n "$latest" ] || { echo "bump-deps.sh: could not find a $dir version at $listing_url" >&2; return; }
	[ "$latest" != "$current" ] || return 0
	url=$(eval echo "$url_tmpl")
	tmp=$(mktemp -d)
	out="$tmp/$(basename "$url")"
	if ! curl -fsSL --retry 3 -o "$out" "$url"; then
		echo "bump-deps.sh: could not download $dir $latest from $url -- not bumping" >&2
		rm -rf "$tmp"; return
	fi
	newsum=$(sha256_of "$out")
	sed -i.bak -E "s/^VERSION=[0-9.]+/VERSION=$latest/" "$file"
	sed -i.bak -E "s/^SHA256=[0-9a-f]+/SHA256=$newsum/" "$file"
	rm -f "$file.bak"
	note "$dir $current -> $latest"
	rm -rf "$tmp"
}

bump_package busybox \
	https://busybox.net/downloads/ \
	'busybox-[0-9]+\.[0-9]+\.[0-9]+\.tar\.bz2' \
	'https://busybox.net/downloads/busybox-$latest.tar.bz2'
bump_package dropbear \
	https://matt.ucc.asn.au/dropbear/releases/ \
	'dropbear-20[0-9]{2}\.[0-9]+\.tar\.bz2' \
	'https://matt.ucc.asn.au/dropbear/releases/dropbear-$latest.tar.bz2'
bump_package iproute2 \
	https://mirrors.edge.kernel.org/pub/linux/utils/net/iproute2/ \
	'iproute2-[0-9]+\.[0-9]+\.[0-9]+\.tar\.xz' \
	'https://mirrors.edge.kernel.org/pub/linux/utils/net/iproute2/iproute2-$latest.tar.xz'

# --- odi-sfp-exporter / odi-ui release tags (src/fetch-releases.sh) -----
bump_release_tag() {
	var=$1 repo=$2
	current=$(grep -m1 -oE "${var}[:-]+v[0-9.]+" src/fetch-releases.sh | grep -oE 'v[0-9.]+' | tail -1)
	latest=$(gh release list --repo "$repo" --limit 1 --json tagName --jq '.[0].tagName' 2>/dev/null || true)
	[ -n "$latest" ] || { echo "bump-deps.sh: could not list releases for $repo (gh not authenticated?)" >&2; return; }
	[ "$latest" != "$current" ] || return 0
	sed -i.bak -E "s/${var}:-$current/${var}:-$latest/; s/${var}-$current/${var}-$latest/" src/fetch-releases.sh
	rm -f src/fetch-releases.sh.bak
	note "$repo tag $current -> $latest"
}
bump_release_tag METRICSD_TAG AndreZiviani/odi-sfp-exporter
bump_release_tag CONFD_TAG AndreZiviani/odi-ui

# --- odi-toolchain images (toolchain/images.env) ------------------------
# odi-toolchain has no GitHub Releases, only git tags per image, prefixed
# by which image they build: uclibc-vN, freestanding-vN, qemu-malta-vN.
# The docker tag actually pinned here is the bare vN.
bump_toolchain_image() {
	var=$1 image=$2 tag_prefix=$3
	current_ref=$(grep -m1 "^${var}=" toolchain/images.env | cut -d= -f2-)
	current_tag=$(echo "$current_ref" | sed -E 's/^[^:]+:([^@]+)@.*/\1/')
	latest_tag=$(gh api "repos/AndreZiviani/odi-toolchain/tags" --jq '.[].name' 2>/dev/null |
		grep -E "^${tag_prefix}v[0-9]+\$" | sed -E "s/^${tag_prefix}//" |
		sort -t v -k2 -n | tail -1)
	[ -n "$latest_tag" ] || { echo "bump-deps.sh: could not list tags for odi-toolchain ($tag_prefix)" >&2; return; }
	[ "$latest_tag" != "$current_tag" ] || return 0
	if ! docker pull "$image:$latest_tag" >/dev/null 2>&1; then
		echo "bump-deps.sh: $image:$latest_tag does not exist yet -- not bumping $var" >&2
		return
	fi
	digest=$(docker inspect --format='{{index .RepoDigests 0}}' "$image:$latest_tag" | sed -E 's/.*(@sha256:[0-9a-f]+)$/\1/')
	sed -i.bak -E "s#^${var}=.*#${var}=$image:$latest_tag$digest#" toolchain/images.env
	rm -f toolchain/images.env.bak
	note "$var $current_tag -> $latest_tag"
}
bump_toolchain_image OSS_IMAGE_PINNED ghcr.io/andreziviani/odi-toolchain-uclibc uclibc-
bump_toolchain_image DIAG_IMAGE_PINNED ghcr.io/andreziviani/odi-toolchain-freestanding freestanding-
bump_toolchain_image QEMU_KERNEL_IMAGE_PINNED ghcr.io/andreziviani/odi-toolchain-qemu-kernel-malta qemu-malta-

[ "$changed" = 1 ] || echo "bump-deps.sh: everything already at its latest pinned version"
