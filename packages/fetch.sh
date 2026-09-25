#!/usr/bin/env sh
#
# Fetch and verify one upstream source tarball.
#
#     fetch.sh <url> <sha256> [outdir]
#     ALT_URLS="<url> <url>" fetch.sh <url> <sha256>
#
# Every package this project builds is pinned by VERSION and verified by
# CONTENT. A tarball that does not match its hash is refused rather than built:
# these produce binaries that go into a device whose only recovery path is a
# serial console.
#
# BECAUSE the content is hash-pinned, WHERE it comes from does not matter.
# That is what makes falling back to another host safe here and not merely
# convenient: a mirror cannot hand us different bytes without the check below
# catching it. So a dead upstream costs nothing but a retry, and the one thing
# that would be dangerous -- trusting an unfamiliar host -- is not what is
# happening.
#
# The last resort is Software Heritage, which stores file contents addressed by
# the very hash we already pin. Coverage is partial (one of three tarballs
# checked on 2026-09-15), so it is a floor rather than a plan.
set -eu

url=${1:?usage: fetch.sh <url> <sha256> [outdir]}
want=${2:?missing sha256}
out=${3:-$(cd "$(dirname "$0")/.." && pwd)/dl}

mkdir -p "$out"
file="$out/$(basename "$url")"

if [ ! -f "$file" ]; then
	got_it=0
	for u in "$url" ${ALT_URLS:-} "https://archive.softwareheritage.org/api/1/content/sha256:$want/raw/"; do
		[ -n "$u" ] || continue
		echo "==> fetching $(basename "$url") from ${u%%/*}//$(echo "$u" | cut -d/ -f3)" >&2
		if curl -fsSL --retry 2 --max-time 900 -o "$file.part" "$u"; then
			mv "$file.part" "$file"
			got_it=1
			break
		fi
		rm -f "$file.part"
		echo "    no answer, trying the next source" >&2
	done
	if [ "$got_it" = 0 ]; then
		echo "Could not fetch $(basename "$url") from any source." >&2
		echo "Drop the file into $out by hand; it is verified either way." >&2
		exit 1
	fi
fi

got=$(shasum -a 256 "$file" | cut -d' ' -f1)
if [ "$got" != "$want" ]; then
	echo "SHA256 MISMATCH for $(basename "$url")" >&2
	echo "  expected $want" >&2
	echo "  got      $got" >&2
	rm -f "$file"
	exit 1
fi

echo "$file"
