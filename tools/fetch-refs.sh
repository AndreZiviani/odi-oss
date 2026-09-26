#!/bin/sh
# Fetch the public reference documents listed in docs/REFERENCES.md into
# refs/ (gitignored) and check each against its recorded SHA-256.
# The ITU download endpoint refuses non-browser clients, so the files come
# from the Internet Archive copy of that same endpoint.
set -eu

cd "$(dirname "$0")/.."
mkdir -p refs

WB=https://web.archive.org/web
ITU="https://www.itu.int/rec/dologin_pub.asp?lang=e&type=items&id="

fetch() {
	name=$1 snap=$2 id=$3 sum=$4
	out=refs/$name
	if [ ! -f "$out" ]; then
		echo "fetch $name"
		curl -fsSL -o "$out.part" "$WB/${snap}id_/${ITU}${id}"
		mv "$out.part" "$out"
	fi
	got=$(shasum -a 256 "$out" | cut -d" " -f1)
	if [ "$got" != "$sum" ]; then
		echo "$name: SHA-256 mismatch ($got), removed" >&2
		rm -f "$out"
		return 1
	fi
	echo "ok    $name"
}

fetch g984.3-201401.pdf 2024 'T-REC-G.984.3-201401-I!!PDF-E' \
	6124e0c8736e6a48bd7d1f9b84dc922e137edf5a76cddaf1131f1bf9d40585e4
fetch g984.3-202003-amd1.pdf 20260301155403 'T-REC-G.984.3-202003-I!Amd1!PDF-E' \
	e48b4f192a7dbf54dd1e7b961c6aa60b7cf56efcff53063d3cc4fbe6c64cc50c
