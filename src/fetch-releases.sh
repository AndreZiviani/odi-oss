#!/usr/bin/env bash
#
# Fetch the binaries that belong to OTHER projects into out/bin.
#
# metricsd (the Prometheus exporter) and confd (the web UI) are separate
# repositories with their own release cycles, so this image consumes their
# releases rather than carrying their source. Everything in src/ here is built
# from source by src/build.sh; these two are not.
#
# Both are freestanding MIPS binaries like ours, so they are dropped in as-is.
set -euo pipefail
# CONFD_BIN, CONFD_ASSETS and METRICSD_BIN are paths as the caller wrote them,
# relative to the caller's directory; this script then moves to src/, where a
# relative path would point somewhere else and the binary be "not found".
CALLER=$PWD
abs() { case $1 in /*) printf '%s' "$1" ;; *) printf '%s/%s' "$CALLER" "$1" ;; esac; }
[ -z "${CONFD_BIN:-}" ] || CONFD_BIN=$(abs "$CONFD_BIN")
[ -z "${CONFD_ASSETS:-}" ] || CONFD_ASSETS=$(abs "$CONFD_ASSETS")
[ -z "${METRICSD_BIN:-}" ] || METRICSD_BIN=$(abs "$METRICSD_BIN")
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
OUT=$ROOT/out/bin
DL=$ROOT/dl/releases
mkdir -p "$OUT" "$DL"

# Pin by tag for a reproducible image; override to move. CONFD_TAG set but
# empty (CONFD_TAG= CONFD_BIN=...) skips the confd release for a local build
# below, so it takes its default only when unset: with :- an empty value
# became the default tag again and CONFD_BIN was never reached.
METRICSD_REPO=${METRICSD_REPO:-AndreZiviani/odi-sfp-exporter}
METRICSD_TAG=${METRICSD_TAG:-v1.7.0}
# METRICSD_BIN=<path> puts a local odi-sfp-exporter build (its build/metricsd)
# in the image instead of the release, for a trial of an unreleased exporter.
# The manifest then says exporter=local.
if [ -n "${METRICSD_BIN:-}" ]; then
	[ -f "$METRICSD_BIN" ] || { echo "METRICSD_BIN=$METRICSD_BIN is not a file" >&2; exit 1; }
	METRICSD_TAG=local
fi
CONFD_REPO=${CONFD_REPO:-AndreZiviani/odi-ui}
CONFD_TAG=${CONFD_TAG-v1.8.0}

have() { command -v "$1" >/dev/null 2>&1; }

# Two ways to download, because a build host need not have gh:
#
#   gh      when it is installed and logged in (gh auth status);
#   curl    otherwise. Both repositories are public, so no login or token
#           is needed; an optional GITHUB_TOKEN, if set, only raises the
#           anonymous rate limit and routes the download through the REST
#           API instead of the plain /releases/download/ URL.
#
# USE_CURL=1 skips gh even when it is there.
# Either way the asset is checked against the release SHA256SUMS below, so
# the transport is not trusted.
use_gh=0
if [ "${USE_CURL:-0}" != 1 ] && have gh && gh auth status >/dev/null 2>&1; then
	use_gh=1
fi
have gh || have curl || { echo "gh or curl is required to fetch releases" >&2; exit 1; }

curl_asset() {
	repo=$1 tag=$2 asset=$3 out=$4
	if [ -z "${GITHUB_TOKEN:-}" ]; then
		curl -fsSL --retry 3 -o "$out" \
			"https://github.com/$repo/releases/download/$tag/$asset"
		return
	fi
	api=https://api.github.com/repos/$repo
	# The asset id, from the release JSON: the last "id" before the
	# "name" that is the asset (an asset object lists its id first, its
	# uploader after its name). Tokenised with tr and read with awk rather
	# than jq, which a build host need not have either; this works on the
	# compact and the pretty-printed form alike.
	id=$(curl -fsSL --retry 3 -H "Authorization: Bearer $GITHUB_TOKEN" \
		-H 'Accept: application/vnd.github+json' "$api/releases/tags/$tag" |
		tr -d ' \n\r\t' | tr ',{}' '\n\n\n' | sed -n '/^"assets":/,$p' |
		awk -F: -v n="\"$asset\"" '$1 == "\"id\"" {id = $2}
			$1 == "\"name\"" && $2 == n {print id; exit}')
	[ -n "$id" ] || { echo "  no asset $asset in $repo $tag" >&2; return 1; }
	curl -fsSL --retry 3 -H "Authorization: Bearer $GITHUB_TOKEN" \
		-H 'Accept: application/octet-stream' -o "$out" "$api/releases/assets/$id"
}

# Cached by tag: a release asset at a given tag never changes, so re-downloading
# it on every image build is pure waste.
fetch_asset() {
	repo=$1 tag=$2 asset=$3
	dest=$DL/$(echo "$repo" | tr / _)-$tag-$asset
	if [ ! -f "$dest" ]; then
		echo "  fetching $asset from $repo $tag" >&2
		if [ "$use_gh" = 1 ]; then
			gh release download "$tag" --repo "$repo" --pattern "$asset" \
				--output "$dest.part" --clobber
		else
			curl_asset "$repo" "$tag" "$asset" "$dest.part" || {
				rm -f "$dest.part"
				echo "  could not download $asset from $repo $tag." >&2
				echo "  Rate-limited? Set GITHUB_TOKEN (or use a logged-in gh)." >&2
				exit 1
			}
		fi
		mv "$dest.part" "$dest"
	fi
	echo "$dest"
}

# Both releases ship SHA256SUMS, so verify rather than trust the transport.
# The name checked is the one in the file, which is the release asset name.
verify_sum() {
	sums=$1 file=$2 name=$3
	want=$(awk -v n="$name" '$2 ~ ("(^|/)" n "$") {print $1; exit}' "$sums")
	got=$( (sha256sum "$file" 2>/dev/null || shasum -a 256 "$file") | cut -d' ' -f1)
	[ -n "$want" ] || { echo "  no $name line in SHA256SUMS" >&2; exit 1; }
	[ "$want" = "$got" ] || { echo "  $name sha256 mismatch: $got" >&2; exit 1; }
}

# What went in, for the image manifest (image/build.sh copies these into
# /etc/odi-build as exporter= and confd= lines, which the exporter itself
# reports as gpon_image_info labels).
printf 'METRICSD_TAG=%s\nCONFD_TAG=%s\n' "$METRICSD_TAG" "${CONFD_TAG:-local}" > "$OUT/releases.env"
if [ -n "${METRICSD_BIN:-}" ]; then
	echo "metricsd  <- METRICSD_BIN=$METRICSD_BIN"
	install -m 755 "$METRICSD_BIN" "$OUT/metricsd"
	echo "  local, $(wc -c < "$METRICSD_BIN" | tr -d ' ') bytes"
else
	echo "metricsd  <- $METRICSD_REPO $METRICSD_TAG"
	bin=$(fetch_asset "$METRICSD_REPO" "$METRICSD_TAG" metricsd)
	sums=$(fetch_asset "$METRICSD_REPO" "$METRICSD_TAG" SHA256SUMS)
	verify_sum "$sums" "$bin" metricsd
	install -m 755 "$bin" "$OUT/metricsd"
	echo "  verified, $(wc -c < "$bin" | tr -d ' ') bytes"
fi

# confd is NOT self-contained. It reads /etc/confd/ for the whole web UI and
# for four .tsv data files, so the release carries an asset bundle beside the
# binary and both have to reach the image. A confd without them starts,
# listens, and answers 404 to everything -- a failure that looks like success
# from the build side.
echo "confd     <- $CONFD_REPO"
ASSETS=$ROOT/out/confd-assets
if [ -n "$CONFD_TAG" ]; then
	bin=$(fetch_asset "$CONFD_REPO" "$CONFD_TAG" confd)
	tgz=$(fetch_asset "$CONFD_REPO" "$CONFD_TAG" confd-assets.tar.gz)
	sums=$(fetch_asset "$CONFD_REPO" "$CONFD_TAG" SHA256SUMS)
	verify_sum "$sums" "$bin" confd
	verify_sum "$sums" "$tgz" confd-assets.tar.gz
	install -m 755 "$bin" "$OUT/confd"
	rm -rf "$ASSETS"
	mkdir -p "$ASSETS"
	tar -xzf "$tgz" -C "$ASSETS"
	echo "  verified, $(wc -c < "$bin" | tr -d ' ') bytes + $(find "$ASSETS" -type f | wc -l | tr -d ' ') asset files"
elif [ -n "${CONFD_BIN:-}" ]; then
	# A local odi-ui build. Its assets live in the checkout, not beside the
	# binary, so point CONFD_ASSETS at that tree (or at the
	# confd-assets.tar.gz its `make assets` builds) as well.
	#
	# Every way this can go wrong stops the build. It used to warn and carry
	# on, and the image then shipped whatever confd and assets an earlier
	# build had left in out/ -- a trial of the wrong web UI that looks right.
	[ -f "$CONFD_BIN" ] || { echo "  CONFD_BIN=$CONFD_BIN is not a file" >&2; exit 1; }
	install -m 755 "$CONFD_BIN" "$OUT/confd"
	echo "  from CONFD_BIN=$CONFD_BIN"
	src=${CONFD_ASSETS:-$(cd "$(dirname "$CONFD_BIN")/.." 2>/dev/null && pwd)}
	if [ -f "$src" ]; then
		rm -rf "$ASSETS"
		mkdir -p "$ASSETS"
		tar -xzf "$src" -C "$ASSETS"
		echo "  assets from $src"
	elif [ -n "$src" ] && [ -d "$src/web" ] && [ -d "$src/schema" ]; then
		rm -rf "$ASSETS"
		mkdir -p "$ASSETS"
		cp "$src"/web/*.html "$src"/web/*.css "$src"/web/*.js "$ASSETS/"
		# settings.tsv is what makes the UI show only the keys this image
		# reads; without it every stock key is offered as editable.
		for f in keys.tsv meta.tsv consumers.tsv features.tsv settings.tsv; do
			cp "$src/schema/$f" "$ASSETS/$f"
		done
		echo "  assets from $src"
	else
		echo "  no assets at ${src:-(none)}: set CONFD_ASSETS to the odi-ui" >&2
		echo "  checkout or its build/confd-assets.tar.gz; a confd without" >&2
		echo "  them serves nothing." >&2
		exit 1
	fi
else
	echo "  no confd: CONFD_TAG is empty and CONFD_BIN is not set." >&2
	exit 1
fi
