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
METRICSD_TAG=${METRICSD_TAG:-v1.0.3}
CONFD_REPO=${CONFD_REPO:-AndreZiviani/odi-ui}
CONFD_TAG=${CONFD_TAG-v1.0.5}

have() { command -v "$1" >/dev/null 2>&1; }

# Two ways to download, because a build host need not have gh:
#
#   gh      when it is installed and logged in (gh auth status), which
#           covers private repositories through its own login;
#   curl    otherwise. A public repository needs nothing; a private one
#           needs GITHUB_TOKEN (any token that can read the repository
#           contents), and goes through the REST API, since the plain
#           /releases/download/ URL does not accept a token.
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
				echo "  A private repository needs GITHUB_TOKEN (or a logged-in gh)." >&2
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
echo "metricsd  <- $METRICSD_REPO $METRICSD_TAG"
bin=$(fetch_asset "$METRICSD_REPO" "$METRICSD_TAG" metricsd)
sums=$(fetch_asset "$METRICSD_REPO" "$METRICSD_TAG" SHA256SUMS)
verify_sum "$sums" "$bin" metricsd
install -m 755 "$bin" "$OUT/metricsd"
echo "  verified, $(wc -c < "$bin" | tr -d ' ') bytes"

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
elif [ -n "${CONFD_BIN:-}" ] && [ -f "${CONFD_BIN}" ]; then
	# A local odi-ui build. Its assets live in the checkout, not beside the
	# binary, so point CONFD_ASSETS at that tree as well or the image gets a
	# daemon with nothing to serve.
	install -m 755 "$CONFD_BIN" "$OUT/confd"
	echo "  from CONFD_BIN=$CONFD_BIN"
	src=${CONFD_ASSETS:-$(cd "$(dirname "$CONFD_BIN")/.." 2>/dev/null && pwd)}
	if [ -n "$src" ] && [ -d "$src/web" ] && [ -d "$src/schema" ]; then
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
		echo "  WARNING: no assets found; set CONFD_ASSETS=<odi-ui checkout>." >&2
		echo "  The daemon would serve nothing." >&2
	fi
else
	echo "  SKIPPED: no CONFD_TAG set and no CONFD_BIN given." >&2
fi
