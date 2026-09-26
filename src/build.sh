#!/usr/bin/env bash
#
# Build our own binaries and collect them into out/bin, which is where
# image/build.sh looks.
#
# These are freestanding -- no libc, no crt0, no PT_INTERP -- so they do not use
# the toolchain in toolchain/oss at all. They are built with a stock Debian
# mips-linux-gnu cross compiler and depend on the kernel syscall ABI and nothing
# else. That is deliberate and is what makes them droppable onto a stock vendor
# image.
#
# metricsd and confd are NOT here. They are separate projects with their own
# releases -- see fetch-releases.sh.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
OUT=$ROOT/out/bin
mkdir -p "$OUT"

# Each entry is <directory>:<built binary>. The CLI tools and the daemons build
# the same way; only the directory differs.
BUILDS="
diag:build/diag
nv:build/nv
omci/cli:build/omcli
omci/respond:build/omcid
omci/probe:build/omciprobe
omci/capture:build/omcicap
igmp:build/igmpd
"

for entry in $BUILDS; do
	dir=${entry%%:*}
	bin=${entry#*:}
	[ -f "$dir/Makefile" ] || { echo "no Makefile in src/$dir" >&2; exit 1; }
	printf '%-16s ' "$dir"
	( cd "$dir" && make >/dev/null )
	[ -f "$dir/$bin" ] || { echo "built nothing at $dir/$bin" >&2; exit 1; }
	install -m 755 "$dir/$bin" "$OUT/$(basename "$bin")"
	echo "$(basename "$bin") $(stat -f%z "$dir/$bin" 2>/dev/null || stat -c%s "$dir/$bin")"
done

echo
echo "out/bin:"
find "$OUT" -type f -exec sh -c 'printf "  %-14s %8d\n" "$(basename "$1")" "$(wc -c < "$1")"' _ {} \;
