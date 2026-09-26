#!/usr/bin/env sh
#
# The ISA gate: refuse a binary that contains instructions this CPU lacks.
#
#     isa-audit.sh <binary> [<binary>...]
#
# The RLX5281 is a Lexra MIPS core missing instructions every modern toolchain
# emits by default. A binary carrying one builds clean, links clean, and traps
# on the device the first time that code path runs -- which is why this is a
# build gate that exits non-zero, not a report.
#
# The audit itself is isa-audit in the toolchain image (the odi-toolchain
# repository, isa/isa-audit): the deny list, the executable-section filter
# that keeps a vmlinux .notes section from decoding as a phantom beql, and the
# reasons for both are there. It runs with our objdump (binutils 2.47,
# mips-linux-uclibc-objdump, from the uclibc image). This wrapper only stages
# the files so one container sees them all: starting the container costs
# more than the audit does.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# isa-allowlist.sh runs this with ISA_TOOL=isa-allowlist.
TOOL=${ISA_TOOL:-isa-audit}

[ $# -gt 0 ] || { echo "usage: $TOOL.sh <binary> [<binary>...]" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM
for b in "$@"; do
	[ -f "$b" ] || { echo "no such binary: $b" >&2; exit 1; }
	cp "$b" "$stage/$(basename "$b")"
done

IMAGE=$("$ROOT/packages/oss-env.sh")
echo "==> $TOOL (our objdump)" >&2
# Relative names, so the report says diag rather than /audit/diag.
docker run --rm -v "$stage:/audit:ro" -w /audit "$IMAGE" sh -c "$TOOL *"
